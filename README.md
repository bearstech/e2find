# e2find

List all inodes of an ext2/3/4 filesystem, by name, as efficiently as possible
(ie. do not recursively traverse directory entries). May also optionnaly dump
ctime and mtime metadata.

This is achieved by opening the blockdevice read-only and reading the
filesystem data structures directly with libe2fs. `e2find` may be used on
mounted filesystems although results might not be consistent - as far as
traversing a whole mounted filesystem may be - which might be a concern or not.

The primary goal of `e2find` is to replace the unscalable readdir() API with a
'find'-like tool which may be plugged with usual Unix tools which have a
--files-from facility (like 'rsync'). On large filesystems (> 10M inodes,
`e2find` has already enabled maintenance tasks which took hours to be run in a
matter of minutes.

Currently `e2find` has been mainly designed for its companion program 'e2sync'
which implements a fast rsync between two local or remote ext2/3/4 filesystems.
It relies on the traditionnal data+metadata sync capabilities of rsync and
`e2find` to quickly list inodes and their modification times.

**Warning**: this program targets spindle-based storage. Optimizing seeks is
almost a no-op on SSD or NVRAM systems; for those `e2find` will be at most a
CPU vs. RAM tradeoff where you may spare a lot of syscalls at the cost of
duplicating a part of the inodes and dentries structures into userland, which
might end up being wasteful depending on your configuration.


## Build

On Debian / Ubuntu :

    sudo apt-get install make gcc e2fslibs-dev comerr-dev libblkid-dev
    make

'e2sync' is a Perl program which requires Perl 5 and rsync >= 3.1.0.


## Performance

Block access has been traced with the help of
[Seekwatcher](https://oss.oracle.com/~mason/seekwatcher/) and blktrace, here on
a large RAID array while extracting exactly the same metadata from all the
inodes of a given filesystem. The first method uses the venerable `find` while
the second one invokes `e2find` :

![e2find vs. find block offsets](perf/compare-offset.png?raw=true)

The operation is much faster on spindles (45 vs. 470 sec) and most importantly
puts much less I/O pressure by using sequential accesses and far less IOPS. See
the [perf/ folder](perf/) for complete reports.

Memory required will depend on the number of allocated inodes, number of
folders and the average file name size. On a 9M inodes filesystem with
an average file name size of 7 chars, 400 MB (names only) to 470 MB
(name+mtime+ctime) of memory was required (on a 64-bit system).

Please note that `e2sync` (which invokes `e2find` on the local and remote ends)
will also require about the same amount of memory on the local end.


## Security

As soon as you give the rights to `e2find` to open the filesystem's backing
blockdevice, you have full read access on all data and metadata of the
filesystem. Consider yourself root on this filesystem. Since synchronizing
whole filesystems requires to be root, this is rather considered a feature than
a bug.

`e2find` inherit's libe2fs ability to work on filesystem images, thus it's
still usable as a non-privileged user (eg. for testing or analysis purposes).


## Implementation

`e2find` uses a two-pass process : it gathers all used inodes in a first pass, then reads all directory entries in inode order in a second pass. Then it displays the names found from the directory entries, associating them with the metadata collected from the inodes.

Through hardlinks, a file-type inode may have several distinct names. As a default all names are printed out. It is possible to ask to print only one name per inode (-u option), although the name chosen to represent the inode in this case is arbitrary (there is no 'canonical' or 'first' name, all directory entries are equal for naming an inode).

The data structures used to implement the process are quite simple (arrays) but care has been taken for all operations to be o(1) or o(log(n)).
