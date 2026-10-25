#include "keyboard.h"
#include "idt.h"
#include "vga.h"
#include "kstring.h"
#include "gui.h"
#include "irq.h"
#include "sched.h"
#include "signal.h"
#include <stdint.h>

#define KB_DATA     0x60
#define KB_STATUS   0x64
#define KB_CMD      0x64
#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC_EOI     0x20

#define KB_OBF      0x01
#define KB_IBF      0x02
#define KB_AUX      0x20

#define KB_BUF  256
static volatile char buf[KB_BUF];
static volatile int  bhead = 0;
static volatile int  btail = 0;

static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v;
}
static inline void io_wait(void) { outb(0x80,0); outb(0x80,0); }

static int wait_obf(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(KB_STATUS) & KB_OBF) return 1;
        io_wait();
    }
    return 0;
}

static int wait_ibf_clear(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(KB_STATUS) & KB_IBF)) return 1;
        io_wait();
    }
    return 0;
}
static void flush(void) {
    for (int i = 0; i < 16 && (inb(KB_STATUS) & KB_OBF); i++)
        inb(KB_DATA);
}
static void cmd(uint8_t c) { wait_ibf_clear(); outb(KB_CMD, c); io_wait(); }
static void dat(uint8_t d) { wait_ibf_clear(); outb(KB_DATA, d); io_wait(); }
static uint8_t resp(void) { return wait_obf() ? inb(KB_DATA) : 0xFF; }

static const char sc_lo[128] = {
    0, 27,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
static const char sc_hi[128] = {
    0, 27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0,' ',
};

static volatile int shift=0, caps=0, ctrl=0, ext=0, alt=0;

static void process_scancode(uint8_t sc) {
    if (sc == 0xE0 || sc == 0xE1) { ext = 1; return; }

    if (sc & 0x80) {
        sc &= 0x7F;
        if (!ext && (sc==0x2A||sc==0x36)) shift=0;
        if (sc==0x1D) ctrl=0;
        if (sc==0x38) alt=0;
        ext = 0; return;
    }

    if (ext) {
        ext = 0;
        if (sc==0x1D) { ctrl=1; return; }
        if (sc==0x38) { alt=1; return; }
        char special = 0;
        switch (sc) {
            case 0x48: special = KEY_UP;    break;
            case 0x50: special = KEY_DOWN;  break;
            case 0x4B: special = KEY_LEFT;  break;
            case 0x4D: special = KEY_RIGHT; break;
            case 0x47: special = KEY_HOME;  break;
            case 0x4F: special = KEY_END;   break;
            case 0x53: special = KEY_DEL;   break;
            default: return;
        }
        int next = (bhead+1)%KB_BUF;
        if (next != btail) { buf[bhead]=special; bhead=next; }
        return;
    }

    if (sc==0x2A||sc==0x36) { shift=1; return; }
    if (sc==0x3A)            { caps=!caps; return; }
    if (sc==0x1D)            { ctrl=1; return; }
    if (sc==0x38)            { alt=1; return; }
    if (sc>=128)             return;

    char c = (shift && sc < (int)sizeof(sc_hi)) ? sc_hi[sc] : sc_lo[sc];
    if (!c) return;

    if (caps) {
        if (c>='a'&&c<='z') c-=32;
        else if (c>='A'&&c<='Z') c+=32;
    }
    if (ctrl&&c>='a'&&c<='z') c-=96;
    if (ctrl&&c>='A'&&c<='Z') c-=64;

    int next = (bhead+1)%KB_BUF;
    if (next != btail) { buf[bhead]=c; bhead=next; }
}

static void kb_irq(registers_t *r) {
    (void)r;
    uint8_t st = inb(KB_STATUS);
    if (!(st & KB_OBF) || (st & KB_AUX)) return;
    process_scancode(inb(KB_DATA));
}

static char queue_pop(void) {
    if (bhead == btail) return 0;
    char c = buf[btail];
    btail = (btail + 1) % KB_BUF;
    return c;
}

static char poll_once(void) {
    uint32_t f = irq_save();
    uint8_t st = inb(KB_STATUS);
    if ((st & KB_OBF) && !(st & KB_AUX)) process_scancode(inb(KB_DATA));
    char c = queue_pop();
    irq_restore(f);
    return c;
}

void keyboard_inject(const char *s) {
    uint32_t f = irq_save();
    for (; *s; s++) {
        int next = (bhead + 1) % KB_BUF;
        if (next == btail) break;
        buf[bhead] = *s;
        bhead = next;
    }
    irq_restore(f);
}

void keyboard_init(void) {
    bhead = btail = 0;
    shift = caps = ctrl = ext = 0;

    cmd(0xAD); cmd(0xA7);
    flush();

    cmd(0x20);
    uint8_t cfg = resp();
    cfg |=  0x01;
    cfg &= ~0x40;
    cmd(0x60); dat(cfg);

    cmd(0xAE); io_wait();

    flush();
    dat(0xFF);
    uint8_t ack = resp();
    if (ack == 0xFA || ack == 0xAA) {
        uint8_t r2 = resp();
        (void)r2;
    }
    flush();

    dat(0xF0); resp();
    dat(0x01); resp();

    dat(0xF4); resp();
    flush();

    uint8_t mask = inb(PIC1_DATA);
    mask &= ~0x03;
    outb(PIC1_DATA, mask);
    io_wait();

    irq_register(1, kb_irq);
}

char keyboard_getchar(void) {
    return poll_once();
}

char keyboard_getchar_blocking(void) {
    for (;;) {
        signal_check();
        if (!gui_ring3_input_focused()) { __asm__ volatile("sti; hlt"); continue; }

        char c = poll_once();
        if (c) return c;

        __asm__ volatile("sti; hlt");
    }
}

#define KBD_HIST_MAX  16
#define KBD_LINE_MAX  256
static char kbd_hist[KBD_HIST_MAX][KBD_LINE_MAX];
static int  kbd_hist_count = 0;
static int  kbd_hist_pos   = -1;
static char kbd_hist_stash[KBD_LINE_MAX];

static void kbd_hist_add(const char *line) {
    if (!*line) return;
    if (kbd_hist_count > 0 && kstrcmp(kbd_hist[kbd_hist_count-1], line) == 0) { kbd_hist_pos = -1; return; }
    if (kbd_hist_count < KBD_HIST_MAX) {
        kstrcpy(kbd_hist[kbd_hist_count++], line);
    } else {
        for (int i=0;i<KBD_HIST_MAX-1;i++) kstrcpy(kbd_hist[i], kbd_hist[i+1]);
        kstrcpy(kbd_hist[KBD_HIST_MAX-1], line);
    }
    kbd_hist_pos = -1;
}

static const char *kbd_words[] = {
    "help","clear","ls","cat","cd","pwd","echo","date","history","exit",
    "run","exec","kush","pkg","ps","meminfo","vmem","kill","gui","hexdump",
    "write","rm","touch","cp","stat","kum","https","ping","ifconfig","disk",
    "beep","uptime","whoami","reboot","dls","dcat","dwrite","drm","dformat",
    "els","ecat","ewrite","erm","eformat","irqinfo","cpuinfo","serial",
    "proc","wait","banner","calc","motd","snake","logo","uname","hostname",
    "mouse","dcp","edisk","netrecv","dns","dhcp","source","sort","uniq","wc",
    "grep","tar","awk","top","crond","ed","vi","fortune",0
};

static int kbd_tab_complete(char *out, int *len, int *cur) {
    for (int i=0;i<*cur;i++) if (out[i]==' ') return 0;
    int wlen = *cur;
    const char *match = 0;
    int nmatch = 0;
    for (int k=0; kbd_words[k]; k++) {
        const char *w = kbd_words[k];
        int ok = 1;
        for (int i=0;i<wlen;i++) if (w[i]!=out[i]) { ok=0; break; }
        if (ok && (int)kstrlen(w) > wlen) { match = w; nmatch++; }
        else if (ok && (int)kstrlen(w) == wlen) { nmatch = 0; break; }
    }
    if (nmatch == 1) {
        int wl = (int)kstrlen(match);
        for (int i=0;i<wl;i++) out[i]=match[i];
        *len = wl; *cur = wl; out[*len]=0;
        return 1;
    }
    return 0;
}

static void kbd_redraw(int row, int col, const char *out, int len, int cur) {
    vga_goto(row, col);
    for (int i=0;i<len;i++) vga_putchar(out[i]);
    vga_putchar(' ');
    vga_goto(row, col+cur);
}

int keyboard_getline(char *out, int maxlen) {
    int len = 0, cur = 0;
    out[0] = 0;
    int start_row = vga_get_row(), start_col = vga_get_col();
    if (maxlen > KBD_LINE_MAX) maxlen = KBD_LINE_MAX;

    for (;;) {
        char c = keyboard_getchar_blocking();
        if (!c) continue;

        if (c == '\n') {
            out[len] = 0;
            vga_goto(start_row, start_col+len);
            vga_putchar('\n');
            kbd_hist_add(out);
            return len;
        }
        else if (c == 3) {
            vga_goto(start_row, start_col+len);
            vga_puts("^C\n");
            out[0] = 0;
            signal_send(sched_current()->pid, SIGINT);
            signal_check();
            return 0;
        }
        else if (c == 4 && len == 0) {
            out[0] = 0;
            return -1;
        }
        else if (c == '\b') {
            if (cur > 0) {
                for (int i=cur-1;i<len-1;i++) out[i]=out[i+1];
                cur--; len--; out[len]=0;
                kbd_redraw(start_row, start_col, out, len, cur);
            }
        }
        else if (c == KEY_DEL) {
            if (cur < len) {
                for (int i=cur;i<len-1;i++) out[i]=out[i+1];
                len--; out[len]=0;
                kbd_redraw(start_row, start_col, out, len, cur);
            }
        }
        else if (c == KEY_LEFT)  { if (cur>0) { cur--; vga_goto(start_row, start_col+cur); } }
        else if (c == KEY_RIGHT) { if (cur<len) { cur++; vga_goto(start_row, start_col+cur); } }
        else if (c == KEY_HOME)  { cur = 0; vga_goto(start_row, start_col); }
        else if (c == KEY_END)   { cur = len; vga_goto(start_row, start_col+len); }
        else if (c == KEY_UP) {
            if (kbd_hist_count == 0) continue;
            if (kbd_hist_pos == -1) { out[len]=0; kstrcpy(kbd_hist_stash, out); kbd_hist_pos = kbd_hist_count-1; }
            else if (kbd_hist_pos > 0) kbd_hist_pos--;
            kstrcpy(out, kbd_hist[kbd_hist_pos]);
            len = cur = (int)kstrlen(out);
            kbd_redraw(start_row, start_col, out, len, cur);
        }
        else if (c == KEY_DOWN) {
            if (kbd_hist_pos == -1) continue;
            kbd_hist_pos++;
            if (kbd_hist_pos >= kbd_hist_count) { kbd_hist_pos = -1; kstrcpy(out, kbd_hist_stash); }
            else kstrcpy(out, kbd_hist[kbd_hist_pos]);
            len = cur = (int)kstrlen(out);
            kbd_redraw(start_row, start_col, out, len, cur);
        }
        else if (c == '\t') {
            if (kbd_tab_complete(out, &len, &cur))
                kbd_redraw(start_row, start_col, out, len, cur);
        }

        else if (c == 1)  { cur = 0;   vga_goto(start_row, start_col); }
        else if (c == 5)  { cur = len; vga_goto(start_row, start_col+len); }
        else if (c == 21) { len = cur = 0; out[0] = 0;
            kbd_redraw(start_row, start_col, out, len, cur); }
        else if (c == 11) { out[cur] = 0; len = cur;
            kbd_redraw(start_row, start_col, out, len, cur); }
        else if (c == 23) {
            int end = cur;
            while (cur > 0 && out[cur-1] == ' ') cur--;
            while (cur > 0 && out[cur-1] != ' ') cur--;
            for (int i = cur; i + (end-cur) < len; i++) out[i] = out[i + (end-cur)];
            len -= (end - cur); out[len] = 0;
            kbd_redraw(start_row, start_col, out, len, cur);
        }
        else if (len < maxlen-1 && c >= 32 && (uint8_t)c < 127) {
            for (int i=len;i>cur;i--) out[i]=out[i-1];
            out[cur]=c; len++; cur++; out[len]=0;
            kbd_redraw(start_row, start_col, out, len, cur);
        }
    }
}

int keyboard_ctrl_held(void) { return ctrl; }
int keyboard_alt_held(void)  { return alt; }
int keyboard_has_input(void) { return bhead != btail; }

static int queue_take(char want) {
    uint32_t f = irq_save();
    uint8_t st = inb(KB_STATUS);
    if ((st & KB_OBF) && !(st & KB_AUX)) process_scancode(inb(KB_DATA));
    for (int idx = btail; idx != bhead; idx = (idx+1)%KB_BUF) {
        if (buf[idx] != want) continue;
        for (int j = idx; j != btail; ) {
            int prev = (j - 1 + KB_BUF) % KB_BUF;
            buf[j] = buf[prev];
            j = prev;
        }
        btail = (btail + 1) % KB_BUF;
        irq_restore(f);
        return 1;
    }
    irq_restore(f);
    return 0;
}

int keyboard_check_ctrlc(void) {
    if (!gui_ring3_input_focused()) return 0;
    return queue_take(3);
}

int keyboard_check_ctrlz(void) {
    if (!gui_ring3_input_focused()) return 0;
    return queue_take(26);
}

int keyboard_check_alttab(void) {
    if (!alt) return 0;
    return queue_take('\t');
}
