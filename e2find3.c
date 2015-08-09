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

/* Holds the dirname (parent path) when iterating dirents. We need to know 1/
 * if it's been allocated and 2/ how because it might be from heap or from
 * libext2 own's allocation routines */
char *ppath;
char ppath_err[16];
enum ppath_st_e {
  STATUS_NONE,
  STATUS_ALLOC,
  STATUS_STATIC
} ppath_st;

/* A bitfield marking seleted inodes, used when a critera such as --after is invoked */
char *iselect = NULL;

static struct option optl[] = {
  {"after",      required_argument, NULL, 'a'},
  {"help",       no_argument,       NULL, 'h'},
  {"show-mtime", no_argument,       NULL, 'm'},
  {"verbose",    no_argument,       NULL, 'v'},
  {NULL, 0, NULL, 0},
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

int dirent_cb(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private) {
  char *name;
  int filetype;;
  int name_len;
  char prefix[16];
  struct ext2_inode inode;
  ext2_ino_t ino;
  ext2_ino_t parent_ino;

  ino = dirent->inode;
  parent_ino = *(ext2_ino_t*)private;
  filetype = dirent->name_len >> 8;

  if (ino != parent_ino && (filetype & 2) && ino != EXT2_ROOT_INO)
    /* Do not consider directory dirents other than '.' because they'll be
     * handed as the parent ino of their own dirent scan. Except for the root
     * folder which has no parent */
    return 0;

  if (iselect && !(iselect[ino >> 3] & (1 << (ino & 7))))
    return 0; /* Selection says we're not interested in this inode */

  name = dirent->name;
  name_len = dirent->name_len & 0xff;

  /* Skip .. entry */
  if (name_len == 2 && name[0] == '.' && name[1] == '.')
    return 0;

  /* Lazy parent path lookup : this lookup should be run before iterating this
   * dir, but it might be not necessary to run it if it turns out that no
   * dirent is selected. */
  if (ppath_st == STATUS_NONE) {
    if (parent_ino == EXT2_ROOT_INO) {
      ppath = "";  /* libext2fs resolves this to '/' but we prefer it '' to
                      work with path components concatenation */
      ppath_st = STATUS_STATIC;
    } else {
      int err;

      err =ext2fs_get_pathname(fs, parent_ino, 0, &ppath);
      if (err) {
        dbg("warning: get_pathname(%d): error %d", parent_ino, err);
        snprintf(ppath_err, 16, "<%-8d>", parent_ino);
        ppath = ppath_err;
        ppath_st = STATUS_STATIC;
      } else {
        ppath_st = STATUS_ALLOC;
      }
    }
  }

  if (opt_show_mtime) {
    int ret;

    ret = ext2fs_read_inode(fs, ino, &inode);
    if (ret) {
      fprintf(stderr, "warning: read_inode #%d: error %d\n", ino, ret);
      snprintf(prefix, 16, "%10s ", "?");
    } else {
      snprintf(prefix, 16, "%10d ", inode.i_mtime > inode.i_ctime ? inode.i_mtime : inode.i_ctime);
    }
  } else {
    prefix[0] = '\0';
  }

  if (ino == parent_ino) /* aka '.' */
    printf("%s%s\n", prefix, ppath[0] ? ppath : "/");
  else
    printf("%s%s/%.*s\n", prefix, ppath, name_len, name);

  return 0;
}


int main(int argc, char **argv) {
  int opti = 0;
  int optc;
  int ret;
  char *blkpath;
  struct stat stat;

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
      case '?':
        exit(10);
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
  dbg("fs open: %d inodes, %d used (%.1f%%)",
    fs->super->s_inodes_count,
    fs->super->s_inodes_count - fs->super->s_free_inodes_count,
    (fs->super->s_inodes_count - fs->super->s_free_inodes_count) * 100. / fs->super->s_inodes_count);

  /* If we have a selection criteria, we run a first inode scan and mark the
   * selected inode in a bitfield */
  if (opt_after) {
    size_t bytes;
    int selected = 0;

    bytes = (fs->super->s_inodes_count + 7) / 8;
    dbg("selection: allocating iselect bitfield (%d bytes)", bytes);
    iselect = calloc(bytes, 1);
    if (!iselect)
      err(6, "calloc(%d bytes) for iselect", bytes);

    ret = ext2fs_open_inode_scan(fs, buffer_blocks, &scan);
    if (ret)
      err(7, "ext2fs_open_inode_scan: error %d", ret);

    dbg("selection: starting inode scan");
    while(1) {
      ext2_ino_t ino;
      struct ext2_inode inode;

      ret = ext2fs_get_next_inode(scan, &ino, &inode);
      if (ret) {
        fprintf(stderr, "selection: warning: inode #%d: scan error %d\n", ino, ret);
        continue;
      }

      if (ino == 0) {
        dbg("all inodes seen, ending scan loop");
        break;
      }

      if ((ino < EXT2_GOOD_OLD_FIRST_INO && ino != EXT2_ROOT_INO) || /* Ignore special inodes - except the root one */
          inode.i_links_count == 0)                                  /* Ignore unused inode */
        continue;

      if (inode.i_mtime >= opt_after || inode.i_ctime >= opt_after) {
        iselect[ino >> 3] |= 1 << (ino & 7);
        selected++;
      }
      /* Use the more recent timestamp between ctime (meta mod) and mtime (data mod) */
      //m.mtime = inode.i_mtime > inode.i_ctime ? inode.i_mtime : inode.i_ctime;
    }
    dbg("selection: inode scan done (%d inodes selected)", selected);

    ext2fs_close_inode_scan(scan);
  }

  /* We scan all inodes, searching for dirs, then iterate over all dirents for
   * every dir. As soon as we have an ino->name relationship, we use it to
   * resolve to a full pathname */
  ret = ext2fs_open_inode_scan(fs, buffer_blocks, &scan);
  if (ret)
    err(7, "ext2fs_open_inode_scan: error %d", ret);

  dbg("dirs: starting inode scan");
  while(1) {
    ext2_ino_t ino;
    struct ext2_inode inode;
    /* The block_buf parameter should either be NULL, or if the
     * ext2fs_dir_iterate function is called repeatedly, the overhead of
     * allocating and freeing scratch memory can be avoided by passing a
     * pointer to a scratch buffer which must be at least as big as the
     * filesystem’s blocksize. */
    char dirbuf[64*1024];

    ret = ext2fs_get_next_inode(scan, &ino, &inode);
    if (__builtin_expect(ret, 0)) {
      fprintf(stderr, "dirs: warning: inode #%d: scan error %d\n", ino, ret);
      continue;
    }

    if (ino == 0) {
      dbg("all inodes seen, ending scan loop");
      break;
    }

    if ((ino < EXT2_GOOD_OLD_FIRST_INO && ino != EXT2_ROOT_INO) || /* Ignore special inodes - except the root one */
        inode.i_links_count == 0 ||                                /* Ignore unused inode */
        !LINUX_S_ISDIR(inode.i_mode))                              /* Only consider dirs */
      continue;

    dbg("%-8d fetching dirents", ino);
    ppath_st = STATUS_NONE;
    ret = ext2fs_dir_iterate(fs, ino, 0, dirbuf, dirent_cb, &ino);
    if (ret)
      err(8, "ext2fs_dir_iterate: error %d", ret);
    if (ppath_st == STATUS_ALLOC)
      ext2fs_free_mem(&ppath);
  }
  dbg("selection: inode scan done");

  ext2fs_close_inode_scan(scan);
  ext2fs_close(fs);

  return 0;
}
