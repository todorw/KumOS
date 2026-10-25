#include "kumos_libc.h"

#define LINES_MAX  512
#define LINE_LEN   256
#define ESC        27
#define CTRL(x)    ((x)&0x1F)

static char  text[LINES_MAX][LINE_LEN];
static int   nlines  = 0;
static int   cy      = 0;
static int   cx      = 0;
static int   topline = 0;
static char  fname[64];
static int   modified = 0;
static int   rows = 22, cols = 78;

typedef enum { MODE_NORMAL, MODE_INSERT, MODE_CMD } mode_t;
static mode_t mode = MODE_NORMAL;
static char   status[128];
static char   cmdbuf[128];
static int    cmdpos = 0;

static void set_status(const char *s) { strncpy(status, s, 127); }

static void screen_clear(void) { fputs("\033[2J\033[H"); }
static void move_to(int r, int c) { printf("\033[%d;%dH", r+1, c+1); }
static void attr_reverse(void)  { fputs("\033[7m"); }
static void attr_reset(void)    { fputs("\033[0m"); }

static void draw_screen(void) {
    screen_clear();
    for (int i = 0; i < rows; i++) {
        move_to(i, 0);
        int ln = topline + i;
        if (ln < nlines) {
            int len = (int)strlen(text[ln]);
            if (len > cols) len = cols;
            for (int j = 0; j < len; j++) putchar(text[ln][j]);
        } else {
            putchar('~');
        }
    }

    move_to(rows, 0);
    attr_reverse();
    char bar[128];
    const char *mname = (mode==MODE_INSERT)?"-- INSERT --":
                        (mode==MODE_CMD)   ?"-- COMMAND --":"";
    snprintf(bar, 127, " %s  %s  L%d/%d  C%d  %s",
             fname[0]?fname:"[No Name]",
             modified?"[+]":"",
             cy+1, nlines, cx+1, mname);
    int blen=(int)strlen(bar);
    fputs(bar);
    for(int i=blen;i<cols+2;i++) putchar(' ');
    attr_reset();

    if (mode == MODE_CMD) {
        move_to(rows+1, 0);
        putchar(':');
        fputs(cmdbuf);
    } else if (status[0]) {
        move_to(rows+1, 0);
        fputs(status);
        status[0]=0;
    }

    int sc = cy - topline;
    move_to(sc, cx);
}

static void load_file(const char *fn) {
    int fd = open(fn);
    if (fd < 0) { nlines=1; text[0][0]=0; return; }
    char buf[32768]; int n=fread(fd,buf,sizeof(buf)-1);
    close(fd); if(n<0)n=0; buf[n]=0;
    nlines=0; char *p=buf;
    while(*p&&nlines<LINES_MAX){
        char *nl=strchr(p,'\n'); if(nl)*nl=0;
        strncpy(text[nlines++],p,LINE_LEN-1);
        p=nl?nl+1:p+strlen(p);
    }
    if(!nlines){nlines=1;text[0][0]=0;}
    snprintf(status,127,"\"%s\" %d lines",fn,nlines);
}

static void save_file(const char *fn) {
    char buf[LINES_MAX*LINE_LEN]; int pos=0;
    for(int i=0;i<nlines;i++){
        int l=(int)strlen(text[i]);
        memcpy(buf+pos,text[i],(size_t)l); pos+=l;
        buf[pos++]='\n';
    }
    int fd=_syscall(SYS_OPEN,(int)fn,O_WRONLY|O_CREAT|O_TRUNC,0);
    if(fd<0){set_status("E: can't write");return;}
    _syscall(SYS_FWRITE,fd,(int)buf,pos);
    close(fd);
    modified=0;
    snprintf(status,127,"Written: %s (%d lines)",fn,nlines);
}

static void insert_char(char c) {
    if(cy>=LINES_MAX) return;
    char *line=text[cy];
    int len=(int)strlen(line);
    if(len>=LINE_LEN-1) return;
    memmove(line+cx+1,line+cx,(size_t)(len-cx+1));
    line[cx++]=c;
    modified=1;
}

static void delete_char(void) {
    char *line=text[cy];
    int len=(int)strlen(line);
    if(cx>=len) return;
    memmove(line+cx,line+cx+1,(size_t)(len-cx));
    modified=1;
}

static void new_line_below(void) {
    if(nlines>=LINES_MAX) return;
    for(int i=nlines;i>cy+1;i--) memcpy(text[i],text[i-1],LINE_LEN);
    text[cy+1][0]=0; nlines++; cy++; cx=0; modified=1;
}

static void join_lines(void) {
    if(cy>=nlines-1) return;
    int len=(int)strlen(text[cy]);
    strncat(text[cy],text[cy+1],LINE_LEN-len-1);
    for(int i=cy+1;i<nlines-1;i++) memcpy(text[i],text[i+1],LINE_LEN);
    text[--nlines][0]=0; modified=1;
}

static void clamp_cursor(void) {
    if(cy<0)cy=0; if(cy>=nlines)cy=nlines-1;
    int len=(int)strlen(text[cy]);
    if(cx<0)cx=0;
    if(mode==MODE_NORMAL&&cx>len-1&&len>0) cx=len-1;
    if(cx<0)cx=0;
    if(topline>cy) topline=cy;
    if(topline<cy-rows+1) topline=cy-rows+1;
    if(topline<0)topline=0;
}

static int handle_cmd(void) {
    if(strcmp(cmdbuf,"q")==0)  { if(modified){set_status("Unsaved! Use :q!");return 0;}return 1;}
    if(strcmp(cmdbuf,"q!")==0) return 1;
    if(strcmp(cmdbuf,"w")==0||strcmp(cmdbuf,"w!")==0) { save_file(fname); return 0; }
    if(strcmp(cmdbuf,"wq")==0||strcmp(cmdbuf,"wq!")==0) { save_file(fname); return 1; }
    char *sp=strchr(cmdbuf,' ');
    if(cmdbuf[0]=='w'&&sp) { save_file(sp+1); return 0; }
    int lno=atoi(cmdbuf);
    if(lno>0){ cy=lno-1; clamp_cursor(); return 0; }
    set_status("Unknown command");
    return 0;
}

static void normal_key(char k) {
    int len=(int)strlen(text[cy]);
    switch(k){
    case 'h': cx--; break;
    case 'l': cx++; break;
    case 'j': cy++; break;
    case 'k': cy--; break;
    case '0': cx=0; break;
    case '$': cx=len>0?len-1:0; break;
    case 'g': cy=0; cx=0; break;
    case 'G': cy=nlines-1; cx=0; break;
    case 'i': mode=MODE_INSERT; break;
    case 'a': if(cx<len)cx++; mode=MODE_INSERT; break;
    case 'A': cx=len; mode=MODE_INSERT; break;
    case 'o': new_line_below(); mode=MODE_INSERT; break;
    case 'O':
        if(nlines<LINES_MAX){
            for(int i=nlines;i>cy;i--) memcpy(text[i],text[i-1],LINE_LEN);
            text[cy][0]=0; nlines++; cx=0; mode=MODE_INSERT; modified=1;
        } break;
    case 'x': delete_char(); break;
    case 'd': join_lines(); break;
    case 'D': text[cy][cx]=0; modified=1; break;
    case ':': mode=MODE_CMD; cmdbuf[0]=0; cmdpos=0; break;
    case '/': mode=MODE_CMD; cmdbuf[0]=0; cmdpos=0; set_status("Search not implemented"); break;
    }
    clamp_cursor();
}

static void insert_key(char k) {
    if(k==ESC){ mode=MODE_NORMAL; if(cx>0)cx--; clamp_cursor(); return; }
    if(k=='\n'||k=='\r'){
        if(nlines<LINES_MAX){
            char rest[LINE_LEN]; strcpy(rest,text[cy]+cx);
            text[cy][cx]=0;
            for(int i=nlines;i>cy+1;i--) memcpy(text[i],text[i-1],LINE_LEN);
            strcpy(text[++cy],rest); nlines++; cx=0; modified=1;
        } return;
    }
    if(k==127||k==8){
        if(cx>0){cx--;delete_char();}
        else if(cy>0){int pl=(int)strlen(text[cy-1]);join_lines();cy--;cx=pl;}
        return;
    }
    if(k>=32&&k<127) insert_char(k);
}

int main(void) {
    char args[64]; int n=args_line(args,64);
    if(n>0){args[n]=0;while(n>0&&(args[n-1]=='\n'||args[n-1]==' '))args[--n]=0;}
    strncpy(fname,args,63);
    if(fname[0]) load_file(fname);
    else { nlines=1; text[0][0]=0; }

    mode=MODE_NORMAL;
    for(;;){
        clamp_cursor();
        draw_screen();
        char k=getkey();
        if(mode==MODE_NORMAL){ normal_key(k); }
        else if(mode==MODE_INSERT){ insert_key(k); }
        else if(mode==MODE_CMD){
            if(k==ESC){ mode=MODE_NORMAL; }
            else if(k=='\n'||k=='\r'){
                mode=MODE_NORMAL;
                if(handle_cmd()) break;
            } else if((k==127||k==8)&&cmdpos>0){
                cmdbuf[--cmdpos]=0;
            } else if(k>=32&&k<127&&cmdpos<126){
                cmdbuf[cmdpos++]=k; cmdbuf[cmdpos]=0;
            }
        }
    }
    screen_clear();
    move_to(0,0);
    return 0;
}
