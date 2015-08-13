# e2find

List all inodes of an ext2/3/4 filesystem, by name, as efficiently as possible (ie. do not recursively traverse directory entries).

This is achieved by directly opening the blockdevice as readonly and reading the filesystem data structures with libe2fs. e2find may be used on mounted filesystems although results might not be consistent - as far as traversing a whole mounted filesystem may be (read : not at all), which might be a concern or not.

The primary goal of e2find is to replace the unscalable readdir() API with a 'find'-like tool which may be plugged with usual Unix tools which have a --files-from facility (like 'rsync'). On large filesystems (> 10M inodes and > 1 TB data), e2find has already enabled maintenance tasks which took hours to be run in a matter of minutes.

Currently e2find has been mainly designed for its companion program 'e2sync' which implements a fast rsync between two local or remote ext2/3/4 filesystems. It relies on the traditionnal data+metadata sync capabilities of rsync and e2find to quickly list inodes and their modification times.

## Performance

TODO : graphes seekwatcher sur find vs e2find pour le listing complet d'un fs avec affichage ctime+mtime.

## Security

As soon as you give the rights to e2find to open the filesystem's backing blockdevice, you have full read access on all data and metadata of the filesystem. Consider yourself root on this filesystem. Since synchronizing filesystems in all cases require to be root, it's not considered as a bug but as a feature.

On the other hand, it does mean that e2find is an operator tool and is not suitable for end-users working on shared ressources.

## Build

On Debian / Ubuntu :

    sudo apt-get install make gcc e2fslibs-dev comerr-dev libblkid-dev
    make

'e2sync' is a Perl program and only requires Perl core.
