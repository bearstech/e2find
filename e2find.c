/* This program is free software; you can redistribute it and/or */
/* modify it under the terms of the GNU General Public License */
/* as published by the Free Software Foundation; either version 3 */
/* of the License, or (at your option) any later version. */

/* This program is distributed in the hope that it will be useful, */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the */
/* GNU General Public License for more details. */

/* You should have received a copy of the GNU General Public License */
/* along with this program; if not, write to the Free Software Foundation, */
/* Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA. */

/* @file e2find.c */
/* @author Olivier ANDRE <zitune@bearstech.com> */
/* @date 2015 */
/* @brief List all inodes of an ext2/3/4 filesystem, by name, as */
/*   efficiently as possible (ie. do not recursively traverse directory */
/*   entries). */


#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <ext2fs/ext2fs.h>
#include "ext2fs/ext2_fs.h"
#include <time.h>
#include <sys/stat.h>
#include <et/com_err.h>
#include <blkid/blkid.h>



static int alloc_size = 1024;




struct inode_walk_struct {
  ext2_ino_t		dir;
  
  ext2_ino_t		*iarray;
  int			num_inodes;
  int			position;
  char			*parent;
  unsigned int		get_pathname_failed:1;
  unsigned int		check_dirent:1;
  ext2_filsys		*fs;
  int			uniq;
};



int ext2fs_dirent_name_len(const struct ext2_dir_entry *entry)
{
  return entry->name_len & 0xff;
}


void
print_usage() {
  printf("Usage: e2find [options] /path\n");
  printf("\n");
  printf("List all inodes of an ext2/3/4 filesystem, by name, as efficiently\n");
  printf("as possible (ie. do not recursively traverse directory entries).\n");
  printf("Path may be a file or folder on a filesystem (eg. /var), or a\n");
  printf("backing block device (eg. /dev/sda1).\n");
  printf("\n");
  printf("Options :\n");
  printf("\n");
  printf("  -a, --after TIMESPEC  Only show files modified after TIMESPEC\n");
  printf("  -s, --single-link     Do not show more than one name per inode\n");
  printf("\n");
  printf("  TIMESPEC is expressed as Unix epoch (local) time.sage: machin [options] /path\n");
}







static int ncheck_proc(struct ext2_dir_entry *dirent,
		       int	offset EXT2FS_ATTR((unused)),
		       int	blocksize EXT2FS_ATTR((unused)),
		       char	*buf EXT2FS_ATTR((unused)),
		       void	*private)
{
  struct inode_walk_struct *iw = (struct inode_walk_struct *) private;
  errcode_t	retval;
  int		i;


  iw->position++;
  if (iw->position <= 2)
    return 0;
  for (i=0; i < iw->num_inodes; i++) {
    if (iw->iarray[i] == dirent->inode) {
      if (!iw->parent && !iw->get_pathname_failed) {
	retval = ext2fs_get_pathname(*iw->fs,
				     iw->dir,
				     0, &iw->parent);
	if (retval) {
	  com_err("ncheck", retval,
		  "while calling ext2fs_get_pathname for inode #%u", iw->dir);
	  iw->get_pathname_failed = 1;
	}
      }
      if (iw->parent)
	
	printf("%s/%.*s", iw->parent,
	       ext2fs_dirent_name_len(dirent),
	       dirent->name);	
      else
	printf("<%u>/%.*s", iw->dir,
	       ext2fs_dirent_name_len(dirent),
	       dirent->name);
      putc('\n', stdout);
      if (iw->uniq) 
	iw->iarray[i] = 0;
    }
  }

  return 0;
}

void do_ncheck(struct inode_walk_struct *iw)
{
  ext2_inode_scan		scan = 0;
  ext2_ino_t		ino;
  struct ext2_inode	inode;
  errcode_t		retval;


  if (!iw->iarray) {
    com_err("ibt", ENOMEM,
	    "while allocating inode number array");
    return;
  }

  retval = ext2fs_open_inode_scan(*iw->fs, 0, &scan);
  if (retval) {
    com_err("ibt", retval, "while opening inode scan");
    goto error_out;
  }

  do {
    retval = ext2fs_get_next_inode(scan, &ino, &inode);
  } while (retval == EXT2_ET_BAD_BLOCK_IN_INODE_TABLE);
  if (retval) {
    com_err("ibt", retval, "while starting inode scan");
    goto error_out;
  }

  while (ino) {
    if (!inode.i_links_count)
      goto next;
    /*
     * To handle filesystems touched by 0.3c extfs; can be
     * removed later.
     */
    if (inode.i_dtime)
      goto next;
    /* Ignore anything that isn't a directory */
    if (!LINUX_S_ISDIR(inode.i_mode))
      goto next;

    iw->position = 0;
    iw->parent = 0;
    iw->dir = ino;
    iw->get_pathname_failed = 0;

    retval = ext2fs_dir_iterate(*iw->fs, ino, 0, 0,
				ncheck_proc, iw);
    ext2fs_free_mem(&iw->parent);
    if (retval) {
      com_err("ibt", retval,
	      "while calling ext2_dir_iterate");
      goto next;
    }


  next:
    do {
      retval = ext2fs_get_next_inode(scan, &ino, &inode);
    } while (retval == EXT2_ET_BAD_BLOCK_IN_INODE_TABLE);

    if (retval) {
      com_err("ibt", retval,
	      "while doing inode scan");
      goto error_out;
    }
  }

 error_out:
  free(iw->iarray);
  if (scan)
    ext2fs_close_inode_scan(scan);
  return;
}




void get_inodes(struct inode_walk_struct *iw, int r_time) {

  errcode_t err = 0 ;
  ext2_inode_scan scan = 0;
  ext2_ino_t ino;
  struct ext2_inode inode;

  /*
   * We use libext2fs's inode scanning routines, which are particularly
   * robust.  (Note that getino cannot return an error.)
   */
  err = ext2fs_open_inode_scan(*iw->fs, 0, &scan);
  if (err) {
    com_err("ibt", err,
	    "error while opening inodes\n");
    exit(1);
  }
  for (;;) {
    err = ext2fs_get_next_inode(scan, &ino, &inode);
    if (err == EXT2_ET_BAD_BLOCK_IN_INODE_TABLE)
      continue;
    if (err) {
      com_err("ibt", err,"while scanning inode #%ld\n",
	      (long)ino);
      exit(1);
    }
    if (ino == 0)
      break;

    if (inode.i_mtime >= r_time){
      if (iw->num_inodes >= alloc_size) {
	  alloc_size += 1024;
	  iw->iarray = realloc(iw->iarray, sizeof(ext2_ino_t) * alloc_size);
	}
      iw->iarray[iw->num_inodes] = ino;
      iw->num_inodes++;
    }
    
  }
  ext2fs_close_inode_scan(scan);

}



int main(int argc, char **argv) {

  int retval;
  ext2_filsys fs;
  char disk[128] = "";
  //char *name = malloc(1024);
  int option = 0;
  int r_time = 0;
  struct stat s;



  struct inode_walk_struct iw;
  iw.check_dirent = 0;
  iw.uniq = 0;
  iw.iarray = malloc(sizeof(ext2_ino_t) * alloc_size);
  iw.num_inodes = 0;

  //Specifying the expected options
  while ((option = getopt(argc, argv,"a:s")) != -1) {
    switch (option) {
    case 'a' : r_time = atoi(optarg); 
      break;
    case 's' : iw.uniq = 1;
      break;
    default: print_usage(); 
      exit(EXIT_FAILURE);
    }
  }

  
  if (optind >= argc) {
    print_usage(); 
    exit(EXIT_FAILURE);
  }
  strcpy(disk, argv[optind]);
  
  if (strcmp(disk,"") == 0) {
    print_usage();
    exit(EXIT_FAILURE);
  }
  retval = ext2fs_open(disk, EXT2_FLAG_FORCE, 0, 0, unix_io_manager, &fs);

  if (retval) {
    // maybe it's not a device but a path
    // try to guest device

    retval = lstat(disk, &s);
    
    
    if (retval) {
      com_err("e2find", retval, "while opening filesystem %s\n", disk);
      exit(1);
    }
    strcpy(disk, blkid_devno_to_devname(s.st_dev));
    retval = ext2fs_open(disk, EXT2_FLAG_FORCE, 0, 0, unix_io_manager, &fs);
      
    if (retval) {
      com_err("e2find", retval, "while opening filesystem %s\n", disk);
      exit(1);
    }
  }
  iw.fs = &fs;
  
  get_inodes(&iw, r_time);
  do_ncheck(&iw);

  return 0;
}


  
