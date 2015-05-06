# e2find
List all inodes of an ext2/3/4 filesystem, by name, as efficiently as possible (ie. do not recursively traverse directory entries).


##Usage: e2find [options] /path

Path may be a file or folder on a filesystem (eg. /var), or a
backing block device (eg. /dev/sda1).

##Options :

  - -a	TIMESPEC  Only show files modified after TIMESPEC
  - -s	Do not show more than one name per inode 
  
  TIMESPEC is expressed as Unix epoch (local) time.


###Build



####Packages needed


On Debian (as root) :


apt-get install make gcc e2fslibs-dev comerr-dev libblkid-dev


####Compilation

make