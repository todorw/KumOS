
#include <stdint.h>
#include <stddef.h>
#include "vga.h"
#include "keyboard.h"
#include "kstring.h"
#include "kmalloc.h"
#include "fs.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "sched.h"
#include "paging.h"
#include "ata.h"
#include "fat12.h"
#include "syscall.h"
#include "elf.h"
#include "serial.h"
#include "rtc.h"
#include "mouse.h"
#include "gui.h"
#include "vfs.h"
#include "pipe.h"
#include "net.h"
#include "signal.h"
#include "procfs.h"
#include "users.h"
#include "dns.h"
#include "tls.h"
#include "browser.h"
#include "speaker.h"
#include "dmesg.h"
#include "dhcp.h"
#include "ext2.h"
#include "pkg.h"
#include "pkgnet.h"
#include "version.h"
#include "power.h"

#define MULTIBOOT_MAGIC  0x2BADB002
#define MULTIBOOT_FLAG_MEM (1<<0)

typedef struct {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline;
    uint32_t mods_count, mods_addr, syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr;
    uint32_t config_table, boot_loader_name;
} multiboot_info_t;

#define CMD_LEN   256
#define HIST_SIZE  16
static char history[HIST_SIZE][CMD_LEN];
static int  hist_count = 0;
static int  kum_active = 0;
static char hostname[32] = "kumos";
uint32_t total_mem_kb = 0;

static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));
}
static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(port));
}

void machine_reboot(void) {
    __asm__ volatile ("cli");
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++) {}
    outb(0x64, 0xFE);
    __asm__ volatile ("lidt 0\n\t int $0x03");
}

void machine_poweroff(void) {
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
}

static void kprintf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            if      (*fmt == 's') { char *s = __builtin_va_arg(ap,char*); vga_puts(s?s:"(null)"); }
            else if (*fmt == 'd') { int d=__builtin_va_arg(ap,int); if(d<0){vga_putchar('-');d=-d;} vga_put_dec((uint32_t)d); }
            else if (*fmt == 'u') { vga_put_dec(__builtin_va_arg(ap,uint32_t)); }
            else if (*fmt == 'x') { vga_put_hex(__builtin_va_arg(ap,uint32_t)); }
            else if (*fmt == 'c') { vga_putchar((char)__builtin_va_arg(ap,int)); }
            else if (*fmt == '%') { vga_putchar('%'); }
        } else { vga_putchar(*fmt); }
        fmt++;
    }
    __builtin_va_end(ap);
}

/* kprintf above has no width-specifier support (%8u is parsed as an
   unrecognized "%8" - nothing consumed - followed by a literal 'u'), so
   any right-justified numeric column needs this instead. */
static void put_dec_pad(uint32_t v, int width) {
    char buf[12]; kitoa(v, buf, 10);
    int len = (int)kstrlen(buf);
    for (int i=len;i<width;i++) vga_putchar(' ');
    vga_puts(buf);
}

static void print_prompt(void) {
    vga_color accent = kum_active ? VGA_LIGHT_RED : VGA_LIGHT_GREEN;

    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_putchar('(');
    vga_set_color(accent,VGA_BLACK);
    vga_puts(kum_active?"root":"user");
    vga_set_color(VGA_WHITE,VGA_BLACK); vga_putchar('@');
    vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK); vga_puts(hostname);
    vga_set_color(VGA_DARK_GREY,VGA_BLACK); vga_putchar(')');
    vga_putchar(' ');
    vga_set_color(VGA_YELLOW,VGA_BLACK); vga_putchar('~');
    vga_putchar('\n');

    vga_set_color(accent,VGA_BLACK);
    vga_putchar(kum_active?'#':'$');
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_putchar(' ');
}

static void hist_add(const char *cmd) {
    if (!*cmd) return;
    if (hist_count < HIST_SIZE) kstrcpy(history[hist_count++], cmd);
    else {
        for (int i=0;i<HIST_SIZE-1;i++) kstrcpy(history[i],history[i+1]);
        kstrcpy(history[HIST_SIZE-1], cmd);
    }
}

static void split_cmd(const char *line, char *cmd, char *rest, int clen) {
    int i=0;
    while(*line&&*line!=' '&&i<clen-1) cmd[i++]=*line++;
    cmd[i]=0; while(*line==' ')line++;
    kstrcpy(rest,line);
}

static int calc_parse(const char *e) {
    int a=0,b=0,neg=0; const char *p=e;
    while(*p==' ')p++; if(*p=='-'){neg=1;p++;}
    while(*p>='0'&&*p<='9'){a=a*10+(*p-'0');p++;} if(neg)a=-a;
    while(*p==' ')p++; if(!*p)return a;
    char op=*p++; while(*p==' ')p++;
    neg=0; if(*p=='-'){neg=1;p++;}
    while(*p>='0'&&*p<='9'){b=b*10+(*p-'0');p++;} if(neg)b=-b;
    switch(op){case '+':return a+b;case '-':return a-b;
               case '*':return a*b;case '/':return b?a/b:0;
               case '%':return b?a%b:0;} return 0;
}

static void vga_puts_glyph(const char *s, vga_color fg) {
    vga_set_color(fg, VGA_BLACK);
    while (*s) {
        vga_putchar(*s == '#' ? (char)VGA_CH_BLOCK : *s);
        s++;
    }
}

static void draw_welcome_logo(void) {
    vga_puts_glyph("              ##    ## ##     ## ##     ##  #######   ######  \n", VGA_LIGHT_CYAN);
    vga_puts_glyph("              ##   ##  ##     ## ###   ### ##     ## ##    ## \n", VGA_LIGHT_CYAN);
    vga_puts_glyph("              ##  ##   ##     ## #### #### ##     ## ##       \n", VGA_CYAN);
    vga_puts_glyph("              #####    ##     ## ## ### ## ##     ##  ######  \n", VGA_CYAN);
    vga_puts_glyph("              ##  ##   ##     ## ##     ## ##     ##       ## \n", VGA_CYAN);
    vga_puts_glyph("              ##   ##  ##     ## ##     ## ##     ## ##    ## \n", VGA_CYAN);
    vga_puts_glyph("              ##    ##  #######  ##     ##  #######   ######  \n", VGA_DARK_GREY);
    vga_putchar('\n');
}

static void cmd_logo(void) {
    vga_set_color(VGA_CYAN,VGA_BLACK);
    vga_puts("\n  ##    ## ##     ## ##     ##  #######   ######  \n");
    vga_puts("  ##   ##  ##     ## ###   ### ##     ## ##    ## \n");
    vga_puts("  ##  ##   ##     ## #### #### ##     ## ##       \n");
    vga_puts("  #####    ##     ## ## ### ## ##     ##  ######  \n");
    vga_puts("  ##  ##   ##     ## ##     ## ##     ##       ## \n");
    vga_puts("  ##   ##  ##     ## ##     ## ##     ## ##    ## \n");
    vga_puts("  ##    ##  #######  ##     ##  #######   ######  \n");
    vga_set_color(VGA_YELLOW,VGA_BLACK);
    vga_puts("       Minimal x86 OS  v" KUMOS_VERSION "\n\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
}

static void cmd_meminfo(void) {
    vga_set_color(VGA_YELLOW,VGA_BLACK);
    vga_puts("\n  === Memory ===\n\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    kprintf("  Total RAM:       %u KB (%u MB)\n",total_mem_kb,total_mem_kb/1024);
    kprintf("  Phys frames:     %u total  %u used  %u free\n",
            pmm_total(), pmm_used(), pmm_total()-pmm_used());
    kprintf("  Demand faults:   %u handled  %u COW copies\n",
            demand_fault_count(), demand_cow_count());
    vga_puts("\n");
    kprintf("  Kernel heap:     %u used / %u free  (base=0x%x)\n",
            kmalloc_used(), kmalloc_free(), kmalloc_base());
    vga_puts("  Paging:          ENABLED  (CR0.PG=1)\n\n");
}

static void cmd_vmem(void) {
    vga_set_color(VGA_YELLOW,VGA_BLACK);
    vga_puts("\n  === Virtual Memory Map ===\n\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("  Region              Start        End          Info\n");
    vga_puts("  ------              -----        ---          ----\n");
    vga_puts("  BIOS/IVT            0x00000000   0x000FFFFF   identity\n");
    vga_puts("  VGA framebuf        0x000B8000   0x000BFFFF   identity\n");
    vga_puts("  Kernel image        0x00100000   0x003FFFFF   identity R/W\n");
    vga_puts("  User programs       0x00400000   0x007FFFFF   per process\n");
    kprintf("  Kernel heap         0x%x                identity R/W\n", (uint32_t)kmalloc_base());
    vga_puts("  User stack          0x3FFFC000   0x3FFFFFFF   per process\n\n");

    vga_set_color(VGA_CYAN,VGA_BLACK);
    vga_puts("  Live mapped ranges (first 16MB):\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    paging_dump_range(0x00000000, 0x01000000);

    vga_set_color(VGA_YELLOW,VGA_BLACK);
    vga_puts("\n  Demand paging stats:\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    kprintf("    Page faults handled: %u\n", demand_fault_count());
    kprintf("    COW copies made:     %u\n", demand_cow_count());
    kprintf("    PMM frames used:     %u / %u\n", pmm_used(), pmm_total());
    vga_putchar('\n');
}

static void cmd_https(const char *args) {
    char hostport[64], path[192];
    split_cmd(args, hostport, path, 64);
    if (!*hostport) { vga_puts("Usage: https <host>[:port] [path]\n"); return; }
    if (!*path) kstrcpy(path, "/");

    uint32_t ip; uint16_t port = 443;
    char http_host[64];   /* goes in the HTTP Host: header either way */
    char sni_host[64];    /* SNI extension - empty for a raw IP target */

    /* a bare dotted IP (with an optional :port) skips DNS entirely - lets
       you point at a repo/test server directly, e.g. "https 10.0.2.2:8443" */
    if (pkgnet_parse_addr(hostport, &ip, &port) == 0) {
        if (kstrchr(hostport, ':') == 0) port = 443;   /* no :port given, don't inherit pkgnet's default 8080 */
        kprintf("  -> %u.%u.%u.%u:%u (no DNS needed)\n",
                (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF, ip&0xFF, port);
        kstrcpy(http_host, hostport);
        sni_host[0] = 0;
    } else {
        char *colon = kstrchr(hostport, ':');
        if (colon) {
            int n = (int)(colon - hostport);
            for (int i = 0; i < n && i < 63; i++) http_host[i] = hostport[i];
            http_host[n < 63 ? n : 63] = 0;
            uint32_t p = 0; const char *pp = colon + 1;
            while (*pp >= '0' && *pp <= '9') { p = p*10 + (uint32_t)(*pp - '0'); pp++; }
            if (p > 0 && p <= 65535) port = (uint16_t)p;
        } else {
            kstrcpy(http_host, hostport);
        }

        vga_puts("Resolving "); vga_puts(http_host); vga_puts("...\n");
        ip = dns_resolve(http_host);
        if (!ip) { vga_puts("https: DNS resolution failed\n"); return; }
        kprintf("  -> %u.%u.%u.%u:%u\n", (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF, ip&0xFF, port);
        kstrcpy(sni_host, http_host);
    }

    vga_puts("TLS handshake (TLS_RSA_WITH_AES_128_CBC_SHA)...\n");
    /* tls_conn_t is ~16KB (dominated by its receive buffer) - way too big
       for a kernel stack frame (SCHED_STACK_SIZE is 4KB), so it's heap
       allocated rather than a local. */
    tls_conn_t *tls = (tls_conn_t*)kmalloc(sizeof(tls_conn_t));
    if (!tls) { vga_puts("https: out of memory\n"); return; }
    if (tls_connect(tls, ip, port, sni_host) != 0) {
        vga_puts("https: TLS handshake failed (no cert-chain validation is done here -\n");
        vga_puts("       real encryption, not real authentication - see README)\n");
        kfree(tls);
        return;
    }
    vga_puts("Connected.\n\n");

    char req[256];
    kstrcpy(req, "GET "); kstrcat(req, path);
    kstrcat(req, " HTTP/1.0\r\nHost: "); kstrcat(req, http_host);
    kstrcat(req, "\r\nConnection: close\r\n\r\n");
    tls_send(tls, req, (uint32_t)kstrlen(req));

    char buf[2048];
    for (;;) {
        int n = tls_recv(tls, buf, sizeof(buf)-1);
        if (n <= 0) break;
        buf[n] = 0;
        vga_puts(buf);
    }
    vga_putchar('\n');
    tls_close(tls);
    kfree(tls);
}

/* CLI front-end for browser.c's fetch+HTML-to-text pass (the same code
   the GUI's kubrowser window uses) - a real command in its own right for
   a quick page-to-text read without opening the GUI, not just a test
   hook, though it did double as the fastest way to verify the HTML
   parser without fighting QEMU's mouse automation. */
static void cmd_fetch(const char *args) {
    if (!*args) { vga_puts("Usage: fetch <url>\n"); return; }
    vga_puts("Fetching...\n");
    int r = browser_navigate(args);
    kprintf("%s: %s\n\n", browser_status(), browser_title());
    if (r == 0) {
        int n = browser_line_count();
        for (int i = 0; i < n; i++) { vga_puts(browser_line_at(i)); vga_putchar('\n'); }
        kprintf("\n(%d lines)\n", n);
    }
}

static int pkg_str_contains(const char *hay, const char *needle) {
    size_t hl = kstrlen(hay), nl = kstrlen(needle);
    if (!nl) return 1;
    if (nl > hl) return 0;
    for (size_t i=0;i+nl<=hl;i++) {
        size_t j=0;
        while (j<nl && hay[i+j]==needle[j]) j++;
        if (j==nl) return 1;
    }
    return 0;
}

static void cmd_pkg(const char *args) {
    char sub[32], rest2[CMD_LEN];
    split_cmd(args, sub, rest2, 32);

    if (!*sub || kstrcmp(sub,"list")==0) {
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("\n  === Packages ===\n\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        for (int i=0;i<pkg_count();i++) {
            vga_puts("  ");
            vga_puts(pkg_is_installed(i) ? "[x] " : "[ ] ");
            vga_puts(pkg_name_at(i));
            int pad = 10 - (int)kstrlen(pkg_name_at(i));
            for (int p=0;p<pad;p++) vga_putchar(' ');
            vga_puts(pkg_desc_at(i));
            vga_putchar('\n');
        }
        if (pkgnet_count() > 0) {
            vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
            vga_puts("\n  === Network repo ===\n\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            for (int i=0;i<pkgnet_count();i++) {
                vga_puts("  [ ] ");
                vga_puts(pkgnet_name_at(i));
                int pad = 10 - (int)kstrlen(pkgnet_name_at(i));
                for (int p=0;p<pad;p++) vga_putchar(' ');
                vga_puts(pkgnet_desc_at(i));
                vga_putchar('\n');
            }
        }
        vga_putchar('\n');
    } else if (kstrcmp(sub,"update")==0) {
        vga_puts("pkg: fetching index from repo...\n");
        int n = pkgnet_update();
        if (n < 0) vga_puts("pkg: update failed (no network, or repo unreachable)\n");
        else kprintf("pkg: %d package(s) available from repo\n", n);
    } else if (kstrcmp(sub,"repo")==0) {
        if (!*rest2) {
            uint32_t ip = pkgnet_repo_ip();
            kprintf("pkg: repo is %u.%u.%u.%u:%u\n",
                    (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF, ip&0xFF, pkgnet_repo_port());
        } else {
            uint32_t ip; uint16_t port;
            if (pkgnet_parse_addr(rest2, &ip, &port) == 0) {
                pkgnet_set_repo(ip, port);
                vga_puts("pkg: repo updated (run 'pkg update' to refresh)\n");
            } else vga_puts("Usage: pkg repo <ip[:port]>\n");
        }
    } else if (kstrcmp(sub,"search")==0) {
        if (!*rest2) { vga_puts("Usage: pkg search <term>\n"); return; }
        int found = 0;
        for (int i=0;i<pkg_count();i++) {
            if (pkg_str_contains(pkg_name_at(i),rest2) || pkg_str_contains(pkg_desc_at(i),rest2)) {
                kprintf("  %s - %s\n", pkg_name_at(i), pkg_desc_at(i));
                found = 1;
            }
        }
        for (int i=0;i<pkgnet_count();i++) {
            if (pkg_str_contains(pkgnet_name_at(i),rest2) || pkg_str_contains(pkgnet_desc_at(i),rest2)) {
                kprintf("  %s - %s (net)\n", pkgnet_name_at(i), pkgnet_desc_at(i));
                found = 1;
            }
        }
        if (!found) vga_puts("pkg: no matches\n");
    } else if (kstrcmp(sub,"install")==0) {
        if (!*rest2) { vga_puts("Usage: pkg install <name>\n"); return; }
        int r = pkg_install(rest2);
        if (r==0) { kprintf("Installed '%s' -> disk\n", rest2); return; }
        if (r==-2) { vga_puts("pkg: no disk mounted (run dformat first)\n"); return; }
        if (r!=-1) { vga_puts("pkg: install failed (disk full?)\n"); return; }

        r = pkgnet_install(rest2);
        if (r==0) kprintf("Installed '%s' -> disk (via network)\n", rest2);
        else if (r==-1) kprintf("pkg: unknown package '%s' (try 'pkg update' first)\n", rest2);
        else if (r==-2) vga_puts("pkg: no disk mounted (run dformat first)\n");
        else if (r==-3) vga_puts("pkg: download failed (no network, or repo unreachable)\n");
        else vga_puts("pkg: install failed (disk full?)\n");
    } else if (kstrcmp(sub,"remove")==0) {
        if (!*rest2) { vga_puts("Usage: pkg remove <name>\n"); return; }
        int r = pkg_remove(rest2);
        if (r==0) kprintf("Removed '%s'\n", rest2);
        else if (r==-1) kprintf("pkg: unknown package '%s'\n", rest2);
        else if (r==-2) vga_puts("pkg: no disk mounted\n");
        else vga_puts("pkg: built-in programs are part of the system\n");
    } else if (kstrcmp(sub,"info")==0) {
        int found=0;
        for (int i=0;i<pkg_count();i++) {
            if (kstrcmp(pkg_name_at(i), rest2)==0) {
                found=1;
                kprintf("  %s -> %s\n", pkg_name_at(i), pkg_fname_at(i));
                kprintf("  %s\n", pkg_desc_at(i));
                vga_puts(pkg_is_installed(i) ? "  Installed\n" : "  Not installed\n");
            }
        }
        for (int i=0;i<pkgnet_count();i++) {
            if (kstrcmp(pkgnet_name_at(i), rest2)==0) {
                found=1;
                kprintf("  %s -> %s (network repo)\n", pkgnet_name_at(i), pkgnet_fname_at(i));
                kprintf("  %s\n", pkgnet_desc_at(i));
            }
        }
        if (!found) kprintf("pkg: unknown package '%s'\n", rest2);
    } else {
        vga_puts("  Usage:\n");
        vga_puts("    pkg list             - show available + network packages\n");
        vga_puts("    pkg update           - fetch the network repo's index\n");
        vga_puts("    pkg search <term>    - search name/description\n");
        vga_puts("    pkg install <name>   - install (checks disk repo, then network)\n");
        vga_puts("    pkg remove <name>    - delete package from disk\n");
        vga_puts("    pkg info <name>      - show package details\n");
        vga_puts("    pkg repo [ip[:port]] - show/set the network repo address\n");
    }
}

static void cmd_mmap(const char *args) {
    char sub[32], rest2[CMD_LEN];
    split_cmd(args, sub, rest2, 32);

    if (kstrcmp(sub,"alloc")==0) {

        uint32_t kb = 0;
        const char *p = rest2;
        while(*p>='0'&&*p<='9'){kb=kb*10+(*p-'0');p++;}
        if (!kb) kb = 4;
        void *ptr = vmalloc(kb * 1024);
        if (ptr) {
            kprintf("  vmalloc(%u KB) -> 0x%x\n", kb, (uint32_t)ptr);

            *(volatile uint8_t *)ptr = 0xAB;
            kprintf("  First byte written OK (demand page fired if needed)\n");
        } else {
            vga_puts("  vmalloc failed (OOM or region full)\n");
        }

    } else if (kstrcmp(sub,"free")==0) {

        uint32_t addr = 0;
        const char *p = rest2;
        if (p[0]=='0'&&p[1]=='x') p+=2;
        while((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||(*p>='A'&&*p<='F')) {
            uint8_t d = *p>='a' ? *p-'a'+10 : *p>='A' ? *p-'A'+10 : *p-'0';
            addr = addr*16+d; p++;
        }
        vmfree((void*)addr);
        kprintf("  vmfree(0x%x) done\n", addr);

    } else if (kstrcmp(sub,"cow")==0) {

        uint32_t addr = 0;
        const char *p = rest2;
        if (p[0]=='0'&&p[1]=='x') p+=2;
        while((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||(*p>='A'&&*p<='F')) {
            uint8_t d = *p>='a' ? *p-'a'+10 : *p>='A' ? *p-'A'+10 : *p-'0';
            addr = addr*16+d; p++;
        }
        if (vmalloc_copy_on_write(addr))
            kprintf("  Page 0x%x marked COW (read-only until write)\n", addr);
        else
            vga_puts("  COW failed (page not mapped?)\n");

    } else if (kstrcmp(sub,"query")==0) {

        uint32_t addr = 0;
        const char *p = rest2;
        if (p[0]=='0'&&p[1]=='x') p+=2;
        while((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||(*p>='A'&&*p<='F')) {
            uint8_t d = *p>='a' ? *p-'a'+10 : *p>='A' ? *p-'A'+10 : *p-'0';
            addr = addr*16+d; p++;
        }
        if (paging_is_mapped(addr)) {
            uint32_t phys = paging_virt_to_phys(addr);
            kprintf("  0x%x -> phys 0x%x  refcount=%u\n",
                    addr, phys, pmm_refcount(phys));
        } else {
            kprintf("  0x%x is NOT mapped\n", addr);
        }

    } else {
        vga_puts("  Usage:\n");
        vga_puts("    mmap alloc <kb>       - allocate virtual region\n");
        vga_puts("    mmap free <0xaddr>    - free virtual region\n");
        vga_puts("    mmap cow <0xaddr>     - mark page copy-on-write\n");
        vga_puts("    mmap query <0xaddr>   - query mapping + refcount\n");
    }
}

static void cmd_cpuinfo(void) {
    uint32_t eax,ebx,ecx,edx; char vendor[13];
    __asm__ volatile("cpuid":"=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(0));
    kmemcpy(vendor+0,&ebx,4); kmemcpy(vendor+4,&edx,4);
    kmemcpy(vendor+8,&ecx,4); vendor[12]=0;
    __asm__ volatile("cpuid":"=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(1));
    vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("\n  === CPU ===\n\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    kprintf("  Vendor:   %s\n",vendor);
    kprintf("  Family:   %u  Model: %u  Stepping: %u\n",(eax>>8)&0xF,(eax>>4)&0xF,eax&0xF);
    vga_puts("  Features: ");
    if(edx&(1<<0))  vga_puts("FPU ");  if(edx&(1<<4))  vga_puts("TSC ");
    if(edx&(1<<9))  vga_puts("APIC "); if(edx&(1<<23)) vga_puts("MMX ");
    if(edx&(1<<25)) vga_puts("SSE ");  if(edx&(1<<26)) vga_puts("SSE2 ");
    if(ecx&(1<<0))  vga_puts("SSE3 "); if(ecx&(1<<5))  vga_puts("VMX ");
    vga_puts("\n\n");
}

static void cmd_irqinfo(void) {
    vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("\n  === Interrupt System ===\n\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("  GDT:  6 segments (null/kcode/kdata/ucode/udata/tss)\n");
    vga_puts("  IDT:  256 entries  (INT 0-31=exceptions  32-47=IRQs)\n");
    vga_puts("  PIC:  8259A  Master=INT32-39  Slave=INT40-47\n");
    vga_puts("  IRQ0: PIT timer  100 Hz   ticks=");
    vga_put_dec(timer_ticks()); vga_putchar('\n');
    vga_puts("  IRQ1: PS/2 keyboard  interrupt-driven  buffer=256B\n");
    vga_puts("  Sched: preemptive round-robin  quantum=10 ticks\n\n");

    uint8_t pic_mask = inb(0x21);
    uint8_t kb_stat  = inb(0x64);
    vga_puts("  [kbd] PIC1 mask=");
    vga_put_hex(pic_mask);
    vga_puts("  IRQ1=");
    vga_puts((pic_mask & 0x02) ? "MASKED(bad)" : "unmasked(ok)");
    vga_puts("\n  8042 status=");
    vga_put_hex(kb_stat);
    vga_puts("  mode=hybrid(IRQ+poll)\n\n");
}

static void snake_game(void) {
#define SW 40
#define SH 18
#define SXO 20
#define SYO 4
#define SMAX 200
    vga_clear();
    vga_fill_rect(SXO-1,SYO-1,SW+2,SH+2,' ',VGA_BLACK,VGA_DARK_GREY);
    vga_fill_rect(SXO,SYO,SW,SH,' ',VGA_BLACK,VGA_BLACK);
    vga_puts_at("SNAKE - WASD to move, Q to quit",24,1,VGA_YELLOW,VGA_BLACK);
    vga_puts_at("Score: 0",24,2,VGA_WHITE,VGA_BLACK);

    int sx[SMAX],sy[SMAX],len=3,dir=3,running=1,score=0;
    int fx=30,fy=10;
    for(int i=0;i<len;i++){sx[i]=SXO+SW/2-i;sy[i]=SYO+SH/2;}
    vga_puts_at("*",fx,fy,VGA_RED,VGA_BLACK);

    while(running){
        char c=keyboard_getchar();
        if(c=='w'&&dir!=1)dir=0; if(c=='s'&&dir!=0)dir=1;
        if(c=='a'&&dir!=3)dir=2; if(c=='d'&&dir!=2)dir=3;
        if(c=='q')running=0;

        int nx=sx[0],ny=sy[0];
        if(dir==0)ny--; if(dir==1)ny++;
        if(dir==2)nx--; if(dir==3)nx++;

        if(nx<SXO||nx>=SXO+SW||ny<SYO||ny>=SYO+SH) break;
        for(int i=0;i<len;i++) if(sx[i]==nx&&sy[i]==ny){running=0;break;}
        if(!running) break;

        char blank[2]={' ',0};
        vga_puts_at(blank,sx[len-1],sy[len-1],VGA_BLACK,VGA_BLACK);
        for(int i=len-1;i>0;i--){sx[i]=sx[i-1];sy[i]=sy[i-1];}
        sx[0]=nx; sy[0]=ny;
        for(int i=0;i<len;i++)
            vga_puts_at(i==0?"O":"o",sx[i],sy[i],i==0?VGA_GREEN:VGA_LIGHT_GREEN,VGA_BLACK);

        if(nx==fx&&ny==fy){
            score++; if(len<SMAX)len++;
            fx=SXO+((fx*7+3)%SW); fy=SYO+((fy*13+5)%SH);
            vga_puts_at("*",fx,fy,VGA_RED,VGA_BLACK);
            char sb[20]="Score: "; char sn[10]; kitoa(score,sn,10);
            kstrcat(sb,sn); kstrcat(sb,"   ");
            vga_puts_at(sb,24,2,VGA_WHITE,VGA_BLACK);
        }
        timer_sleep(80);
    }
    vga_puts_at("GAME OVER! Press any key...",27,23,VGA_RED,VGA_BLACK);
    keyboard_getchar_blocking();
}

static int cal_day_of_week(int y, int m, int d) {
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y -= 1;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

static int cal_is_leap(int y) { return (y%4==0 && y%100!=0) || y%400==0; }

static void cmd_cal(void) {
    static const char *mnames[] = {"January","February","March","April","May","June",
                                    "July","August","September","October","November","December"};
    static const int mdays[]    = {31,28,31,30,31,30,31,31,30,31,30,31};

    rtc_time_t now = rtc_read();
    int y = now.year, m = now.month, today = now.day;
    int days = mdays[m-1] + ((m==2 && cal_is_leap(y)) ? 1 : 0);
    int start_wd = cal_day_of_week(y, m, 1);

    vga_set_color(VGA_YELLOW,VGA_BLACK);
    char hdr[32]; kstrcpy(hdr, "  "); kstrcat(hdr, mnames[m-1]); kstrcat(hdr, " ");
    char yb[8]; kitoa((uint32_t)y, yb, 10); kstrcat(hdr, yb);
    vga_putchar('\n'); vga_puts(hdr); vga_putchar('\n');
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("  Su Mo Tu We Th Fr Sa\n  ");

    for (int i=0;i<start_wd;i++) vga_puts("   ");
    int col = start_wd;
    for (int d=1; d<=days; d++) {
        if (d == today) vga_set_color(VGA_BLACK,VGA_LIGHT_CYAN);
        if (d < 10) vga_putchar(' ');
        vga_put_dec((uint32_t)d);
        if (d == today) vga_set_color(VGA_WHITE,VGA_BLACK);
        vga_putchar(' ');
        col++;
        if (col == 7) { col = 0; vga_puts("\n  "); }
    }
    vga_puts("\n\n");
}

static void cmd_free(void) {
    uint32_t phys_used_kb = pmm_used() * (PAGE_SIZE/1024);
    uint32_t phys_free_kb = (pmm_total()-pmm_used()) * (PAGE_SIZE/1024);
    uint32_t heap_used_kb = kmalloc_used() / 1024;

    vga_set_color(VGA_YELLOW,VGA_BLACK);
    vga_puts("\n  === Memory ===\n\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("                total       used       free\n");
    vga_puts("  Mem:      "); put_dec_pad(total_mem_kb,8); vga_puts("K  ");
                              put_dec_pad(phys_used_kb,8); vga_puts("K  ");
                              put_dec_pad(phys_free_kb,8); vga_puts("K\n");
    vga_puts("  Heap:     "); put_dec_pad((kmalloc_used()+kmalloc_free())/1024,8); vga_puts("K  ");
                              put_dec_pad(heap_used_kb,8); vga_puts("K  ");
                              put_dec_pad(kmalloc_free()/1024,8); vga_puts("K\n");
    vga_putchar('\n');
}

static void df_row(const char *name, const char *mnt, uint32_t total_kb, uint32_t free_kb) {
    uint32_t used_kb = total_kb > free_kb ? total_kb - free_kb : 0;
    uint32_t pct = total_kb ? (used_kb * 100) / total_kb : 0;
    vga_puts("  "); vga_puts(name);
    int pad = 8 - (int)kstrlen(name);
    for (int p=0;p<pad;p++) vga_putchar(' ');
    put_dec_pad(total_kb,8); vga_puts("K  ");
    put_dec_pad(used_kb,8);  vga_puts("K  ");
    put_dec_pad(free_kb,8);  vga_puts("K  ");
    put_dec_pad(pct,3);      vga_puts("%  ");
    vga_puts(mnt); vga_putchar('\n');
}

static void cmd_df(void) {
    vga_set_color(VGA_YELLOW,VGA_BLACK);
    vga_puts("\n  === Disk Usage ===\n\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("  Filesystem     Total       Used       Free  Use%  Mounted\n");
    vga_puts("  ----------  --------   --------   --------  ----  -------\n");
    uint32_t total_kb, free_kb;
    if (fat12_mounted() && fat12_space(&total_kb,&free_kb)==0)
        df_row("fat12", "/disk", total_kb, free_kb);
    else
        vga_puts("  fat12       (not mounted)\n");
    if (ext2_mounted() && ext2_space(&total_kb,&free_kb)==0)
        df_row("ext2", "/ext2", total_kb, free_kb);
    else
        vga_puts("  ext2        (not mounted)\n");
    vga_putchar('\n');
}

static void cmd_disk(void) {
    ata_print_info();
    fat12_info();
}

static void cmd_dls(void) {
    if (!fat12_mounted()) {
        vga_puts("  No disk mounted. Run 'dformat' first or attach a disk.\n");
        return;
    }
    fat12_entry_t entries[64];
    int n = fat12_list(entries, 64);
    if (n <= 0) { vga_puts("  (empty)\n"); return; }
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("\n  NAME             SIZE\n");
    vga_puts("  ----             ----\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    for (int i = 0; i < n; i++) {
        vga_puts("  ");
        vga_puts(entries[i].name);
        int pad = 17 - (int)kstrlen(entries[i].name);
        for (int p = 0; p < pad; p++) vga_putchar(' ');
        vga_put_dec(entries[i].size);
        vga_puts(" B\n");
    }
    vga_putchar('\n');
}

static void cmd_dcat(const char *name) {
    if (!*name) { vga_puts("Usage: dcat <filename>\n"); return; }
    if (!fat12_mounted()) { vga_puts("No disk mounted.\n"); return; }
    static uint8_t fbuf[4096];
    int n = fat12_read(name, fbuf, sizeof(fbuf)-1);
    if (n < 0) { kprintf("dcat: '%s' not found on disk.\n", name); return; }
    fbuf[n] = 0;
    vga_putchar('\n');
    vga_puts((char *)fbuf);
    vga_putchar('\n');
}

static void cmd_dwrite(const char *args) {
    char fname[32], data[CMD_LEN];
    split_cmd(args, fname, data, 32);
    if (!*fname || !*data) {
        vga_puts("Usage: dwrite <filename> <content>\n"); return;
    }
    if (!fat12_mounted()) { vga_puts("No disk mounted.\n"); return; }
    if (fat12_write(fname, data, kstrlen(data)) == 0)
        kprintf("Written '%s' (%u bytes) to disk.\n", fname, kstrlen(data));
    else
        vga_puts("dwrite: failed (disk full or I/O error)\n");
}

static void cmd_drm(const char *name) {
    if (!*name) { vga_puts("Usage: drm <filename>\n"); return; }
    if (!fat12_mounted()) { vga_puts("No disk mounted.\n"); return; }
    if (!kum_active) { vga_puts("Permission denied. Use 'kum drm <file>'\n"); return; }
    if (fat12_delete(name) == 0)
        kprintf("Deleted '%s' from disk.\n", name);
    else
        kprintf("drm: '%s' not found.\n", name);
}

static void cmd_dformat(void) {
    if (!kum_active) {
        vga_puts("Permission denied. Use 'kum dformat'\n"); return;
    }
    if (ata_get(0) == 0) {
        vga_puts("dformat: no ATA drive found.\n"); return;
    }
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("Formatting drive 0 with FAT12 (label: KUMOS)...\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    if (fat12_format(0, "KUMOS") == 0) {
        vga_puts("Format complete. Drive mounted as FAT12.\n");

        const char *welcome =
            "Welcome to KumOS!\n"
            "This file lives on your ATA disk and survives reboots.\n"
            "Use dls, dcat, dwrite, drm to manage disk files.\n";
        fat12_write("README.TXT", welcome, kstrlen(welcome));
        vga_puts("Created README.TXT on disk.\n");
    } else {
        vga_puts("dformat: failed.\n");
    }
}

static void cmd_dcp(const char *args) {

    char src[32], dst[32];
    split_cmd(args, src, dst, 32);
    if (!*src || !*dst) { vga_puts("Usage: dcp <diskfile> <memfile>\n"); return; }
    if (!fat12_mounted()) { vga_puts("No disk mounted.\n"); return; }
    static uint8_t cbuf[4096];
    int n = fat12_read(src, cbuf, sizeof(cbuf)-1);
    if (n < 0) { kprintf("dcp: '%s' not found on disk.\n", src); return; }
    cbuf[n] = 0;
    fs_write(dst, (char *)cbuf);
    kprintf("Copied disk:'%s' -> mem:'%s' (%d bytes)\n", src, dst, n);
}

static void cmd_edisk(void) {
    ext2_info();
}

static void cmd_els(void) {
    if (!ext2_mounted()) {
        vga_puts("  No EXT2 volume mounted. Run 'eformat' first.\n");
        return;
    }
    static char buf[2048];
    int n = ext2_list_dir("/", buf, sizeof(buf));
    if (n <= 0) { vga_puts("  (empty)\n"); return; }
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("\n  EXT2 /\n\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("  ");
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') vga_puts("\n  "); else vga_putchar(buf[i]);
    }
    vga_putchar('\n');
}

static void cmd_ecat(const char *name) {
    if (!*name) { vga_puts("Usage: ecat <filename>\n"); return; }
    if (!ext2_mounted()) { vga_puts("No EXT2 volume mounted.\n"); return; }
    static uint8_t fbuf[8192];
    int n = ext2_read_file(name, fbuf, sizeof(fbuf)-1);
    if (n < 0) { kprintf("ecat: '%s' not found on ext2.\n", name); return; }
    fbuf[n] = 0;
    vga_putchar('\n');
    vga_puts((char *)fbuf);
    vga_putchar('\n');
}

static void cmd_ewrite(const char *args) {
    char fname[64], data[CMD_LEN];
    split_cmd(args, fname, data, 64);
    if (!*fname || !*data) {
        vga_puts("Usage: ewrite <filename> <content>\n"); return;
    }
    if (!ext2_mounted()) { vga_puts("No EXT2 volume mounted.\n"); return; }
    int n = ext2_write_file(fname, data, kstrlen(data));
    if (n >= 0)
        kprintf("Written '%s' (%u bytes) to ext2.\n", fname, kstrlen(data));
    else
        vga_puts("ewrite: failed (volume full or I/O error)\n");
}

static void cmd_erm(const char *name) {
    if (!*name) { vga_puts("Usage: erm <filename>\n"); return; }
    if (!ext2_mounted()) { vga_puts("No EXT2 volume mounted.\n"); return; }
    if (!kum_active) { vga_puts("Permission denied. Use 'kum erm <file>'\n"); return; }
    if (ext2_delete_file(name) == 0)
        kprintf("Deleted '%s' from ext2.\n", name);
    else
        kprintf("erm: '%s' not found.\n", name);
}

static void cmd_eformat(void) {
    if (!kum_active) {
        vga_puts("Permission denied. Use 'kum eformat'\n"); return;
    }
    if (ata_get(1) == 0) {
        vga_puts("eformat: no second ATA drive found (need a drive-1 image).\n"); return;
    }
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("Formatting drive 1 with EXT2...\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    if (ext2_format(1, 0) == 0) {
        vga_puts("Format complete. Volume mounted at /ext2.\n");
        const char *welcome =
            "Welcome to KumOS!\n"
            "This file lives on the ext2 volume and survives reboots.\n"
            "Use els, ecat, ewrite, erm to manage it.\n";
        ext2_write_file("README.TXT", welcome, kstrlen(welcome));
        vga_puts("Created README.TXT on ext2.\n");
    } else {
        vga_puts("eformat: failed.\n");
    }
}

static void dispatch(const char *line);

static void run_and_wait(const char *fname, const char *name) {
    const char *argv[1] = { name };
    int pid = elf_run(fname, 1, argv, -1);
    if (pid < 0) { kprintf("%s: could not start\n", name); return; }
    int code = sched_waitpid(pid);
    __asm__ volatile("sti");
    kprintf("[%s exited with code %d]\n", name, code);
}

static void cmd_help(void) {
    vga_set_color(VGA_CYAN,VGA_BLACK);
    vga_puts("\n === KumOS " KUMOS_VERSION " text console ===\n\n");
    vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("  General:\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("    help, clear, echo, uname, whoami, hostname, uptime, history\n");
    vga_puts("    date              - Current date/time (RTC)\n");
    vga_puts("    cal               - This month's calendar\n");
    vga_puts("    passwd            - Change your password\n");
    vga_puts("    mouse             - PS/2 mouse state\n");
    vga_puts("    beep [hz] [ms]    - PC speaker beep (default 1000hz 200ms)\n");
    vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("  Files:\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("    ls, cat <f>, touch <f>, write <f> <data>, rm <f>\n");
    vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("  System:\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("    ps       - Task list (real scheduler)\n");
    vga_puts("    meminfo  - RAM, frames, heap, demand-paging stats\n");
    vga_puts("    df       - Disk usage summary (fat12 + ext2)\n");
    vga_puts("    free     - Memory summary (Linux-style)\n");
    vga_puts("    vmem     - Virtual memory map + live page table walk\n");
    vga_puts("    mmap     - Map/unmap/COW/query virtual pages\n");
    vga_puts("    pkg      - list/update/search/install/remove packages (net-aware)\n");
    vga_puts("    https    - fetch a page over real TLS 1.2 (no cert validation)\n");
    vga_puts("    cpuinfo  - CPU via CPUID\n");
    vga_puts("    irqinfo  - GDT/IDT/PIC/PIT/Sched live status\n");
    vga_puts("    ifconfig          - NIC info + IP address\n");
    vga_puts("    ping [host]       - Real ICMP echo, RTT + loss stats (default gw)\n");
    vga_puts("    netrecv [port]    - Listen for UDP packet\n");
    vga_puts("    kill <pid> [sig]  - Send signal to process\n");
    vga_puts("    proc [file]       - Read /proc/file (meminfo,ps,uptime...)\n");
    vga_puts("    serial   - COM1 status + test message\n");
    vga_puts("    gui      - Launch graphical desktop (Mode 13h)\n");
    vga_puts("    hexdump <addr> [len] - memory hex dump\n");
    vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("  Disk (FAT12 on ATA):\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("    disk              - ATA drive info + FAT12 volume\n");
    vga_puts("    dls               - List files on disk\n");
    vga_puts("    dcat <f>          - Print file from disk\n");
    vga_puts("    dwrite <f> <data> - Write file to disk\n");
    vga_puts("    drm <f>           - Delete file from disk (kum)\n");
    vga_puts("    dformat           - Format disk FAT12 (kum)\n");
    vga_puts("    dcp <disk> <mem>  - Copy disk file to memory fs\n");
    vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("  Disk (EXT2 on 2nd ATA drive):\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("    edisk             - EXT2 volume info\n");
    vga_puts("    els               - List files on ext2\n");
    vga_puts("    ecat <f>          - Print file from ext2\n");
    vga_puts("    ewrite <f> <data> - Write file to ext2\n");
    vga_puts("    erm <f>           - Delete file from ext2 (kum)\n");
    vga_puts("    eformat           - Format ext2 volume (kum)\n");
    vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("  Userspace (ring 3):\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("    run               - List user programs\n");
    vga_puts("    run hello         - Hello world from ring 3\n");
    vga_puts("    run counter       - Count 0-9 with sleep\n");
    vga_puts("    run top           - Live uptime (5 passes)\n");
    vga_puts("    run echo          - Read + echo a line\n");
    vga_puts("    run cat           - Read README from disk\n");
    vga_puts("    reboot   - Reboot (kum required)\n");
    vga_puts("    shutdown - Halt   (kum required)\n");
    vga_puts("    wait <pid>        - Wait for process to exit\n");
    vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("  KUM:\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("    kum              - Enter privileged session\n");
    vga_puts("    kum exit         - Drop privileges\n");
    vga_puts("    kum <cmd>        - Run one command elevated\n");
    vga_puts("    kum kill <pid>   - Kill a task\n");
    vga_puts("    kum sethost <n>  - Change hostname\n");
    vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("  Fun:\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("    snake, calc <expr>, banner <text>, logo, motd\n\n");
}

static void cmd_kum(const char *args) {
    while(*args==' ')args++;
    if(!*args){ kum_active=1; vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("[kum] Privileged session. 'kum exit' to leave.\n");
        vga_set_color(VGA_WHITE,VGA_BLACK); return; }
    if(kstrcmp(args,"exit")==0){ kum_active=0;
        vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("[kum] Dropped privileges.\n");
        vga_set_color(VGA_WHITE,VGA_BLACK); return; }
    if(kstartswith(args,"kill ")){
        const char *p=args+5; int pid=0;
        while(*p>='0'&&*p<='9'){pid=pid*10+(*p-'0');p++;}
        if(signal_send(pid,SIGKILL)<0){vga_puts("[kum] No such user process.\n");return;}
        kprintf("[kum] Killed PID %d\n",pid); return; }
    if(kstartswith(args,"sethost ")){
        kstrcpy(hostname,args+8);
        kprintf("[kum] Hostname set to '%s'\n",hostname); return; }
    int prev=kum_active; kum_active=1;
    vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("[kum] ");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    dispatch(args); kum_active=prev;
}

static void dispatch(const char *line) {
    while(*line==' ')line++; if(!*line)return;
    char cmd[64],rest[CMD_LEN];
    split_cmd(line,cmd,rest,64);

    if(kstrcmp(cmd,"help")==0){ cmd_help(); }
    else if(kstrcmp(cmd,"clear")==0){ vga_clear(); }
    else if(kstrcmp(cmd,"logo")==0){ cmd_logo(); }
    else if(kstrcmp(cmd,"uname")==0){ vga_puts("KumOS " KUMOS_VERSION " i686\n"); }
    else if(kstrcmp(cmd,"whoami")==0) {
        user_t *u = users_get_current();
        kprintf("%s (uid=%u)\n", u->name, u->uid);
    }
    else if(kstrcmp(cmd,"users")==0) { users_list(); }
    else if(kstrcmp(cmd,"passwd")==0) {
        user_t *u = users_get_current();
        char oldp[32], newp[32], confirm[32];
        vga_puts("Current password: "); keyboard_getline(oldp, 32);
        if (users_check_pass(u->name, oldp) < 0) { vga_puts("passwd: incorrect password\n"); return; }
        vga_puts("New password: "); keyboard_getline(newp, 32);
        vga_puts("Confirm password: "); keyboard_getline(confirm, 32);
        if (kstrcmp(newp, confirm) != 0) { vga_puts("passwd: passwords don't match\n"); return; }
        users_set_pass(u->uid, newp);
        vga_set_color(VGA_GREEN,VGA_BLACK);
        vga_puts("Password updated.\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
    }
    else if(kstrcmp(cmd,"login")==0) {
        char uname[32], upass[32];
        vga_puts("Username: "); keyboard_getline(uname, 32);
        vga_puts("Password: "); keyboard_getline(upass, 32);
        int uid = users_login(uname, upass);
        if (uid >= 0) {
            vga_set_color(VGA_GREEN,VGA_BLACK);
            kprintf("Welcome, %s!\n", uname);
            vga_set_color(VGA_WHITE,VGA_BLACK);
        } else {
            vga_set_color(VGA_RED,VGA_BLACK);
            vga_puts("Login failed.\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
        }
    }
    else if(kstrcmp(cmd,"date")==0) {
        vga_puts("  ");
        rtc_print();
        vga_putchar('\n');
    }
    else if(kstrcmp(cmd,"mouse")==0) {
        mouse_state_t *m = mouse_get();
        if (!mouse_ready()) {
            vga_puts("  Mouse not detected.\n");
        } else {
            vga_set_color(VGA_YELLOW,VGA_BLACK);
            vga_puts("\n  === Mouse State ===\n\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            kprintf("  Position: x=%d  y=%d\n", m->x, m->y);
            kprintf("  Buttons:  L=%d  R=%d  M=%d\n",
                    m->left, m->right, m->middle);
            kprintf("  Last delta: dx=%d  dy=%d\n\n", m->dx, m->dy);
        }
    }
    else if(kstrcmp(cmd,"wait")==0) {
        if (!*rest) { vga_puts("Usage: wait <pid>\n"); }
        else {
            int pid = 0;
            const char *p = rest;
            while(*p>='0'&&*p<='9'){pid=pid*10+(*p-'0');p++;}
            kprintf("Waiting for PID %d...\n", pid);
            int code = sched_waitpid(pid);
            /* sched_waitpid()'s busy-wait loop resumes this task through
               switch_context()'s raw popfd/ret, not an interrupt's iret -
               it only comes back with interrupts on if something further
               up the call chain (a syscall ISR's epilogue, say) eventually
               irets and restores them. Called straight from this bare
               kernel shell (task 0, no such ISR above it), that never
               happens: popfd just replays whatever IF was live at the cli
               right before the last switch, which is always 0 - leaving
               every hlt from here on hanging forever. Every sched_waitpid()
               call site that isn't itself inside an interrupt handler
               needs this same explicit sti. */
            __asm__ volatile("sti");
            kprintf("PID %d exited with code %d\n", pid, code);
        }
    }
    else if(kstrcmp(cmd,"beep")==0) {
        uint32_t freq = 1000, ms = 200;
        const char *p = rest;
        if (*p) {
            freq = 0;
            while (*p>='0'&&*p<='9') { freq = freq*10+(uint32_t)(*p-'0'); p++; }
            while (*p==' ') p++;
            if (*p) {
                ms = 0;
                while (*p>='0'&&*p<='9') { ms = ms*10+(uint32_t)(*p-'0'); p++; }
            }
        }
        speaker_beep(freq, ms);
    }
    else if(kstrcmp(cmd,"hostname")==0){ vga_puts(hostname); vga_putchar('\n'); }
    else if(kstrcmp(cmd,"uptime")==0){
        uint32_t s=timer_seconds(),m=s/60; s%=60; uint32_t h=m/60; m%=60;
        vga_puts("  uptime: ");
        if(h){vga_put_dec(h);vga_puts("h ");}
        if(m){vga_put_dec(m);vga_puts("m ");}
        vga_put_dec(s); vga_puts("s  |  ticks: "); vga_put_dec(timer_ticks());
        vga_puts("  |  100 Hz PIT\n"); }
    else if(kstrcmp(cmd,"echo")==0){ vga_puts(rest); vga_putchar('\n'); }
    else if(kstrcmp(cmd,"ls")==0){ vga_putchar('\n'); fs_list(); vga_putchar('\n'); }
    else if(kstrcmp(cmd,"cat")==0){
        if(!*rest){vga_puts("Usage: cat <file>\n");return;}
        fs_file_t *f=fs_find(rest);
        if(!f){kprintf("cat: %s: No such file\n",rest);return;}
        vga_putchar('\n'); vga_puts(f->data); vga_putchar('\n'); }
    else if(kstrcmp(cmd,"touch")==0){
        if(!*rest){vga_puts("Usage: touch <file>\n");return;}
        if(fs_create(rest,"",0)==0) kprintf("Created '%s'\n",rest);
        else kprintf("touch: '%s' exists or no space\n",rest); }
    else if(kstrcmp(cmd,"write")==0){
        char fn[64],dt[CMD_LEN]; split_cmd(rest,fn,dt,64);
        if(!*fn){vga_puts("Usage: write <file> <data>\n");return;}
        fs_write(fn,dt); kprintf("Written '%s'\n",fn); }
    else if(kstrcmp(cmd,"rm")==0){
        if(!*rest){vga_puts("Usage: rm <file>\n");return;}
        if(!kum_active){vga_puts("Permission denied. Use 'kum rm <file>'\n");return;}
        if(fs_delete(rest)==0) kprintf("Deleted '%s'\n",rest);
        else kprintf("rm: '%s': Not found\n",rest); }
    else if(kstrcmp(cmd,"ps")==0){ vga_putchar('\n'); sched_list(); vga_putchar('\n'); }
    else if(kstrcmp(cmd,"meminfo")==0){ cmd_meminfo(); }
    else if(kstrcmp(cmd,"vmem")==0){ cmd_vmem(); }
    else if(kstrcmp(cmd,"mmap")==0){ cmd_mmap(rest); }
    else if(kstrcmp(cmd,"pkg")==0){ cmd_pkg(rest); }
    else if(kstrcmp(cmd,"https")==0){ cmd_https(rest); }
    else if(kstrcmp(cmd,"fetch")==0){ cmd_fetch(rest); }
    else if(kstrcmp(cmd,"cpuinfo")==0){ cmd_cpuinfo(); }
    else if(kstrcmp(cmd,"irqinfo")==0){ cmd_irqinfo(); }
    else if(kstrcmp(cmd,"ifconfig")==0) {
        net_print_info();
    }
    else if(kstrcmp(cmd,"dmesg")==0) {
        vga_putchar('\n');
        dmesg_print_all();
        vga_putchar('\n');
    }
    else if(kstrcmp(cmd,"dhcp")==0) {
        if (!net_ready()) { vga_puts("No NIC.\n"); }
        else {
            vga_puts("Sending DHCP DISCOVER...\n");
            if (dhcp_request() == 0) {
                uint32_t ip = dhcp_get_ip();
                uint32_t gw = dhcp_get_gateway();
                kprintf("  IP:  %u.%u.%u.%u\n",(ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF);
                kprintf("  GW:  %u.%u.%u.%u\n",(gw>>24)&0xFF,(gw>>16)&0xFF,(gw>>8)&0xFF,gw&0xFF);
                dns_init(dhcp_get_dns() ? dhcp_get_dns() : NET_IP(8,8,8,8));
                vga_puts("  DNS updated.\n");
            } else {
                vga_puts("  DHCP failed.\n");
            }
        }
    }
    else if(kstrcmp(cmd,"dns")==0) {
        if (!*rest) { vga_puts("Usage: dns <hostname>\n"); }
        else {
            if (!net_ready()) { vga_puts("No NIC.\n"); }
            else {
                kprintf("Resolving '%s'...\n", rest);
                uint32_t ip = dns_resolve(rest);
                if (ip) {
                    kprintf("  %s = %u.%u.%u.%u\n", rest,
                        (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF);
                } else {
                    vga_puts("  Resolution failed.\n");
                }
            }
        }
    }
    else if(kstrcmp(cmd,"ping")==0) {
        if (!net_ready()) { vga_puts("No NIC.\n"); }
        else {
            char target[64];
            kstrcpy(target, *rest ? rest : "10.0.2.2");

            uint32_t dst_ip; uint16_t junk_port;
            if (pkgnet_parse_addr(target, &dst_ip, &junk_port) != 0) {
                dst_ip = dns_resolve(target);
                if (!dst_ip) { kprintf("ping: %s: could not resolve\n", target); return; }
            }
            kprintf("PING %s (%u.%u.%u.%u): 32 data bytes\n", target,
                    (dst_ip>>24)&0xFF,(dst_ip>>16)&0xFF,(dst_ip>>8)&0xFF,dst_ip&0xFF);

            int sent=0, recvd=0;
            uint32_t rtt_total=0, rtt_min=0xFFFFFFFF, rtt_max=0;
            uint8_t payload[32];
            for (int i=0;i<32;i++) payload[i]=(uint8_t)i;

            for (int seq=1; seq<=4; seq++) {
                uint32_t t0 = timer_ticks();
                net_send_icmp_echo(dst_ip, 0x4B75, (uint16_t)seq, payload, 32);
                sent++;

                int got=0;
                uint32_t deadline = t0 + 100;
                while (timer_ticks() < deadline) {
                    uint32_t from; uint16_t rid, rseq; uint8_t ttl;
                    if (net_icmp_poll_reply(&from,&rid,&rseq,&ttl) &&
                        from==dst_ip && rid==0x4B75 && rseq==(uint16_t)seq) {
                        uint32_t rtt_ms = (timer_ticks()-t0)*10;
                        kprintf("32 bytes from %u.%u.%u.%u: icmp_seq=%d ttl=%d time=%ums\n",
                                (from>>24)&0xFF,(from>>16)&0xFF,(from>>8)&0xFF,from&0xFF,
                                seq, ttl, rtt_ms);
                        recvd++; got=1;
                        if (rtt_ms<rtt_min) rtt_min=rtt_ms;
                        if (rtt_ms>rtt_max) rtt_max=rtt_ms;
                        rtt_total += rtt_ms;
                        break;
                    }
                }
                if (!got) kprintf("Request timeout for icmp_seq %d\n", seq);
                timer_sleep(50);
            }

            kprintf("\n--- %s ping statistics ---\n", target);
            kprintf("%d packets transmitted, %d received, %d%% packet loss\n",
                    sent, recvd, sent ? (100*(sent-recvd)/sent) : 0);
            if (recvd) kprintf("rtt min/avg/max = %u/%u/%u ms\n", rtt_min, rtt_total/(uint32_t)recvd, rtt_max);
        }
    }
    else if(kstrcmp(cmd,"netrecv")==0) {
        if (!net_ready()) { vga_puts("No NIC.\n"); }
        else {
            uint16_t port = 1234;
            if (*rest) { port=0; const char *p=rest; while(*p>='0'&&*p<='9'){port=(uint16_t)(port*10+(*p-'0'));p++;} }
            kprintf("Listening on UDP port %u (polling 2s)...\n", port);
            uint8_t buf[256]; uint32_t src_ip=0; uint16_t src_port=0;
            uint32_t end = timer_ticks() + 200;
            int got = 0;
            while (timer_ticks() < end) {
                net_poll();
                int n = net_recv_udp(port, buf, 255, &src_ip, &src_port);
                if (n > 0) {
                    buf[n]=0;
                    kprintf("  From %u.%u.%u.%u:%u  \"%s\"\n",
                        (src_ip>>24)&0xFF,(src_ip>>16)&0xFF,(src_ip>>8)&0xFF,src_ip&0xFF,
                        src_port, buf);
                    got=1; break;
                }
            }
            if (!got) vga_puts("  No packets received.\n");
        }
    }
    else if(kstrcmp(cmd,"proc")==0) {
        const char *file = *rest ? rest : "meminfo";
        char path[64]; kstrcpy(path,"/proc/"); kstrcat(path,file);
        int fd = vfs_open(path, 0);
        if (fd < 0) { kprintf("proc: /proc/%s not found\n", file);
                      vga_puts("  Files: meminfo uptime version ps net date\n"); }
        else {
            vga_putchar('\n');
            char buf[512]; int n;
            while ((n=vfs_read(fd,buf,511))>0) { buf[n]=0; vga_puts(buf); }
            vfs_close(fd);
            vga_putchar('\n');
        }
    }
    else if(kstrcmp(cmd,"kill")==0) {
        char arg1[16], arg2[16];
        split_cmd(rest, arg1, arg2, 16);
        int sig=15, pid=0;
        if (*arg2) { const char *p=arg1; while(*p>='0'&&*p<='9'){pid=pid*10+(*p-'0');p++;} sig=0; const char *q=arg2; while(*q>='0'&&*q<='9'){sig=sig*10+(*q-'0');q++;} }
        else { const char *p=arg1; while(*p>='0'&&*p<='9'){pid=pid*10+(*p-'0');p++;} }
        if (!pid) { vga_puts("Usage: kill <pid> [signal]\n"); }
        else { signal_send(pid, sig); kprintf("Sent signal %d to PID %d\n", sig, pid); }
    }
    else if(kstrcmp(cmd,"kush")==0) { run_and_wait("KUSH.ELF", "kush"); }
    else if(kstrcmp(cmd,"gui")==0) {
        vga_set_color(VGA_CYAN,VGA_BLACK);
        vga_puts("Switching to GUI mode (320x200)...\n");
        vga_puts("Press ESC inside GUI to return.\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        timer_sleep(500);
        gui_run();

        vga_clear();
        vga_set_color(VGA_GREEN,VGA_BLACK);
        vga_puts("  Returned from GUI.\n\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
    }
    else if(kstrcmp(cmd,"serial")==0) {
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("\n  === Serial Port (COM1) ===\n\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        vga_puts("  Port:    0x3F8 (COM1)\n");
        vga_puts("  Baud:    115200\n");
        vga_puts("  Format:  8N1\n");
        vga_puts("  Status:  ");
        if (serial_ready()) {
            vga_set_color(VGA_GREEN,VGA_BLACK);
            vga_puts("ONLINE\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            vga_puts("\n  QEMU:  -serial stdio        (live terminal)\n");
            vga_puts("         -serial file:k.log   (capture to file)\n\n");

            serial_printf("\r\n[serial] Test message from KumOS shell at uptime=%us\r\n",
                         timer_seconds());
            vga_puts("  Test message sent to serial.\n\n");
        } else {
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            vga_puts("NOT DETECTED\n\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
        }
    }
    else if(kstrcmp(cmd,"hexdump")==0) {

        if (!*rest) { vga_puts("Usage: hexdump <0xaddr> [len]\n"); }
        else {
            uint32_t addr = 0; uint32_t len = 64;
            const char *p = rest;
            if (p[0]=='0'&&p[1]=='x') p+=2;
            while((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||(*p>='A'&&*p<='F')) {
                uint8_t d=*p>='a'?*p-'a'+10:*p>='A'?*p-'A'+10:*p-'0';
                addr=addr*16+d; p++;
            }
            while(*p==' ')p++;
            if(*p) { len=0; while(*p>='0'&&*p<='9'){len=len*10+(*p-'0');p++;} }
            if(len>512)len=512;
            if(!paging_is_mapped(addr)){
                kprintf("hexdump: 0x%x is not mapped\n",addr);
            } else {
                vga_set_color(VGA_CYAN,VGA_BLACK);
                kprintf("\n  hexdump 0x%x  len=%u\n\n", addr, len);
                vga_set_color(VGA_WHITE,VGA_BLACK);
                serial_hexdump(COM1, (void*)addr, len);

                const uint8_t *bp = (const uint8_t*)addr;
                for(uint32_t i=0;i<len;i+=16){
                    vga_put_hex(addr+i); vga_puts(": ");
                    for(uint32_t j=0;j<16&&i+j<len;j++){
                        char hx[3]; const char *h="0123456789ABCDEF";
                        hx[0]=h[bp[i+j]>>4]; hx[1]=h[bp[i+j]&0xF]; hx[2]=0;
                        vga_puts(hx); vga_putchar(' ');
                    }
                    vga_putchar('\n');
                }
                vga_putchar('\n');
            }
        }
    }
    else if(kstrcmp(cmd,"df")==0)    { cmd_df(); }
    else if(kstrcmp(cmd,"free")==0)  { cmd_free(); }
    else if(kstrcmp(cmd,"cal")==0)   { cmd_cal(); }
    else if(kstrcmp(cmd,"disk")==0)  { cmd_disk(); }
    else if(kstrcmp(cmd,"dls")==0)   { cmd_dls(); }
    else if(kstrcmp(cmd,"dcat")==0)  { cmd_dcat(rest); }
    else if(kstrcmp(cmd,"dwrite")==0){ cmd_dwrite(rest); }
    else if(kstrcmp(cmd,"drm")==0)   { cmd_drm(rest); }
    else if(kstrcmp(cmd,"dformat")==0){ cmd_dformat(); }
    else if(kstrcmp(cmd,"dcp")==0)   { cmd_dcp(rest); }
    else if(kstrcmp(cmd,"edisk")==0) { cmd_edisk(); }
    else if(kstrcmp(cmd,"els")==0)   { cmd_els(); }
    else if(kstrcmp(cmd,"ecat")==0)  { cmd_ecat(rest); }
    else if(kstrcmp(cmd,"ewrite")==0){ cmd_ewrite(rest); }
    else if(kstrcmp(cmd,"erm")==0)   { cmd_erm(rest); }
    else if(kstrcmp(cmd,"eformat")==0){ cmd_eformat(); }
    else if(kstrcmp(cmd,"run")==0) {
        int i = *rest ? pkg_find_index(rest) : -1;
        if (i >= 0) run_and_wait(pkg_fname_at(i), rest);
        else {
            if (*rest) kprintf("run: unknown program '%s'\n", rest);
            vga_puts("  Available programs:");
            for (int j = 0; j < pkg_count(); j++) { vga_putchar(' '); vga_puts(pkg_name_at(j)); }
            vga_putchar('\n');
        }
    }
    else if(kstrcmp(cmd,"history")==0){
        vga_puts("\n  Command history:\n");
        for(int i=0;i<hist_count;i++){vga_puts("  ");vga_put_dec(i+1);vga_puts(". ");vga_puts(history[i]);vga_putchar('\n');}
        vga_putchar('\n'); }
    else if(kstrcmp(cmd,"calc")==0){
        if(!*rest){vga_puts("Usage: calc <expr>  e.g. calc 6 * 7\n");return;}
        kprintf("  = %d\n",calc_parse(rest)); }
    else if(kstrcmp(cmd,"banner")==0){
        if(!*rest){vga_puts("Usage: banner <text>\n");return;}
        vga_set_color(VGA_YELLOW,VGA_BLACK); vga_puts("\n");
        const char *p=rest; while(*p&&p-rest<8){vga_putchar(*p);vga_putchar(*p);vga_putchar(*p);vga_putchar(' ');p++;}
        vga_putchar('\n');
        p=rest; while(*p&&p-rest<8){vga_putchar(*p);vga_putchar(*p);vga_putchar(*p);vga_putchar(' ');p++;}
        vga_puts("\n"); vga_set_color(VGA_WHITE,VGA_BLACK); }
    else if(kstrcmp(cmd,"snake")==0){ snake_game(); vga_clear(); }
    else if(kstrcmp(cmd,"motd")==0){
        fs_file_t *f=fs_find("motd.txt");
        if(f){vga_set_color(VGA_CYAN,VGA_BLACK);vga_puts(f->data);vga_set_color(VGA_WHITE,VGA_BLACK);} }
    else if(kstrcmp(cmd,"kum")==0){ cmd_kum(rest); }
    else if(kstrcmp(cmd,"reboot")==0){
        if(!kum_active){vga_puts("Permission denied. Use 'kum reboot'\n");return;}
        vga_puts("Rebooting...\n"); timer_sleep(300);
        machine_reboot(); }
    else if(kstrcmp(cmd,"shutdown")==0||kstrcmp(cmd,"halt")==0){
        if(!kum_active){vga_puts("Permission denied. Use 'kum shutdown'\n");return;}
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("\nPowering off...\n");
        machine_poweroff();
        vga_puts("System halted. Power off safely.\n");
        __asm__ volatile("cli; hlt"); }
    else if(kstrcmp(cmd,"exec")==0) {
        if (!*rest) vga_puts("Usage: exec <file.elf>\n");
        else run_and_wait(rest, rest);
    }
    else if(kstrcmp(cmd,"elfinfo")==0) {
        if (!*rest) { vga_puts("Usage: elfinfo <file.elf>\n"); }
        else {
            static uint8_t ebuf[65536];
            int n = fat12_mounted() ? fat12_read(rest, ebuf, sizeof(ebuf)) : -1;
            if (n <= 0) {
                fs_file_t *mf = fs_find(rest);
                if (mf) { n=(int)mf->size; kmemcpy(ebuf,mf->data,(uint32_t)n); }
            }
            if (n > 0) elf_print_info(ebuf, (uint32_t)n);
            else kprintf("elfinfo: '%s' not found\n", rest);
        }
    }
    else {
        kprintf("ksh: command not found: %s\n",cmd);
        vga_puts("Type 'help' for available commands.\n");
        speaker_beep(300, 100);
    }
}

void kernel_main(uint32_t magic, multiboot_info_t *mbi) {

    vga_init();
    vga_font_snapshot();
    dmesg_init();
    dmesg_log("KumOS " KUMOS_VERSION " booting");

    serial_init(COM1);
    serial_puts(COM1, "\r\n\r\n");
    serial_puts(COM1, "========================================\r\n");
    serial_puts(COM1, "  KumOS " KUMOS_VERSION " - serial boot log\r\n");
    serial_puts(COM1, "========================================\r\n");

    if (magic == MULTIBOOT_MAGIC && mbi && (mbi->flags & MULTIBOOT_FLAG_MEM))
        total_mem_kb = mbi->mem_lower + mbi->mem_upper;
    else
        total_mem_kb = 32768;
    serial_printf("[boot] RAM: %u KB (%u MB)\r\n", total_mem_kb, total_mem_kb/1024);

    kmalloc_init(KHEAP_BASE, KHEAP_SIZE);
    serial_printf("[boot] Heap @ %x, %u KB\r\n", KHEAP_BASE, KHEAP_SIZE / 1024);

    gdt_init();
    serial_printf("[boot] GDT loaded (6 descriptors)\r\n");

    idt_init();
    serial_printf("[boot] IDT loaded (256 entries), PIC remapped\r\n");

    paging_init(total_mem_kb);
    serial_printf("[boot] Paging enabled, %u frames total\r\n", pmm_total());

    demand_paging_init();
    serial_printf("[boot] Demand paging active (INT14 hooked)\r\n");

    timer_init(100);
    serial_printf("[boot] PIT timer @ 100Hz (IRQ0)\r\n");

    keyboard_init();
    serial_printf("[boot] Keyboard driver active (IRQ1)\r\n");

    fs_init();
    serial_printf("[boot] In-memory FS ready\r\n");

    ata_init();
    fat12_mount(0);
    if (fat12_mounted())
        serial_printf("[boot] FAT12 mounted on ATA drive 0\r\n");
    else
        serial_printf("[boot] No FAT12 disk found\r\n");

    vfs_init();
    proc_fs_init();
    serial_printf("[boot] VFS ready (/mem /disk /dev /proc)\r\n");

    signal_init();
    serial_printf("[boot] Signal subsystem ready\r\n");

    users_init();
    serial_printf("[boot] User accounts ready (root/user/guest)\r\n");

    sched_init();
    serial_printf("[boot] Scheduler ready\r\n");

    syscall_init();
    serial_printf("[boot] Syscall interface ready (INT 0x80)\r\n");

    rtc_init();
    mouse_init();
    serial_printf("[boot] RTC ready  Mouse: %s\r\n",
                  mouse_ready() ? "detected" : "not found");

    if (net_init() == 0) {
        serial_printf("[boot] RTL8139 NIC ready  IP: 10.0.2.15\r\n");
        dmesg_log("net: RTL8139 ready, IP 10.0.2.15");
        dns_init(NET_IP(8,8,8,8));
        dmesg_log("dns: resolver ready (8.8.8.8)");
        if (dhcp_request() == 0) {
            serial_printf("[boot] DHCP: IP obtained\r\n");
            dmesg_log("dhcp: IP obtained");
            dns_init(dhcp_get_dns() ? dhcp_get_dns() : NET_IP(8,8,8,8));
        }
    } else {
        serial_printf("[boot] No NIC found\r\n");
        dmesg_log("net: no NIC found");
    }

    if (ext2_init(1, 0) == 0) {
        vfs_mount("/ext2", &ext2_vfs_ops);
        serial_printf("[boot] EXT2 mounted at /ext2 (drive 1)\r\n");
        dmesg_log("ext2: mounted at /ext2");
    } else {
        serial_printf("[boot] No EXT2 volume on drive 1 (run 'eformat')\r\n");
    }

    serial_printf("[boot] Enabling interrupts...\r\n");
    __asm__ volatile ("sti");

    const char *kush_argv[1] = { "kush" };
    gui_set_terminal_pid(0, elf_run("KUSH.ELF", 1, kush_argv, 0));
    serial_printf("[boot] GUI started\r\n");
    gui_run();

    vga_clear();
    vga_goto(1, 0);
    draw_welcome_logo();
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    kprintf("  Text console  .  %u MB RAM  .  type 'help', or 'gui' for the desktop\n\n", total_mem_kb/1024);
    vga_set_color(VGA_WHITE,VGA_BLACK);

    char line[CMD_LEN];
    while (1) {
        print_prompt();
        keyboard_getline(line, CMD_LEN);
        if (!*line) continue;
        hist_add(line);
        serial_printf("[ksh] %s\r\n", line);
        dispatch(line);
    }
}