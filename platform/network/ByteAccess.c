#include "ByteAccess.h"
#include <stdint.h>

/* Volatile byte accesses deliberately forbid compiler loop replacement and
   unaligned word reads, including when this file is built with LTO. */
void *network_memcpy(void *destination, const void *source, size_t size) {
    volatile unsigned char *out = destination;
    const volatile unsigned char *in = source;
    for(size_t i=0; i<size; ++i) out[i]=in[i];
    return destination;
}
void *network_memmove(void *destination, const void *source, size_t size) {
    volatile unsigned char *out = destination;
    const volatile unsigned char *in = source;
    if((uintptr_t)out > (uintptr_t)in) {
        while(size) { --size; out[size]=in[size]; }
    } else for(size_t i=0; i<size; ++i) out[i]=in[i];
    return destination;
}
void *network_memset(void *destination, int value, size_t size) {
    volatile unsigned char *out = destination;
    for(size_t i=0; i<size; ++i) out[i]=(unsigned char)value;
    return destination;
}
int network_memcmp(const void *left, const void *right, size_t size) {
    const volatile unsigned char *a=left, *b=right;
    for(size_t i=0; i<size; ++i) { int d=(int)a[i]-(int)b[i]; if(d) return d; }
    return 0;
}
void *network_memchr(const void *data, int value, size_t size) {
    const volatile unsigned char *in=data;
    for(size_t i=0; i<size; ++i) if(in[i]==(unsigned char)value) return (void *)((uintptr_t)data+i);
    return NULL;
}
size_t network_strlen(const char *text) {
    const volatile char *in=text; size_t count=0;
    while(in[count]) ++count;
    return count;
}
size_t network_strnlen(const char *text, size_t size) {
    const volatile char *in=text; size_t count=0;
    while(count<size && in[count]) ++count;
    return count;
}
int network_strncmp(const char *left, const char *right, size_t size) {
    const volatile unsigned char *a=(const void *)left, *b=(const void *)right;
    for(size_t i=0; i<size; ++i) { unsigned av=a[i], bv=b[i]; if(av!=bv || !av) return (int)av-(int)bv; }
    return 0;
}
int network_strcmp(const char *left, const char *right) { return network_strncmp(left,right,SIZE_MAX); }
char *network_strcpy(char *destination, const char *source) {
    network_memcpy(destination,source,network_strlen(source)+1); return destination;
}
char *network_strncpy(char *destination, const char *source, size_t size) {
    size_t count=network_strnlen(source,size);
    network_memcpy(destination,source,count);
    network_memset(destination+count,0,size-count);
    return destination;
}
char *network_strchr(const char *text, int value) { return network_memchr(text,value,network_strlen(text)+1); }
char *network_strrchr(const char *text, int value) {
    const volatile unsigned char *in=(const void *)text; char *last=NULL;
    for(size_t i=0;;++i) { unsigned v=in[i]; if(v==(unsigned char)value) last=(char *)((uintptr_t)text+i); if(!v) break; }
    return last;
}
char *network_strstr(const char *text, const char *needle) {
    size_t n=network_strlen(needle), size=network_strlen(text);
    if(n>size) return NULL;
    for(size_t i=0;i<=size-n;++i) if(!network_memcmp(text+i,needle,n)) return (char *)((uintptr_t)text+i);
    return NULL;
}
