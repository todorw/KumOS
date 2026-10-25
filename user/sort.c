#include "kumos_libc.h"

static char buf[32768];
static char *lines[1024];

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char**)a, *(const char**)b);
}
static int cmp_str_r(const void *a, const void *b) {
    return strcmp(*(const char**)b, *(const char**)a);
}

int main(void) {
    char args[64]; int an=args_line(args,64);
    if(an>0){args[an]=0;while(an>0&&(args[an-1]=='\n'||args[an-1]==' '))args[--an]=0;}
    int reverse = (!strcmp(args,"-r"));

    int total=0, n;
    while((n=fread(0,buf+total,(uint32_t)(sizeof(buf)-total-1)))>0) total+=n;
    buf[total]=0;

    int nlines=0; char *p=buf;
    while(*p&&nlines<1024) {
        char *nl=strchr(p,'\n'); if(nl)*nl=0;
        lines[nlines++]=p;
        p=nl?nl+1:p+strlen(p);
    }

    qsort(lines,(size_t)nlines,sizeof(char*),reverse?cmp_str_r:cmp_str);
    for(int i=0;i<nlines;i++) { fputs(lines[i]); putchar('\n'); }
    return 0;
}
