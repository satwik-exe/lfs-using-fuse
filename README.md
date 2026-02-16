# Log Structured File System (LFS) using FUSE

University mini-project implementing a basic Log-Structured File System
in user space using FUSE and C.

## Features implemented
- Virtual disk image
- Superblock
- Inode structure
- Inode map (IMAP)
- Root directory
- Directory entries
- Static file creation via mkfs
- FUSE mount (getattr, readdir, read – partial)

## Tech Stack
- C
- FUSE3
- Ubuntu 22.04
- VirtualBox

## Status
Work in progress.

## How to work on it

###Clone the repo
git clone https://github.com/satwik-exe/lfs-fuse.git
cd lfs-fuse

###Install dependencies inside Ubuntu
sudo apt update
sudo apt install build-essential libfuse3-dev fuse3 git pkg-config

###Verify FUSE
pkg-config fuse3 --cflags --libs

###Create local mount dir
mkdir -p mount

###Build and run locally every time
gcc src/mkfs_lfs.c -o mkfs_lfs
./mkfs_lfs

gcc src/lfs.c src/disk.c -o lfs `pkg-config fuse3 --cflags --libs`
./lfs mount
