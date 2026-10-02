#ifndef RUNTIME_UTIME_H
#define RUNTIME_UTIME_H

#include_next <utime.h>

/* Bare-metal Newlib provides utimbuf but omits the syscall declaration. */
#ifdef __cplusplus
extern "C" {
#endif
int utime(const char* path, const struct utimbuf* times);
#ifdef __cplusplus
}
#endif

#endif
