/*
 * dirent.h - directory streams over SYS_getdents.
 */
#ifndef DIRENT_H
#define DIRENT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __dirstream DIR;

struct dirent {
    unsigned long  d_ino;
    unsigned long  d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
};

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR  2
#define DT_DIR  4
#define DT_BLK  6
#define DT_REG  8
#define DT_LNK  10

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#ifdef __cplusplus
}
#endif

#endif /* DIRENT_H */
