/* e2find-alt - ext2/3/4 file search (alternate algorithm)
 * Copyright (C) 2015 Bearstech
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* e2find-alt was another attempt to tackle the problem of fast filesystem
 * traversal with inode->pathname resolution.
 *
 * Here we rely less on libext2fs and more on a few internal structures and
 * lookups : there is a single inode scan which also reads all dirents in a
 * simple data structure. Obviously, it uses more memory but less obviously
 * it's much faster than many calls to ext2fs_get_pathname(). The latter is
 * smart with memory : it's managed by Linux's page cache, which is a big win
 * from programming and ressource economy views. But it's more expensive to
 * reparse the inodes and dentries everytime we need to recursively resolve a
 * path.
 *
 * Tested on a basic RAID1 soft raid with old SATA disks on an old AMD CPU,
 * with a 8M inodes filesystem where 3.6M inodes are used :
 *
 *             Walllclock   CPU  RSS max
 *   e2find          72 s  36 s     1 MB
 *   e2find-alt      47 s  15 s   156 MB
 *
 * Although it looks interesting to trade memory for speed at a cheap rate of
 * 50 MB for 1M inode (we aim to comfortably work with 100M inodes, and 5 GB
 * memory is cheap these days), it has other drawbacks that forced us to put it
 * aside :
 * - it's hard to find all names/dirents for a given inode, since it's based on
 *   a per-inode iteration (while e2find is based on a per-dirent iteration)
 * - it has limitations due to simple data structures (4G max buffer for names)
 * - memory might be high on some filesystems (long names)
 * - it's only better than e2find when listing lots of inodes (ie. dump all of
 *   them), but e2find has been designed to be mostly used to cherry pick
 *   inodes; thus in the desired cases, e2find is as good as this e2find-alt
 */
#define _GNU_SOURCE /* For: getopt_long() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <blkid/blkid.h>
#include <ext2fs/ext2fs.h>

static const char *program_name = "e2find";
/*static const char *program_version = "0.1";*/

static unsigned int opt_after = 0;
static int opt_show_mtime = 0;
static int opt_verbose = 0;

static char *fspath;
static ext2_filsys fs;
static ext2_inode_scan scan = 0;
/* Doc says:The buffer_blocks parameter controls how many blocks of the inode
 * table are read in at a time. A large number of blocks requires more memory,
 * but reduces the overhead in seeking and reading from the disk. If
 * buffer_blocks is zero, a suitable default value will be used. */
static int buffer_blocks = 0;

static struct option optl[] = {
  {"after",      required_argument, NULL, 'a'},
  {"help",       no_argument,       NULL, 'h'},
  {"show-mtime", no_argument,       NULL, 'm'},
  {"verbose",    no_argument,       NULL, 'v'},
};


void show_help() {
  fprintf(stderr,
    "Usage: e2find [options] /path\n" \
    "\n" \
    "List all inodes of an ext2/3/4 filesystem, by name, as efficiently\n" \
    "as possible (ie. do not recursively traverse directory entries).\n" \
    "Path may be a file or folder on a filesystem (eg. /var), or a\n" \
    "backing block device (eg. /dev/sda1).\n" \
    "\n" \
    "Options :\n" \
    "\n" \
    "  -a, --after TIMESPEC  Only show files modified after TIMESPEC\n" \
    "  -h, --help            This help\n" \
    "  -m, --mtime           Prefix file names with mtime (as epoch)\n" \
    "  -v, --verbose         Show debug/progress informations\n" \
    "\n" \
    "  TIMESPEC is expressed as Unix epoch (local) time.sage: machin [options] /path\n");
}

/* Make sure we only call dbg() if necessary (might be used in tight loops for
 * debug purposes), but make dbg() usage easy */
#define dbg(...) if (opt_verbose) { __dbg(__VA_ARGS__); }

void __dbg(const char *msg, ...) {
  va_list ap;

  fprintf(stderr, "-- ");
  va_start(ap, msg);
  vfprintf(stderr, msg, ap);
  va_end(ap);
  fprintf(stderr, "\n");
}

void err(int ret, const char *msg, ...) {
  va_list ap;

  fprintf(stderr, "%s: ", program_name);
  va_start(ap, msg);
  vfprintf(stderr, msg, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(ret);
}


/* Simple dynamic array implementation :
 * - starts with at least ARRAY_MIN_BYTES for a first allocation
 * - then double the capacity when needed (arithmetic growth)
 * - unless capacity reaches ARRAY_INC_MAX_BYTES, where it is only
 *   increased by ARRAY_INC_MAX_BYTES (linear growth) */
#define ARRAY_MIN_BYTES       (64*1024)
#define ARRAY_INC_MAX_BYTES (1024*1024)

struct array {
  size_t count;
  size_t bytes_used;
  size_t bytes_alloc;;
  char  *buffer;
};

int array_init(struct array *a) {
  a->count       = 0;
  a->bytes_used  = 0;
  a->bytes_alloc = ARRAY_MIN_BYTES; 
  a->buffer      = malloc(a->bytes_alloc);
  return a->buffer != NULL;
}

int array_add(struct array *a, void *elt, size_t bytes) {
  if (a->bytes_used + bytes > a->bytes_alloc) {
    void *_buffer;
    int _bytes_alloc;
    _bytes_alloc = a->bytes_alloc + (a->bytes_alloc >= ARRAY_INC_MAX_BYTES ? ARRAY_INC_MAX_BYTES : a->bytes_alloc);
    dbg("array[%p]: reallocating from %d to %d bytes (used: %d; demand %d)", a, a->bytes_alloc, _bytes_alloc, a->bytes_used, bytes);
    _buffer      = realloc(a->buffer, _bytes_alloc);
    if (_buffer == NULL)
      return 0;
    a->bytes_alloc = _bytes_alloc;
    a->buffer      = _buffer;
  }
  memcpy(a->buffer + a->bytes_used, elt, bytes);
  a->count++;
  a->bytes_used += bytes;
  return 1;
}


#define IMATCH_SELECT (1 << 0)
#define IMATCH_DIR    (1 << 1)

struct imatch_t {
  ext2_ino_t ino;
  ext2_ino_t parent;
  __u32 mtime;
  unsigned int namei; /* Limits the total name storage to 4G chars, might not be enough for 100M+ inodes */
  unsigned int flags;
};
struct array imatch; /* Dynamic array of 'struct imatch_t' elements */
struct array inames; /* Dynamic vector of concatenated zero-terminated strings, indexed by imatch_t.namei */

struct imatch_t* imatch_by_ino(ext2_ino_t ino) {
  struct imatch_t *imatch_p;
  struct imatch_t *i;
  int index;
  int ihalf;

  imatch_p = (struct imatch_t*)imatch.buffer;

  /* Bisect the imatch array */
  index = imatch.count / 2;
  ihalf = index / 2;
  do {
    i = &imatch_p[index];

    if (i->ino == ino)
      return i;

    if (i->ino < ino)
      index += ihalf;
    else
      index -= ihalf;
  } while ((ihalf /= 2) > 0);

  /* When we're almost there, finish with a short scan. Two cases, two directions */
  if (i->ino < ino) {
    do {
      i++;
      if (i->ino == ino)
        return i;
    } while (i->ino < ino);
  } else {
    do {
      i--;
      if (i->ino == ino)
        return i;
    } while (i->ino > ino);
  }

  return NULL;
}

#define NAME_LEN_MAX 255

void imatch_set_name(struct imatch_t* imatch_p, const char* name, int name_len) {
  /* Insert name in inames array, fetching its index in the array beforehand */
  int _namei;
  char _name[NAME_LEN_MAX + 1];
  int _name_len;

  _namei = inames.bytes_used;

  /* The source is not a null-terminated string, copy it to a temp one with proper limits */
  _name_len = name_len < NAME_LEN_MAX ? name_len : NAME_LEN_MAX;
  strncpy(_name, name, _name_len);
  _name[_name_len] = '\0';
  array_add(&inames, _name, _name_len + 1);

  imatch_p->namei = _namei;
  dbg("#%-8d name is '%s' (inames index %d)", imatch_p->ino, _name, _namei);
}

int dir_cb(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private) {
  int name_len;
  struct imatch_t *imatch_p = NULL;
  ext2_ino_t *parent_p = (ext2_ino_t*)private;

  name_len = dirent->name_len & 0xff;

  /* Skip . and .. entries */
  if (dirent->name[0] == '.') {
    if (name_len == 1) return 0;
    if (name_len == 2 && dirent->name[1] == '.') return 0;
  }

  imatch_p = imatch_by_ino(dirent->inode);
  if (imatch_p && !imatch_p->namei) {
    imatch_p->parent = *parent_p;
    imatch_set_name(imatch_p, dirent->name, name_len);
  }
  return 0;
}

int _inode_to_path_r(ext2_ino_t ino, char *path, int *path_pos, int iter) {
  char *ipath;
  int ipath_len;
  struct imatch_t *imatch_p;
 
  /* Prepend a / path separator if it's not the first iteraiton (aka last path component) */
  if (iter || ino == 2) {
    if (*path_pos < 1)
      return 1; /* Not enough space */
    path[--*path_pos] = '/';
  }

  /*dbg("  #%-8d  iter=%d path='%s'", ino, iter, &path[*path_pos]);*/
  if (ino == 2)
    return 0; /* root inode, we're done */

  imatch_p = imatch_by_ino(ino);
  if (!imatch_p)
    return 2; /* inode not found */
  /*dbg("  imatch: parent=%d", imatch_p->parent);*/

  ipath = &inames.buffer[imatch_p->namei]; /* imatch -> index in inames -> string */
  ipath_len = strlen(ipath);
  if (*path_pos < ipath_len)
    return 1; /* Not enough space */
  *path_pos -= ipath_len;
  memcpy(&path[*path_pos], ipath, ipath_len);
  /*dbg("#%-8d  iter=%d parent=%-8d pos=%d ipath=%s ipath_len=%d, partial=%s", ino, iter, imatch_p->parent, *path_pos, ipath, ipath_len, &path[*path_pos]);*/
  return _inode_to_path_r(imatch_p->parent, path, path_pos, iter + 1);
}

/* Inode to name resolution goes backwards, ie. /foo/bar/baz solves baz, then
 * bar, then foo. We thus build the string starting from the end of the buffer,
 * and move it the beginning when the result is available */
int inode_to_path(ext2_ino_t ino, char *path, int path_max) {
  int path_pos;
  int ret;

  /*dbg("inode_to_path ino %-8d", ino);*/
  path_pos = path_max - 1;
  path[path_pos] = '\0';
  ret = _inode_to_path_r(ino, path, &path_pos, 0);
  if (ret)
    return ret;

  memcpy(path, &path[path_pos], path_max - path_pos);
  return 0;
}

void cancel(int sig) {
  exit(0);
}


int main(int argc, char **argv) {
  int opti = 0;
  int optc;
  int ret;
  int i;
  char *blkpath;
  struct stat stat;
  struct imatch_t *imatch_p;

  signal(SIGINT, cancel);

  while ((optc = getopt_long(argc, argv, "a:hmv", optl, &opti)) != -1) {
    switch (optc) {
      case 'a':
        if (!sscanf(optarg, "%u", &opt_after))
          err(11, "--after: positive integer expected");
        break;
      case 'h':
        show_help();
        exit(0);
      case 'm':
        opt_show_mtime = 1;
        break;
      case 'v':
        opt_verbose = 1;
        break;
    }
  }

  if (optind >= argc)
    err(1, "missing filesystem path or blockdev");
  fspath = argv[optind];

  if (strncmp(fspath, "/dev/", 5) != 0) {
    dbg("'%s' does not look like a blkdev, calling blkid", fspath);
   if (lstat(fspath, &stat) != 0)
      err(3, "lstat(%s): %s", fspath, strerror(errno));

    blkpath = blkid_devno_to_devname(stat.st_dev);
    if (!blkpath)
      err(4, "blkid_devno_to_devname(%d) failed", stat.st_dev);
    dbg("'%s' mapped to blkdev '%s'", fspath, blkpath);
    fspath = blkpath;
  }

  dbg("opening fs '%s'", fspath);
  ret = ext2fs_open(fspath, 0, 0, 0, unix_io_manager, &fs);
  if (ret)
    err(5, "ext2fs_open(%s): error %d", fspath, ret);

  dbg("initializing inode scan");
  ret = ext2fs_open_inode_scan(fs, buffer_blocks, &scan);
  if (ret)
    err(6, "ext2fs_open_inode_scan: error %d", ret);

  array_init(&imatch);
  array_init(&inames);
  array_add(&inames, "?", 2); /* This way the 0-index might be used to mark unmapped/undefined names */
  //array_add(&inames, "", 2); /* The root folder cannot be mapped from the inode scan, there is no corresponding dirent */

  dbg("[pass 1] scan all inodes and store all folder inodes and matching inodes");
  while(1) {
    ext2_ino_t ino;
    struct ext2_inode inode;
    struct imatch_t m;
    int isdir;
    int select;

    ret = ext2fs_get_next_inode(scan, &ino, &inode);
    if (ret) {
      fprintf(stderr, "warning: inode #%d: scan error %d\n", ino, ret);
      continue;
    }

    if (ino == 0) {
      dbg("all inodes seen, ending scan loop");
      break;
    }

    if (ino < EXT2_GOOD_OLD_FIRST_INO && ino != EXT2_ROOT_INO)
      continue; /* Ignore special inodes - except the root one */

    if (inode.i_links_count == 0)
      continue; /* Unused inode */

    isdir = LINUX_S_ISDIR(inode.i_mode);
    select = !opt_after || inode.i_mtime >= opt_after || inode.i_ctime >= opt_after;
    if (!isdir && !select) /* Not a folder and not a matching inode ? Skip it */
      continue;

    m.ino = ino;
    m.parent = 0;
    m.namei = 0;
    m.flags = 0;
    /* Use the more recent timestamp between ctime (meta mod) and mtime (data mod) */
    m.mtime = inode.i_mtime > inode.i_ctime ? inode.i_mtime : inode.i_ctime;
    if (select) m.flags |= IMATCH_SELECT;
    if (isdir)  m.flags |= IMATCH_DIR;
    array_add(&imatch, &m, sizeof(struct imatch_t));
    dbg("%c%c #%-8d", select ? 'S' : ' ', isdir ? 'D' : ' ', ino);
  }
  dbg("%d inodes stored", imatch.count);

  dbg("[pass 2] scan dirents from stored folder inodes, store names necessary for lookups");
  for (i = 0, imatch_p = (struct imatch_t*)imatch.buffer; i < imatch.count; i++, imatch_p++) {
    /* The block_buf parameter should either be NULL, or if the
     * ext2fs_dir_iterate function is called repeatedly, the overhead of
     * allocating and freeing scratch memory can be avoided by passing a
     * pointer to a scratch buffer which must be at least as big as the
     * filesystem’s blocksize. */
    char dirbuf[64*1024];

    if (imatch_p->flags & IMATCH_DIR) {
      ext2_ino_t ino = imatch_p->ino;
      dbg("Fetching dirents from #%-8d", ino);
      ret = ext2fs_dir_iterate(fs, ino, 0, dirbuf, dir_cb, &ino);
      if (ret)
        err(7, "ext2fs_dir_iterate: error %d", ret);
    }
  }
  dbg("%d names stored", inames.count);

  dbg("[pass 3] resolve inodes as pathnames");
  for (i = 0, imatch_p = (struct imatch_t*)imatch.buffer; i < imatch.count; i++, imatch_p++) {
    ext2_ino_t ino;
    char path[PATH_MAX];
    int ret;

    if (! imatch_p->flags & IMATCH_SELECT)
      continue;

    ino = imatch_p->ino;
    ret = inode_to_path(imatch_p->ino, path, PATH_MAX);
    if (ret) {
      fprintf(stderr, "warning: inode #%d: lookup error %d\n", ino, ret);
      continue;
    }
    if (opt_show_mtime)
      printf("%10d %s\n", imatch_p->mtime, path);
    else
      printf("%s\n", path);
  }

  dbg("shutting down inode scan");
  ext2fs_close_inode_scan(scan);

  return 0;
}
