#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include "duckyfs.h"

static int img_fd = -1;

// Helper: Find a file by name
static int find_file(const char *name, struct duckyfs_file *f) {
    lseek(img_fd, sizeof(struct duckyfs_superblock), SEEK_SET);
    for (int i = 0; i < MAX_FILES; i++) {
        read(img_fd, f, sizeof(struct duckyfs_file));
        if (f->used && strcmp(f->name, name) == 0) {
            return i; // Return index in file array
        }
    }
    return -1;
}

// 1. Get file/directory stats (ls, stat)
static int ducky_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    struct duckyfs_file f;
    if (find_file(path + 1, &f) >= 0) {
        stbuf->st_mode = S_IFREG | 0666;
        stbuf->st_nlink = 1;
        stbuf->st_size = f.size;
        return 0;
    }

    return -ENOENT;
}

// 2. Read directory contents (ls)
static int ducky_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    if (strcmp(path, "/") != 0) return -ENOENT;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    struct duckyfs_file f;
    lseek(img_fd, sizeof(struct duckyfs_superblock), SEEK_SET);
    for (int i = 0; i < MAX_FILES; i++) {
        read(img_fd, &f, sizeof(struct duckyfs_file));
        if (f.used) {
            filler(buf, f.name, NULL, 0, 0);
        }
    }
    return 0;
}

// 3. Create a new file (touch)
static int ducky_mknod(const char *path, mode_t mode, dev_t rdev) {
    const char *filename = path + 1;
    struct duckyfs_file f;

    lseek(img_fd, sizeof(struct duckyfs_superblock), SEEK_SET);
    for (int i = 0; i < MAX_FILES; i++) {
        off_t pos = lseek(img_fd, 0, SEEK_CUR);
        read(img_fd, &f, sizeof(struct duckyfs_file));
        if (!f.used) {
            memset(&f, 0, sizeof(f));
            strncpy(f.name, filename, MAX_NAME_LEN - 1);
            f.used = 1;
            f.size = 0;

            lseek(img_fd, pos, SEEK_SET);
            write(img_fd, &f, sizeof(f));
            return 0;
        }
    }
    return -ENOSPC; // Disk full
}

// 4. Read data from a file (cat)
static int ducky_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct duckyfs_file f;
    if (find_file(path + 1, &f) < 0) return -ENOENT;

    if (offset >= f.size) return 0;
    if (offset + size > f.size) size = f.size - offset;

    memcpy(buf, f.data + offset, size);
    return size;
}

// 5. Write data to a file (echo "quack" > file)
static int ducky_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct duckyfs_file f;
    int idx = find_file(path + 1, &f);
    if (idx < 0) return -ENOENT;

    if (offset + size > BLOCK_SIZE) size = BLOCK_SIZE - offset;

    memcpy(f.data + offset, buf, size);
    f.size = offset + size;

    off_t pos = sizeof(struct duckyfs_superblock) + (idx * sizeof(struct duckyfs_file));
    lseek(img_fd, pos, SEEK_SET);
    write(img_fd, &f, sizeof(f));

    return size;
}

static const struct fuse_operations ducky_oper = {
    .getattr = ducky_getattr,
    .readdir = ducky_readdir,
    .mknod   = ducky_mknod,
    .read    = ducky_read,
    .write   = ducky_write,
};

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <disk_image> <mountpoint>\n", argv[0]);
        return 1;
    }

    img_fd = open(argv[1], O_RDWR);
    if (img_fd < 0) {
        perror("Failed to open image file");
        return 1;
    }

    struct duckyfs_superblock sb;
    read(img_fd, &sb, sizeof(sb));
    if (sb.magic != DUCKYFS_MAGIC) {
        fprintf(stderr, "Error: Not a valid DuckyFS image!\n");
        return 1;
    }

    argv[1] = argv[0];
    return fuse_main(argc - 1, &argv[1], &ducky_oper, NULL);
}
