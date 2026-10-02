#ifndef RUNTIME_FILEX_DIRENT_H
#define RUNTIME_FILEX_DIRENT_H

#include <sys/dirent.h>

#ifdef __cplusplus
extern "C" {
#endif

DIR* opendir(const char* path);
struct dirent* readdir(DIR* directory);
int closedir(DIR* directory);
void rewinddir(DIR* directory);
long telldir(DIR* directory);
void seekdir(DIR* directory, long position);
int dirfd(DIR* directory);

#ifdef __cplusplus
}
#endif

#endif
