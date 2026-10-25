#include "kumos_libc.h"

static void count_fd(int fd, const char *name, int show_name,
                     uint32_t *tl, uint32_t *tw, uint32_t *tb) {
    char buf[512]; int n; int in_word=0;
    uint32_t lines=0, words=0, bytes=0;
    while((n=fread(fd,buf,512))>0) {
        bytes+=(uint32_t)n;
        for(int i=0;i<n;i++) {
            if(buf[i]=='\n') lines++;
            if(buf[i]==' '||buf[i]=='\t'||buf[i]=='\n') in_word=0;
            else if(!in_word) { in_word=1; words++; }
        }
    }
    if(show_name) printf("%6u %6u %6u %s\n",lines,words,bytes,name);
    else          printf("%6u %6u %6u\n",lines,words,bytes);
    *tl+=lines; *tw+=words; *tb+=bytes;
}

int main(void) {
    char args[256]; int n=args_line(args,256);
    if(n>0){args[n]=0;while(n>0&&(args[n-1]=='\n'||args[n-1]==' '))args[--n]=0;}
    char *argv[16]; int argc=0; char *p=args;
    while(*p&&argc<15){while(*p==' ')p++;if(!*p)break;argv[argc++]=p;while(*p&&*p!=' ')p++;if(*p)*p++=0;}

    uint32_t tl=0,tw=0,tb=0;
    if(!argc) { count_fd(STDIN_FILENO,"",0,&tl,&tw,&tb); return 0; }
    for(int i=0;i<argc;i++) {
        int fd=open(argv[i]);
        if(fd<0){printf("wc: %s: not found\n",argv[i]);continue;}
        count_fd(fd,argv[i],argc>1,&tl,&tw,&tb); close(fd);
    }
    if(argc>1) printf("%6u %6u %6u total\n",tl,tw,tb);
    return 0;
}
