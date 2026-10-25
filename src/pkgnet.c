#include "pkgnet.h"
#include "net.h"
#include "kstring.h"
#include "kmalloc.h"
#include "fat12.h"
#include "timer.h"
#include <stdint.h>

#define PKGNET_MAX        32
#define PKGNET_NAME_LEN   16
#define PKGNET_FNAME_LEN  16
#define PKGNET_DESC_LEN   48

typedef struct {
    char name[PKGNET_NAME_LEN];
    char fname[PKGNET_FNAME_LEN];
    char desc[PKGNET_DESC_LEN];
} remote_pkg_t;

static remote_pkg_t remote_pkgs[PKGNET_MAX];
static int          remote_count = 0;

static uint32_t repo_ip   = 0;
static uint16_t repo_port = 8080;

void pkgnet_set_repo(uint32_t ip, uint16_t port) { repo_ip = ip; repo_port = port; }
uint32_t pkgnet_repo_ip(void)   { return repo_ip ? repo_ip : NET_IP(10,0,2,2); }
uint16_t pkgnet_repo_port(void) { return repo_port; }

int pkgnet_parse_addr(const char *s, uint32_t *ip, uint16_t *port) {
    uint32_t octs[4] = {0,0,0,0};
    const char *p = s;
    for (int i=0;i<4;i++) {
        if (*p<'0'||*p>'9') return -1;
        uint32_t v=0;
        while (*p>='0'&&*p<='9') { v=v*10+(uint32_t)(*p-'0'); p++; }
        octs[i]=v;
        if (i<3) { if (*p!='.') return -1; p++; }
    }
    uint16_t prt = 8080;
    if (*p==':') {
        p++;
        if (*p<'0'||*p>'9') return -1;
        uint32_t v=0;
        while (*p>='0'&&*p<='9') { v=v*10+(uint32_t)(*p-'0'); p++; }
        prt=(uint16_t)v;
    } else if (*p) return -1;

    *ip = NET_IP(octs[0],octs[1],octs[2],octs[3]);
    *port = prt;
    return 0;
}

int pkgnet_count(void) { return remote_count; }
const char *pkgnet_name_at(int i)  { return (i>=0&&i<remote_count) ? remote_pkgs[i].name  : ""; }
const char *pkgnet_fname_at(int i) { return (i>=0&&i<remote_count) ? remote_pkgs[i].fname : ""; }
const char *pkgnet_desc_at(int i)  { return (i>=0&&i<remote_count) ? remote_pkgs[i].desc  : ""; }

int pkgnet_find(const char *name) {
    for (int i=0;i<remote_count;i++)
        if (kstrcmp(remote_pkgs[i].name, name)==0) return i;
    return -1;
}

#define HTTP_BUF_SIZE  (256u*1024u)
#define HTTP_IDLE_MAX  300

static uint8_t *http_fetch(const char *path, uint32_t *body_off, uint32_t *body_len) {
    int s = tcp_connect(pkgnet_repo_ip(), pkgnet_repo_port());
    if (s < 0) return 0;

    char req[160];
    kstrcpy(req, "GET ");
    kstrcat(req, path);
    kstrcat(req, " HTTP/1.0\r\nHost: kumos-repo\r\nConnection: close\r\n\r\n");
    tcp_send(s, req, (uint16_t)kstrlen(req));

    uint8_t *buf = (uint8_t*)kmalloc(HTTP_BUF_SIZE);
    if (!buf) { tcp_close(s); return 0; }

    uint32_t total = 0;
    int header_end = -1;
    long content_len = -1;
    int idle = 0;

    while (total < HTTP_BUF_SIZE - 1 && idle < HTTP_IDLE_MAX) {
        net_poll();
        uint16_t want = (uint16_t)((HTTP_BUF_SIZE - 1 - total) > 512 ? 512 : (HTTP_BUF_SIZE - 1 - total));
        int n = tcp_recv(s, buf + total, want);
        if (n > 0) {
            total += (uint32_t)n;
            buf[total] = 0;
            idle = 0;

            tcp_send(s, 0, 0);

            if (header_end < 0) {
                for (uint32_t i = 0; i + 3 < total; i++) {
                    if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') {
                        header_end = (int)(i + 4);
                        break;
                    }
                }
                if (header_end >= 0) {
                    const char *key = "Content-Length:";
                    int klen = (int)kstrlen(key);
                    for (int i = 0; i + klen < header_end; i++) {
                        if (kstrncmp((const char*)buf + i, key, (size_t)klen) == 0) {
                            const char *p = (const char*)buf + i + klen;
                            while (*p == ' ') p++;
                            long v = 0;
                            while (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); p++; }
                            content_len = v;
                            break;
                        }
                    }
                }
            }

            if (header_end >= 0 && content_len >= 0 &&
                (long)(total - (uint32_t)header_end) >= content_len)
                break;
        } else {
            idle++;
            timer_sleep(10);
        }
    }
    tcp_close(s);

    if (header_end < 0) { kfree(buf); return 0; }

    uint32_t blen = total - (uint32_t)header_end;
    if (content_len >= 0 && (uint32_t)content_len < blen) blen = (uint32_t)content_len;

    *body_off = (uint32_t)header_end;
    *body_len = blen;
    return buf;
}

int pkgnet_update(void) {
    uint32_t off, len;
    uint8_t *buf = http_fetch("/index.txt", &off, &len);
    if (!buf) return -1;

    remote_count = 0;
    const char *p   = (const char*)buf + off;
    const char *end = p + len;

    while (p < end && remote_count < PKGNET_MAX) {
        const char *line_end = p;
        while (line_end < end && *line_end != '\n') line_end++;
        const char *le = line_end;
        if (le > p && *(le-1) == '\r') le--;

        if (le > p) {
            remote_pkg_t *e = &remote_pkgs[remote_count];

            const char *f1 = p, *sep1 = f1;
            while (sep1 < le && *sep1 != '|') sep1++;
            int n1 = (int)(sep1 - f1); if (n1 >= PKGNET_NAME_LEN) n1 = PKGNET_NAME_LEN-1;
            kmemcpy(e->name, f1, (size_t)n1); e->name[n1] = 0;

            const char *f2 = (sep1 < le) ? sep1+1 : le, *sep2 = f2;
            while (sep2 < le && *sep2 != '|') sep2++;
            int n2 = (int)(sep2 - f2); if (n2 >= PKGNET_FNAME_LEN) n2 = PKGNET_FNAME_LEN-1;
            kmemcpy(e->fname, f2, (size_t)n2); e->fname[n2] = 0;

            const char *f3 = (sep2 < le) ? sep2+1 : le;
            int n3 = (int)(le - f3); if (n3 >= PKGNET_DESC_LEN) n3 = PKGNET_DESC_LEN-1;
            kmemcpy(e->desc, f3, (size_t)n3); e->desc[n3] = 0;

            if (n1 > 0) remote_count++;
        }
        p = (line_end < end) ? line_end + 1 : end;
    }

    kfree(buf);
    return remote_count;
}

int pkgnet_install(const char *name) {
    int i = pkgnet_find(name);
    if (i < 0) return -1;
    if (!fat12_mounted()) return -2;

    char path[PKGNET_FNAME_LEN + 2];
    kstrcpy(path, "/");
    kstrcat(path, remote_pkgs[i].fname);

    uint32_t off, len;
    uint8_t *buf = http_fetch(path, &off, &len);
    if (!buf) return -3;

    int r = fat12_write(remote_pkgs[i].fname, buf + off, len);
    kfree(buf);
    return (r == 0) ? 0 : -4;
}
