#include "kumos_libc.h"

#define MAX_LINES  256
#define LINE_LEN   256

static char lines[MAX_LINES][LINE_LEN];
static int  nlines = 0;
static char filename[64];
static int  modified = 0;

static void ed_load(const char *fname) {
    int fd = open(fname);
    if (fd < 0) { printf("New file: %s\n", fname); return; }
    char buf[4096]; int n = fread(fd, buf, sizeof(buf)-1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *p = buf; nlines = 0;
    while (*p && nlines < MAX_LINES) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        strncpy(lines[nlines++], p, LINE_LEN-1);
        p = nl ? nl+1 : p+strlen(p);
    }
    printf("Loaded %d lines from %s\n", nlines, fname);
}

static void ed_save(void) {
    char buf[MAX_LINES * LINE_LEN];
    int pos = 0;
    for (int i = 0; i < nlines; i++) {
        int l = (int)strlen(lines[i]);
        memcpy(buf+pos, lines[i], (size_t)l); pos+=l;
        buf[pos++] = '\n';
    }
    buf[pos] = 0;
    int fd = _syscall(SYS_OPEN, (int)filename, O_WRONLY|O_CREAT|O_TRUNC, 0);
    if (fd < 0) { printf("Error: can't save %s\n", filename); return; }
    _syscall(SYS_FWRITE, fd, (int)buf, pos);
    close(fd);
    printf("Saved %d lines to %s\n", nlines, filename);
    modified = 0;
}

static void ed_list(int from, int to) {
    if (from < 0) from = 0;
    if (to >= nlines) to = nlines-1;
    for (int i = from; i <= to; i++)
        printf("%3d | %s\n", i+1, lines[i]);
}

static void ed_help(void) {
    puts("\n  ed - KumOS line editor");
    puts("  Commands:");
    puts("    l [from] [to]  - list lines");
    puts("    a <text>       - append line");
    puts("    i <n> <text>   - insert at line n");
    puts("    d <n>          - delete line n");
    puts("    c <n> <text>   - change line n");
    puts("    s <n> <from> <to> - substitute in line n");
    puts("    p              - print all");
    puts("    w              - save");
    puts("    q              - quit");
    puts("    wq             - save and quit\n");
}

int main(void) {
    int argc_buf = 0;
    char args[256];
    int n = args_line(args, 256);
    if (n > 0) { args[n]=0; while(n>0&&(args[n-1]=='\n'||args[n-1]==' '))args[--n]=0; }

    if (!args[0]) { puts("Usage: ed <filename>"); return 1; }
    strncpy(filename, args, 63);

    ed_load(filename);
    ed_help();
    (void)argc_buf;

    char line[LINE_LEN];
    for (;;) {
        fputs("ed> ");
        int ln = read(line, LINE_LEN-1);
        if (ln <= 0) continue;
        line[ln]=0;
        while(ln>0&&(line[ln-1]=='\n'||line[ln-1]==' '))line[--ln]=0;
        if (!*line) continue;

        char cmd = line[0];
        char *arg = line[1]==' ' ? line+2 : line+1;

        if (cmd=='q') {
            if (modified) { puts("Unsaved changes. Use 'wq' to save and quit or 'q!' to force."); }
            else break;
        } else if (cmd=='q'&&line[1]=='!') { break;
        } else if (cmd=='w') {
            if (line[1]=='q') { ed_save(); break; }
            ed_save();
        } else if (cmd=='p') {
            ed_list(0, nlines-1);
        } else if (cmd=='l') {
            int from=0, to=nlines-1;
            sscanf(arg, "%d %d", &from, &to);
            from--; to--;
            ed_list(from<0?0:from, to<0?nlines-1:to);
        } else if (cmd=='a') {
            if (nlines < MAX_LINES) {
                strncpy(lines[nlines++], arg, LINE_LEN-1);
                modified=1;
                printf("Appended line %d\n", nlines);
            } else puts("Max lines reached.");
        } else if (cmd=='i') {
            int lno = atoi(arg)-1;
            char *text = strchr(arg,' ');
            if (!text||lno<0||lno>nlines){puts("Bad line number.");continue;}
            text++;
            if (nlines < MAX_LINES) {
                for (int i=nlines;i>lno;i--) memcpy(lines[i],lines[i-1],LINE_LEN);
                strncpy(lines[lno], text, LINE_LEN-1);
                nlines++; modified=1;
                printf("Inserted at line %d\n", lno+1);
            }
        } else if (cmd=='d') {
            int lno = atoi(arg)-1;
            if (lno<0||lno>=nlines){puts("Bad line number.");continue;}
            for (int i=lno;i<nlines-1;i++) memcpy(lines[i],lines[i+1],LINE_LEN);
            lines[--nlines][0]=0; modified=1;
            printf("Deleted line %d\n", lno+1);
        } else if (cmd=='c') {
            int lno = atoi(arg)-1;
            char *text = strchr(arg,' ');
            if (!text||lno<0||lno>=nlines){puts("Bad line number.");continue;}
            text++;
            strncpy(lines[lno], text, LINE_LEN-1);
            modified=1; printf("Changed line %d\n", lno+1);
        } else if (cmd=='s') {
            int lno = atoi(arg)-1;
            if (lno<0||lno>=nlines){puts("Bad line number.");continue;}
            char *p = strchr(arg,' '); if(!p) continue; p++;
            char *q = strchr(p,' ');  if(!q) continue; *q++=0;
            char *from=p, *to=q;
            char *pos = strstr(lines[lno], from);
            if (!pos) { puts("Pattern not found."); continue; }
            char newline[LINE_LEN];
            int prefix = (int)(pos-lines[lno]);
            memcpy(newline, lines[lno], (size_t)prefix);
            newline[prefix]=0;
            strcat(newline, to);
            strcat(newline, pos+strlen(from));
            strncpy(lines[lno], newline, LINE_LEN-1);
            modified=1; printf("Substituted on line %d\n", lno+1);
        } else if (cmd=='?'||cmd=='h') {
            ed_help();
        } else {
            printf("Unknown command '%c'. Type '?' for help.\n", cmd);
        }
    }
    return 0;
}
