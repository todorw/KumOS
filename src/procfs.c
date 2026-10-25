#include "vfs.h"
#include "sched.h"
#include "paging.h"
#include "kmalloc.h"
#include "timer.h"
#include "rtc.h"
#include "net.h"
#include "vga.h"
#include "dmesg.h"
#include "kstring.h"
#include "version.h"
extern uint32_t total_mem_kb;
#include <stdint.h>



static char proc_buf[2048];

static void uint_to_str(uint32_t n, char *buf) {
    if (!n) { buf[0]='0'; buf[1]=0; return; }
    char tmp[12]; int i=10; tmp[11]=0;
    while(n){ tmp[i--]='0'+n%10; n/=10; }
    kstrcpy(buf, tmp+i+1);
}

static void build_meminfo(void) {
    char n[16];
    kstrcpy(proc_buf, "MemTotal:  "); uint_to_str(total_mem_kb,n); kstrcat(proc_buf,n); kstrcat(proc_buf," KB\n");
    kstrcat(proc_buf,"MemUsed:   "); uint_to_str(pmm_used()*4,n); kstrcat(proc_buf,n); kstrcat(proc_buf," KB\n");
    kstrcat(proc_buf,"MemFree:   "); uint_to_str((pmm_total()-pmm_used())*4,n); kstrcat(proc_buf,n); kstrcat(proc_buf," KB\n");
    kstrcat(proc_buf,"HeapUsed:  "); uint_to_str(kmalloc_used(),n); kstrcat(proc_buf,n); kstrcat(proc_buf," B\n");
    kstrcat(proc_buf,"Paging:    enabled\n");
}

static void build_uptime(void) {
    char n[16]; uint_to_str(timer_seconds(),n);
    kstrcpy(proc_buf, n); kstrcat(proc_buf," seconds\n");
}

static void build_version(void) {
    kstrcpy(proc_buf, "KumOS " KUMOS_VERSION " i686\n");
}

static void build_ps(void) {
    kstrcpy(proc_buf,"PID  STATE  NAME\n");
    for (int i=1; i<=SCHED_MAX_TASKS; i++) {
        task_t *t = sched_get_task(i);
        if (!t || t->state==TASK_DEAD) continue;
        char n[12];
        uint_to_str((uint32_t)t->pid,n);
        kstrcat(proc_buf, n);
        kstrcat(proc_buf, "    ");
        const char *st=(t->state==TASK_RUNNING?"RUN":t->state==TASK_READY?"RDY":
                        t->state==TASK_SLEEPING?"SLP":t->state==TASK_ZOMBIE?"ZOM":"???");
        kstrcat(proc_buf, st); kstrcat(proc_buf, "  ");
        kstrcat(proc_buf, t->name); kstrcat(proc_buf, "\n");
    }
}

static void build_net(void) {
    if (!net_ready()) { kstrcpy(proc_buf,"no network\n"); return; }
    uint8_t mac[6]; net_get_mac(mac);
    const char *h="0123456789ABCDEF";
    kstrcpy(proc_buf,"mac: ");
    for (int i=0;i<6;i++) {
        char tmp[3]; tmp[0]=h[mac[i]>>4]; tmp[1]=h[mac[i]&0xF]; tmp[2]=0;
        kstrcat(proc_buf,tmp);
        if(i<5) kstrcat(proc_buf,":");
    }
    kstrcat(proc_buf,"\nip:  10.0.2.15\n");
}

static void build_date(void) {
    rtc_time_t t = rtc_read();
    char buf[32],n[8];
    uint_to_str(t.year,n); kstrcpy(buf,n); kstrcat(buf,"-");
    if(t.month<10)kstrcat(buf,"0"); uint_to_str(t.month,n); kstrcat(buf,n); kstrcat(buf,"-");
    if(t.day<10)kstrcat(buf,"0"); uint_to_str(t.day,n); kstrcat(buf,n); kstrcat(buf," ");
    if(t.hour<10)kstrcat(buf,"0"); uint_to_str(t.hour,n); kstrcat(buf,n); kstrcat(buf,":");
    if(t.minute<10)kstrcat(buf,"0"); uint_to_str(t.minute,n); kstrcat(buf,n); kstrcat(buf,":");
    if(t.second<10)kstrcat(buf,"0"); uint_to_str(t.second,n); kstrcat(buf,n); kstrcat(buf,"\n");
    kstrcpy(proc_buf, buf);
}

static int proc_pos = 0;

static int proc_open(const char *path, int flags) {
    (void)flags;
    /* proc_pos used to be cleared only in proc_close(), never here - an
       open of a second proc file (or a re-open of the same fd slot)
       before the previous one is closed left it at wherever the last
       read stopped, which underflows proc_read's `avail` once the
       freshly-rebuilt proc_buf is shorter than that stale offset and
       reads proc_buf out of bounds into the caller's buffer. */
    proc_pos = 0;
    if (kstrcmp(path,"meminfo")==0)   { build_meminfo(); return 1; }
    if (kstrcmp(path,"uptime")==0)    { build_uptime();  return 2; }
    if (kstrcmp(path,"version")==0)   { build_version(); return 3; }
    if (kstrcmp(path,"ps")==0)        { build_ps();      return 4; }
    if (kstrcmp(path,"net")==0)       { build_net();     return 5; }
    if (kstrcmp(path,"date")==0)      { build_date();    return 6; }
    if (kstrcmp(path,"dmesg")==0)     { dmesg_read(proc_buf, sizeof(proc_buf)); return 7; }
    return -1;
}

static int proc_close(int d) { (void)d; proc_pos=0; return 0; }
static int proc_read(int d, void *buf, uint32_t len) {
    (void)d;
    uint32_t avail = kstrlen(proc_buf) - (uint32_t)proc_pos;
    if (len > avail) len = avail;
    if (!len) return 0;
    kmemcpy(buf, proc_buf + proc_pos, len);
    proc_pos += (int)len;
    return (int)len;
}
static int proc_write(int d, const void *buf, uint32_t len) {
    (void)d;(void)buf;(void)len; return -1;
}
static int proc_stat(const char *path, vfs_stat_t *st) {
    (void)path; st->type=VFS_FILE; st->size=0;
    kstrcpy(st->name, path);
    return 0;
}
static int proc_readdir(const char *path, char *buf, uint32_t sz) {
    (void)path; (void)sz;
    kstrcpy(buf,"meminfo\nuptime\nversion\nps\nnet\ndate\ndmesg\n");
    return (int)kstrlen(buf);
}

static vfs_ops_t proc_ops = {
    proc_open, proc_close, proc_read, proc_write,
    proc_stat, proc_readdir, 0, 0
};

void proc_fs_init(void) {
    vfs_mount("/proc", &proc_ops);
}
