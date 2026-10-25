#include "kumos_libc.h"

int main(void) {
    static char buf[32768]; int total=0, n;
    while((n=fread(0,buf+total,(uint32_t)(sizeof(buf)-total-1)))>0) total+=n;
    buf[total]=0;
    char prev[512]; prev[0]=0;
    char *p=buf;
    while(*p) {
        char *nl=strchr(p,'\n'); if(nl)*nl=0;
        if(strcmp(p,prev)!=0) { fputs(p); putchar('\n'); strncpy(prev,p,511); }
        p=nl?nl+1:p+strlen(p);
    }
    return 0;
}
