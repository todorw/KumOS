#include "dhcp.h"
#include "net.h"
#include "kstring.h"
#include "timer.h"
#include "serial.h"
#include <stdint.h>

#define DHCP_DISCOVER  1
#define DHCP_OFFER     2
#define DHCP_REQUEST   3
#define DHCP_ACK       5
#define DHCP_PORT_CLI  68
#define DHCP_PORT_SRV  67
#define DHCP_MAGIC     0x63825363

typedef struct {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[308];
} __attribute__((packed)) dhcp_pkt_t;

static uint32_t offered_ip  = 0;
static uint32_t offered_gw  = 0;
static uint32_t offered_dns = 0;
static uint32_t offered_mask = 0;

static void build_discover(dhcp_pkt_t *p, uint32_t xid, uint8_t mac[6]) {
    kmemset(p, 0, sizeof(dhcp_pkt_t));
    p->op    = 1;
    p->htype = 1;
    p->hlen  = 6;
    p->xid   = xid;
    p->flags = NET_HTONS(0x8000);
    kmemcpy(p->chaddr, mac, 6);
    p->magic = NET_HTONL(DHCP_MAGIC);
    uint8_t *o = p->options;
    *o++=53; *o++=1; *o++=DHCP_DISCOVER;
    *o++=61; *o++=7; *o++=1; kmemcpy(o,mac,6); o+=6;
    *o++=55; *o++=3; *o++=1; *o++=3; *o++=6;
    *o++=255;
}

static void build_request(dhcp_pkt_t *p, uint32_t xid,
                           uint8_t mac[6], uint32_t offer_ip, uint32_t srv_ip) {
    kmemset(p, 0, sizeof(dhcp_pkt_t));
    p->op=1; p->htype=1; p->hlen=6;
    p->xid=xid; p->flags=NET_HTONS(0x8000);
    kmemcpy(p->chaddr,mac,6);
    p->magic=NET_HTONL(DHCP_MAGIC);
    uint8_t *o=p->options;
    *o++=53; *o++=1; *o++=DHCP_REQUEST;
    *o++=50; *o++=4;
    *o++=(offer_ip>>24)&0xFF; *o++=(offer_ip>>16)&0xFF;
    *o++=(offer_ip>>8)&0xFF;  *o++=offer_ip&0xFF;
    *o++=54; *o++=4;
    *o++=(srv_ip>>24)&0xFF; *o++=(srv_ip>>16)&0xFF;
    *o++=(srv_ip>>8)&0xFF;  *o++=srv_ip&0xFF;
    *o++=255;
}

static uint32_t parse_ip(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

static int parse_offer(dhcp_pkt_t *p, uint32_t *srv_ip) {
    if (NET_HTONL(p->magic) != DHCP_MAGIC) return 0;
    uint8_t *o = p->options;
    uint8_t msg_type = 0;
    *srv_ip = 0;
    while ((o - p->options) < 306 && *o != 255) {
        uint8_t tag = *o++;
        if (tag == 0) continue;
        uint8_t len = *o++;
        if (tag == 53) msg_type = o[0];
        else if (tag == 54 && len==4) *srv_ip = parse_ip(o);
        else if (tag == 3  && len>=4) offered_gw  = parse_ip(o);
        else if (tag == 6  && len>=4) offered_dns = parse_ip(o);
        else if (tag == 1  && len==4) offered_mask = parse_ip(o);
        o += len;
    }
    if (msg_type == DHCP_OFFER || msg_type == DHCP_ACK) {
        offered_ip = NET_HTONL(p->yiaddr);
        return msg_type;
    }
    return 0;
}

int dhcp_request(void) {
    if (!net_ready()) return -1;

    uint8_t mac[6]; net_get_mac(mac);
    uint32_t xid = 0xDEAD0042;
    dhcp_pkt_t pkt;

    build_discover(&pkt, xid, mac);
    net_send_udp(0xFFFFFFFF, DHCP_PORT_CLI, DHCP_PORT_SRV,
                 &pkt, sizeof(pkt));
    serial_printf("[dhcp] DISCOVER sent\r\n");

    uint32_t srv_ip = 0;
    for (int t = 0; t < 100; t++) {
        timer_sleep(20);
        net_poll();
        uint8_t buf[sizeof(dhcp_pkt_t)+8];
        kmemset(buf, 0, sizeof(buf));
        uint32_t from=0; uint16_t sport=0;
        int n = net_recv_udp(DHCP_PORT_CLI, buf, sizeof(buf), &from, &sport);
        if (n < 240) continue;
        int type = parse_offer((dhcp_pkt_t*)buf, &srv_ip);
        if (type == DHCP_OFFER) {
            serial_printf("[dhcp] OFFER received: %u.%u.%u.%u\r\n",
                (offered_ip>>24)&0xFF,(offered_ip>>16)&0xFF,
                (offered_ip>>8)&0xFF, offered_ip&0xFF);
            build_request(&pkt, xid, mac, offered_ip, srv_ip);
            net_send_udp(0xFFFFFFFF, DHCP_PORT_CLI, DHCP_PORT_SRV,
                         &pkt, sizeof(pkt));
        } else if (type == DHCP_ACK) {
            serial_printf("[dhcp] ACK: IP=%u.%u.%u.%u GW=%u.%u.%u.%u\r\n",
                (offered_ip>>24)&0xFF,(offered_ip>>16)&0xFF,
                (offered_ip>>8)&0xFF, offered_ip&0xFF,
                (offered_gw>>24)&0xFF,(offered_gw>>16)&0xFF,
                (offered_gw>>8)&0xFF, offered_gw&0xFF);
            net_set_config(offered_ip, offered_gw, offered_mask);
            return 0;
        }
    }
    return -1;
}

uint32_t dhcp_get_ip(void)      { return offered_ip; }
uint32_t dhcp_get_gateway(void) { return offered_gw; }
uint32_t dhcp_get_dns(void)     { return offered_dns; }
