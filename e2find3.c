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
static const char *program_version = "0.3";

static unsigned int opt_after = 0;
static int opt_show_mtime = 0;
static int opt_show_ctime = 0;
static int opt_debug = 0;
static int opt_unique = 0;
static int opt_mountpoint = 0;
static char separator = '\n';

static char *fspath;
static ext2_filsys fs;
static ext2_inode_scan scan = 0;
/* Doc says: The buffer_blocks parameter controls how many blocks of the inode
 * table are read in at a time. A large number of blocks requires more memory,
 * but reduces the overhead in seeking and reading from the disk. If
 * buffer_blocks is zero, a suitable default value will be used. */
static int buffer_blocks = 0;

/* Holds the dirname (parent path) when iterating dirents. We need to know
 * 1/ if it's been allocated and 2/ how because it might be from stack or from
 * libext2 own's allocation routines */
static char *ppath;
static char ppath_err[16];
static enum ppath_st_e {
  STATUS_NONE,
  STATUS_ALLOC,
  STATUS_STATIC
} ppath_st;

/* A bitfield marking seleted inodes, used when a critera such as --after is
 * invoked */
static char *iselect = NULL;

static struct option optl[] = {
  {"print0",     no_argument,       NULL, '0'},
  {"after",      required_argument, NULL, 'a'},
  {"show-ctime", no_argument,       NULL, 'c'},
  {"debug",      no_argument,       NULL, 'd'},
  {"help",       no_argument,       NULL, 'h'},
  {"show-mtime", no_argument,       NULL, 'm'},
  {"mountpoint", no_argument,       NULL, 'p'},
  {"unique",     no_argument,       NULL, 'u'},
  {"version",    no_argument,       NULL, 'v'},
  {NULL, 0, NULL, 0},
};


#define dbg(msg, ...) if (opt_debug) { fprintf(stderr, "-- " msg "\n", ##__VA_ARGS__); }
#define err(ret, msg, ...) do { fprintf(stderr, "%s: " msg "\n", program_name, ##__VA_ARGS__); exit(ret); } while (0);

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

int dirent_cb(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private) {
  char *name;
  int filetype;;
  int name_len;
  char prefix[32];
  struct ext2_inode inode;
  ext2_ino_t ino;
  ext2_ino_t parent_ino;

  ino = dirent->inode;
  parent_ino = *(ext2_ino_t*)private;
  filetype = dirent->name_len >> 8;

  if (ino != parent_ino && (filetype == EXT2_FT_DIR) && ino != EXT2_ROOT_INO)
    /* Do not consider directory dirents other than '.' because they'll be
     * handed as the parent ino of their own dirent scan. Except for the root
     * folder which has no parent */
    return 0;

  if (iselect) {
    int ibyte = ino >> 3;
    int ibit  = 1 << (ino & 7);

    /* Selection says we're not interested in this inode */
    if (!(iselect[ibyte] & ibit))
      return 0;

    /* Clear the selection bit to show only one name per inode if requested */
    if (opt_unique)
      iselect[ibyte] &= ~ibit;
  }

  name = dirent->name;
  name_len = dirent->name_len & 0xff;

  /* Skip .. entry */
  if (name_len == 2 && name[0] == '.' && name[1] == '.')
    return 0;

  /* Lazy parent path lookup : this lookup should be run before iterating this
   * dir, but it might be not necessary to run it if it turns out that no
   * dirent here is selected. */
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

  if (opt_show_mtime || opt_show_ctime) {
    int ret;

    ret = ext2fs_read_inode(fs, ino, &inode);
    if (ret) {
      fprintf(stderr, "warning: read_inode #%d: error %d\n", ino, ret);
      snprintf(prefix, 16, "%s ", "?");
    } else {
      if (opt_show_mtime && opt_show_ctime)
        snprintf(prefix, 32, "%10d %10d ", inode.i_mtime, inode.i_ctime);
      else
      if (opt_show_mtime)
        snprintf(prefix, 32, "%10d ", inode.i_mtime);
      else
        snprintf(prefix, 32, "%10d ", inode.i_ctime);
    }
  } else {
    prefix[0] = '\0';
  }

  if (ino == parent_ino) /* aka '.' */
    printf("%s%s%c", prefix, ppath[0] ? ppath : "/", separator);
  else
    printf("%s%s/%.*s%c", prefix, ppath, name_len, name, separator);

  return 0;
}


int main(int argc, char **argv) {
  int opti = 0;
  int optc;
  int ret;
  char *blkpath;
  int scanned;
  int used;
  int selected;

  while ((optc = getopt_long(argc, argv, "0a:cdhmpuv", optl, &opti)) != -1) {
    switch (optc) {
      case '0':
        separator = '\0';
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

  if (strncmp(fspath, "/dev/", 5) != 0) {
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

  if (opt_after || opt_unique) {
    size_t bytes;

    bytes = (fs->super->s_inodes_count + 7) / 8;
    dbg("selection: allocating iselect bitfield (%zu bytes)", bytes);
    iselect = malloc(bytes);
    if (!iselect)
      err(6, "malloc(%zu bytes) for iselect", bytes);

    /* Either it's a selection bitfield (start with 0s, selection process will
     * set 1s), either it's a do-not-output twice bitfield (start with 1s,
     * clear when done). If both are required, selection process takes
     * precedence. */
    memset(iselect, opt_after ? 0 : 0xff, bytes);
  }

  /* If we have a selection criteria, we run a first inode scan and mark the
   * selected inode in a bitfield */
  if (opt_after) {
    ret = ext2fs_open_inode_scan(fs, buffer_blocks, &scan);
    if (ret)
      err(7, "ext2fs_open_inode_scan: error %d", ret);

    dbg("selection: starting inode scan");
    scanned = 0;
    used = 0;
    selected = 0;
    while(1) {
      ext2_ino_t ino;
      struct ext2_inode inode;

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
      used++;

      if (inode.i_mtime >= opt_after || inode.i_ctime >= opt_after) {
        iselect[ino >> 3] |= 1 << (ino & 7);
        selected++;
      }
    }
    dbg("dirs: %d inode scan done (%.1f%%)", scanned, scanned * 100. / fs->super->s_inodes_count);
    dbg("dirs: %d inode selected out of %d used inodes (%.1f%%)", selected, used, selected * 100. / used);

    ext2fs_close_inode_scan(scan);
  }

  /* We scan all inodes, searching for dirs, then iterate over all dirents for
   * every dir. As soon as we have an ino->name relationship, we use it to
   * resolve to a full pathname */
  ret = ext2fs_open_inode_scan(fs, buffer_blocks, &scan);
  if (ret)
    err(7, "ext2fs_open_inode_scan: error %d", ret);

  dbg("dirs: starting inode scan");
  scanned = 0;
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
    if (ret) {
      fprintf(stderr, "dirs: warning: inode #%d: scan error %d\n", ino, ret);
      continue;
    }

    if (ino == 0) {
      dbg("dirs: all inodes seen, ending scan loop");
      break;
    }
    scanned++;

    if ((ino < EXT2_GOOD_OLD_FIRST_INO && ino != EXT2_ROOT_INO) || /* Ignore special inodes - except the root one */
        inode.i_links_count == 0 ||                                /* Ignore unlinked inode */
        !LINUX_S_ISDIR(inode.i_mode))                              /* Only consider dirs */
      continue;

    dbg("#%-8d fetching dirents", ino);
    ppath_st = STATUS_NONE;
    ret = ext2fs_dir_iterate(fs, ino, 0, dirbuf, dirent_cb, &ino);
    if (ret)
      err(8, "ext2fs_dir_iterate: error %d", ret);
    if (ppath_st == STATUS_ALLOC)
      ext2fs_free_mem(&ppath);
  }
  dbg("dirs: %d inode scan done (%.1f%%)", scanned, scanned * 100. / fs->super->s_inodes_count);

  ext2fs_close_inode_scan(scan);
  ext2fs_close(fs);

  return 0;
}
