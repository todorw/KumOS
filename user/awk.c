#include "kumos_libc.h"

#define AWK_FIELDS  32
#define AWK_LINELEN 512

static char  line[AWK_LINELEN];
static char *fields[AWK_FIELDS];
static int   nfields = 0;
static char  fs_char = ' ';
static int   NR = 0;
static int   NF = 0;

static void split_line(char *ln, char sep) {
    nfields=0; char *p=ln;
    if(sep==' ') {
        while(*p&&nfields<AWK_FIELDS-1) {
            while(*p==' '||*p=='\t') p++;
            if(!*p) break;
            fields[nfields++]=p;
            while(*p&&*p!=' '&&*p!='\t') p++;
            if(*p) *p++=0;
        }
    } else {
        fields[nfields++]=p;
        while(*p) {
            if(*p==sep) { *p++=0; fields[nfields++]=p; }
            else p++;
        }
    }
    NF=nfields;
    fields[nfields]=0;
}

static int eval_cond(const char *cond) {
    if(!*cond||strcmp(cond,"1")==0) return 1;
    if(strcmp(cond,"0")==0) return 0;
    if(strncmp(cond,"NR==",4)==0) return NR==atoi(cond+4);
    if(strncmp(cond,"NR>",3)==0)  return NR>atoi(cond+3);
    if(strncmp(cond,"NR<",3)==0)  return NR<atoi(cond+3);
    if(strncmp(cond,"NF==",4)==0) return NF==atoi(cond+4);
    const char *ops[] = {"==","!=",">=","<=",">","<",0};
    for(int o=0;ops[o];o++) {
        const char *op=strstr(cond,ops[o]);
        if(!op) continue;
        char fld[32]; strncpy(fld,cond,(size_t)(op-cond)); fld[op-cond]=0;
        const char *rhs=op+strlen(ops[o]);
        const char *lhs_val="";
        if(fld[0]=='$') {
            int fi=atoi(fld+1);
            if(fi==0) lhs_val=line;
            else if(fi>0&&fi<=nfields) lhs_val=fields[fi-1];
        }
        int l=atoi(lhs_val), r=atoi(rhs);
        int ls=strcmp(lhs_val,rhs);
        if(strcmp(ops[o],"==")==0) return ls==0||l==r;
        if(strcmp(ops[o],"!=")==0) return ls!=0;
        if(strcmp(ops[o],">=")==0) return l>=r;
        if(strcmp(ops[o],"<=")==0) return l<=r;
        if(strcmp(ops[o],">")==0)  return l>r;
        if(strcmp(ops[o],"<")==0)  return l<r;
    }
    if(strstr(cond,"$0~/")) {
        const char *pat=strstr(cond,"$0~/")+4;
        char pbuf[64]; int pi=0;
        while(*pat&&*pat!='/')  pbuf[pi++]=*pat++;
        pbuf[pi]=0;
        return strstr(line,pbuf)!=0;
    }
    return 1;
}

static void eval_action(const char *action) {
    if(!*action||strcmp(action,"print")==0||strcmp(action,"print $0")==0) {
        puts(line); return;
    }
    if(strncmp(action,"print ",6)==0) {
        const char *p=action+6;
        int first=1;
        while(*p) {
            while(*p==' ')p++;
            if(!*p) break;
            char tok[64]; int ti=0;
            while(*p&&*p!=','&&*p!=' ') tok[ti++]=*p++;
            tok[ti]=0;
            if(!first) putchar(' '); first=0;
            if(tok[0]=='$') {
                int fi=atoi(tok+1);
                if(fi==0) fputs(line);
                else if(fi>0&&fi<=nfields) fputs(fields[fi-1]);
            } else if(tok[0]=='"') {
                for(int i=1;tok[i]&&tok[i]!='"';i++) putchar(tok[i]);
            } else {
                fputs(tok);
            }
            if(*p==',') p++;
        }
        putchar('\n'); return;
    }
    if(strncmp(action,"printf ",7)==0) {
        printf("%s",action+7); return;
    }
}

int main(void) {
    char args[256]; int an=args_line(args,256);
    if(an>0){args[an]=0;while(an>0&&(args[an-1]=='\n'||args[an-1]==' '))args[--an]=0;}
    if(!*args){
        puts("Usage: awk [-F<sep>] '<pattern> {action}'");
        puts("  awk '{print $1}'          print first field");
        puts("  awk -F: '{print $1}'      colon delimiter");
        puts("  awk 'NR>2 {print}'        skip first 2 lines");
        puts("  awk '$0~/foo/ {print $2}' grep+print");
        return 1;
    }

    char *p=args;
    char prog[128]; prog[0]=0;
    while(*p) {
        if(strncmp(p,"-F",2)==0) { fs_char=p[2]; p+=3; while(*p==' ')p++; }
        else if(*p=='\'') {
            p++;
            int pi=0;
            while(*p&&*p!='\''&&pi<127) prog[pi++]=*p++;
            prog[pi]=0;
            if(*p=='\'') p++;
            while(*p==' ')p++;
        } else p++;
    }

    if(!prog[0]) { puts("awk: no program"); return 1; }

    char cond[64]="1"; char action[128]="print";
    char *ob=strchr(prog,'{');
    if(ob) {
        size_t clen=(size_t)(ob-prog);
        strncpy(cond,prog,clen<63?clen:63); cond[clen<63?clen:63]=0;
        while(cond[0]==' '||cond[0]=='\t') memmove(cond,cond+1,strlen(cond));
        int cl=(int)strlen(cond)-1;
        while(cl>=0&&(cond[cl]==' '||cond[cl]=='\t')) cond[cl--]=0;
        char *cb=ob+1; char *ce=strchr(cb,'}');
        if(ce) {
            size_t alen=(size_t)(ce-cb);
            strncpy(action,cb,alen<127?alen:127);
            action[alen<127?alen:127]=0;
            while(action[0]==' ') memmove(action,action+1,strlen(action));
            int al=(int)strlen(action)-1;
            while(al>=0&&(action[al]==' '||action[al]=='\t')) action[al--]=0;
        }
    } else strcpy(action,prog);

    char buf[AWK_LINELEN]; int n;
    static char all[65536]; int total=0;
    while((n=fread(0,all+total,(uint32_t)(sizeof(all)-total-1)))>0) total+=n;
    all[total]=0;

    char *lp=all;
    while(*lp) {
        char *nl=strchr(lp,'\n'); if(nl)*nl=0;
        strncpy(line,lp,AWK_LINELEN-1);
        split_line(line,fs_char);
        NR++;
        if(eval_cond(cond)) eval_action(action);
        lp=nl?nl+1:lp+strlen(lp);
    }
    (void)buf;
    return 0;
}
