#ifndef KUMOS_LIBC_H
#define KUMOS_LIBC_H

#define O_RDONLY   0x00
#define O_WRONLY   0x01
#define O_RDWR     0x02
#define O_CREAT    0x04
#define O_TRUNC    0x08
#define O_APPEND   0x10

#define KUMOS_VERSION "2.0"

typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;
typedef int            int32_t;
typedef unsigned int   size_t;
typedef int            ssize_t;

typedef struct {
    uint8_t  second, minute, hour;
    uint8_t  day, month;
    uint16_t year;
} time_t;

#define SYS_EXIT     0
#define SYS_WRITE    1
#define SYS_READ     2
#define SYS_GETPID   3
#define SYS_SLEEP    4
#define SYS_YIELD    5
#define SYS_SBRK     6
#define SYS_UPTIME   7
#define SYS_GETKEY   8
#define SYS_PUTCHAR  9
#define SYS_OPEN    10
#define SYS_CLOSE   11
#define SYS_FREAD   12
#define SYS_FWRITE  13
#define SYS_LISTDIR 14
#define SYS_WAITPID 15
#define SYS_WAIT    16
#define SYS_GETTIME 17
#define SYS_SERIAL  18

static inline int _syscall(int num, int a, int b, int c) {
    int r;
    __asm__ volatile(
        "int $0x80"
        : "=a"(r)
        : "a"(num), "b"(a), "c"(b), "d"(c)
        : "memory"
    );
    return r;
}

static inline void exit(int code) {
    _syscall(SYS_EXIT, code, 0, 0);
    while(1);
}
static inline int getpid(void) {
    return _syscall(SYS_GETPID, 0, 0, 0);
}
static inline void sleep(uint32_t ms) {
    _syscall(SYS_SLEEP, (int)ms, 0, 0);
}
static inline void yield(void) {
    _syscall(SYS_YIELD, 0, 0, 0);
}
static inline int waitpid(int pid) {
    return _syscall(SYS_WAITPID, pid, 0, 0);
}
static inline uint32_t uptime(void) {
    return (uint32_t)_syscall(SYS_UPTIME, 0, 0, 0);
}
static inline int gettime(time_t *t) {
    return _syscall(SYS_GETTIME, (int)t, 0, 0);
}

static inline void putchar(char c) {
    _syscall(SYS_PUTCHAR, (int)c, 0, 0);
}
static inline char getkey(void) {
    return (char)_syscall(SYS_GETKEY, 0, 0, 0);
}
static inline int write(const char *buf, uint32_t len) {
    return _syscall(SYS_WRITE, (int)buf, (int)len, 0);
}
static inline int read(char *buf, uint32_t len) {
    return _syscall(SYS_READ, (int)buf, (int)len, 0);
}

static inline int open(const char *name) {
    return _syscall(SYS_OPEN, (int)name, 0, 0);
}
static inline int close(int fd) {
    return _syscall(SYS_CLOSE, fd, 0, 0);
}
static inline int fread(int fd, void *buf, uint32_t sz) {
    return _syscall(SYS_FREAD, fd, (int)buf, (int)sz);
}
static inline int fwrite(int fd, const void *buf, uint32_t sz) {
    return _syscall(SYS_FWRITE, fd, (int)buf, (int)sz);
}

static inline size_t strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static inline int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static inline int strncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    if (n == (size_t)-1) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}
static inline char *strcpy(char *d, const char *s) {
    char *r = d; while ((*d++ = *s++)); return r;
}
static inline char *strncpy(char *d, const char *s, size_t n) {
    char *r = d;
    while (n-- && (*d++ = *s++));
    while (n-- != (size_t)-1) *d++ = 0;
    return r;
}
static inline char *strcat(char *d, const char *s) {
    char *r = d; while (*d) d++; while ((*d++ = *s++)); return r;
}
static inline char *strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char *)s; s++; }
    return 0;
}

static inline void *memset(void *s, int c, size_t n) {
    unsigned char *p = s; while (n--) *p++ = (unsigned char)c; return s;
}
static inline void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    while (n--) *dd++ = *ss++; return d;
}
static inline int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p=a, *q=b;
    while (n--) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}

static inline int atoi(const char *s) {
    int n=0, neg=0;
    while (*s==' ') s++;
    if (*s=='-'){neg=1;s++;} else if(*s=='+')s++;
    while (*s>='0'&&*s<='9') n=n*10+(*s++-'0');
    return neg?-n:n;
}

static inline char *itoa(int val, char *buf, int base) {
    const char *digits = "0123456789ABCDEF";
    char tmp[34]; int i=0, neg=0;
    if (val==0) { buf[0]='0'; buf[1]=0; return buf; }
    if (base==10 && val<0) { neg=1; val=-val; }
    unsigned int u = (unsigned int)val;
    while (u>0) { tmp[i++]=digits[u%base]; u/=base; }
    if (neg) tmp[i++]='-';
    int j=0;
    while (i>0) buf[j++]=tmp[--i];
    buf[j]=0;
    return buf;
}

static inline char *utoa(uint32_t val, char *buf, int base) {
    return itoa((int)val, buf, base);
}

static inline int puts(const char *s) {
    write(s, (uint32_t)strlen(s));
    putchar('\n');
    return 0;
}
static inline int fputs(const char *s) {
    return write(s, (uint32_t)strlen(s));
}
static inline char *gets(char *buf, int max) {
    int n = read(buf, (uint32_t)(max-1));
    buf[n<0?0:n] = 0;
    return buf;
}

static int printf(const char *fmt, ...) {
    char buf[1024];
    int  pos = 0;

    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    while (*fmt && pos < 1022) {
        if (*fmt != '%') { buf[pos++] = *fmt++; continue; }
        fmt++;

        int left = 0, zero_pad = 0, width = 0;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt-'0'); fmt++; }

        if (*fmt == 's') {
            char *s = __builtin_va_arg(ap, char*);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            if (!left) for (int p = len; p < width; p++) buf[pos++] = ' ';
            while (*s && pos < 1022) buf[pos++] = *s++;
            if (left) for (int p = len; p < width && pos < 1022; p++) buf[pos++] = ' ';
        } else if (*fmt == 'd' || *fmt == 'i') {
            int d = __builtin_va_arg(ap, int);
            char tmp[16]; int ti=0, neg=0;
            if (d<0){neg=1;d=-d;}
            if (!d) tmp[ti++]='0';
            else { unsigned u=(unsigned)d; while(u){tmp[ti++]='0'+u%10;u/=10;} }
            if (neg) tmp[ti++]='-';
            int pad = width - ti; char pc = zero_pad?'0':' ';
            if (!left) while (pad-->0 && pos<1022) buf[pos++]=pc;
            while (ti>0 && pos<1022) buf[pos++]=tmp[--ti];
            if (left) while (pad-->0 && pos<1022) buf[pos++]=' ';
        } else if (*fmt == 'u') {
            unsigned u = __builtin_va_arg(ap, unsigned);
            char tmp[16]; int ti=0;
            if (!u) tmp[ti++]='0';
            else { while(u){tmp[ti++]='0'+u%10;u/=10;} }
            int pad=width-ti; char pc=zero_pad?'0':' ';
            if (!left) while(pad-->0&&pos<1022)buf[pos++]=pc;
            while(ti>0&&pos<1022)buf[pos++]=tmp[--ti];
            if (left) while(pad-->0&&pos<1022)buf[pos++]=' ';
        } else if (*fmt == 'x' || *fmt == 'X') {
            unsigned u = __builtin_va_arg(ap, unsigned);
            const char *h = (*fmt=='x')?"0123456789abcdef":"0123456789ABCDEF";
            char tmp[16]; int ti=0;
            if (!u) tmp[ti++]='0';
            else { while(u){tmp[ti++]=h[u&0xF];u>>=4;} }
            int pad=width-ti; char pc=zero_pad?'0':' ';
            if (!left) while(pad-->0&&pos<1022)buf[pos++]=pc;
            while(ti>0&&pos<1022)buf[pos++]=tmp[--ti];
            if (left) while(pad-->0&&pos<1022)buf[pos++]=' ';
        } else if (*fmt == 'c') {
            buf[pos++] = (char)__builtin_va_arg(ap, int);
        } else if (*fmt == '%') {
            buf[pos++] = '%';
        }
        fmt++;
    }
    buf[pos] = 0;
    __builtin_va_end(ap);
    write(buf, (uint32_t)pos);
    return pos;
}

#define _HEAP_SIZE (64 * 1024)
typedef struct _mblk { uint32_t size; uint32_t used; } _mblk_t;
static uint32_t _heap_arena[_HEAP_SIZE / 4];
static uint32_t _heap_top = 0;

static inline void *malloc(size_t size) {
    if (!size) return 0;
    size = (size + 7) & ~7u;
    uint8_t *base = (uint8_t *)_heap_arena;
    for (uint32_t off = 0; off < _heap_top; ) {
        _mblk_t *m = (_mblk_t *)(base + off);
        if (!m->used && m->size >= size) {
            if (m->size >= size + sizeof(_mblk_t) + 16) {
                _mblk_t *rest = (_mblk_t *)(base + off + sizeof(_mblk_t) + size);
                rest->size = m->size - size - sizeof(_mblk_t);
                rest->used = 0;
                m->size = size;
            }
            m->used = 1;
            return m + 1;
        }
        off += sizeof(_mblk_t) + m->size;
    }
    if (_heap_top + sizeof(_mblk_t) + size > _HEAP_SIZE) return 0;
    _mblk_t *m = (_mblk_t *)(base + _heap_top);
    m->size = size; m->used = 1;
    _heap_top += sizeof(_mblk_t) + size;
    return m + 1;
}

static inline void free(void *ptr) {
    if (!ptr) return;
    ((_mblk_t *)ptr - 1)->used = 0;
    uint8_t *base = (uint8_t *)_heap_arena;
    for (uint32_t off = 0; off < _heap_top; ) {
        _mblk_t *m = (_mblk_t *)(base + off);
        uint32_t next = off + sizeof(_mblk_t) + m->size;
        if (!m->used) {
            while (next < _heap_top && !((_mblk_t *)(base + next))->used) {
                m->size += sizeof(_mblk_t) + ((_mblk_t *)(base + next))->size;
                next = off + sizeof(_mblk_t) + m->size;
            }
            if (next >= _heap_top) { _heap_top = off; break; }
        }
        off = next;
    }
}

static inline void *calloc(size_t n, size_t sz) {
    void *p = malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}

static inline void print_time(void) {
    time_t t;
    if (gettime(&t) == 0) {
        char buf[32];

        itoa(t.year, buf, 10);
        fputs(buf); fputs("-");
        if (t.month < 10) fputs("0");
        itoa(t.month, buf, 10); fputs(buf); fputs("-");
        if (t.day < 10) fputs("0");
        itoa(t.day, buf, 10); fputs(buf); fputs(" ");
        if (t.hour < 10) fputs("0");
        itoa(t.hour, buf, 10); fputs(buf); fputs(":");
        if (t.minute < 10) fputs("0");
        itoa(t.minute, buf, 10); fputs(buf); fputs(":");
        if (t.second < 10) fputs("0");
        itoa(t.second, buf, 10); fputs(buf);
    }
}

static inline int sys_kill(int pid, int sig) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(30),"b"(pid),"c"(sig):"memory"); return r;
}
static inline int sys_signal(int sig, void (*handler)(int)) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(31),"b"(sig),"c"(handler):"memory"); return r;
}
static inline int sys_fork(void) {
    return _syscall(20, 0, 0, 0);
}

#define KUSH_SIGCONT 18
#define KUSH_SIGSTOP 19
static inline int sys_waitstatus(int pid, int *status) {
    return _syscall(47, pid, (int)status, 0);
}
static inline int sys_procstate(int pid) {
    return _syscall(48, pid, 0, 0);
}

static inline int sprintf(char *buf, const char *fmt, ...) {
    int pos=0;
    __builtin_va_list ap; __builtin_va_start(ap,fmt);
    while(*fmt&&pos<1022){
        if(*fmt!='%'){buf[pos++]=*fmt++;continue;}
        fmt++;
        int width=0,zero_pad=0;
        if(*fmt=='0'){zero_pad=1;fmt++;}
        while(*fmt>='0'&&*fmt<='9'){width=width*10+(*fmt-'0');fmt++;}
        if(*fmt=='s'){
            char*s=__builtin_va_arg(ap,char*); if(!s)s="(null)";
            int l=(int)strlen(s); int pad=width-l;
            while(pad-->0&&pos<1022)buf[pos++]=' ';
            while(*s&&pos<1022)buf[pos++]=*s++;
        }else if(*fmt=='d'||*fmt=='i'){
            int d=__builtin_va_arg(ap,int);
            char tmp[16];int ti=0,neg=0;
            if(d<0){neg=1;d=-d;}
            if(!d){tmp[ti++]='0';}else{unsigned u=(unsigned)d;while(u){tmp[ti++]='0'+u%10;u/=10;}}
            if(neg)tmp[ti++]='-';
            int pad=width-ti;char pc=zero_pad?'0':' ';
            while(pad-->0&&pos<1022)buf[pos++]=pc;
            while(ti>0&&pos<1022)buf[pos++]=tmp[--ti];
        }else if(*fmt=='u'){
            unsigned u=__builtin_va_arg(ap,unsigned);
            char tmp[16];int ti=0;
            if(!u){tmp[ti++]='0';}else{while(u){tmp[ti++]='0'+u%10;u/=10;}}
            int pad=width-ti;char pc=zero_pad?'0':' ';
            while(pad-->0&&pos<1022)buf[pos++]=pc;
            while(ti>0&&pos<1022)buf[pos++]=tmp[--ti];
        }else if(*fmt=='x'||*fmt=='X'){
            unsigned u=__builtin_va_arg(ap,unsigned);
            const char*h=(*fmt=='x')?"0123456789abcdef":"0123456789ABCDEF";
            char tmp[16];int ti=0;
            if(!u){tmp[ti++]='0';}else{while(u){tmp[ti++]=h[u&0xF];u>>=4;}}
            int pad=width-ti;char pc=zero_pad?'0':' ';
            while(pad-->0&&pos<1022)buf[pos++]=pc;
            while(ti>0&&pos<1022)buf[pos++]=tmp[--ti];
        }else if(*fmt=='c'){buf[pos++]=(char)__builtin_va_arg(ap,int);
        }else if(*fmt=='%'){buf[pos++]='%';}
        fmt++;
    }
    buf[pos]=0; __builtin_va_end(ap);
    return pos;
}

static inline int snprintf(char *buf, size_t sz, const char *fmt, ...) {
    char tmp[1024];
    __builtin_va_list ap; __builtin_va_start(ap,fmt);
    int n=0;
    while(*fmt&&n<1022){
        if(*fmt!='%'){tmp[n++]=*fmt++;continue;}
        fmt++;
        if(*fmt=='s'){char*s=__builtin_va_arg(ap,char*);if(!s)s="";while(*s&&n<1022)tmp[n++]=*s++;
        }else if(*fmt=='d'){int d=__builtin_va_arg(ap,int);char t[12];int i=10;t[11]=0;if(!d){tmp[n++]='0';}else{if(d<0){tmp[n++]='-';d=-d;}unsigned u=(unsigned)d;while(u){t[i--]='0'+u%10;u/=10;}while(++i<=10)tmp[n++]=t[i];}
        }else if(*fmt=='u'){unsigned u=__builtin_va_arg(ap,unsigned);char t[12];int i=10;t[11]=0;if(!u){tmp[n++]='0';}else{while(u){t[i--]='0'+u%10;u/=10;}while(++i<=10)tmp[n++]=t[i];}
        }else if(*fmt=='c'){tmp[n++]=(char)__builtin_va_arg(ap,int);
        }else if(*fmt=='%'){tmp[n++]='%';}
        fmt++;
    }
    tmp[n]=0; __builtin_va_end(ap);
    size_t copy=n<(int)sz-1?(size_t)n:sz-1;
    memcpy(buf,tmp,copy); buf[copy]=0;
    return n;
}

static inline long strtol(const char *s, char **end, int base) {
    while(*s==' ')s++;
    int neg=0; if(*s=='-'){neg=1;s++;}else if(*s=='+')s++;
    if(base==0){if(*s=='0'&&(s[1]=='x'||s[1]=='X')){base=16;s+=2;}else if(*s=='0'){base=8;s++;}else base=10;}
    else if(base==16&&*s=='0'&&(s[1]=='x'||s[1]=='X'))s+=2;
    long n=0;
    while(*s){
        int d;
        if(*s>='0'&&*s<='9') d=*s-'0';
        else if(*s>='a'&&*s<='z') d=*s-'a'+10;
        else if(*s>='A'&&*s<='Z') d=*s-'A'+10;
        else break;
        if(d>=base)break;
        n=n*base+d; s++;
    }
    if(end)*end=(char*)s;
    return neg?-n:n;
}

static inline int isdigit(int c){ return c>='0'&&c<='9'; }
static inline int isalpha(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
static inline int isalnum(int c){ return isdigit(c)||isalpha(c); }
static inline int isspace(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'; }
static inline int toupper(int c){ return (c>='a'&&c<='z')?c-32:c; }
static inline int tolower(int c){ return (c>='A'&&c<='Z')?c+32:c; }
static inline int abs(int n){ return n<0?-n:n; }

static void qsort(void *base, size_t n, size_t sz,
                  int(*cmp)(const void*,const void*)) {
    if(n<2)return;
    char *arr=(char*)base; char tmp[256];
    for(size_t i=1;i<n;i++){
        memcpy(tmp,arr+i*sz,sz);
        size_t j=i;
        while(j>0&&cmp(arr+(j-1)*sz,tmp)>0){
            memcpy(arr+j*sz,arr+(j-1)*sz,sz); j--;
        }
        memcpy(arr+j*sz,tmp,sz);
    }
}

static char *strdup(const char *s) {
    size_t l=strlen(s)+1;
    char *p=(char*)malloc(l);
    if(p) memcpy(p,s,l);
    return p;
}

static char *strtok_r(char *str, const char *delim, char **save) {
    if(str) *save=str;
    if(!*save) return 0;
    while(**save&&strchr(delim,**save))(*save)++;
    if(!**save) return 0;
    char *tok=*save;
    while(**save&&!strchr(delim,**save))(*save)++;
    if(**save){**save=0;(*save)++;}
    return tok;
}

static char *strtok(char *str, const char *delim) {
    static char *saved;
    return strtok_r(str, delim, &saved);
}

static char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char*)hay;
    for (; *hay; hay++) {
        const char *h=hay, *n=needle;
        while (*h && *n && *h==*n){h++;n++;}
        if (!*n) return (char*)hay;
    }
    return 0;
}

static int sscanf(const char *str, const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap,fmt);
    int count=0;
    while(*fmt&&*str){
        if(*fmt=='%'){
            fmt++;
            if(*fmt=='d'||*fmt=='i'){
                while(*str==' ')str++;
                int neg=0,n=0;
                if(*str=='-'){neg=1;str++;}
                while(*str>='0'&&*str<='9'){n=n*10+(*str-'0');str++;}
                *__builtin_va_arg(ap,int*)=neg?-n:n; count++;
            } else if(*fmt=='s'){
                char*out=__builtin_va_arg(ap,char*);
                while(*str&&*str!=' ')(*out++=*str++);
                *out=0; count++;
            } else if(*fmt=='u'){
                while(*str==' ')str++;
                unsigned n=0;
                while(*str>='0'&&*str<='9'){n=n*10+(unsigned)(*str-'0');str++;}
                *__builtin_va_arg(ap,unsigned*)=n; count++;
            }
            fmt++;
        } else if(*fmt==*str){fmt++;str++;}
        else break;
    }
    __builtin_va_end(ap);
    return count;
}

static inline int sys_exec(const char *path) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(19),"b"(path):"memory"); return r;
}

#ifndef STDIN_FILENO
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2
#endif

static inline void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d=(uint8_t*)dst; const uint8_t *s=(const uint8_t*)src;
    if(d<s||d>=s+n) { while(n--)*d++=*s++; }
    else { d+=n; s+=n; while(n--)*--d=*--s; }
    return dst;
}

static inline char *strncat(char *dst, const char *src, size_t n) {
    char *r=dst; while(*dst)dst++;
    while(n--&&(*dst++=*src++));
    *dst=0; return r;
}

#define kmemset  memset
#define kmemcpy  memcpy

static inline int execve(const char *path, char *const argv[]) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(44),"b"(path),"c"(argv):"memory"); return r;
}
static inline int select_fd(int nfds, uint32_t *rfds) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(45),"b"(nfds),"c"(rfds):"memory"); return r;
}
static inline int poll_fd(int *fds, int nfds) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(46),"b"(fds),"c"(nfds):"memory"); return r;
}

static int    __argc;
static char **__argv;

static inline int args_line(char *buf, int max) {
    int n = 0;
    for (int i = 1; i < __argc && n < max - 1; i++) {
        if (i > 1) buf[n++] = ' ';
        for (const char *p = __argv[i]; *p && n < max - 1; p++) buf[n++] = *p;
    }
    buf[n] = 0;
    return n;
}

int main();

void _start(int argc, char **argv) {
    __argc = argc;
    __argv = argv;
    exit(main(argc, argv));
}

#endif
