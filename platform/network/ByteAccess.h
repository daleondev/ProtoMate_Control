#pragma once
#include <stddef.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Byte-safe operations for buffers backed by FMC SDRAM (ES0445 2.7.3).
   These are scoped to networking/UA; graphics and the general runtime keep
   their existing optimized libc implementations. */
void *network_memcpy(void *destination, const void *source, size_t size);
void *network_memmove(void *destination, const void *source, size_t size);
void *network_memset(void *destination, int value, size_t size);
int network_memcmp(const void *left, const void *right, size_t size);
void *network_memchr(const void *data, int value, size_t size);
size_t network_strlen(const char *text);
size_t network_strnlen(const char *text, size_t size);
int network_strcmp(const char *left, const char *right);
int network_strncmp(const char *left, const char *right, size_t size);
char *network_strcpy(char *destination, const char *source);
char *network_strncpy(char *destination, const char *source, size_t size);
char *network_strchr(const char *text, int value);
char *network_strrchr(const char *text, int value);
char *network_strstr(const char *text, const char *needle);
#ifdef __cplusplus
}
#endif
#ifdef NETWORK_BYTE_ACCESS_OVERRIDE
#define memcpy network_memcpy
#define memmove network_memmove
#define memset network_memset
#define memcmp network_memcmp
#define memchr network_memchr
#define strlen network_strlen
#define strnlen network_strnlen
#define strcmp network_strcmp
#define strncmp network_strncmp
#define strcpy network_strcpy
#define strncpy network_strncpy
#define strchr network_strchr
#define strrchr network_strrchr
#define strstr network_strstr
#endif
