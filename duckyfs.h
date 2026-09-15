#ifndef DUCKYFS_H
#define DUCKYFS_H

#include <stdint.h>

#define DUCKYFS_MAGIC 0x4455434B // "DUCK"
#define BLOCK_SIZE    4096
#define MAX_FILES     16
#define MAX_NAME_LEN  32

struct duckyfs_superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
};

struct duckyfs_file {
    char name[MAX_NAME_LEN];
    uint32_t size;
    uint32_t used; // 0 = empty slot, 1 = active file
    char data[BLOCK_SIZE]; // Simple inline data block per file
};

#endif
