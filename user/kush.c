
#include "kumos_libc.h"

/* All of these need a "memory" clobber - without it, GCC doesn't know the
   int 0x80 call writes through a pointer argument (fds/buf/st) and is free
   to keep a stale cached copy of anything it thinks it already knows the
   value of, instead of reloading from memory afterward. kumos_libc.h's own
   _syscall() already has this; these were hand-duplicated here without it.
   Real, reproducible bug: sys_pipe() on a freshly-declared, compiler-known
   {-1,-1} array let GCC skip the reload and pass -1 to the very next
   sys_dup2() call, silently no-opping the pipe redirect (output leaked
   straight to the console instead of into the pipe) - only surfaced once
   run_pipeline()'s pipe-stage loop put pipefd[] in a shape (freshly
   re-declared each iteration) the optimizer could exploit; the single,
   outer-scope pipefd[] the old 2-stage-only code used happened not to
   trigger it. */
static inline int sys_pipe(int fds[2]) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(21),"b"(fds):"memory"); return r;
}
static inline int sys_dup2(int old, int nw) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(22),"b"(old),"c"(nw):"memory"); return r;
}
static inline int sys_getcwd(char *buf, uint32_t sz) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(23),"b"(buf),"c"(sz):"memory"); return r;
}
static inline int sys_chdir(const char *path) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(24),"b"(path):"memory"); return r;
}
static inline int sys_stat(const char *path, void *st) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(25),"b"(path),"c"(st):"memory"); return r;
}
static inline int sys_unlink(const char *path) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(26),"b"(path):"memory"); return r;
}
static inline int sys_chmod(const char *path, const char *mode) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(27),"b"(path),"c"(mode):"memory"); return r;
}
static inline int sys_isatty(int fd) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(29),"b"(fd):"memory"); return r;
}
static inline int sys_listdir(const char *path, char *buf, uint32_t sz) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(14),"b"(path),"c"(buf),"d"(sz):"memory"); return r;
}

typedef struct { uint32_t size; uint8_t type; char name[64]; } stat_t;

#define CMD_MAX   256
#define ARG_MAX    32
#define HIST_MAX   16
#define KUSH_VER  KUMOS_VERSION

static char history[HIST_MAX][CMD_MAX];
static int  hist_count = 0;
static char cwd_buf[128];

static void hist_add(const char *cmd) {
    if (!*cmd) return;
    if (hist_count < HIST_MAX) strcpy(history[hist_count++], cmd);
    else {
        for (int i=0;i<HIST_MAX-1;i++) strcpy(history[i],history[i+1]);
        strcpy(history[HIST_MAX-1], cmd);
    }
}

static int split(char *line, char *argv[], int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max-1) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    argv[argc] = 0;
    return argc;
}

/* No -e flag needed/supported (kush's tokenizer has no quoting, so a flag
   before a string with spaces wouldn't parse cleanly anyway) - backslash
   escapes are just always decoded, same default-on behavior as dash/sh's
   builtin echo. Mainly \e matters here: it's the only way to type a real
   ESC byte at the kush prompt to drive the VGA ANSI parser by hand (e.g.
   `echo "\e[?1049h"` for the alt screen buffer) since kush has no way to
   insert a literal control character into a typed line otherwise. */
static int do_echo(char *argv[], int argc) {
    for (int i=1;i<argc;i++) {
        const char *s = argv[i];
        while (*s) {
            if (s[0] == '\\' && s[1]) {
                switch (s[1]) {
                    case 'n': putchar('\n'); break;
                    case 't': putchar('\t'); break;
                    case 'r': putchar('\r'); break;
                    case 'e': putchar(27);   break;
                    case '\\': putchar('\\'); break;
                    default: putchar(s[0]); putchar(s[1]); break;
                }
                s += 2;
            } else {
                putchar(*s); s++;
            }
        }
        if (i<argc-1) putchar(' ');
    }
    putchar('\n');
    return 0;
}

static int do_ls(char *argv[], int argc) {
    const char *path = argc>1 ? argv[1] : cwd_buf;
    char buf[2048];
    int n = sys_listdir(path, buf, sizeof(buf));
    if (n <= 0) { printf("ls: nothing in %s\n", path); return 1; }
    buf[n] = 0;

    char *p = buf;
    int col = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        printf("  %-18s", p);
        if (++col % 3 == 0) putchar('\n');
        p = nl ? nl+1 : p+strlen(p);
    }
    if (col % 3 != 0) putchar('\n');
    return 0;
}

static int do_cat(char *argv[], int argc) {
    if (argc < 2) { fputs("cat: need filename\n"); return 1; }
    int fd = open(argv[1]);
    if (fd < 0) { printf("cat: %s: not found\n", argv[1]); return 1; }
    char buf[512]; int n;
    while ((n = fread(fd, buf, sizeof(buf)-1)) > 0) {
        buf[n]=0;
        write(buf, (uint32_t)n);
    }
    close(fd);
    return 0;
}

static int do_wc(char *argv[], int argc) {
    (void)argv; (void)argc;
    char buf[512]; int n;
    uint32_t lines=0, words=0, bytes=0;
    int in_word=0;
    while ((n = fread(0, buf, sizeof(buf))) > 0) {
        bytes += (uint32_t)n;
        for (int i=0;i<n;i++) {
            if (buf[i]=='\n') lines++;
            if (buf[i]==' '||buf[i]=='\t'||buf[i]=='\n') in_word=0;
            else if (!in_word) { in_word=1; words++; }
        }
    }
    printf("%6u %6u %6u\n", lines, words, bytes);
    return 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

static int do_sort(char *argv[], int argc) {
    (void)argv; (void)argc;
    char *lines[512]; int nlines=0;
    static char buf[32768]; int total=0; int n;
    while ((n = fread(0, buf+total, (uint32_t)(sizeof(buf)-total-1))) > 0) total+=n;
    buf[total]=0;
    char *p=buf;
    while (*p && nlines<512) {
        char *nl=strchr(p,'\n');
        if (nl) *nl=0;
        lines[nlines++]=p;
        p = nl ? nl+1 : p+strlen(p);
    }
    qsort(lines,(size_t)nlines,sizeof(char*),cmp_str);
    for (int i=0;i<nlines;i++) { fputs(lines[i]); putchar('\n'); }
    return 0;
}

static int do_uniq(char *argv[], int argc) {
    (void)argv; (void)argc;
    static char all[32768]; int total=0; int n;
    while ((n = fread(0, all+total, (uint32_t)(sizeof(all)-total-1))) > 0) total+=n;
    all[total]=0;
    char prev[512]; prev[0]=0;
    char *p=all;
    while (*p) {
        char *nl=strchr(p,'\n'); if (nl) *nl=0;
        if (strcmp(p,prev)!=0) {
            fputs(p); putchar('\n');
            strncpy(prev,p,511);
        }
        p = nl ? nl+1 : p+strlen(p);
    }
    return 0;
}

/* accepts both "-n N" (two args) and "-N" (bundled), like real head/tail */
static int parse_n_flag(char *argv[], int argc, int *fi_out) {
    int n = 10, fi = 1;
    if (argc>2 && !strcmp(argv[1],"-n")) { n = atoi(argv[2]); fi = 3; }
    else if (argc>1 && argv[1][0]=='-' && argv[1][1]) { n = atoi(argv[1]+1); fi = 2; }
    *fi_out = fi;
    return n;
}

static int do_head(char *argv[], int argc) {
    int fi; int n = parse_n_flag(argv, argc, &fi);
    int fd = 0;
    if (argc>fi) { fd = open(argv[fi]); if (fd<0) { printf("head: %s: not found\n", argv[fi]); return 1; } }
    static char buf[8192]; int total=0; int r;
    while (total<(int)sizeof(buf)-1 && (r=fread(fd,buf+total,(uint32_t)(sizeof(buf)-total-1)))>0) total+=r;
    buf[total]=0;
    if (fd) close(fd);
    char *p=buf; int lines=0;
    while (*p && lines<n) {
        char *nl=strchr(p,'\n');
        if (nl) *nl=0;
        fputs(p); putchar('\n');
        p = nl ? nl+1 : p+strlen(p);
        lines++;
    }
    return 0;
}

static int do_tail(char *argv[], int argc) {
    int fi; int n = parse_n_flag(argv, argc, &fi);
    int fd = 0;
    if (argc>fi) { fd = open(argv[fi]); if (fd<0) { printf("tail: %s: not found\n", argv[fi]); return 1; } }
    static char buf[8192]; int total=0; int r;
    while (total<(int)sizeof(buf)-1 && (r=fread(fd,buf+total,(uint32_t)(sizeof(buf)-total-1)))>0) total+=r;
    buf[total]=0;
    if (fd) close(fd);
    char *lines[512]; int nlines=0;
    char *p=buf;
    while (*p && nlines<512) {
        char *nl=strchr(p,'\n');
        if (nl) *nl=0;
        lines[nlines++]=p;
        p = nl ? nl+1 : p+strlen(p);
    }
    int start = nlines>n ? nlines-n : 0;
    for (int i=start;i<nlines;i++) { fputs(lines[i]); putchar('\n'); }
    return 0;
}

static int do_grep(char *argv[], int argc) {
    if (argc < 2) { fputs("grep: grep <pattern> [file]\n"); return 1; }
    const char *pat = argv[1];
    int fd = 0;
    if (argc > 2) { fd = open(argv[2]); if (fd<0) { printf("grep: %s: not found\n", argv[2]); return 1; } }
    static char buf[8192]; int total=0; int r;
    while (total<(int)sizeof(buf)-1 && (r=fread(fd,buf+total,(uint32_t)(sizeof(buf)-total-1)))>0) total+=r;
    buf[total]=0;
    if (fd) close(fd);
    char *p=buf;
    int found = 0;
    while (*p) {
        char *nl=strchr(p,'\n');
        if (nl) *nl=0;
        if (strstr(p,pat)) { fputs(p); putchar('\n'); found = 1; }
        p = nl ? nl+1 : p+strlen(p);
    }
    /* real grep exit status: 0 if a match was found, 1 if not - this is
       what makes `if grep pat file` (or in a script's if/while condition)
       actually mean something instead of always being true. */
    return found ? 0 : 1;
}

static int do_cp(char *argv[], int argc) {
    if (argc < 3) { fputs("cp: cp <src> <dst>\n"); return 1; }
    int src = open(argv[1]);
    if (src < 0) { printf("cp: %s: not found\n", argv[1]); return 1; }

    char dst_path[128] = "/disk/";
    strcat(dst_path, argv[2]);
    int dst = _syscall(SYS_OPEN, (int)dst_path, O_WRONLY|O_CREAT|O_TRUNC, 0);
    if (dst < 0) { close(src); printf("cp: can't create %s\n",argv[2]); return 1; }
    char buf[512]; int n;
    while ((n=fread(src,buf,512))>0) _syscall(SYS_FWRITE,dst,(int)buf,(int)n);
    close(src); close(dst);
    printf("Copied %s -> %s\n", argv[1], argv[2]);
    return 0;
}

static int do_mv(char *argv[], int argc) {
    if (argc < 3) { fputs("mv: mv <src> <dst>\n"); return 1; }
    int src = open(argv[1]);
    if (src < 0) { printf("mv: %s: not found\n", argv[1]); return 1; }

    char dst_path[128] = "/disk/";
    strcat(dst_path, argv[2]);
    int dst = _syscall(SYS_OPEN, (int)dst_path, O_WRONLY|O_CREAT|O_TRUNC, 0);
    if (dst < 0) { close(src); printf("mv: can't create %s\n", argv[2]); return 1; }
    char buf[512]; int n;
    while ((n=fread(src,buf,512))>0) _syscall(SYS_FWRITE,dst,(int)buf,(int)n);
    close(src); close(dst);

    if (sys_unlink(argv[1]) != 0) { printf("mv: copied but couldn't remove %s\n", argv[1]); return 1; }
    printf("Moved %s -> %s\n", argv[1], argv[2]);
    return 0;
}

static int do_rm(char *argv[], int argc) {
    if (argc < 2) { fputs("rm: need filename\n"); return 1; }
    if (sys_unlink(argv[1]) == 0) { printf("Deleted %s\n", argv[1]); return 0; }
    printf("rm: %s: failed\n", argv[1]); return 1;
}

static int do_cd(char *argv[], int argc) {
    const char *path = argc>1 ? argv[1] : "/disk";
    if (sys_chdir(path)==0) { sys_getcwd(cwd_buf,sizeof(cwd_buf)); return 0; }
    printf("cd: %s: not found\n", path); return 1;
}

static int do_pwd(char *argv[], int argc) {
    (void)argv;(void)argc;
    sys_getcwd(cwd_buf, sizeof(cwd_buf));
    puts(cwd_buf);
    return 0;
}

static int do_history(char *argv[], int argc) {
    (void)argv;(void)argc;
    for (int i=0;i<hist_count;i++) printf("  %2d  %s\n", i+1, history[i]);
    return 0;
}

static int do_stat(char *argv[], int argc) {
    if (argc<2){fputs("stat: need filename\n");return 1;}
    stat_t st;
    if (sys_stat(argv[1], &st)<0){printf("stat: %s: not found\n",argv[1]);return 1;}
    printf("  File: %s\n  Size: %u bytes\n  Type: %s\n",
           st.name, st.size,
           st.type==1?"file":st.type==2?"dir":st.type==3?"device":"pipe");
    return 0;
}

/* Toggles the real on-disk FAT12 read-only attribute bit (src/fat12.c),
   not a fabricated permission model - the only per-file access control
   this filesystem format actually has, and only /disk has it (no on-disk
   representation on /mem or /dev to attach a bit to). */
static int do_chmod(char *argv[], int argc) {
    if (argc < 3) { fputs("chmod: chmod <mode> <file>\n"); return 1; }
    if (sys_chmod(argv[2], argv[1]) == 0) { printf("chmod: %s: %s\n", argv[2], argv[1]); return 0; }
    printf("chmod: %s: failed (only /disk files have a real read-only bit)\n", argv[2]);
    return 1;
}

/* Non-recursive filename search (ls itself is non-recursive too - there's
   no directory-tree-walk API in the vfs layer yet to build a real
   recursive find on top of). */
static int do_find(char *argv[], int argc) {
    if (argc < 2) { fputs("find: find [dir] <name>\n"); return 1; }
    const char *pat  = argv[argc-1];
    const char *path = argc>2 ? argv[1] : cwd_buf;

    char buf[2048];
    int n = sys_listdir(path, buf, sizeof(buf));
    if (n <= 0) return 0;
    buf[n] = 0;

    char *p = buf; int found = 0;
    while (*p) {
        char *nl = strchr(p,'\n');
        if (nl) *nl = 0;
        if (strstr(p, pat)) { printf("%s/%s\n", path, p); found = 1; }
        p = nl ? nl+1 : p+strlen(p);
    }
    if (!found) printf("find: no matches for '%s' in %s\n", pat, path);
    return 0;
}

static int do_du(char *argv[], int argc) {
    const char *path = argc>1 ? argv[1] : cwd_buf;

    stat_t st;
    if (sys_stat(path, &st) == 0 && st.type != 2) {
        printf("%6u  %s\n", st.size, path);
        return 0;
    }

    char buf[2048];
    int n = sys_listdir(path, buf, sizeof(buf));
    if (n <= 0) { printf("du: %s: not found\n", path); return 1; }
    buf[n] = 0;

    char *p = buf; uint32_t total = 0;
    while (*p) {
        char *nl = strchr(p,'\n');
        if (nl) *nl = 0;
        char full[192]; strcpy(full, path);
        if (full[strlen(full)-1] != '/') strcat(full, "/");
        strcat(full, p);
        stat_t est;
        if (sys_stat(full, &est) == 0) { printf("%6u  %s\n", est.size, full); total += est.size; }
        p = nl ? nl+1 : p+strlen(p);
    }
    printf("%6u  total\n", total);
    return 0;
}

/* Basic sed s/old/new/ [g] substitution - one expression, one file (or
   stdin), no full regex, no other sed commands. */
static int do_sed(char *argv[], int argc) {
    if (argc < 2 || argv[1][0] != 's' || argv[1][1] != '/') { fputs("sed: sed s/old/new/[g] [file]\n"); return 1; }

    char expr[192]; strncpy(expr, argv[1]+2, sizeof(expr)-1); expr[sizeof(expr)-1]=0;
    char *old = expr;
    char *sep2 = strchr(old, '/');
    if (!sep2) { fputs("sed: bad expression, need s/old/new/\n"); return 1; }
    *sep2 = 0;
    char *new_ = sep2+1;
    char *sep3 = strchr(new_, '/');
    int global = 0;
    if (sep3) { global = (sep3[1]=='g'); *sep3 = 0; }
    int oldlen = (int)strlen(old);

    int fd = 0;
    if (argc > 2) { fd = open(argv[2]); if (fd<0) { printf("sed: %s: not found\n", argv[2]); return 1; } }
    static char buf[8192]; int total=0; int r;
    while (total<(int)sizeof(buf)-1 && (r=fread(fd,buf+total,(uint32_t)(sizeof(buf)-total-1)))>0) total+=r;
    buf[total]=0;
    if (fd) close(fd);

    char *p = buf;
    while (*p) {
        char *nl = strchr(p,'\n');
        if (nl) *nl = 0;

        char outline[512]; int oi=0; int replaced=0;
        char *q = p;
        while (*q && oi < (int)sizeof(outline)-1) {
            if ((!replaced || global) && oldlen>0 && strncmp(q, old, (uint32_t)oldlen)==0) {
                for (const char *rp=new_; *rp && oi<(int)sizeof(outline)-1; rp++) outline[oi++]=*rp;
                q += oldlen;
                replaced = 1;
            } else {
                outline[oi++] = *q++;
            }
        }
        outline[oi] = 0;
        fputs(outline); putchar('\n');
        p = nl ? nl+1 : p+strlen(p);
    }
    return 0;
}

/* Line-by-line diff, classic "< a-line" / "> b-line" format for lines
   that differ at the same line number, plus a trailer if one file has
   extra lines the other doesn't. No real line-matching/hunk algorithm
   (that's a much bigger feature) - just a positional compare, which is
   still genuinely useful for "did this file change" style checks.
   Returns 0 if identical, 1 if different - same convention as real
   diff, so `if diff a b` works in scripts. */
static int do_diff(char *argv[], int argc) {
    if (argc < 3) { fputs("diff: diff <file1> <file2>\n"); return 2; }

    int fd1 = open(argv[1]);
    if (fd1 < 0) { printf("diff: %s: not found\n", argv[1]); return 2; }
    static char buf1[8192]; int n1=0,r;
    while (n1<(int)sizeof(buf1)-1 && (r=fread(fd1,buf1+n1,(uint32_t)(sizeof(buf1)-n1-1)))>0) n1+=r;
    buf1[n1]=0; close(fd1);

    int fd2 = open(argv[2]);
    if (fd2 < 0) { printf("diff: %s: not found\n", argv[2]); return 2; }
    static char buf2[8192]; int n2=0;
    while (n2<(int)sizeof(buf2)-1 && (r=fread(fd2,buf2+n2,(uint32_t)(sizeof(buf2)-n2-1)))>0) n2+=r;
    buf2[n2]=0; close(fd2);

    char *end1 = buf1+n1, *end2 = buf2+n2;
    char *p1=buf1, *p2=buf2;
    int lineno = 0, differed = 0;
    while (p1 < end1 || p2 < end2) {
        lineno++;
        char *line1 = 0, *line2 = 0;
        if (p1 < end1) {
            line1 = p1;
            char *nl = strchr(p1,'\n');
            if (nl && nl < end1) { *nl=0; p1 = nl+1; } else p1 = end1;
        }
        if (p2 < end2) {
            line2 = p2;
            char *nl = strchr(p2,'\n');
            if (nl && nl < end2) { *nl=0; p2 = nl+1; } else p2 = end2;
        }
        if (line1 && line2) {
            if (strcmp(line1,line2) != 0) {
                differed = 1;
                printf("%d,%dc%d,%d\n< %s\n---\n> %s\n", lineno, lineno, lineno, lineno, line1, line2);
            }
        } else if (line1) {
            differed = 1;
            printf("%dd\n< %s\n", lineno, line1);
        } else if (line2) {
            differed = 1;
            printf("%da\n> %s\n", lineno, line2);
        }
    }
    return differed;
}

static int do_write(char *argv[], int argc) {
    if (argc<3){fputs("write: write <file> <content>\n");return 1;}

    char content[512]; content[0]=0;
    for (int i=2;i<argc;i++){strcat(content,argv[i]);if(i<argc-1)strcat(content," ");}
    strcat(content,"\n");
    char path[128]="/disk/"; strcat(path,argv[1]);
    int fd=_syscall(SYS_OPEN,(int)path,O_WRONLY|O_CREAT|O_TRUNC,0);
    if(fd<0){printf("write: can't create %s\n",argv[1]);return 1;}
    _syscall(SYS_FWRITE,fd,(int)content,(int)strlen(content));
    close(fd);
    printf("Written to %s\n",argv[1]);
    return 0;
}

static int do_date(char *argv[], int argc) {
    (void)argv;(void)argc;
    print_time();
    putchar('\n');
    return 0;
}

static int do_kill(char *argv[], int argc) {
    if (argc<2){fputs("kill: kill <pid> [sig]\n");return 1;}
    int pid=atoi(argv[1]);
    int sig=(argc>2)?atoi(argv[2]):15;
    int r=sys_kill(pid,sig);
    if(r<0){printf("kill: pid %d not found\n",pid);return 1;}
    printf("Signal %d sent to PID %d\n",sig,pid);
    return 0;
}

static int do_cat_proc(const char *path) {
    int fd=_syscall(SYS_OPEN,(int)path,O_RDONLY,0);
    if(fd<0){printf("cat: %s: not found\n",path);return 1;}
    char buf[2048]; int n=fread(fd,buf,sizeof(buf)-1);
    if(n>0){buf[n]=0;fputs(buf);}
    close(fd);
    return 0;
}

static int do_help(char *argv[], int argc) {
    (void)argv;(void)argc;
    fputs("\n  kush v" KUSH_VER " - KumOS shell\n\n");
    fputs("  Built-ins:\n");
    fputs("    help              - This help\n");
    fputs("    ls [path]         - List files\n");
    fputs("    cat <file>        - Print file\n");
    fputs("    head [-n N] [f]   - First N lines (default 10)\n");
    fputs("    tail [-n N] [f]   - Last N lines (default 10)\n");
    fputs("    grep <pat> [f]    - Print matching lines\n");
    fputs("    cp <src> <dst>    - Copy file\n");
    fputs("    mv <src> <dst>    - Move/rename file\n");
    fputs("    set               - List shell variables\n");
    fputs("    NAME=value        - Set a shell variable ($NAME to use it)\n");
    fputs("    alias             - List aliases\n");
    fputs("    alias name=value  - Define an alias\n");
    fputs("    rm <file>         - Delete file\n");
    fputs("    write <f> <data>  - Write text to file\n");
    fputs("    cd [path]         - Change directory\n");
    fputs("    pwd               - Print working directory\n");
    fputs("    stat <file>       - File info\n");
    fputs("    chmod <mode> <f>  - Toggle real FAT12 read-only bit (+w/-w or e.g 644/444)\n");
    fputs("    find [dir] <name> - Search filenames (non-recursive)\n");
    fputs("    du [path]         - Disk usage\n");
    fputs("    sed s/old/new/[g] [f] - Stream substitution\n");
    fputs("    diff <f1> <f2>    - Line-by-line compare\n");
    fputs("    date              - Current date/time\n");
    fputs("    history           - Command history\n");
    fputs("    !!  !N            - Repeat last / Nth history entry\n");
    fputs("    exit [code]       - Exit shell\n");
    fputs("    kill <pid> [sig]  - Send signal\n");
    fputs("    jobs              - List background jobs\n");
    fputs("    fg [%N]           - Bring job N (default: last) to foreground\n");
    fputs("    bg [%N]           - Resume stopped job N in the background\n");
    fputs("    wait [%N]         - Wait for job N, or all background jobs\n\n");
    fputs("  /proc files:\n");
    fputs("    cat /proc/meminfo  cat /proc/ps\n");
    fputs("    cat /proc/uptime   cat /proc/net\n\n");
    fputs("  ELF programs (from disk):\n");
    fputs("    HELLO.ELF         - Hello world\n");
    fputs("    COUNTER.ELF       - Counter demo\n");
    fputs("    CAT.ELF           - File reader\n");
    fputs("    SYSINFO.ELF       - System info\n\n");
    fputs("  Pipes:  cmd1 | cmd2 | cmd3 (any number of stages)\n\n");
    fputs("  Job control:  cmd &  or  cmd1 | cmd2 &  runs in the background.\n");
    fputs("    Ctrl+Z stops the foreground job (fg/bg to resume it).\n");
    fputs("    Job specs: %N, %+ (current), %- (previous).\n\n");
    fputs("  Scripts (source <file> or . <file>), one keyword per own line:\n");
    fputs("    if <cmd> / then / ... / elif <cmd> / ... / else / ... / fi\n");
    fputs("    for VAR in a b c / do / ... / done\n");
    fputs("    while <cmd> / do / ... / done\n");
    fputs("    break / continue  (inside for/while)\n\n");
    return 0;
}

/* Job control: background (&), jobs/fg/bg/wait. A single external command
   backgrounds via do_exec() just skipping its own waitpid (g_bg_next,
   set by run_pipeline()'s un-piped single-command path). An N-stage
   pipeline ("a | b &") has no single do_exec() fork to skip - run_pipeline()
   forks its own whole driver once instead and backgrounds *that* (see the
   nparts>1 branch). Builtins alone (no external command anywhere in the
   line) still can't usefully be backgrounded - they run in-process with
   no fork of their own - a bare "&" on one is silently a no-op, same as
   in a real shell backgrounding a shell keyword. */
#define KUSH_MAX_JOBS 8
typedef struct {
    int  used;
    int  id;
    int  pid;
    char cmd[48];
} kush_job_t;
static kush_job_t g_jobs[KUSH_MAX_JOBS];
static int g_next_job_id = 1;
static int g_bg_next = 0;

static int job_add(int pid, const char *cmd) {
    for (int i = 0; i < KUSH_MAX_JOBS; i++) {
        if (g_jobs[i].used) continue;
        g_jobs[i].used = 1;
        g_jobs[i].id   = g_next_job_id++;
        g_jobs[i].pid  = pid;
        strncpy(g_jobs[i].cmd, cmd, sizeof(g_jobs[i].cmd)-1);
        g_jobs[i].cmd[sizeof(g_jobs[i].cmd)-1] = 0;
        return g_jobs[i].id;
    }
    return -1;
}

static kush_job_t *job_find(int id) {
    for (int i = 0; i < KUSH_MAX_JOBS; i++)
        if (g_jobs[i].used && g_jobs[i].id == id) return &g_jobs[i];
    return 0;
}

static kush_job_t *job_last(void) {
    kush_job_t *best = 0;
    for (int i = 0; i < KUSH_MAX_JOBS; i++)
        if (g_jobs[i].used && (!best || g_jobs[i].id > best->id)) best = &g_jobs[i];
    return best;
}

/* Second-most-recent job by id - the "%-" job. */
static kush_job_t *job_prev(void) {
    kush_job_t *first = 0, *second = 0;
    for (int i = 0; i < KUSH_MAX_JOBS; i++) {
        if (!g_jobs[i].used) continue;
        if (!first || g_jobs[i].id > first->id) { second = first; first = &g_jobs[i]; }
        else if (!second || g_jobs[i].id > second->id) second = &g_jobs[i];
    }
    return second;
}

/* Resolves a job-control argument the way real shells do: "%N"/"N" is a
   job id, "%+"/"%%" is the current (most recent) job, "%-" is the
   previous one. Shared by fg/bg/wait so all three accept the same
   spelling. */
static kush_job_t *job_by_spec(const char *spec) {
    if (!spec || !spec[0]) return job_last();
    if (spec[0] == '%') spec++;
    if ((spec[0] == '+' || spec[0] == '%') && spec[1] == 0) return job_last();
    if (spec[0] == '-' && spec[1] == 0) return job_prev();
    return job_find(atoi(spec));
}

/* Reaps any background job that finished on its own (sys_procstate()==2,
   exited but not yet waited on) and prints the "Done" line a real shell
   shows just before its next prompt. Non-blocking - sys_waitstatus() on an
   already-zombie pid returns immediately, it's only a blocking loop while
   the child is still alive. */
static void jobs_reap_done(void) {
    for (int i = 0; i < KUSH_MAX_JOBS; i++) {
        if (!g_jobs[i].used) continue;
        int st = sys_procstate(g_jobs[i].pid);
        if (st == 2) {
            int code = 0;
            sys_waitstatus(g_jobs[i].pid, &code);
            printf("[%d]+  Done                    %s\n", g_jobs[i].id, g_jobs[i].cmd);
            g_jobs[i].used = 0;
        } else if (st == -1) {
            g_jobs[i].used = 0;
        }
    }
}

static int do_jobs(char *argv[], int argc) {
    (void)argv; (void)argc;
    jobs_reap_done();
    int any = 0;
    for (int i = 0; i < KUSH_MAX_JOBS; i++) {
        if (!g_jobs[i].used) continue;
        any = 1;
        int st = sys_procstate(g_jobs[i].pid);
        printf("[%d]+  %-9s%s\n", g_jobs[i].id, st == 1 ? "Stopped" : "Running", g_jobs[i].cmd);
    }
    if (!any) fputs("kush: no background jobs\n");
    return 0;
}

/* Blocks on a job's pid exactly like a freshly-forked foreground command
   would (same sys_waitstatus() path do_exec() uses) - shared by fg and
   wait so a re-stopped or Ctrl+C'd job re-enters the job table correctly
   either way, in both callers. Removes j from the job table up front
   (re-added under a fresh id if it stops again, same as do_exec()). */
static int job_wait_fg(kush_job_t *j) {
    int pid = j->pid;
    char cmd[48]; strncpy(cmd, j->cmd, sizeof(cmd)-1); cmd[sizeof(cmd)-1]=0;
    j->used = 0;

    int status = 0;
    int r = sys_waitstatus(pid, &status);
    if (r == 2) {
        int jid = job_add(pid, cmd);
        printf("\n[%d]+  Stopped                 %s\n", jid, cmd);
        return 148;
    }
    return status;
}

/* fg [%N] - bring a job to the foreground: SIGCONT it if stopped, then
   block on it. */
static int do_fg(char *argv[], int argc) {
    kush_job_t *j = (argc > 1) ? job_by_spec(argv[1]) : job_last();
    if (!j) { fputs("kush: fg: no such job\n"); return 1; }
    printf("%s\n", j->cmd);
    sys_kill(j->pid, KUSH_SIGCONT);
    return job_wait_fg(j);
}

/* bg [%N] - resume a stopped job in the background: SIGCONT it, leave it
   in the job table, don't wait. No-op (with a message) on a job that's
   already running. */
static int do_bg(char *argv[], int argc) {
    kush_job_t *j = (argc > 1) ? job_by_spec(argv[1]) : job_last();
    if (!j) { fputs("kush: bg: no such job\n"); return 1; }
    if (sys_procstate(j->pid) != 1) { printf("kush: bg: job %d already running\n", j->id); return 1; }
    sys_kill(j->pid, KUSH_SIGCONT);
    printf("[%d]+  %s &\n", j->id, j->cmd);
    return 0;
}

/* wait [%N] - with no argument, blocks until every current background job
   has exited (real POSIX semantics: doesn't touch jobs that get started
   after wait begins, but that's not possible here since kush is single-
   threaded and can't start a new one mid-wait anyway). With an argument,
   waits for just that one job. A job that gets stopped instead of exiting
   re-enters the job table exactly like fg does rather than hanging. */
static int do_wait(char *argv[], int argc) {
    if (argc > 1) {
        kush_job_t *j = job_by_spec(argv[1]);
        if (!j) { fputs("kush: wait: no such job\n"); return 1; }
        return job_wait_fg(j);
    }
    int any = 0, status = 0;
    for (int i = 0; i < KUSH_MAX_JOBS; i++) {
        if (!g_jobs[i].used) continue;
        any = 1;
        status = job_wait_fg(&g_jobs[i]);
    }
    return any ? status : 0;
}

static int do_exec(char *argv[], int argc) {
    const char *cmd = argv[0];

    char upper[64]; int i=0;
    while(cmd[i]&&i<60){char c=cmd[i];if(c>='a'&&c<='z')c-=32;upper[i++]=c;}
    upper[i]=0;

    if (!strchr(upper, '.')) { strcat(upper, ".ELF"); }

    /* Real fork()+execve()+waitpid(), finally. This took six real bugs
       across this round and the previous ones to actually nail down:
       1) paging_clone_dir() aliased PDE 1 (virt 0x400000, where every user
          ELF's code loads) instead of deep-copying it - fixed in
          src/paging.c (round 14).
       2) sched_yield() had a premature `sti` before the stack-pointer
          swap in switch_context(), letting a nested timer tick corrupt a
          mid-switch task's register frame - fixed in boot/sched_switch.asm
          + src/sched.c (round 14).
       3) Every fork()'d child leaked its entire private page directory on
          exit (sched_exit_code() freed the kernel stack but never touched
          page_dir_phys) - fixed in src/sched.c.
       4) sc_execve() freed the caller's own page directory (still the
          live CR3!) *before* building and switching to the replacement -
          a real use-after-free of actively-mapped page tables. Fixed by
          building+switching first, freeing the old one only once it's no
          longer live - src/syscall.c.
       5) The actual root cause of the long-standing "return to ring3 after
          waitpid lands in garbage EIP/ESP" bug, finally nailed down with a
          hardware watchpoint: paging_clone_dir()'s "PDE1 is special, deep-
          copy it" fix (bug #1 above) missed that the user STACK needs the
          exact same treatment. ELF_USER_STACK_TOP is exactly 0x40000000
          (1GB) and the stack grows down from there, landing at virt
          0x3FFFC000-0x3FFFFFFF - PDE 255, one PDE short of the ">=256 is
          always private" boundary the function assumed covered the whole
          per-process range. PDE 255 was being ALIASED (raw pointer share)
          across fork(), so a forked child's stack lived in the exact same
          physical page table as its parent's. fork() alone (child never
          touching stack pages beyond what it inherited) happened not to
          visibly corrupt anything, but execve() unconditionally remaps
          fresh stack pages - and since that remap hit the shared table, an
          execve()'d child silently overwrote its own parent's live stack
          contents out from under it. Confirmed live: kush's own saved
          return address flipped from a valid value to 0 at the exact
          instant its execve()'d child (hello.elf) zeroed its own fresh
          stack page. kush's syscall-return machinery (isr128, the fabri-
          cated iret frame, switch_context) was correct the entire time -
          fixed in src/paging.c (paging_clone_dir()/paging_free_user()).
       6) One more crash survived all five fixes above, but only on the
          SECOND fork()+execve() in a row (any two programs, not sysinfo-
          specific) - a plain kmemcpy() page fault at exactly CR2=0x1000000
          (16MB). paging_init() only ever identity-mapped the first 16MB of
          physical RAM (4 static page tables, a leftover fixed constant),
          but pmm_alloc() hands out physical frames anywhere across all of
          mem_kb - and paging_clone_dir() (plus other code) treats whatever
          pmm_alloc() returns as a directly-dereferenceable kernel pointer
          via that identity mapping. The first fork()+exec() consumed just
          barely enough memory (clone + ELF pages + both now-deep-copied
          stacks) to still land under 16MB; the second one reliably pushed
          pmm_alloc() past it. Fixed by sizing the identity map to actual
          RAM at boot instead of a fixed 16MB - src/paging.c (paging_init()).
       Also fixed along the way: sc_exit() silently discarded its own
       `code` argument and always exited 0 (never seen because nothing
       ever chained fork()+wait() far enough to observe a real child exit
       code before), and sys_fork()'s inline asm was missing the "memory"
       clobber every other syscall wrapper already has. */
    int pid = sys_fork();
    if (pid < 0) { printf("kush: fork failed\n"); return 1; }
    if (pid == 0) {
        sys_signal(2, 0);
        sys_signal(20, 0);
        char *nargv[16]; int n = 0;
        nargv[n++] = upper;
        for (int j = 1; j < argc && n < 15; j++) nargv[n++] = argv[j];
        nargv[n] = 0;
        execve(upper, nargv);
        printf("kush: %s: not found\n", cmd);
        exit(127);
    }
    if (g_bg_next) {
        g_bg_next = 0;
        int jid = job_add(pid, cmd);
        printf("[%d] %d\n", jid, pid);
        return 0;
    }

    int status = 0;
    int r = sys_waitstatus(pid, &status);
    if (r == 2) {
        int jid = job_add(pid, cmd);
        printf("\n[%d]+  Stopped                 %s\n", jid, cmd);
        return 148;
    }
    return status;
}

static int run_script(const char *filename);
static void do_set(void);

static int dispatch_one(char *argv[], int argc) {
    const char *cmd = argv[0];
    if (!strcmp(cmd,"exit")){exit(argc>1?atoi(argv[1]):0);}
    if (!strcmp(cmd,"set")) { do_set(); return 0; }
    if (!strcmp(cmd,"source")||!strcmp(cmd,".")) {
        if(argc>1) return run_script(argv[1]);
        puts("source: need filename"); return 1;
    }
    if (!strcmp(cmd,"help"))    return do_help(argv,argc);
    if (!strcmp(cmd,"ls"))      return do_ls(argv,argc);
    if (!strcmp(cmd,"cat"))     return do_cat(argv,argc);
    if (!strcmp(cmd,"wc"))      return do_wc(argv,argc);
    if (!strcmp(cmd,"sort"))    return do_sort(argv,argc);
    if (!strcmp(cmd,"uniq"))    return do_uniq(argv,argc);
    if (!strcmp(cmd,"head"))    return do_head(argv,argc);
    if (!strcmp(cmd,"tail"))    return do_tail(argv,argc);
    if (!strcmp(cmd,"grep"))    return do_grep(argv,argc);
    if (!strcmp(cmd,"cp"))      return do_cp(argv,argc);
    if (!strcmp(cmd,"mv"))      return do_mv(argv,argc);
    if (!strcmp(cmd,"rm"))      return do_rm(argv,argc);
    if (!strcmp(cmd,"cd"))      return do_cd(argv,argc);
    if (!strcmp(cmd,"pwd"))     return do_pwd(argv,argc);
    if (!strcmp(cmd,"stat"))    return do_stat(argv,argc);
    if (!strcmp(cmd,"chmod"))   return do_chmod(argv,argc);
    if (!strcmp(cmd,"find"))    return do_find(argv,argc);
    if (!strcmp(cmd,"du"))      return do_du(argv,argc);
    if (!strcmp(cmd,"sed"))     return do_sed(argv,argc);
    if (!strcmp(cmd,"diff"))    return do_diff(argv,argc);
    if (!strcmp(cmd,"echo"))    return do_echo(argv,argc);
    if (!strcmp(cmd,"date"))    return do_date(argv,argc);
    if (!strcmp(cmd,"history")) return do_history(argv,argc);
    if (!strcmp(cmd,"write"))   return do_write(argv,argc);
    if (!strcmp(cmd,"kill"))    return do_kill(argv,argc);
    if (!strcmp(cmd,"jobs"))    return do_jobs(argv,argc);
    if (!strcmp(cmd,"fg"))      return do_fg(argv,argc);
    if (!strcmp(cmd,"bg"))      return do_bg(argv,argc);
    if (!strcmp(cmd,"wait"))    return do_wait(argv,argc);
    if (!strcmp(cmd,"clear"))   { fputs("\033[2J\033[H"); return 0; }
    return do_exec(argv, argc);
}

/* Strips >, >>, < tokens (and their filename argument) out of argv,
   compacting the remaining args in place. Requires whitespace around the
   operator, same as split()'s space-only tokenizing - "cmd>file" with no
   spaces isn't recognized, only "cmd > file". */
/* Strips a trailing bare "&" token (background job marker) off argv, same
   in-place-compaction style as parse_redir() below. Used on the un-piped
   single-command path in run_pipeline() - do_exec() backgrounds those by
   skipping its own waitpid. N-stage pipelines get backgrounded a
   different way (see strip_bg_str() and the nparts>1 branch further
   down) since there's no single do_exec() fork to skip there. */
static int strip_bg(char *argv[], int *argc) {
    if (*argc > 0 && !strcmp(argv[*argc-1], "&")) {
        (*argc)--;
        argv[*argc] = 0;
        return 1;
    }
    return 0;
}

/* Same job as strip_bg() above but works on the raw, not-yet-split text of
   a pipeline's last stage - "&" has to be its own whitespace-delimited
   token, not just a trailing character (so a filename ending in "&"
   through redirection isn't mistaken for one). */
static int strip_bg_str(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && s[len-1] == ' ') len--;
    if (len > 0 && s[len-1] == '&' && (len == 1 || s[len-2] == ' ')) {
        len--;
        while (len > 0 && s[len-1] == ' ') len--;
        s[len] = 0;
        return 1;
    }
    return 0;
}

static void parse_redir(char *argv[], int *argc, char **out_file, int *append, char **in_file) {
    int j = 0;
    for (int i = 0; i < *argc; i++) {
        if (!strcmp(argv[i], ">") && i+1 < *argc)       { *out_file = argv[++i]; *append = 0; }
        else if (!strcmp(argv[i], ">>") && i+1 < *argc) { *out_file = argv[++i]; *append = 1; }
        else if (!strcmp(argv[i], "<") && i+1 < *argc)  { *in_file  = argv[++i]; }
        else argv[j++] = argv[i];
    }
    argv[j] = 0;
    *argc = j;
}

/* Shell variables: VAR=value assignment + $VAR expansion. This is the
   scoped-down piece of "scripting" that landed - real control flow
   (if/for/while) would need a proper parser/interpreter and didn't fit
   in the time available this round. */
#define KUSH_MAX_VARS 16
#define KUSH_VAR_NAME 32
#define KUSH_VAR_VAL  192
static char var_names[KUSH_MAX_VARS][KUSH_VAR_NAME];
static char var_vals[KUSH_MAX_VARS][KUSH_VAR_VAL];
static int  nvars = 0;

static int is_var_char(char c) { return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'; }

static const char *var_get(const char *name, int len) {
    for (int i=0;i<nvars;i++) {
        int j=0; while (j<len && var_names[i][j]==name[j]) j++;
        if (j==len && var_names[i][j]==0) return var_vals[i];
    }
    return 0;
}

static void var_set(const char *name, int len, const char *val) {
    if (len >= KUSH_VAR_NAME) len = KUSH_VAR_NAME-1;
    for (int i=0;i<nvars;i++) {
        int j=0; while (j<len && var_names[i][j]==name[j]) j++;
        if (j==len && var_names[i][j]==0) { strncpy(var_vals[i], val, KUSH_VAR_VAL-1); var_vals[i][KUSH_VAR_VAL-1]=0; return; }
    }
    if (nvars < KUSH_MAX_VARS) {
        strncpy(var_names[nvars], name, len); var_names[nvars][len]=0;
        strncpy(var_vals[nvars], val, KUSH_VAR_VAL-1); var_vals[nvars][KUSH_VAR_VAL-1]=0;
        nvars++;
    }
}

static void do_set(void) {
    for (int i=0;i<nvars;i++) printf("%s=%s\n", var_names[i], var_vals[i]);
}

/* NAME=value with no spaces around '=' - consumes the whole line as an
   assignment (not dispatched as a command) if it matches. */
static int try_assign(const char *line) {
    const char *p = line;
    if (!(( *p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||*p=='_')) return 0;
    const char *start = p;
    while (is_var_char(*p)) p++;
    if (*p != '=' || p == start) return 0;
    int len = (int)(p-start);
    var_set(start, len, p+1);
    return 1;
}

static void expand_vars(const char *in, char *out, int outmax) {
    int oi = 0;
    for (int i=0; in[i] && oi<outmax-1; ) {
        if (in[i]=='$' && is_var_char(in[i+1]) && !(in[i+1]>='0'&&in[i+1]<='9')) {
            int j = i+1;
            while (is_var_char(in[j])) j++;
            const char *val = var_get(in+i+1, j-(i+1));
            if (val) for (int k=0; val[k] && oi<outmax-1; k++) out[oi++] = val[k];
            i = j;
        } else {
            out[oi++] = in[i++];
        }
    }
    out[oi] = 0;
}

#define KUSH_MAX_ALIAS 16
#define KUSH_ALIAS_NAME 16
#define KUSH_ALIAS_VAL  128
static char alias_names[KUSH_MAX_ALIAS][KUSH_ALIAS_NAME];
static char alias_vals[KUSH_MAX_ALIAS][KUSH_ALIAS_VAL];
static int  nalias = 0;

static const char *alias_get(const char *name) {
    for (int i=0;i<nalias;i++) if (!strcmp(alias_names[i],name)) return alias_vals[i];
    return 0;
}

static void alias_set(const char *name, const char *val) {
    for (int i=0;i<nalias;i++) {
        if (!strcmp(alias_names[i],name)) {
            strncpy(alias_vals[i],val,KUSH_ALIAS_VAL-1); alias_vals[i][KUSH_ALIAS_VAL-1]=0;
            return;
        }
    }
    if (nalias < KUSH_MAX_ALIAS) {
        strncpy(alias_names[nalias],name,KUSH_ALIAS_NAME-1); alias_names[nalias][KUSH_ALIAS_NAME-1]=0;
        strncpy(alias_vals[nalias],val,KUSH_ALIAS_VAL-1); alias_vals[nalias][KUSH_ALIAS_VAL-1]=0;
        nalias++;
    }
}

static void do_alias_list(void) {
    for (int i=0;i<nalias;i++) printf("alias %s='%s'\n", alias_names[i], alias_vals[i]);
}

/* "alias" with no args lists them, "alias name=value" defines one - handled
   as a whole-line special case (like try_assign) rather than a normal
   dispatch_one builtin, since it needs the raw unsplit line to capture a
   multi-word value after the '='. */
static int try_alias_cmd(const char *line) {
    if (strncmp(line,"alias",5) != 0) return 0;
    if (line[5] != 0 && line[5] != ' ') return 0;
    const char *rest = line+5;
    while (*rest==' ') rest++;
    if (!*rest) { do_alias_list(); return 1; }
    const char *eq = strchr(rest,'=');
    if (!eq) { fputs("alias: usage: alias name=value\n"); return 1; }
    char name[KUSH_ALIAS_NAME]; int nlen=(int)(eq-rest);
    if (nlen >= KUSH_ALIAS_NAME) nlen = KUSH_ALIAS_NAME-1;
    strncpy(name, rest, (size_t)nlen); name[nlen]=0;
    alias_set(name, eq+1);
    return 1;
}

/* Expands a leading alias reference by splicing its value in place of the
   first word - single pass, non-recursive (an alias whose value is itself
   an alias name won't chain, a deliberate simplification). */
static void expand_alias(char *line, int max) {
    char first[KUSH_ALIAS_NAME]; int fi=0;
    char *q = line;
    while (*q==' ') q++;
    while (*q && *q!=' ' && fi<KUSH_ALIAS_NAME-1) first[fi++]=*q++;
    first[fi]=0;
    const char *aval = alias_get(first);
    if (!aval) return;
    char combined[CMD_MAX];
    strncpy(combined, aval, CMD_MAX-1); combined[CMD_MAX-1]=0;
    strncat(combined, q, CMD_MAX-1-strlen(combined));
    strncpy(line, combined, (size_t)max-1); line[max-1]=0;
}

static int run_pipeline(char *line) {

    /* cmd1; cmd2; cmd3 - run sequentially left to right, each segment
       fully independent (its own var/alias expansion, its own pipe
       parsing), same as putting them on separate lines in a script.
       kush has no quoting at all yet, so a literal ';' can't currently
       appear inside an argument - not handling that case. */
    {
        char *semi = strchr(line, ';');
        if (semi) {
            int ret = 0;
            char *seg = line;
            while (semi) {
                *semi = 0;
                while (*seg == ' ') seg++;
                if (*seg) ret = run_pipeline(seg);
                seg = semi + 1;
                semi = strchr(seg, ';');
            }
            while (*seg == ' ') seg++;
            if (*seg) ret = run_pipeline(seg);
            return ret;
        }
    }

    if (try_assign(line)) return 0;
    if (try_alias_cmd(line)) return 0;

    char expanded[CMD_MAX];
    expand_vars(line, expanded, CMD_MAX);
    expand_alias(expanded, CMD_MAX);
    line = expanded;

    /* Snapshot before the '|' tokenizer below starts overwriting bytes of
       `line` with NULs - only actually used as a job's display name if
       this turns out to be a backgrounded N-stage pipeline, but cheap
       enough (CMD_MAX=256) to always take. */
    char pipeline_src[CMD_MAX];
    strncpy(pipeline_src, line, sizeof(pipeline_src)-1);
    pipeline_src[sizeof(pipeline_src)-1] = 0;

    char *parts[8]; int nparts = 0;
    char *p = line;
    parts[nparts++] = p;
    while (*p) {
        if (*p == '|') { *p++ = 0; parts[nparts++] = p; }
        else p++;
    }

    if (nparts == 1) {

        char *argv[ARG_MAX]; int argc = split(parts[0], argv, ARG_MAX);
        if (!argc) return 0;

        int background = strip_bg(argv, &argc);
        if (!argc) return 0;

        char *out_file = 0, *in_file = 0; int append = 0;
        parse_redir(argv, &argc, &out_file, &append, &in_file);
        if (!argc) return 0;

        /* vfs_dup2() only refcounts pipes, not regular files - closing
           the original fd right after dup2'ing it onto 1/0 (the way the
           pipe code below does) frees the underlying FAT12 file handle
           the dup'd descriptor still depends on, so every write after
           that silently goes nowhere (file gets created via O_CREAT but
           stays 0 bytes). Fix: keep the original fd open until *after*
           fd 1/0 have been restored to their saved values, so it's no
           longer aliased by anything when it's finally closed. */
        int out_fd = -1, in_fd = -1;
        int saved_out = -1, saved_in = -1;
        if (out_file) {
            char path[128] = "/disk/"; strcat(path, out_file);
            out_fd = _syscall(SYS_OPEN, (int)path, O_WRONLY|O_CREAT|(append?O_APPEND:O_TRUNC), 0);
            if (out_fd < 0) { printf("kush: can't open %s\n", out_file); return 1; }
            saved_out = sys_dup2(1, 12);
            sys_dup2(out_fd, 1);
        }
        if (in_file) {
            in_fd = open(in_file);
            if (in_fd < 0) {
                printf("kush: %s: not found\n", in_file);
                if (saved_out >= 0) { sys_dup2(saved_out, 1); close(saved_out); }
                if (out_fd >= 0) close(out_fd);
                return 1;
            }
            saved_in = sys_dup2(0, 13);
            sys_dup2(in_fd, 0);
        }

        g_bg_next = background;
        int ret = dispatch_one(argv, argc);
        g_bg_next = 0;

        if (saved_out >= 0) { sys_dup2(saved_out, 1); close(saved_out); }
        if (saved_in  >= 0) { sys_dup2(saved_in,  0); close(saved_in);  }
        if (out_fd >= 0) close(out_fd);
        if (in_fd  >= 0) close(in_fd);
        return ret;
    }

    /* Whole-pipeline background ("a | b &"): unlike the single-command
       case, there's no single do_exec() fork here to just skip the wait
       on - a pipeline's stages already run through this same function
       sequentially (see the loop below), so instead the whole driver
       forks *itself* once. The child runs the stage loop exactly as the
       foreground case would and exits with the result; the parent (the
       interactive kush the user is typing into) registers the forked pid
       as a job and returns immediately without touching the stage loop
       at all. This nests fork() one level deeper than usual - each
       stage's do_exec() still forks its own child from *inside* this
       already-forked pipeline driver - but that's the same fork/execve/
       waitpid path already verified solid, just invoked from a process
       that itself came from fork() instead of from kush's original pid. */
    int pipeline_bg = strip_bg_str(parts[nparts-1]);
    if (pipeline_bg) {
        /* pipeline_src was snapshotted before the "&" got stripped off
           parts[] above - strip it here too so the job table shows the
           bare pipeline ("ls /disk | grep") instead of trailing "&". */
        strip_bg_str(pipeline_src);
        int pid = sys_fork();
        if (pid < 0) { fputs("kush: fork failed\n"); return 1; }
        if (pid != 0) {
            int jid = job_add(pid, pipeline_src);
            printf("[%d] %d\n", jid, pid);
            return 0;
        }
        /* child falls through to the normal stage loop below, then must
           exit (not return) at the bottom - it's a forked copy of the
           whole shell, returning would drop back into the interactive
           read loop as a second kush fighting over the same console. */
    }

    /* N-stage pipeline (a|b|c|...). Builtins run in-process (no fork), so
       each stage runs to full completion before the next one starts -
       stage i's whole output goes into a pipe, then stage i+1 reads it
       back out. Was hardcoded to exactly 2 stages (parts[0]/parts[1]),
       silently dropping anything past the second '|' - now loops over
       however many stages split() found (up to 8). */
    int prev_read = -1;
    int last_ret = 0;
    for (int i = 0; i < nparts; i++) {
        char *argv[ARG_MAX]; int argc = split(parts[i], argv, ARG_MAX);
        if (!argc) { if (prev_read >= 0) close(prev_read); return 0; }

        int is_first = (i == 0);
        int is_last  = (i == nparts - 1);

        /* `< infile | ...` on the first stage, `... | cmd > outfile` on
           the last - was only ever handled for a lone (non-piped)
           command, so e.g. `cat file | grep x > out.txt` silently passed
           ">" and "out.txt" as literal arguments to grep instead of
           redirecting. Mid-pipeline redirects on stages in between don't
           make much sense (their fds are already wired to the pipe) so
           those aren't parsed for redirect tokens at all. */
        char *out_file = 0, *in_file = 0; int append = 0;
        if (is_first || is_last) parse_redir(argv, &argc, &out_file, &append, &in_file);
        if (!argc) { if (prev_read >= 0) close(prev_read); return 0; }

        int pipefd[2] = {-1, -1};
        if (!is_last && sys_pipe(pipefd) < 0) {
            fputs("pipe failed\n");
            if (prev_read >= 0) close(prev_read);
            return 1;
        }
        int saved_stdin = -1, saved_stdout = -1;
        int redir_out_fd = -1, redir_in_fd = -1;

        if (prev_read >= 0) {
            saved_stdin = sys_dup2(0, 11);
            sys_dup2(prev_read, 0);
            close(prev_read);
        } else if (in_file) {
            redir_in_fd = open(in_file);
            if (redir_in_fd < 0) { printf("kush: %s: not found\n", in_file); return 1; }
            saved_stdin = sys_dup2(0, 13);
            sys_dup2(redir_in_fd, 0);
        }

        if (!is_last) {
            saved_stdout = sys_dup2(1, 10);
            sys_dup2(pipefd[1], 1);
            close(pipefd[1]);
        } else if (out_file) {
            char path[128] = "/disk/"; strcat(path, out_file);
            redir_out_fd = _syscall(SYS_OPEN, (int)path, O_WRONLY|O_CREAT|(append?O_APPEND:O_TRUNC), 0);
            if (redir_out_fd < 0) { printf("kush: can't open %s\n", out_file); return 1; }
            saved_stdout = sys_dup2(1, 12);
            sys_dup2(redir_out_fd, 1);
        }

        /* Routes through dispatch_one itself (not a hand-copied builtin
           subset) so every builtin works as a pipe stage automatically. */
        last_ret = dispatch_one(argv, argc);

        if (saved_stdout >= 0) { sys_dup2(saved_stdout, 1); close(saved_stdout); }
        if (saved_stdin  >= 0) { sys_dup2(saved_stdin,  0); close(saved_stdin);  }
        if (redir_out_fd >= 0) close(redir_out_fd);
        if (redir_in_fd  >= 0) close(redir_in_fd);

        prev_read = is_last ? -1 : pipefd[0];
    }
    /* Real pipeline exit status: the last stage's, not always 0 - so
       `if a | b` (used heavily by scripts, e.g. `if cmd | grep pat`)
       actually reflects whether the pipeline succeeded. */
    if (pipeline_bg) exit(last_ret);
    return last_ret;
}

static void print_prompt(void) {
    sys_getcwd(cwd_buf, sizeof(cwd_buf));
    printf("\033[32mkush\033[0m:\033[36m%s\033[0m$ ", cwd_buf);
}

/* fastfetch-style: logo left, real system info right, row for row */
static void motd(void) {
    const char *logo[6] = {
        "  _  ___   _ ____  _   _  ",
        " | |/ / | | / ___|| | | | ",
        " | ' /| | | \\___ \\| |_| | ",
        " | . \\| |_| |___) |  _  | ",
        " |_|\\_\\\\___/|____/|_| |_| ",
        "                           ",
    };
    char info[6][40];
    sprintf(info[0], "OS: KumOS v%s", KUMOS_VERSION);
    sprintf(info[1], "Kernel: x86 (i686)");
    sprintf(info[2], "Shell: kush v%s", KUSH_VER);
    sprintf(info[3], "Uptime: %us", uptime());
    time_t t;
    if (gettime(&t) == 0)
        sprintf(info[4], "Date: %04u-%02u-%02u", (unsigned)t.year, (unsigned)t.month, (unsigned)t.day);
    else
        info[4][0] = 0;
    sprintf(info[5], "Type 'help' to get started");

    fputs("\n");
    for (int i = 0; i < 6; i++) {
        fputs(logo[i]);
        fputs("  ");
        fputs(info[i]);
        fputs("\n");
    }
    fputs("\n");
}

/* Real control flow for scripts: if/elif/then/else/fi, for VAR in .../do/
   done, while <cmd>/do/done, break/continue. Deliberately scoped down
   from real shell syntax - each keyword needs its own line (no "if cond;
   then" on one line) - to keep the block matcher a simple depth counter
   instead of a real tokenizer. Nesting works (any mix of if/for/while).
   Still no arithmetic, no functions. */
#define SCRIPT_MAX_LINES 200

/* break/continue can't just `return` out of the loop's own C for/while -
   they're hit inside script_exec_block(), which may be several levels of
   *nested* recursive calls deep (e.g. break inside an if inside a while).
   These flags let a break/continue unwind back up through every enclosing
   non-loop block (checked at the top of script_exec_block's own loop) and
   get caught by the nearest actual while/for, same idea as a real
   language's loop-unwind, just done by hand with two static flags instead
   of a real exception mechanism. */
static int loop_break    = 0;
static int loop_continue = 0;

static int is_block_open(const char *l) {
    return !strncmp(l,"if ",3) || !strcmp(l,"if") || !strncmp(l,"for ",4) || !strncmp(l,"while ",6);
}
static int is_block_close(const char *l) {
    return !strcmp(l,"fi") || !strcmp(l,"done");
}

/* Advances *idx to the matching fi/done for the block whose body starts
   at *idx (already past the opening line), landing ON that close line -
   used both for "skip the branch we're not taking" and "find where a
   loop body ends" duty. */
static void skip_block(char *lines[], int n, int *idx) {
    int depth = 1;
    while (*idx < n) {
        char *l = lines[*idx];
        if (is_block_open(l)) depth++;
        else if (is_block_close(l)) { depth--; if (!depth) return; }
        (*idx)++;
    }
}

/* Same, but also stops early on a same-depth "else"/"elif" (for skipping
   an if/elif-branch that wasn't taken, which needs to land on whichever
   of else/elif/fi comes next so the if-handler can decide what to do). */
static void skip_to_else_or_close(char *lines[], int n, int *idx) {
    int depth = 1;
    while (*idx < n) {
        char *l = lines[*idx];
        if (is_block_open(l)) { depth++; (*idx)++; continue; }
        if (is_block_close(l)) { depth--; if (!depth) return; (*idx)++; continue; }
        if (depth==1 && (!strcmp(l,"else") || !strncmp(l,"elif ",5))) return;
        (*idx)++;
    }
}

static int script_exec_block(char *lines[], int n, int *idx) {
    int ret = 0;
    while (*idx < n) {
        if (loop_break || loop_continue) return ret;

        char *line = lines[*idx];
        if (!strcmp(line,"fi") || !strcmp(line,"done") || !strcmp(line,"else") || !strncmp(line,"elif ",5))
            return ret;

        if (!strcmp(line,"break"))    { loop_break = 1;    (*idx)++; return ret; }
        if (!strcmp(line,"continue")) { loop_continue = 1; (*idx)++; return ret; }

        if (!strncmp(line,"if ",3) || !strcmp(line,"if")) {
            char cond[CMD_MAX]; strncpy(cond, line+2, CMD_MAX-1); cond[CMD_MAX-1]=0;
            char *c = cond; while (*c==' ') c++;
            (*idx)++;
            if (*idx<n && !strcmp(lines[*idx],"then")) (*idx)++;

            int matched = 0;
            int cret = run_pipeline(c);
            if (cret == 0) { matched = 1; ret = script_exec_block(lines, n, idx); }
            else            skip_to_else_or_close(lines, n, idx);

            /* any number of elif clauses, first matching one wins */
            while (!matched && *idx<n && !strncmp(lines[*idx],"elif ",5)) {
                char econd[CMD_MAX]; strncpy(econd, lines[*idx]+5, CMD_MAX-1); econd[CMD_MAX-1]=0;
                (*idx)++;
                if (*idx<n && !strcmp(lines[*idx],"then")) (*idx)++;
                int eret = run_pipeline(econd);
                if (eret == 0) { matched = 1; ret = script_exec_block(lines, n, idx); }
                else            skip_to_else_or_close(lines, n, idx);
            }

            if (*idx<n && !strcmp(lines[*idx],"else")) {
                (*idx)++;
                if (!matched) { matched = 1; ret = script_exec_block(lines, n, idx); }
                else           skip_block(lines, n, idx);
            }
            if (*idx<n && !strcmp(lines[*idx],"fi")) (*idx)++;
            continue;
        }

        if (!strncmp(line,"while ",6)) {
            char cond[CMD_MAX]; strncpy(cond, line+6, CMD_MAX-1); cond[CMD_MAX-1]=0;
            (*idx)++;
            if (*idx<n && !strcmp(lines[*idx],"do")) (*idx)++;
            int body_start = *idx;

            int guard = 0; /* hard cap - a script bug shouldn't hang the shell forever */
            while (run_pipeline(cond) == 0 && guard < 10000) {
                int bi = body_start;
                ret = script_exec_block(lines, n, &bi);
                guard++;
                loop_continue = 0;
                if (loop_break) { loop_break = 0; break; }
            }
            *idx = body_start;
            skip_block(lines, n, idx);
            if (*idx<n && !strcmp(lines[*idx],"done")) (*idx)++;
            continue;
        }

        if (!strncmp(line,"for ",4)) {
            char rest[CMD_MAX]; strncpy(rest, line+4, CMD_MAX-1); rest[CMD_MAX-1]=0;
            char *fargv[ARG_MAX]; int fargc = split(rest, fargv, ARG_MAX);
            (*idx)++;
            if (*idx<n && !strcmp(lines[*idx],"do")) (*idx)++;
            int body_start = *idx;

            if (fargc < 3 || strcmp(fargv[1],"in") != 0) {
                fputs("kush: for: syntax: for VAR in item1 item2 ...\n");
            } else {
                for (int k=2; k<fargc; k++) {
                    var_set(fargv[0], (int)strlen(fargv[0]), fargv[k]);
                    int bi = body_start;
                    ret = script_exec_block(lines, n, &bi);
                    loop_continue = 0;
                    if (loop_break) { loop_break = 0; break; }
                }
            }
            *idx = body_start;
            skip_block(lines, n, idx);
            if (*idx<n && !strcmp(lines[*idx],"done")) (*idx)++;
            continue;
        }

        ret = run_pipeline(line);
        (*idx)++;
    }
    return ret;
}

static int run_script(const char *filename) {
    int fd = open(filename);
    if (fd < 0) { printf("kush: %s: not found\n", filename); return 1; }
    static char buf[4096]; int n = fread(fd, buf, sizeof(buf)-1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;

    static char *lines[SCRIPT_MAX_LINES];
    int nlines = 0;
    char *p = buf;
    while (*p && nlines < SCRIPT_MAX_LINES) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p);
        while (end > p && (end[-1]==' '||end[-1]=='\t'||end[-1]=='\r')) *--end = 0;
        if (*p && *p != '#') lines[nlines++] = p;
        p = nl ? nl+1 : p+strlen(p);
    }

    int idx = 0;
    return script_exec_block(lines, nlines, &idx);
}

int main(void) {
    sys_signal(2, (void (*)(int))1);
    sys_signal(20, (void (*)(int))1);
    sys_getcwd(cwd_buf, sizeof(cwd_buf));
    if (!cwd_buf[0]) strcpy(cwd_buf, "/disk");

    motd();

    char line[CMD_MAX];
    int last_exit = 0;

    for (;;) {

        jobs_reap_done();
        if (last_exit != 0) printf("[%d] ", last_exit);
        print_prompt();

        int n = read(line, CMD_MAX-1);
        if (n <= 0) continue;
        line[n] = 0;

        int len = (int)strlen(line);
        while (len>0 && (line[len-1]=='\n'||line[len-1]==' '||line[len-1]=='\r'))
            line[--len]=0;

        if (!*line) continue;

        if (line[0]=='!') {
            if (line[1]=='!' && line[2]==0) {
                if (!hist_count) { fputs("kush: no history\n"); continue; }
                strcpy(line, history[hist_count-1]);
                printf("%s\n", line);
            } else if (line[1]>='0'&&line[1]<='9') {
                int idx = atoi(line+1);
                if (idx<1 || idx>hist_count) { printf("kush: !%d: event not found\n", idx); continue; }
                strcpy(line, history[idx-1]);
                printf("%s\n", line);
            }
        }

        hist_add(line);
        last_exit = run_pipeline(line);
    }
}