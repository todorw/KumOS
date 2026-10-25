#include "browser.h"
#include "net.h"
#include "tls.h"
#include "dns.h"
#include "pkgnet.h"
#include "kstring.h"
#include "kmalloc.h"
#include "timer.h"
#include <stdint.h>

#define FETCH_BUF_SIZE   (64u*1024u)
#define FETCH_IDLE_MAX   300

static char browser_lines_buf[BROWSER_MAX_LINES][BROWSER_LINE_LEN+1];
static int  browser_nlines = 0;
static char browser_title_buf[80] = "";
static char browser_status_buf[80] = "";
static char browser_url_buf[160] = "";

int         browser_line_count(void) { return browser_nlines; }
const char *browser_line_at(int i)   { return (i>=0 && i<browser_nlines) ? browser_lines_buf[i] : ""; }
const char *browser_title(void)      { return browser_title_buf[0] ? browser_title_buf : browser_url_buf; }
const char *browser_status(void)     { return browser_status_buf; }
const char *browser_url(void)        { return browser_url_buf; }

static const char *find_sub(const char *hay, const char *hay_end, const char *needle) {
    int nlen = (int)kstrlen(needle);
    for (const char *p = hay; p + nlen <= hay_end; p++) {
        int i = 0;
        while (i < nlen && p[i] == needle[i]) i++;
        if (i == nlen) return p;
    }
    return 0;
}
static int ci_eq_n(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) break;
    }
    return 1;
}
static const char *find_header(const char *headers, const char *headers_end, const char *name) {
    int nlen = (int)kstrlen(name);
    for (const char *p = headers; p + nlen < headers_end; p++) {
        if ((p == headers || p[-1] == '\n') && ci_eq_n(p, name, nlen)) {
            const char *v = p + nlen;
            while (v < headers_end && *v == ' ') v++;
            return v;
        }
    }
    return 0;
}

static void parse_url(const char *url, int *use_tls, char *host, int host_sz,
                       uint16_t *port, char *path, int path_sz) {
    const char *p = url;
    *use_tls = 1;
    if (kstartswith(p, "https://")) { *use_tls = 1; p += 8; }
    else if (kstartswith(p, "http://")) { *use_tls = 0; p += 7; }

    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < host_sz-1) host[hi++] = *p++;
    host[hi] = 0;

    *port = *use_tls ? 443 : 80;
    if (*p == ':') {
        p++;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') { v = v*10 + (uint32_t)(*p-'0'); p++; }
        if (v > 0 && v <= 65535) *port = (uint16_t)v;
    }

    if (*p == '/') kstrncpy(path, p, (size_t)path_sz-1);
    else kstrcpy(path, "/");
    path[path_sz-1] = 0;
}

static int fetch_via_tcp(uint32_t ip, uint16_t port, const char *req, uint8_t *buf, uint32_t *total) {
    int s = tcp_connect(ip, port);
    if (s < 0) return -1;
    tcp_send(s, req, (uint16_t)kstrlen(req));

    *total = 0;
    int idle = 0;
    while (*total < FETCH_BUF_SIZE - 1 && idle < FETCH_IDLE_MAX) {
        net_poll();
        uint16_t want = (uint16_t)((FETCH_BUF_SIZE - 1 - *total) > 1024 ? 1024 : (FETCH_BUF_SIZE - 1 - *total));
        int n = tcp_recv(s, buf + *total, want);
        if (n > 0) {
            *total += (uint32_t)n;
            idle = 0;
            tcp_send(s, 0, 0);
        } else {
            idle++;
            timer_sleep(10);
        }
    }
    tcp_close(s);
    buf[*total] = 0;
    return 0;
}

static int fetch_via_tls(uint32_t ip, uint16_t port, const char *sni, const char *req,
                          uint8_t *buf, uint32_t *total) {
    tls_conn_t *tls = (tls_conn_t*)kmalloc(sizeof(tls_conn_t));
    if (!tls) return -1;
    if (tls_connect(tls, ip, port, sni) != 0) { kfree(tls); return -1; }
    tls_send(tls, req, (uint32_t)kstrlen(req));

    *total = 0;
    for (;;) {
        if (*total >= FETCH_BUF_SIZE - 1) break;
        uint32_t want = FETCH_BUF_SIZE - 1 - *total;
        int n = tls_recv(tls, buf + *total, want);
        if (n <= 0) break;
        *total += (uint32_t)n;
    }
    tls_close(tls);
    kfree(tls);
    buf[*total] = 0;
    return 0;
}

static uint32_t dechunk(uint8_t *body, uint32_t body_len) {
    uint8_t *out = body;
    const uint8_t *p = body, *end = body + body_len;
    for (;;) {
        uint32_t size = 0;
        int any = 0;
        while (p < end && *p != '\r' && *p != '\n') {
            uint8_t c = *p;
            int v = (c>='0'&&c<='9') ? c-'0' : (c>='a'&&c<='f') ? c-'a'+10 : (c>='A'&&c<='F') ? c-'A'+10 : -1;
            if (v < 0) break;
            size = size*16 + (uint32_t)v;
            any = 1; p++;
        }
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
        if (!any || size == 0) break;
        if (p + size > end) size = (uint32_t)(end - p);
        for (uint32_t i = 0; i < size; i++) *out++ = p[i];
        p += size;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
    return (uint32_t)(out - body);
}

static int decode_entity(const char *p, const char *end, char *out) {
    if (p[0]!='&') return 0;
    struct { const char *name; char ch; } ents[] = {
        {"amp;",'&'}, {"lt;",'<'}, {"gt;",'>'}, {"quot;",'"'}, {"#39;",'\''},
        {"apos;",'\''}, {"nbsp;",' '}, {"mdash;",'-'}, {"ndash;",'-'},
        {"hellip;",'.'}, {"rsquo;",'\''}, {"lsquo;",'\''}, {"rdquo;",'"'}, {"ldquo;",'"'},
    };
    for (unsigned i = 0; i < sizeof(ents)/sizeof(ents[0]); i++) {
        int n = (int)kstrlen(ents[i].name);
        if (p+1+n <= end && ci_eq_n(p+1, ents[i].name, n)) { *out = ents[i].ch; return 1+n; }
    }
    return 0;
}

static int  out_row = 0;
static char out_line[BROWSER_LINE_LEN+1];
static int  out_col = 0;
static int  out_last_space = -1;

static void out_flush(void) {
    if (out_row >= BROWSER_MAX_LINES) return;
    out_line[out_col] = 0;
    kstrcpy(browser_lines_buf[out_row], out_line);
    out_row++;
    out_col = 0;
    out_last_space = -1;
}
static void out_newline(void) {
    if (out_col == 0 && out_row > 0 && browser_lines_buf[out_row-1][0] == 0) return;
    out_flush();
}
static void out_char(char c) {
    if (c == ' ' && out_col == 0) return;
    if (out_col >= BROWSER_LINE_LEN) {
        if (out_last_space > 0) {
            char tail[BROWSER_LINE_LEN+1];
            int tn = 0;
            for (int i = out_last_space+1; i < out_col; i++) tail[tn++] = out_line[i];
            out_col = out_last_space;
            out_flush();
            for (int i = 0; i < tn; i++) out_line[out_col++] = tail[i];
            out_last_space = -1;
        } else {
            out_flush();
        }
    }
    if (c == ' ') out_last_space = out_col;
    out_line[out_col++] = c;
}

static void html_to_text(const char *body, uint32_t len) {
    out_row = 0; out_col = 0; out_last_space = -1;
    browser_title_buf[0] = 0;
    int title_len = 0, in_title = 0;

    const char *p = body, *end = body + len;
    while (p < end) {
        if (*p == '<') {
            char tag[16]; int tn = 0;
            const char *q = p+1;
            int closing = (q < end && *q == '/'); if (closing) q++;
            while (q < end && tn < 15 && ((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9'))) tag[tn++] = *q++;
            tag[tn] = 0;
            for (int i = 0; i < tn; i++) if (tag[i]>='A'&&tag[i]<='Z') tag[i]+=32;

            if (!closing && (kstrcmp(tag,"script")==0 || kstrcmp(tag,"style")==0)) {
                char endtag[10] = "</";
                kstrcat(endtag, tag); kstrcat(endtag, ">");
                const char *skip_to = find_sub(p, end, endtag);
                p = skip_to ? skip_to + kstrlen(endtag) : end;
                continue;
            }
            if (!closing && kstrcmp(tag,"title")==0) in_title = 1;
            if (closing && kstrcmp(tag,"title")==0) in_title = 0;

            if (kstrcmp(tag,"br")==0 || kstrcmp(tag,"p")==0 || kstrcmp(tag,"div")==0 ||
                kstrcmp(tag,"li")==0 || kstrcmp(tag,"tr")==0 || kstrcmp(tag,"h1")==0 ||
                kstrcmp(tag,"h2")==0 || kstrcmp(tag,"h3")==0 || kstrcmp(tag,"h4")==0 ||
                kstrcmp(tag,"h5")==0 || kstrcmp(tag,"h6")==0 || kstrcmp(tag,"ul")==0 ||
                kstrcmp(tag,"ol")==0 || kstrcmp(tag,"table")==0 || kstrcmp(tag,"hr")==0 ||
                kstrcmp(tag,"section")==0 || kstrcmp(tag,"header")==0 || kstrcmp(tag,"footer")==0 ||
                kstrcmp(tag,"article")==0 || kstrcmp(tag,"nav")==0)
                out_newline();

            while (p < end && *p != '>') p++;
            if (p < end) p++;
            continue;
        }
        char ent;
        int elen = decode_entity(p, end, &ent);
        if (elen) {
            if (in_title) { if (title_len < 78) browser_title_buf[title_len++] = ent; }
            else out_char(ent);
            p += elen;
            continue;
        }
        char c = *p;
        if (c == '\r') { p++; continue; }
        if (c == '\n' || c == '\t') c = ' ';
        if (in_title) { if (title_len < 78 && c != '\n') browser_title_buf[title_len++] = c; }
        else out_char(c);
        p++;
    }
    if (out_col > 0) out_flush();
    browser_title_buf[title_len] = 0;
    browser_nlines = out_row;
}

int browser_navigate(const char *url) {
    kstrncpy(browser_url_buf, url, sizeof(browser_url_buf)-1);
    browser_url_buf[sizeof(browser_url_buf)-1] = 0;
    browser_status_buf[0] = 0;
    browser_nlines = 0;

    for (int hop = 0; hop < 2; hop++) {
        int use_tls; char host[64]; uint16_t port; char path[128];
        parse_url(browser_url_buf, &use_tls, host, sizeof(host), &port, path, sizeof(path));

        uint32_t ip = 0; uint16_t ip_port;
        if (pkgnet_parse_addr(host, &ip, &ip_port) != 0) {
            ip = dns_resolve(host);
            if (!ip) { kstrcpy(browser_status_buf, "DNS resolution failed"); return -1; }
        }

        char req[320];
        kstrcpy(req, "GET "); kstrcat(req, path);
        kstrcat(req, " HTTP/1.1\r\nHost: "); kstrcat(req, host);
        kstrcat(req, "\r\nUser-Agent: kubrowser/1.0 (KumOS)\r\nAccept: text/html\r\nConnection: close\r\n\r\n");

        uint8_t *buf = (uint8_t*)kmalloc(FETCH_BUF_SIZE);
        if (!buf) { kstrcpy(browser_status_buf, "Out of memory"); return -1; }

        uint32_t total = 0;
        int r = use_tls ? fetch_via_tls(ip, port, host, req, buf, &total)
                         : fetch_via_tcp(ip, port, req, buf, &total);
        if (r != 0 || total == 0) {
            kfree(buf);
            kstrcpy(browser_status_buf, use_tls ? "TLS connect failed" : "Connect failed");
            return -1;
        }

        const char *hend = find_sub((const char*)buf, (const char*)buf+total, "\r\n\r\n");
        if (!hend) {
            kfree(buf);
            kstrcpy(browser_status_buf, "Malformed response (no headers)");
            return -1;
        }
        const char *headers = (const char*)buf;
        uint32_t status_code = 0;
        {
            const char *sp = headers;
            while (sp < hend && *sp != ' ') sp++;
            if (sp < hend) { sp++; while (sp < hend && *sp>='0' && *sp<='9') { status_code = status_code*10 + (uint32_t)(*sp-'0'); sp++; } }
        }

        if ((status_code>=301 && status_code<=303) || status_code==307 || status_code==308) {
            const char *loc = find_header(headers, hend, "Location:");
            if (loc && hop == 0) {
                const char *le = loc;
                while (le < hend && *le != '\r' && *le != '\n') le++;
                char next[160];
                int o = 0;
                if (*loc == '/') {
                    const char *scheme = use_tls ? "https://" : "http://";
                    for (const char *p = scheme; *p; p++) next[o++] = *p;
                    for (const char *p = host; *p && o < 120; p++) next[o++] = *p;
                }
                for (const char *p = loc; p < le && o < (int)sizeof(next) - 1; p++) next[o++] = *p;
                next[o] = 0;
                kstrcpy(browser_url_buf, next);
                kfree(buf);
                continue;
            }
        }

        uint8_t *body = buf + (hend - (const char*)buf) + 4;
        uint32_t body_len = total - (uint32_t)((hend - (const char*)buf) + 4);

        const char *te = find_header(headers, hend, "Transfer-Encoding:");
        if (te && ci_eq_n(te, "chunked", 7)) body_len = dechunk(body, body_len);

        html_to_text((const char*)body, body_len);
        kfree(buf);

        if (status_code != 0 && status_code != 200) {
            char buf2[32]; kitoa(status_code, buf2, 10);
            kstrcpy(browser_status_buf, "HTTP "); kstrcat(browser_status_buf, buf2);
        } else {
            kstrcpy(browser_status_buf, "Loaded");
        }
        return 0;
    }

    kstrcpy(browser_status_buf, "Too many redirects");
    return -1;
}
