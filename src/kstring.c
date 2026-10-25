#include "kstring.h"

size_t kstrlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int kstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int kstrncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    if (n == (size_t)-1) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *kstrcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *kstrncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n > 0 && *src) { *d++ = *src++; n--; }
    while (n-- > 0) *d++ = 0;
    return dst;
}

char *kstrcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

void *kmemset(void *s, int c, size_t n) {
    void *d = s;
    uint32_t v = (uint8_t)c * 0x01010101u;
    size_t dw = n >> 2, rest = n & 3;
    __asm__ volatile ("cld; rep stosl" : "+D"(d), "+c"(dw) : "a"(v) : "memory");
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(rest) : "a"(v) : "memory");
    return s;
}

void *kmemcpy(void *dst, const void *src, size_t n) {
    void *d = dst;
    const void *sp = src;
    size_t dw = n >> 2, rest = n & 3;
    __asm__ volatile ("cld; rep movsl" : "+D"(d), "+S"(sp), "+c"(dw) :: "memory");
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(sp), "+c"(rest) :: "memory");
    return dst;
}

int kstartswith(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

void kitoa(uint32_t val, char *buf, int base) {
    const char *digits = "0123456789ABCDEF";
    char tmp[33]; int i = 0;
    if (val == 0) { buf[0]='0'; buf[1]=0; return; }
    while (val > 0) {
        tmp[i++] = digits[val % base];
        val /= base;
    }
    for (int j = 0; j < i; j++) buf[j] = tmp[i-1-j];
    buf[i] = 0;
}
char *kstrchr(const char *s, int c) {
    while(*s){ if(*s==(char)c) return (char*)s; s++; }
    return (c==0)?(char*)s:0;
}
char *kstrrchr(const char *s, int c) {
    const char *last=0;
    while(*s){ if(*s==(char)c) last=s; s++; }
    return (char*)last;
}
int kstrncpy_safe(char *dst, const char *src, size_t n) {
    size_t i=0; while(i<n-1&&src[i]){dst[i]=src[i];i++;} dst[i]=0; return (int)i;
}
