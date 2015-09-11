/* e2find - ext2/3/4 file search
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

#define _GNU_SOURCE /* For: getopt_long() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <blkid/blkid.h>
#include <ext2fs/ext2fs.h>

static const char *program_name = "e2find";
static const char *program_version = "0.5";

static unsigned int opt_after = 0;
static int opt_show_mtime = 0;
static int opt_show_ctime = 0;
static int opt_debug = 0;
static int opt_unique = 0;
static int opt_mountpoint = 0;
static int opt_image = 0;
static char newline = '\n';

static char *fspath;
static ext2_filsys fs;
static ext2_inode_scan scan = 0;
/* Doc says: The buffer_blocks parameter controls how many blocks of the inode
 * table are read in at a time. A large number of blocks requires more memory,
 * but reduces the overhead in seeking and reading from the disk. If
 * buffer_blocks is zero, a suitable default value will be used. */
static int buffer_blocks = 0;

static struct option optl[] = {
  {"print0",     no_argument,       NULL, '0'},
  {"after",      required_argument, NULL, 'a'},
  {"show-ctime", no_argument,       NULL, 'c'},
  {"debug",      no_argument,       NULL, 'd'},
  {"help",       no_argument,       NULL, 'h'},
  {"image",      no_argument,       NULL, 'i'},
  {"show-mtime", no_argument,       NULL, 'm'},
  {"mountpoint", no_argument,       NULL, 'p'},
  {"unique",     no_argument,       NULL, 'u'},
  {"version",    no_argument,       NULL, 'v'},
  {NULL, 0, NULL, 0},
};


#define dbg(msg, ...) if (opt_debug) { fprintf(stderr, "-- " msg "\n", ##__VA_ARGS__); }
#define err(ret, msg, ...) do { fprintf(stderr, "%s: " msg "\n", program_name, ##__VA_ARGS__); exit(ret); } while (0);


/* Two bitfields to store per-inode flags, they are bit-addressed by #ino */
static char *iisdir  = NULL;
static char *iselect = NULL;

void bitfield_init(char** buffer, size_t nb_bits) {
  size_t bytes;

  bytes = (nb_bits + 7) / 8;
  *buffer = calloc(1, bytes);
  dbg("%p: allocating bitfield for %zu bits (%zu bytes)", *buffer, nb_bits, bytes);
  if (!*buffer)
    err(6, "calloc(1x %zu bytes) for bitfield", bytes);
}

void bitfield_fill(char* buffer, size_t nb_bits, char bit) {
  size_t bytes;
  
  bytes = (nb_bits + 7) / 8;
  memset(buffer, bit ? 0xff : 0, bytes);
}

void bitfield_set(char* buffer, size_t offset) {
  buffer[offset >> 3] |= 1 << (offset & 7);
}

void bitfield_clear(char* buffer, size_t offset) {
  buffer[offset >> 3] &= ~(1 << (offset & 7));
}

char bitfield_get(char* buffer, size_t offset) {
  return (buffer[offset >> 3] >> (offset & 7)) & 1;
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
    size_t _bytes_alloc;

    _bytes_alloc = a->bytes_alloc + (a->bytes_alloc >= ARRAY_INC_MAX_BYTES ? ARRAY_INC_MAX_BYTES : a->bytes_alloc);
    dbg("array[%p]: reallocating from %zu to %zu bytes (used: %zu; demand %zu)", a, a->bytes_alloc, _bytes_alloc, a->bytes_used, bytes);
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


struct inode_t {
  ext2_ino_t   ino;
  unsigned int dirent;
  __u32        time1;
  __u32        time2;
};

struct array inodes;  /* Array of inode_t structs */
size_t inodes_elsize;
enum {
  INODES_NONE,
  INODES_MTIME,
  INODES_CTIME,
  INODES_MTIME_CTIME,
} inodes_eltype;

struct dirent_empty_t { /* Only used to sizeof() the struct without the variable name[] array */
  unsigned int ino;
  unsigned int parent;
};
struct dirent_t {
  unsigned int ino;
  unsigned int parent;
  char name[255+3];
};
struct array dirents; /* Array of dirent_t structs, those are variable size elements */


void show_help() {
  printf(
    "Usage: e2find [options] /path\n" \
    "\n" \
    "List all inodes of an ext2/3/4 filesystem, by name, as efficiently\n" \
    "as possible (ie. do not recursively traverse directory entries).\n" \
    "Path may be a file or folder on a filesystem (eg. /var), or a\n" \
    "backing block device (eg. /dev/sda1).\n" \
    "\n" \
    "Options :\n" \
    "\n" \
    "  -0, --print0          Use 0 characters instead of newlines\n" \
    "  -a, --after TIMESPEC  Only show files modified after TIMESPEC\n" \
    "  -c, --ctime           Prefix file names with ctime (as epoch)\n" \
    "  -d, --debug           Show debug/progress informations\n" \
    "  -h, --help            This help\n" \
    "  -i, --image           Open /path as an image file\n" \
    "  -p, --mountpoint      Ensure /path is the fs mountpoint\n" \
    "  -m, --mtime           Prefix file names with mtime (as epoch)\n" \
    "  -u, --unique          Output at most one name per inode\n" \
    "  -v, --version         Show program name and version)\n" \
    "\n" \
    "TIMESPEC is expressed as Unix epoch (local) time.\n" \
    "If both --show-mtime and --show-ctime are used, mtime is\n" \
    "displayed first and ctime second\n");
}

void show_version() {
  printf("%s %s\n", program_name, program_version);
}


struct inode_t * inode_lookup(ext2_ino_t ino, unsigned int *pos) {
  char *inode_p;
  struct inode_t *i = NULL;
  int index;
  int ihalf;

  inode_p = inodes.buffer;

  /* Bisect the inodes[] array */
  index = inodes.count;
  ihalf = inodes.count;
  while ((ihalf /= 2) > 1) {
    if (i && i->ino < ino)
      index += ihalf;
    else
      index -= ihalf;

    i = (struct inode_t *)(inode_p + inodes_elsize * index);
    //dbg("lookup(%d): index=%d i->ino=%d", ino, index, i->ino);

    if (i->ino == ino) {
      if (pos) *pos = index;
      return i;
    }
  }

  /* When we're almost there, finish with a short scan. Two cases, two directions */
  if (!i || i->ino < ino) {
    /* For short lists (1 or 2 elements), the precedent loop might have not
     * initialized i. Force a scan starting from first element. */
    if (!i)
      index = -1;

    //dbg("lookup(%d): going up", ino);
    do {
      index++;
      i = (struct inode_t *)(inode_p + inodes_elsize * index);
      if (i->ino == ino) {
        if (pos) *pos = index;
        return i;
      }
    } while (i->ino < ino);
  } else {
    //dbg("lookup(%d): going down", ino);
    do {
      index--;
      i = (struct inode_t *)(inode_p + inodes_elsize * index);
      //dbg("lookup(%d):   index=%d i->ino=%d", ino, index, i->ino);
      if (i->ino == ino) {
        if (pos) *pos = index;
        return i;
      }
    } while (i->ino > ino);
  }

  return NULL;
}

struct dirent_cb_t {
  ext2_ino_t parent_ino;
  unsigned int parent_ino_idx;
};

int dirent_cb(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private) {
  struct dirent_cb_t *cb;
  char *name;
  int name_len;
  ext2_ino_t ino;
  unsigned int ino_idx;
  struct inode_t *i;
  struct dirent_t d;
  int padding;
  int p;

  cb = (struct dirent_cb_t *)private;
  ino = dirent->inode;

  /* Skip '.' entry because it will be handed as the parent ino of their own
   * dirent scan. Except for the root folder which has no parent */
  if (ino == cb->parent_ino && ino != EXT2_ROOT_INO)
    return 0;

  name = dirent->name;
  name_len = dirent->name_len & 0xff;
  //filetype = dirent->name_len >> 8;

  /* Skip '..' entry */
  if (name_len == 2 && name[0] == '.' && name[1] == '.')
    return 0;

  /* Store the root folder as an empty name, it's easier to handle later */
  if (ino == EXT2_ROOT_INO)
    name_len = 0;

  i = inode_lookup(ino, &ino_idx);
  if (!i)
    err(10, "inode_lookup(#%d) failed", ino);
  d.ino = ino_idx;
  d.parent = cb->parent_ino_idx;
  i->dirent = dirents.bytes_used;

  /* Fill in d.name + padd with zeros, aligning on 4 bytes */
  memcpy(d.name, name, name_len);
  padding = 4 - (name_len & 3);
  for (p = 0; p < padding; p++)
    d.name[name_len + p] = '\0';

  dbg("  #%-8d i%-8d d%-8d  '%s'", ino, d.ino, i->dirent, d.name);
  array_add(&dirents, &d, sizeof(struct dirent_empty_t) + name_len + padding);

  return 0;
}


/* As input, we have a dirent_t, thus the file basename, and a reference to its
 * parent dirent. We simply walk the chain up to the root. We fill in the
 * result buffer backwards, then offset it to 0.
 */
int dirent_to_path(struct dirent_t* d, char *path, int path_max) {
  int pos;
  int i = 0;

  pos = path_max;
  path[--pos] = '\0';
  //dbg("dirent_to_path(ino=i%d)", d->ino);

  while (1) {
    int len;
    int isroot;

    isroot = (*d->name == '\0');

    /* Inserts a / starting from the second iteration (i > 0) or if
     * we already hit the root folder */
    if (i++ || isroot) {
      if (pos < 1)
        return 1; /* path[] overflow */
      path[--pos] = '/';
    }
    //dbg("  loop %2d: pos=%3d path='%s'", i, pos, &path[pos]);

    if (i > 255) /* Too many components */
      return 2;

    if (isroot)
      break;

    len = strlen(d->name);
    if (len > pos)
      return 1; /* path[] overflow */
    pos -= len;
    memcpy(&path[pos], d->name, len);
    //dbg("    adding '%s': pos=%3d path='%s'", d->name, pos, &path[pos]);

    d = (struct dirent_t*)(dirents.buffer + d->parent);
  }

  memmove(path, &path[pos], path_max - pos);
  return 0;
}


int main(int argc, char **argv) {
  int opti = 0;
  int optc;
  int ret;
  char *blkpath;
  unsigned int scanned;
  unsigned int used;
  unsigned int selected;
  char *anyp;
  unsigned int index;

  while ((optc = getopt_long(argc, argv, "0a:cdhimpuv", optl, &opti)) != -1) {
    switch (optc) {
      case '0':
        newline = '\0';
        break;
      case 'a':
        if (!sscanf(optarg, "%u", &opt_after))
          err(11, "--after: positive integer expected");
        break;
      case 'c':
        opt_show_ctime = 1;
        break;
      case 'd':
        opt_debug = 1;
        break;
      case 'h':
        show_help();
        exit(0);
      case 'i':
        opt_image = 1;
        break;
      case 'm':
        opt_show_mtime = 1;
        break;
      case 'p':
        opt_mountpoint = 1;
        break;
      case 'u':
        opt_unique = 1;
        break;
      case 'v':
        show_version();
        exit(0);
      case '?':
        exit(10);
    }
  }

  if (optind >= argc)
    err(1, "missing filesystem path or blockdev");
  fspath = argv[optind];

  if (!opt_image && strncmp(fspath, "/dev/", 5) != 0) {
    struct stat stat;

    dbg("'%s' does not look like a blkdev, calling blkid", fspath);
    if (lstat(fspath, &stat) != 0)
      err(3, "lstat(%s): %s", fspath, strerror(errno));

    if (opt_mountpoint && stat.st_ino != EXT2_ROOT_INO)
      err(9, "%s is not an ext2/3/4 mountpoint", fspath);

    blkpath = blkid_devno_to_devname(stat.st_dev);
    if (!blkpath)
      err(4, "blkid_devno_to_devname(%lu) failed", stat.st_dev);
    dbg("'%s' mapped to blkdev '%s'", fspath, blkpath);
    fspath = blkpath;
  } else {
    if (opt_mountpoint)
      err(9, "%s is not an ext2/3/4 mountpoint", fspath);
  }

  dbg("opening fs '%s'", fspath);
  ret = ext2fs_open(fspath, 0, 0, 0, unix_io_manager, &fs);
  if (ret)
    err(5, "ext2fs_open(%s): error %d", fspath, ret);
  dbg("fs open: %d inodes, %d used (%.1f%%)",
    fs->super->s_inodes_count,
    fs->super->s_inodes_count - fs->super->s_free_inodes_count,
    (fs->super->s_inodes_count - fs->super->s_free_inodes_count) * 100. / fs->super->s_inodes_count);

  bitfield_init(&iisdir, fs->super->s_inodes_count);
  bitfield_init(&iselect, fs->super->s_inodes_count);
  /* No search criterion : pre-select everything. This bitfield is still useful
   * for --unique deduplication. */
  if (!opt_after)
    bitfield_fill(iselect, fs->super->s_inodes_count, 1);

  array_init(&inodes);  /* Dynamically grows, no initial size */
  array_init(&dirents); /* Dynamically grows, no initial size */

  if (opt_show_mtime && opt_show_ctime) {
    inodes_eltype = INODES_MTIME_CTIME;
    inodes_elsize = sizeof(struct inode_t);
  }
  else if (opt_show_mtime) {
    inodes_eltype = INODES_MTIME;
    inodes_elsize = sizeof(struct inode_t) - 4;
  }
  else if (opt_show_ctime) {
    inodes_eltype = INODES_CTIME;
    inodes_elsize = sizeof(struct inode_t) - 4;
  }
  else {
    inodes_eltype = INODES_NONE;
    inodes_elsize = sizeof(struct inode_t) - 8;
  }
  dbg("inodes[] element size is %zu bytes", inodes_elsize);

  /* Pass 1 : inode scan. Fills in :
   *
   * - inodes[] : FIXME
   * - iisdir[] :
   * - iselect[] :
   */
  ret = ext2fs_open_inode_scan(fs, buffer_blocks, &scan);
  if (ret)
    err(7, "ext2fs_open_inode_scan: error %d", ret);

  dbg("[1] Inode scan");
  scanned = 0;
  used = 0;
  selected = 0;
  while(1) {
    ext2_ino_t ino;
    struct ext2_inode inode;
    struct inode_t i;

    ret = ext2fs_get_next_inode(scan, &ino, &inode);
    if (ret) {
      fprintf(stderr, "selection: warning: inode #%d: scan error %d\n", ino, ret);
      continue;
    }

    if (ino == 0) {
      dbg("selection: all inodes seen, ending scan loop");
      break;
    }
    scanned++;

    if ((ino < EXT2_GOOD_OLD_FIRST_INO && ino != EXT2_ROOT_INO) || /* Ignore special inodes - except the root one */
        inode.i_links_count == 0)                                  /* Ignore unused inode */
      continue;
    used++; /* OK, this is a used inode, let's record some data */

    /* Update iflags[] */
    if (LINUX_S_ISDIR(inode.i_mode))
      bitfield_set(iisdir, ino);
    if (opt_after && (inode.i_mtime >= opt_after || inode.i_ctime >= opt_after))
      bitfield_set(iselect, ino);

    /* Update inodes[] */
    i.ino = ino;
    i.dirent = 0;
    switch (inodes_eltype) {
      case INODES_NONE:
        break;
      case INODES_MTIME:
        i.time1 = inode.i_mtime;
        break;
      case INODES_CTIME:
        i.time1 = inode.i_ctime;
        break;
      case INODES_MTIME_CTIME: ;
        i.time1 = inode.i_mtime;
        i.time2 = inode.i_ctime;
        break;
    }
    dbg("+%8d #%8d", used-1, ino);
    array_add(&inodes, &i, inodes_elsize);
  }
  dbg("inode scan done, %d scanned (%.1f%%)", scanned, scanned * 100. / fs->super->s_inodes_count);
  dbg("%d inode selected out of %d used inodes (%.1f%%)", selected, used, selected * 100. / used);

  ext2fs_close_inode_scan(scan);


  /* Pass 2 : dirent scan.
   *
   * In order to run ino->fullpath inverse resolutions, we need to collect all
   * dirents with parenting information. This loop run ext2fs_dir_iterate() on
   * every folder inode. The dirent_cb() callback fills dirents[] in.
   */
  dbg("[2] Dirent scan");
  for (index = 0, anyp = inodes.buffer; index < inodes.count; anyp += inodes_elsize, index++) {
    /* The block_buf parameter should either be NULL, or if the
     * ext2fs_dir_iterate function is called repeatedly, the overhead of
     * allocating and freeing scratch memory can be avoided by passing a
     * pointer to a scratch buffer which must be at least as big as the
     * filesystem’s blocksize. */
    char dirbuf[64*1024];
    struct inode_t *ip;
    ext2_ino_t ino;
    struct dirent_cb_t cb;

    ip = (struct inode_t *)anyp;
    ino = ip->ino;

    if (!bitfield_get(iisdir, ip->ino)) /* Filter non-dir inodes */
      continue;

    dbg("#%-8d i%d (folder)", ino, index);
    cb.parent_ino = ino;
    cb.parent_ino_idx = index;
    ret = ext2fs_dir_iterate(fs, ino, 0, dirbuf, dirent_cb, &cb);
    if (ret)
      err(8, "ext2fs_dir_iterate: error %d", ret);
  }
  dbg("dirent scan done (%zu dirents)", dirents.count);

  ext2fs_close(fs);

  /* Pass 2.5 : fix dirents[] .parent-as-inodes[]-index into .parent-as-dirents[]-index
   */
  dbg("[2.5] Converting dirents[].parent");
  for (index = 0, anyp = dirents.buffer; index < dirents.count; index++) {
    struct dirent_t *d;
    struct inode_t *ip;
    size_t name_len;

    d = (struct dirent_t *)anyp;
    ip = (struct inode_t *)(inodes.buffer + inodes_elsize * d->parent);
    dbg("d%-8ld %-24s : i%-8d -> d%-8d", anyp - dirents.buffer, d->name, d->parent,  ip->dirent);
    d->parent = ip->dirent;

    name_len = strlen(d->name);
    anyp += sizeof(struct dirent_empty_t) + ((name_len + 4) & ~3);
  }

  /* Pass 3 : iterate over dirents[], resolving fullpaths and displaying result
   */
  dbg("[3] Iterate over dirents");
  for (index = 0, anyp = dirents.buffer; index < dirents.count; index++) {
    struct dirent_t *d;
    struct inode_t *i;
    char path[PATH_MAX];
    char prefix[32];
    int ret;
    size_t name_len;

    d = (struct dirent_t *)anyp;
    i = (struct inode_t *)(inodes.buffer + inodes_elsize * d->ino);
    if (!bitfield_get(iselect, i->ino))
      goto next_dirent; /* Not selected for output */
    if (opt_unique)
      bitfield_clear(iselect, i->ino); /* Don't print another name for this inode */

    ret = dirent_to_path(d, path, PATH_MAX);
    if (ret) {
      fprintf(stderr, "warning: #%d/'%s': path resolution error %d", d->ino, d->name, ret);
      goto next_dirent;
    }
    dbg("#%-8d i%-8d d%-8ld '%s'", i->ino, d->ino, anyp - dirents.buffer, path);

    switch (inodes_eltype) {
      case INODES_NONE:
        *prefix = '\0';
        break;
      case INODES_MTIME:
      case INODES_CTIME:
        snprintf(prefix, 32, "%10d ", i->time1);
        break;
      case INODES_MTIME_CTIME: ;
        snprintf(prefix, 32, "%10d %10d ", i->time1, i->time2);
        break;
    }
    printf("%s%s%c", prefix, path, newline);

  next_dirent:
    name_len = strlen(d->name);
    anyp += sizeof(struct dirent_empty_t) + ((name_len + 4) & ~3);
  }

  return 0;
}
