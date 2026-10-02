#ifndef RUNTIME_FILEX_SYS_DIRENT_H
#define RUNTIME_FILEX_SYS_DIRENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

struct dirent
{
    uint32_t d_ino;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};

typedef struct runtime_filex_directory_stream DIR;

#ifdef __cplusplus
}
#endif

#endif
