CC      = gcc
CFLAGS  = -Wall -Wextra -g $(shell pkg-config --cflags fuse3)
LDFLAGS = $(shell pkg-config --libs fuse3)

# Source files shared between lfs and mkfs
COMMON_SRCS = disk.c log.c inode.c
COMMON_OBJS = $(COMMON_SRCS:.c=.o)

LFS_SRCS    = lfs.c $(COMMON_SRCS)
LFS_OBJS    = $(LFS_SRCS:.c=.o)

MKFS_SRCS   = mkfs_lfs.c $(COMMON_SRCS)
MKFS_OBJS   = $(MKFS_SRCS:.c=.o)

.PHONY: all clean mount umount format

all: lfs mkfs_lfs

lfs: $(LFS_OBJS)
	$(CC) $(CFLAGS) -o ../lfs $^ $(LDFLAGS)

mkfs_lfs: $(MKFS_OBJS)
	$(CC) $(CFLAGS) -o ../mkfs_lfs $^

%.o: %.c lfs.h
	$(CC) $(CFLAGS) -c -o $@ $<

# --- convenience targets ---

format: mkfs_lfs
	../mkfs_lfs

mount: lfs
	mkdir -p mount
	./lfs -f mount &

umount:
	fusermount3 -u mount

clean:
	rm -f *.o lfs mkfs_lfs lfs.img
