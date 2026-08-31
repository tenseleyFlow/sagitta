#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    BLOCK_SIZE = 4096,
    BLOCK_COUNT = 8192,
    INODE_COUNT = 1024,
    INODE_SIZE = 128,
    INODE_TABLE_BLOCK = 4,
    ROOT_BLOCK = 36,
    USED_BLOCKS = 37,
    RESERVED_INODES = 10
};

static void put16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value & UINT16_C(0xff));
    p[1] = (unsigned char)(value >> 8U);
}

static void put32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value & UINT32_C(0xff));
    p[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    p[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    p[3] = (unsigned char)(value >> 24U);
}

static int write_at(FILE *file, long offset, const void *bytes, size_t len)
{
    return fseek(file, offset, SEEK_SET) == 0 &&
           fwrite(bytes, 1U, len, file) == len;
}

static int build_image(FILE *file)
{
    static const unsigned char uuid[16] = {
        0x59U, 0x45U, 0x57U, 0x57U, 0x4fU, 0x52U, 0x4bU, 0x00U,
        0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x57U
    };
    unsigned char super[1024];
    unsigned char group[BLOCK_SIZE];
    unsigned char block_bitmap[BLOCK_SIZE];
    unsigned char inode_bitmap[BLOCK_SIZE];
    unsigned char inode_block[BLOCK_SIZE];
    unsigned char directory[BLOCK_SIZE];
    unsigned char *root_inode = inode_block + INODE_SIZE;
    unsigned int i;

    if (fseek(file, (long)BLOCK_SIZE * BLOCK_COUNT - 1L, SEEK_SET) != 0 ||
        fputc(0, file) == EOF)
        return 0;

    (void)memset(super, 0, sizeof(super));
    put32(super + 0, INODE_COUNT);
    put32(super + 4, BLOCK_COUNT);
    put32(super + 12, BLOCK_COUNT - USED_BLOCKS);
    put32(super + 16, INODE_COUNT - RESERVED_INODES);
    put32(super + 20, 0U);
    put32(super + 24, 2U);
    put32(super + 28, 2U);
    put32(super + 32, BLOCK_COUNT);
    put32(super + 36, BLOCK_COUNT);
    put32(super + 40, INODE_COUNT);
    put16(super + 52, 0U);
    put16(super + 54, UINT16_C(0xffff));
    put16(super + 56, UINT16_C(0xef53));
    put16(super + 58, 1U);
    put16(super + 60, 1U);
    put32(super + 72, 0U);
    put32(super + 76, 1U);
    put32(super + 84, 11U);
    put16(super + 88, INODE_SIZE);
    put32(super + 96, 2U);
    (void)memcpy(super + 104, uuid, sizeof(uuid));
    (void)memcpy(super + 120, "YEW_WORK", 8U);

    (void)memset(group, 0, sizeof(group));
    put32(group + 0, 2U);
    put32(group + 4, 3U);
    put32(group + 8, INODE_TABLE_BLOCK);
    put16(group + 12, (uint16_t)(BLOCK_COUNT - USED_BLOCKS));
    put16(group + 14, (uint16_t)(INODE_COUNT - RESERVED_INODES));
    put16(group + 16, 1U);

    (void)memset(block_bitmap, 0, sizeof(block_bitmap));
    for (i = 0U; i < USED_BLOCKS; i++)
        block_bitmap[i / 8U] |= (unsigned char)(1U << (i % 8U));

    (void)memset(inode_bitmap, 0, sizeof(inode_bitmap));
    for (i = 0U; i < RESERVED_INODES; i++)
        inode_bitmap[i / 8U] |= (unsigned char)(1U << (i % 8U));

    (void)memset(inode_block, 0, sizeof(inode_block));
    put16(root_inode + 0, UINT16_C(0040755));
    put32(root_inode + 4, BLOCK_SIZE);
    put16(root_inode + 26, 2U);
    put32(root_inode + 28, BLOCK_SIZE / 512U);
    put32(root_inode + 40, ROOT_BLOCK);

    (void)memset(directory, 0, sizeof(directory));
    put32(directory + 0, 2U);
    put16(directory + 4, 12U);
    directory[6] = 1U;
    directory[7] = 2U;
    directory[8] = '.';
    put32(directory + 12, 2U);
    put16(directory + 16, BLOCK_SIZE - 12U);
    directory[18] = 2U;
    directory[19] = 2U;
    directory[20] = '.';
    directory[21] = '.';

    return write_at(file, 1024L, super, sizeof(super)) &&
           write_at(file, (long)BLOCK_SIZE, group, sizeof(group)) &&
           write_at(file, 2L * BLOCK_SIZE, block_bitmap,
                    sizeof(block_bitmap)) &&
           write_at(file, 3L * BLOCK_SIZE, inode_bitmap,
                    sizeof(inode_bitmap)) &&
           write_at(file, (long)INODE_TABLE_BLOCK * BLOCK_SIZE,
                    inode_block, sizeof(inode_block)) &&
           write_at(file, (long)ROOT_BLOCK * BLOCK_SIZE,
                    directory, sizeof(directory));
}

int main(int argc, char **argv)
{
    FILE *file;
    int ok;
    int saved;

    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s OUTPUT\n", argv[0]);
        return 2;
    }
    file = fopen(argv[1], "wb");
    if (file == NULL) {
        (void)fprintf(stderr, "%s: %s: %s\n", argv[0], argv[1],
                      strerror(errno));
        return 1;
    }
    ok = build_image(file);
    if (ok && fflush(file) != 0)
        ok = 0;
    saved = errno;
    if (fclose(file) != 0 && ok) {
        saved = errno;
        ok = 0;
    }
    if (!ok) {
        (void)remove(argv[1]);
        (void)fprintf(stderr, "%s: cannot write %s: %s\n", argv[0],
                      argv[1], strerror(saved));
        return 1;
    }
    return 0;
}
