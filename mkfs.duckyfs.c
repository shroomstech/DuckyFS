#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "duckyfs.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <disk_image>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        perror("Failed to open disk image");
        return 1;
    }

    // 1. Write Superblock
    struct duckyfs_superblock sb = {
        .magic = DUCKYFS_MAGIC,
        .block_size = BLOCK_SIZE,
        .total_blocks = 16,
        .free_blocks = 15
    };
    write(fd, &sb, sizeof(sb));

    // 2. Zero out file entries
    struct duckyfs_file empty_files[MAX_FILES];
    memset(empty_files, 0, sizeof(empty_files));
    write(fd, empty_files, sizeof(empty_files));

    close(fd);
    printf("Successfully formatted %s with DuckyFS! (Magic: 0x%X)\n", argv[1], sb.magic);
    return 0;
}
