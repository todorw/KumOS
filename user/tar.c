#include "kumos_libc.h"

#define TAR_BLOCK  512

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} tar_header_t;

static uint32_t oct2uint(const char *s, int len) {
    uint32_t n=0;
    for(int i=0;i<len&&s[i]>='0'&&s[i]<='7';i++) n=n*8+(s[i]-'0');
    return n;
}

static void uint2oct(uint32_t v, char *s, int len) {
    for(int i=len-2;i>=0;i--){s[i]='0'+(v&7);v>>=3;}
    s[len-1]=0;
}

static uint32_t tar_checksum(tar_header_t *h) {
    uint8_t *p=(uint8_t*)h; uint32_t sum=0;
    for(int i=0;i<TAR_BLOCK;i++) sum+=(i>=148&&i<156)?32:p[i];
    return sum;
}

static int cmd_list(int fd) {
    tar_header_t hdr;
    puts("\n  Files in archive:\n");
    while(1) {
        int n=fread(fd,(void*)&hdr,TAR_BLOCK);
        if(n<TAR_BLOCK||!hdr.name[0]) break;
        uint32_t size=oct2uint(hdr.size,12);
        printf("  %-32s %u bytes\n",hdr.name,size);
        uint32_t blocks=(size+TAR_BLOCK-1)/TAR_BLOCK;
        for(uint32_t i=0;i<blocks;i++) fread(fd,(void*)&hdr,TAR_BLOCK);
    }
    puts("");
    return 0;
}

static int cmd_extract(int fd) {
    tar_header_t hdr; char buf[TAR_BLOCK];
    while(1) {
        int n=fread(fd,(void*)&hdr,TAR_BLOCK);
        if(n<TAR_BLOCK||!hdr.name[0]) break;
        uint32_t size=oct2uint(hdr.size,12);
        printf("  Extracting: %s (%u bytes)\n",hdr.name,size);
        int out=_syscall(SYS_OPEN,(int)hdr.name,O_WRONLY|O_CREAT|O_TRUNC,0);
        uint32_t remaining=size;
        uint32_t blocks=(size+TAR_BLOCK-1)/TAR_BLOCK;
        for(uint32_t i=0;i<blocks;i++) {
            fread(fd,buf,TAR_BLOCK);
            if(out>=0) {
                uint32_t w=remaining>TAR_BLOCK?TAR_BLOCK:remaining;
                _syscall(SYS_FWRITE,out,(int)buf,(int)w);
                remaining-=w;
            }
        }
        if(out>=0) close(out);
    }
    return 0;
}

static int cmd_create(const char *archive, char **files, int nfiles) {
    int out=_syscall(SYS_OPEN,(int)archive,O_WRONLY|O_CREAT|O_TRUNC,0);
    if(out<0){printf("tar: can't create %s\n",archive);return 1;}

    char block[TAR_BLOCK]; char fbuf[512];
    for(int i=0;i<nfiles;i++) {
        int fd=open(files[i]);
        if(fd<0){printf("tar: %s: not found\n",files[i]);continue;}

        uint32_t size=0;
        while(fread(fd,fbuf,sizeof(fbuf))>0) {}

        _syscall(SYS_CLOSE,fd,0,0);
        fd=open(files[i]);

        tar_header_t *hdr=(tar_header_t*)block;
        memset(block,0,TAR_BLOCK);
        strncpy(hdr->name,files[i],99);
        strcpy(hdr->mode,"0000644");
        uint2oct(0,hdr->uid,8); uint2oct(0,hdr->gid,8);

        char tmp[512]; int n=0; uint32_t total=0;
        int fd2=open(files[i]);
        while((n=fread(fd2,tmp,512))>0) total+=(uint32_t)n;
        close(fd2); size=total;

        uint2oct(size,hdr->size,12);
        uint2oct(0,hdr->mtime,12);
        hdr->type='0';
        strcpy(hdr->magic,"ustar");
        uint2oct(tar_checksum(hdr),hdr->checksum,7); hdr->checksum[7]=' ';
        _syscall(SYS_FWRITE,out,(int)block,TAR_BLOCK);

        int rd=open(files[i]); uint32_t written=0;
        while(written<size) {
            memset(block,0,TAR_BLOCK);
            int r=fread(rd,block,(uint32_t)(size-written>TAR_BLOCK?TAR_BLOCK:size-written));
            if(r<=0) break;
            _syscall(SYS_FWRITE,out,(int)block,TAR_BLOCK);
            written+=(uint32_t)r;
        }
        close(rd);
        printf("  Added: %s (%u bytes)\n",files[i],size);
        (void)fd;
    }

    memset(block,0,TAR_BLOCK);
    _syscall(SYS_FWRITE,out,(int)block,TAR_BLOCK);
    _syscall(SYS_FWRITE,out,(int)block,TAR_BLOCK);
    close(out);
    printf("Created: %s\n",archive);
    return 0;
}

int main(void) {
    char args[256]; int n=args_line(args,256);
    if(n>0){args[n]=0;while(n>0&&(args[n-1]=='\n'||args[n-1]==' '))args[--n]=0;}
    if(!*args){
        puts("Usage: tar <c|x|t> <archive.tar> [files...]");
        puts("  c = create  x = extract  t = list");
        return 1;
    }
    char *argv[16]; int argc=0; char *p=args;
    while(*p&&argc<15){while(*p==' ')p++;if(!*p)break;argv[argc++]=p;while(*p&&*p!=' ')p++;if(*p)*p++=0;}

    if(argc<2){puts("tar: need command and archive");return 1;}
    char cmd=argv[0][0];

    if(cmd=='t'||cmd=='x') {
        int fd=open(argv[1]);
        if(fd<0){printf("tar: %s: not found\n",argv[1]);return 1;}
        int r=(cmd=='t')?cmd_list(fd):cmd_extract(fd);
        close(fd); return r;
    } else if(cmd=='c') {
        if(argc<3){puts("tar: need files to add");return 1;}
        return cmd_create(argv[1],argv+2,argc-2);
    }
    printf("tar: unknown command '%c'\n",cmd);
    return 1;
}
