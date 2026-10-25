#include "kumos_libc.h"

static int match(const char *pat, const char *str) {
    if (!*pat) return 1;
    if (*pat == '*') {
        do { if (match(pat+1, str)) return 1; } while (*str++);
        return 0;
    }
    if (*pat == '?' || *pat == *str)
        return *str ? match(pat+1, str+1) : 0;
    return 0;
}

static int contains(const char *line, const char *pat) {
    int plen = (int)strlen(pat);
    int llen = (int)strlen(line);
    for (int i = 0; i <= llen - plen; i++) {
        int ok = 1;
        for (int j = 0; j < plen; j++) {
            if (line[i+j] != pat[j]) { ok=0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static void grep_stream(const char *pat, int fd, const char *fname,
                        int show_name, int invert, int ignore_case) {
    char buf[4096]; int n;
    char line[256]; int lpos=0;
    int linenum=0, matches=0;

    while ((n=fread(fd,buf+lpos,sizeof(buf)-lpos-1))>0) {
        buf[n+lpos]=0;
        char *p=buf;
        while (*p) {
            char *nl=strchr(p,'\n');
            if (nl) *nl=0;
            strncpy(line,p,255);
            linenum++;
            char cmp_line[256]; strncpy(cmp_line,line,255);
            char cmp_pat[128];  strncpy(cmp_pat,pat,127);
            if (ignore_case) {
                for(int i=0;cmp_line[i];i++) if(cmp_line[i]>='A'&&cmp_line[i]<='Z') cmp_line[i]+=32;
                for(int i=0;cmp_pat[i];i++)  if(cmp_pat[i]>='A'&&cmp_pat[i]<='Z')  cmp_pat[i]+=32;
            }
            int hit = contains(cmp_line, cmp_pat);
            if (invert) hit=!hit;
            if (hit) {
                if (show_name) printf("%s:%d: %s\n", fname, linenum, line);
                else           printf("%s\n", line);
                matches++;
            }
            if (nl) { p=nl+1; } else { p+=strlen(p); break; }
        }
        lpos=0;
    }
    if (!matches && !invert) {}
}

int main(void) {
    char args[256]; int n=args_line(args,256);
    if(n>0){args[n]=0;while(n>0&&(args[n-1]=='\n'||args[n-1]==' '))args[--n]=0;}

    if (!*args) {
        puts("Usage: grep [-i] [-v] <pattern> [file...]");
        puts("  -i  ignore case");
        puts("  -v  invert match");
        return 1;
    }

    char *argv[16]; int argc=0;
    char *p=args;
    while(*p&&argc<15) {
        while(*p==' ')p++;
        if(!*p) break;
        argv[argc++]=p;
        while(*p&&*p!=' ')p++;
        if(*p)*p++=0;
    }
    argv[argc]=0;

    int invert=0, ignore_case=0, ai=0;
    while(ai<argc&&argv[ai][0]=='-') {
        char *f=argv[ai]+1;
        while(*f) { if(*f=='v')invert=1; if(*f=='i')ignore_case=1; f++; }
        ai++;
    }

    if(ai>=argc){puts("grep: need pattern");return 1;}
    char *pat=argv[ai++];

    if(ai>=argc) {
        grep_stream(pat, STDIN_FILENO, "(stdin)", 0, invert, ignore_case);
    } else {
        int multi=(argc-ai)>1;
        while(ai<argc) {
            int fd=open(argv[ai]);
            if(fd<0){printf("grep: %s: not found\n",argv[ai]);ai++;continue;}
            grep_stream(pat,fd,argv[ai],multi,invert,ignore_case);
            close(fd);
            ai++;
        }
    }
    return 0;
}
