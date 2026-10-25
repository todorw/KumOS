#include "dns.h"
#include "net.h"
#include "kstring.h"
#include "timer.h"
#include <stdint.h>

static uint32_t dns_server = 0;

void dns_init(uint32_t server_ip) {
    dns_server = server_ip;
}

static uint16_t dns_id = 1;

static int encode_name(const char *name, uint8_t *out) {
    int pos = 0;
    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int len = (int)(dot - p);
        out[pos++] = (uint8_t)len;
        kmemcpy(out + pos, p, (uint32_t)len);
        pos += len;
        if (*dot) dot++;
        p = dot;
    }
    out[pos++] = 0;
    return pos;
}

uint32_t dns_resolve(const char *hostname) {
    if (!dns_server || !net_ready() || kstrlen(hostname) > 250) return 0;

    uint8_t pkt[512];
    kmemset(pkt, 0, sizeof(pkt));

    uint16_t id = dns_id++;
    pkt[0] = (uint8_t)(id >> 8);
    pkt[1] = (uint8_t)(id & 0xFF);
    pkt[2] = 0x01;
    pkt[3] = 0x00;
    pkt[4] = 0x00; pkt[5] = 0x01;
    pkt[6] = 0x00; pkt[7] = 0x00;
    pkt[8] = 0x00; pkt[9] = 0x00;
    pkt[10]= 0x00; pkt[11]= 0x00;

    int pos = 12;
    pos += encode_name(hostname, pkt + pos);
    pkt[pos++] = 0x00; pkt[pos++] = 0x01;
    pkt[pos++] = 0x00; pkt[pos++] = 0x01;

    net_send_udp(dns_server, 1053, 53, pkt, (uint16_t)pos);

    for (int tries = 0; tries < 50; tries++) {
        timer_sleep(20);
        net_poll();
        uint8_t resp[512];
        uint32_t src_ip = 0; uint16_t src_port = 0;
        int n = net_recv_udp(1053, resp, sizeof(resp), &src_ip, &src_port);
        if (n < 12) continue;

        uint16_t rid = (uint16_t)((resp[0]<<8)|resp[1]);
        if (rid != id) continue;

        uint16_t ancount = (uint16_t)((resp[6]<<8)|resp[7]);
        if (ancount == 0) return 0;

        int rpos = 12;
        while (rpos < n && resp[rpos]) {
            if ((resp[rpos] & 0xC0) == 0xC0) { rpos += 2; break; }
            rpos += resp[rpos] + 1;
        }
        if (rpos >= n) continue;
        if (resp[rpos] == 0) rpos++;
        rpos += 4;

        for (int i = 0; i < ancount && rpos + 12 <= n; i++) {
            if ((resp[rpos] & 0xC0) == 0xC0) rpos += 2;
            else { while (rpos < n && resp[rpos]) rpos += resp[rpos]+1; rpos++; }

            if (rpos + 10 > n) break;
            uint16_t rtype  = (uint16_t)((resp[rpos]<<8)|resp[rpos+1]);
            uint16_t rdlen  = (uint16_t)((resp[rpos+8]<<8)|resp[rpos+9]);
            rpos += 10;

            if (rtype == 1 && rdlen == 4 && rpos + 4 <= n) {
                uint32_t ip = ((uint32_t)resp[rpos]<<24)|((uint32_t)resp[rpos+1]<<16)
                             |((uint32_t)resp[rpos+2]<<8)|(uint32_t)resp[rpos+3];
                return ip;
            }
            rpos += rdlen;
        }
    }
    return 0;
}
