#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include "duckyfs.h"

static int img_fd = -1;

// Helper: Current unix timestamp
static uint32_t now_ts(void) {
    return (uint32_t)time(NULL);
}

// Helper: Read the superblock
static int read_sb(struct duckyfs_superblock *sb) {
    if (lseek(img_fd, 0, SEEK_SET) < 0) return -1;
    if (read(img_fd, sb, sizeof(*sb)) != (ssize_t)sizeof(*sb)) return -1;
    return 0;
}

// Helper: Write the superblock back
static int write_sb(const struct duckyfs_superblock *sb) {
    if (lseek(img_fd, 0, SEEK_SET) < 0) return -1;
    if (write(img_fd, sb, sizeof(*sb)) != (ssize_t)sizeof(*sb)) return -1;
    return 0;
}

// Helper: Persist one file entry back to its slot
static int write_file(int idx, const struct duckyfs_file *f) {
    off_t pos = sizeof(struct duckyfs_superblock) + (idx * sizeof(struct duckyfs_file));
    if (lseek(img_fd, pos, SEEK_SET) < 0) return -1;
    if (write(img_fd, f, sizeof(*f)) != (ssize_t)sizeof(*f)) return -1;
    return 0;
}

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
        stbuf->st_blocks = (f.size + 511) / 512;
        stbuf->st_mtime = f.mtime;
        stbuf->st_atime = f.atime;
        stbuf->st_ctime = f.ctime;
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

    if (strlen(filename) >= MAX_NAME_LEN) return -ENAMETOOLONG;

    uint32_t ts = now_ts();

    lseek(img_fd, sizeof(struct duckyfs_superblock), SEEK_SET);
    for (int i = 0; i < MAX_FILES; i++) {
        off_t pos = lseek(img_fd, 0, SEEK_CUR);
        read(img_fd, &f, sizeof(struct duckyfs_file));
        if (strcmp(f.name, filename) == 0 && f.used) return -EEXIST;
        if (!f.used) {
            memset(&f, 0, sizeof(f));
            strncpy(f.name, filename, MAX_NAME_LEN - 1);
            f.name[MAX_NAME_LEN - 1] = '\0';
            f.used = 1;
            f.size = 0;
            f.atime = f.mtime = f.ctime = ts;

            lseek(img_fd, pos, SEEK_SET);
            write(img_fd, &f, sizeof(f));

            struct duckyfs_superblock sb;
            if (read_sb(&sb) == 0 && sb.free_blocks > 0) {
                sb.free_blocks--;
                write_sb(&sb);
            }
            return 0;
        }
    }
    return -ENOSPC; // Disk full
}

// 4. Open a file (handles O_TRUNC so >file truncates properly)
static int ducky_open(const char *path, struct fuse_file_info *fi) {
    struct duckyfs_file f;
    int idx = find_file(path + 1, &f);
    if (idx < 0) return -ENOENT;

    if (fi->flags & O_TRUNC) {
        f.size = 0;
        memset(f.data, 0, BLOCK_SIZE);
        f.mtime = f.ctime = now_ts();
        write_file(idx, &f);
    }
    return 0;
}

// 5. Read data from a file (cat)
static int ducky_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct duckyfs_file f;
    int idx = find_file(path + 1, &f);
    if (idx < 0) return -ENOENT;

    if (offset >= f.size) return 0;
    if (offset + size > f.size) size = f.size - offset;

    memcpy(buf, f.data + offset, size);

    f.atime = now_ts();
    write_file(idx, &f);

    return size;
}

// 6. Write data to a file (echo "quack" > file)
static int ducky_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct duckyfs_file f;
    int idx = find_file(path + 1, &f);
    if (idx < 0) return -ENOENT;

    if (offset + size > BLOCK_SIZE) size = BLOCK_SIZE - offset;

    memcpy(f.data + offset, buf, size);
    f.size = offset + size;
    f.mtime = f.ctime = now_ts();

    write_file(idx, &f);

    return size;
}

// 7. Change a file's size (truncate)
static int ducky_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    struct duckyfs_file f;
    int idx = find_file(path + 1, &f);
    if (idx < 0) return -ENOENT;

    if (size < 0) return -EINVAL;
    if (size > BLOCK_SIZE) return -EFBIG;

    if (size > f.size) {
        memset(f.data + f.size, 0, size - f.size);
    }
    f.size = size;
    f.mtime = f.ctime = now_ts();

    write_file(idx, &f);
    return 0;
}

// 8. Update access/modification times (touch -d, cp, editors)
static int ducky_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    struct duckyfs_file f;
    int idx = find_file(path + 1, &f);
    if (idx < 0) return -ENOENT;

    if (tv) {
        f.atime = (tv[0].tv_nsec == UTIME_OMIT) ? f.atime : (uint32_t)tv[0].tv_sec;
        f.mtime = (tv[1].tv_nsec == UTIME_OMIT) ? f.mtime : (uint32_t)tv[1].tv_sec;
    } else {
        f.atime = f.mtime = now_ts();
    }
    f.ctime = now_ts();

    write_file(idx, &f);
    return 0;
}

// 9. Delete a file (rm)
static int ducky_unlink(const char *path) {
    struct duckyfs_file f;
    int idx = find_file(path + 1, &f);
    if (idx < 0) return -ENOENT;

    memset(&f, 0, sizeof(f));
    write_file(idx, &f);

    struct duckyfs_superblock sb;
    if (read_sb(&sb) == 0) {
        sb.free_blocks++;
        write_sb(&sb);
    }
    return 0;
}

// 10. Rename/move a file (mv)
static int ducky_rename(const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL;

    struct duckyfs_file f;
    int idx = find_file(from + 1, &f);
    if (idx < 0) return -ENOENT;
    if (strlen(to + 1) >= MAX_NAME_LEN) return -ENAMETOOLONG;

    struct duckyfs_file existing;
    if (find_file(to + 1, &existing) >= 0) return -EEXIST;

    strncpy(f.name, to + 1, MAX_NAME_LEN - 1);
    f.name[MAX_NAME_LEN - 1] = '\0';
    f.ctime = now_ts();

    write_file(idx, &f);
    return 0;
}

// 11. Filesystem statistics (df)
static int ducky_statfs(const char *path, struct statvfs *stbuf) {
    struct duckyfs_superblock sb;
    if (read_sb(&sb) != 0) return -EIO;

    uint32_t used = 0;
    struct duckyfs_file f;
    lseek(img_fd, sizeof(struct duckyfs_superblock), SEEK_SET);
    for (int i = 0; i < MAX_FILES; i++) {
        read(img_fd, &f, sizeof(struct duckyfs_file));
        if (f.used) used++;
    }

    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize = BLOCK_SIZE;
    stbuf->f_frsize = BLOCK_SIZE;
    stbuf->f_blocks = sb.total_blocks;
    stbuf->f_bfree = sb.total_blocks > used + 1 ? sb.total_blocks - used - 1 : 0;
    stbuf->f_bavail = stbuf->f_bfree;
    stbuf->f_files = MAX_FILES - used;
    stbuf->f_ffree = MAX_FILES - used;
    return 0;
}

static const struct fuse_operations ducky_oper = {
    .getattr  = ducky_getattr,
    .readdir  = ducky_readdir,
    .mknod    = ducky_mknod,
    .open     = ducky_open,
    .read     = ducky_read,
    .write    = ducky_write,
    .truncate = ducky_truncate,
    .utimens  = ducky_utimens,
    .unlink   = ducky_unlink,
    .rename   = ducky_rename,
    .statfs   = ducky_statfs,
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