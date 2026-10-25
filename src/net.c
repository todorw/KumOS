#include "net.h"
#include "vga.h"
#include "kstring.h"
#include "timer.h"
#include <stdint.h>

static inline void outb(uint16_t p,uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline void outw(uint16_t p,uint16_t v){__asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p));}
static inline void outl(uint16_t p,uint32_t v){__asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t  inb(uint16_t p){uint8_t v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
static inline uint16_t inw(uint16_t p){uint16_t v;__asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p));return v;}
static inline uint32_t inl(uint16_t p){uint32_t v;__asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p));return v;}

#define PCI_ADDR   0xCF8
#define PCI_DATA   0xCFC

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    outl(PCI_ADDR, 0x80000000u|(uint32_t)bus<<16|(uint32_t)dev<<11|(uint32_t)fn<<8|(reg&0xFC));
    return inl(PCI_DATA);
}
static void pci_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val) {
    outl(PCI_ADDR, 0x80000000u|(uint32_t)bus<<16|(uint32_t)dev<<11|(uint32_t)fn<<8|(reg&0xFC));
    outl(PCI_DATA, val);
}

#define RTL_VENDOR  0x10EC
#define RTL_DEVICE  0x8139

#define RTL_MAC0    0x00
#define RTL_MAR0    0x08
#define RTL_TSD0    0x10
#define RTL_TSAD0   0x20
#define RTL_RBSTART 0x30
#define RTL_CMD     0x37
#define RTL_CAPR    0x38
#define RTL_CBR     0x3A
#define RTL_IMR     0x3C
#define RTL_ISR     0x3E
#define RTL_TCR     0x40
#define RTL_RCR     0x44
#define RTL_MPC     0x4C
#define RTL_CONFIG1 0x52

#define TX_BUFS     4
#define TX_BUF_SIZE 1536
#define RX_BUF_SIZE (8192+16+1500)

static uint16_t rtl_iobase = 0;
static uint8_t  tx_buf[TX_BUFS][TX_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t  rx_buf[RX_BUF_SIZE+4]         __attribute__((aligned(4)));
static int      tx_idx = 0;
static int      rtl_ready_flag = 0;

static uint8_t  my_mac[ETH_ALEN];
static uint32_t my_ip  = NET_IP(10,0,2,15);
static uint32_t gw_ip  = NET_IP(10,0,2,2);
static uint32_t net_mask = NET_IP(255,255,255,0);

static uint8_t  arp_cache_ip[8][4];
static uint8_t  arp_cache_mac[8][ETH_ALEN];
static int      arp_count = 0;

static uint32_t tx_packets = 0, rx_packets = 0;

#define RX_RING_SIZE 4
static struct { uint8_t buf[1600]; uint16_t len; } rx_ring[RX_RING_SIZE];
static int rx_head = 0, rx_tail = 0;

static int find_rtl8139(uint8_t *bus_out, uint8_t *dev_out) {
    for (uint8_t bus=0; bus<8; bus++) {
        for (uint8_t dev=0; dev<32; dev++) {
            uint32_t id = pci_read(bus,dev,0,0);
            uint16_t vendor=(uint16_t)(id&0xFFFF);
            uint16_t device=(uint16_t)(id>>16);
            if (vendor==RTL_VENDOR && device==RTL_DEVICE) {
                *bus_out=bus; *dev_out=dev; return 1;
            }
        }
    }
    return 0;
}

static uint16_t ip_checksum(const void *data, uint32_t len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum>>16) sum = (sum&0xFFFF)+(sum>>16);
    return (uint16_t)~sum;
}

int net_init(void) {
    uint8_t bus=0, dev_n=0;
    if (!find_rtl8139(&bus, &dev_n)) return -1;

    uint32_t bar0 = pci_read(bus,dev_n,0,0x10);
    if (!(bar0&1)) return -1;
    rtl_iobase = (uint16_t)(bar0 & 0xFFFC);

    uint32_t cmd = pci_read(bus,dev_n,0,0x04);
    pci_write(bus,dev_n,0,0x04, cmd|0x07);

    outb(rtl_iobase+RTL_CONFIG1, 0x00);
    outb(rtl_iobase+RTL_CMD, 0x10);
    while (inb(rtl_iobase+RTL_CMD)&0x10) {}

    for (int i=0;i<ETH_ALEN;i++) my_mac[i]=inb(rtl_iobase+RTL_MAC0+i);

    outl(rtl_iobase+RTL_RBSTART, (uint32_t)rx_buf);
    outw(rtl_iobase+RTL_IMR, 0x0005);
    outl(rtl_iobase+RTL_RCR, 0x0000008F);
    outl(rtl_iobase+RTL_TCR, 0x03000700);
    outb(rtl_iobase+RTL_CMD, 0x0C);

    for (int i=0;i<TX_BUFS;i++)
        outl(rtl_iobase+RTL_TSAD0+i*4, (uint32_t)tx_buf[i]);

    rtl_ready_flag = 1;
    return 0;
}

int  net_ready(void) { return rtl_ready_flag; }
void net_get_mac(uint8_t mac[ETH_ALEN]) { kmemcpy(mac, my_mac, ETH_ALEN); }
uint32_t net_get_ip(void) { return my_ip; }

int net_send_raw(const void *frame, uint16_t len) {
    if (!rtl_ready_flag || len > TX_BUF_SIZE) return -1;
    kmemcpy(tx_buf[tx_idx], frame, len);
    if (len < 60) { kmemset(tx_buf[tx_idx] + len, 0, 60u - len); len = 60; }
    outl(rtl_iobase+RTL_TSD0+tx_idx*4, len);
    tx_idx = (tx_idx+1) % TX_BUFS;
    int tries=10000;
    while(tries--) { if(inl(rtl_iobase+RTL_TSD0+((tx_idx+TX_BUFS-1)%TX_BUFS)*4)&0x8000) break; }
    tx_packets++;
    return 0;
}

void net_get_stats(uint32_t *tx, uint32_t *rx) {
    if (tx) *tx = tx_packets;
    if (rx) *rx = rx_packets;
}

static void arp_learn(uint32_t ip, const uint8_t mac[ETH_ALEN]) {
    for (int i=0;i<arp_count;i++) {
        uint32_t cached; kmemcpy(&cached, arp_cache_ip[i], 4);
        if (cached==ip) { kmemcpy(arp_cache_mac[i],mac,ETH_ALEN); return; }
    }
    if (arp_count<8) {
        kmemcpy(arp_cache_ip[arp_count],&ip,4);
        kmemcpy(arp_cache_mac[arp_count],mac,ETH_ALEN);
        arp_count++;
    }
}

static int arp_lookup(uint32_t ip, uint8_t mac_out[ETH_ALEN]) {
    for (int i=0;i<arp_count;i++) {
        uint32_t cached; kmemcpy(&cached,arp_cache_ip[i],4);
        if (cached==ip) { kmemcpy(mac_out,arp_cache_mac[i],ETH_ALEN); return 1; }
    }
    return 0;
}

static void send_arp_request(uint32_t target_ip) {
    uint8_t frame[42]; kmemset(frame,0,42);
    eth_hdr_t *eth=(eth_hdr_t*)frame;
    kmemset(eth->dst,0xFF,ETH_ALEN);
    kmemcpy(eth->src,my_mac,ETH_ALEN);
    eth->type=NET_HTONS(ETH_P_ARP);
    arp_pkt_t *arp=(arp_pkt_t*)(frame+14);
    arp->hw_type[0]=0; arp->hw_type[1]=1;
    arp->proto_type[0]=8; arp->proto_type[1]=0;
    arp->hw_len=6; arp->proto_len=4;
    arp->operation=NET_HTONS(1);
    kmemcpy(arp->sender_mac,my_mac,ETH_ALEN);
    arp->sender_ip=NET_HTONL(my_ip);
    kmemset(arp->target_mac,0,ETH_ALEN);
    arp->target_ip=NET_HTONL(target_ip);
    net_send_raw(frame,42);
}

static uint32_t next_hop(uint32_t dst_ip) {
    return ((dst_ip & net_mask) == (my_ip & net_mask)) ? dst_ip : gw_ip;
}

static void resolve_mac(uint32_t dst_ip, uint8_t mac[ETH_ALEN], int may_wait) {
    if (dst_ip == 0xFFFFFFFF) { kmemset(mac, 0xFF, ETH_ALEN); return; }
    uint32_t hop = next_hop(dst_ip);
    if (arp_lookup(hop, mac)) return;
    send_arp_request(hop);
    if (may_wait) {
        for (int i = 0; i < 10; i++) {
            timer_sleep(10);
            net_poll();
            if (arp_lookup(hop, mac)) return;
        }
    }
    kmemset(mac, 0xFF, ETH_ALEN);
}

void net_set_config(uint32_t ip, uint32_t gw, uint32_t mask) {
    if (ip)   my_ip = ip;
    if (gw)   gw_ip = gw;
    if (mask) net_mask = mask;
}

uint32_t net_get_gateway(void) { return gw_ip; }

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_hdr_t;

#define ICMP_ECHO_REQUEST  8
#define ICMP_ECHO_REPLY    0

static int      icmp_got = 0;
static uint32_t icmp_from_ip = 0;
static uint16_t icmp_reply_id = 0, icmp_reply_seq = 0;
static uint8_t  icmp_reply_ttl = 0;

int net_send_icmp_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                       const void *payload, uint16_t plen) {
    if (!rtl_ready_flag) return -1;
    uint8_t dst_mac[ETH_ALEN];
    resolve_mac(dst_ip, dst_mac, 1);
    if (plen > 1400) plen = 1400;
    uint16_t icmp_len = 8 + plen;
    uint16_t ip_len   = 20 + icmp_len;
    uint8_t  pkt[1500];
    eth_hdr_t *eth=(eth_hdr_t*)pkt;
    kmemcpy(eth->dst,dst_mac,ETH_ALEN);
    kmemcpy(eth->src,my_mac,ETH_ALEN);
    eth->type=NET_HTONS(ETH_P_IP);
    ip_hdr_t *ip=(ip_hdr_t*)(pkt+14);
    ip->version_ihl=0x45; ip->tos=0;
    ip->total_len=NET_HTONS(ip_len);
    ip->id=NET_HTONS(1); ip->flags_frag=0;
    ip->ttl=64; ip->protocol=IPPROTO_ICMP;
    ip->checksum=0;
    ip->src_ip=NET_HTONL(my_ip);
    ip->dst_ip=NET_HTONL(dst_ip);
    ip->checksum=ip_checksum(ip,20);
    icmp_hdr_t *icmp=(icmp_hdr_t*)(pkt+14+20);
    icmp->type=ICMP_ECHO_REQUEST; icmp->code=0;
    icmp->id=NET_HTONS(id); icmp->seq=NET_HTONS(seq);
    icmp->checksum=0;
    if (plen) kmemcpy(pkt+14+20+8,payload,plen);
    icmp->checksum=ip_checksum(icmp,icmp_len);
    return net_send_raw(pkt,(uint16_t)(14+ip_len));
}

int net_icmp_poll_reply(uint32_t *from_ip, uint16_t *id, uint16_t *seq, uint8_t *ttl) {
    net_poll();
    if (!icmp_got) return 0;
    icmp_got = 0;
    if (from_ip) *from_ip = icmp_from_ip;
    if (id)      *id      = icmp_reply_id;
    if (seq)     *seq     = icmp_reply_seq;
    if (ttl)     *ttl     = icmp_reply_ttl;
    return 1;
}

int net_send_udp(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                 const void *data, uint16_t data_len) {
    if (!rtl_ready_flag) return -1;
    if (data_len > 1472) return -1;
    uint8_t dst_mac[ETH_ALEN];
    resolve_mac(dst_ip, dst_mac, 1);
    uint16_t ip_len  = 20 + 8 + data_len;
    uint16_t udp_len = 8 + data_len;
    uint8_t  pkt[1500];
    eth_hdr_t *eth=(eth_hdr_t*)pkt;
    kmemcpy(eth->dst,dst_mac,ETH_ALEN);
    kmemcpy(eth->src,my_mac,ETH_ALEN);
    eth->type=NET_HTONS(ETH_P_IP);
    ip_hdr_t *ip=(ip_hdr_t*)(pkt+14);
    ip->version_ihl=0x45; ip->tos=0;
    ip->total_len=NET_HTONS(ip_len);
    ip->id=NET_HTONS(1); ip->flags_frag=0;
    ip->ttl=64; ip->protocol=IPPROTO_UDP;
    ip->checksum=0;
    ip->src_ip=NET_HTONL(my_ip);
    ip->dst_ip=NET_HTONL(dst_ip);
    ip->checksum=ip_checksum(ip,20);
    udp_hdr_t *udp=(udp_hdr_t*)(pkt+14+20);
    udp->src_port=NET_HTONS(src_port);
    udp->dst_port=NET_HTONS(dst_port);
    udp->length=NET_HTONS(udp_len);
    udp->checksum=0;
    kmemcpy(pkt+14+20+8,data,data_len);
    return net_send_raw(pkt,(uint16_t)(14+ip_len));
}

void net_poll(void) {
    if (!rtl_ready_flag) return;
    uint16_t isr = inw(rtl_iobase+RTL_ISR);
    if (isr) outw(rtl_iobase+RTL_ISR, isr);

    while (!(inb(rtl_iobase+RTL_CMD)&0x01)) {
        uint16_t off = inw(rtl_iobase+RTL_CAPR)+16;
        uint8_t *p = rx_buf + (off % (8192+16));
        uint16_t rx_status = *(uint16_t*)p;
        uint16_t pkt_len = *(uint16_t*)(p+2);
        if (!(rx_status & 1) || pkt_len < 18 || pkt_len > 1518) break;

        uint16_t frame_len = (uint16_t)(pkt_len >= 4 ? pkt_len - 4 : 0);
        uint8_t *payload = p+4;
        rx_packets++;
        eth_hdr_t *eth=(eth_hdr_t*)payload;
        uint16_t etype=NET_HTONS(eth->type);
        if (etype==ETH_P_ARP && frame_len>=14+sizeof(arp_pkt_t)) {
            arp_pkt_t *arp=(arp_pkt_t*)(payload+14);
            arp_learn(NET_HTONL(arp->sender_ip), arp->sender_mac);
        } else if (etype==ETH_P_IP && frame_len>=14+20) {
            ip_hdr_t *ip=(ip_hdr_t*)(payload+14);
            if (ip->protocol==IPPROTO_UDP && frame_len>=14+20+8) {
                if ((rx_head+1)%RX_RING_SIZE != rx_tail) {
                    kmemcpy(rx_ring[rx_head].buf, payload, frame_len);
                    rx_ring[rx_head].len = frame_len;
                    rx_head=(rx_head+1)%RX_RING_SIZE;
                }
            } else if (ip->protocol==IPPROTO_TCP && frame_len>=14+20+20) {
                tcp_process(payload, frame_len);
            } else if (ip->protocol==IPPROTO_ICMP && frame_len>=14+20+8) {
                icmp_hdr_t *icmp=(icmp_hdr_t*)(payload+14+20);
                if (icmp->type==ICMP_ECHO_REPLY) {
                    icmp_got = 1;
                    icmp_from_ip   = NET_HTONL(ip->src_ip);
                    icmp_reply_id  = NET_HTONS(icmp->id);
                    icmp_reply_seq = NET_HTONS(icmp->seq);
                    icmp_reply_ttl = ip->ttl;
                }
            }
        }
        uint16_t rx_ptr = (uint16_t)((off + pkt_len + 4 + 3) & ~3);
        outw(rtl_iobase+RTL_CAPR, (uint16_t)(rx_ptr-16));
    }
}

int net_recv_udp(uint16_t port, void *buf, uint16_t bufsz,
                 uint32_t *src_ip, uint16_t *src_port) {
    net_poll();
    if (rx_head == rx_tail) return 0;
    uint8_t *f = rx_ring[rx_tail].buf;
    uint16_t len = rx_ring[rx_tail].len;
    rx_tail=(rx_tail+1)%RX_RING_SIZE;
    if (len < 14+20+8) return 0;
    ip_hdr_t  *ip  = (ip_hdr_t*) (f+14);
    udp_hdr_t *udp = (udp_hdr_t*)(f+14+20);
    if (NET_HTONS(udp->dst_port) != port) return 0;
    uint16_t data_len = (uint16_t)(NET_HTONS(udp->length)-8);
    if (data_len > bufsz) data_len = (uint16_t)bufsz;
    kmemcpy(buf, f+14+20+8, data_len);
    if (src_ip)   *src_ip   = NET_HTONL(ip->src_ip);
    if (src_port) *src_port = NET_HTONS(udp->src_port);
    return data_len;
}

void net_print_info(void) {
    vga_set_color(VGA_YELLOW,VGA_BLACK);
    vga_puts("\n  === Network ===\n\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    if (!rtl_ready_flag) { vga_puts("  No NIC found.\n\n"); return; }
    vga_puts("  NIC:  RTL8139\n  MAC:  ");
    for (int i=0;i<ETH_ALEN;i++) {
        const char *h="0123456789ABCDEF";
        vga_putchar(h[my_mac[i]>>4]); vga_putchar(h[my_mac[i]&0xF]);
        if(i<5) vga_putchar(':');
    }
    vga_putchar('\n');
    char b[16];
    vga_puts("  IP:   "); net_ip_to_str(my_ip, b); vga_puts(b);
    vga_puts("\n  GW:   "); net_ip_to_str(gw_ip, b); vga_puts(b);
    vga_puts("\n\n");
}

#define TCP_MAX_SOCKETS  8
#define TCP_MSS          1460
#define TCP_BUF_SIZE     16384

typedef enum {
    TCP_CLOSED=0, TCP_SYN_SENT, TCP_SYN_RECV,
    TCP_ESTABLISHED, TCP_FIN_WAIT1, TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT, TCP_LAST_ACK, TCP_TIME_WAIT
} tcp_state_t;

typedef struct {
    tcp_state_t state;
    uint32_t    local_ip, remote_ip;
    uint16_t    local_port, remote_port;
    uint32_t    seq, ack;
    uint8_t     rbuf[TCP_BUF_SIZE];
    uint32_t    rbuf_head, rbuf_tail;
    int         used;
} tcp_socket_t;

static tcp_socket_t tcp_sockets[TCP_MAX_SOCKETS];
static uint16_t     tcp_next_port = 49152;

typedef struct {
    uint16_t src_port, dst_port;
    uint32_t seq, ack_seq;
    uint8_t  data_offset;
    uint8_t  flags;
    uint16_t window, checksum, urg_ptr;
} __attribute__((packed)) tcp_hdr_t;

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_ACK  0x10

static uint16_t tcp_checksum(uint32_t src_ip_net, uint32_t dst_ip_net,
                              const void *tcp_seg, uint16_t tcp_len) {
    uint8_t buf[12 + 1480];
    kmemcpy(buf+0, &src_ip_net, 4);
    kmemcpy(buf+4, &dst_ip_net, 4);
    buf[8] = 0; buf[9] = IPPROTO_TCP;
    uint16_t tlen_n = NET_HTONS(tcp_len);
    kmemcpy(buf+10, &tlen_n, 2);
    kmemcpy(buf+12, tcp_seg, tcp_len);
    return ip_checksum(buf, (uint32_t)(12 + tcp_len));
}

static void tcp_send_raw(tcp_socket_t *s, uint8_t flags,
                         const void *data, uint16_t dlen) {
    uint8_t pkt[1500]; kmemset(pkt,0,sizeof(pkt));
    uint16_t tcp_len = 20 + dlen;
    uint16_t ip_len  = 20 + tcp_len;

    if (dlen > TCP_MSS) dlen = TCP_MSS;
    uint8_t dst_mac[ETH_ALEN];
    resolve_mac(s->remote_ip, dst_mac, 0);

    eth_hdr_t *eth=(eth_hdr_t*)pkt;
    kmemcpy(eth->dst,dst_mac,ETH_ALEN);
    kmemcpy(eth->src,my_mac,ETH_ALEN);
    eth->type=NET_HTONS(ETH_P_IP);

    ip_hdr_t *ip=(ip_hdr_t*)(pkt+14);
    ip->version_ihl=0x45; ip->total_len=NET_HTONS(ip_len);
    ip->ttl=64; ip->protocol=IPPROTO_TCP;
    ip->src_ip=NET_HTONL(s->local_ip);
    ip->dst_ip=NET_HTONL(s->remote_ip);
    ip->checksum=ip_checksum(ip,20);

    tcp_hdr_t *tcp=(tcp_hdr_t*)(pkt+14+20);
    tcp->src_port = NET_HTONS(s->local_port);
    tcp->dst_port = NET_HTONS(s->remote_port);
    tcp->seq      = NET_HTONL(s->seq);
    tcp->ack_seq  = (flags&TCP_ACK)?NET_HTONL(s->ack):0;
    tcp->data_offset = 0x50;
    tcp->flags    = flags;
    {
        uint32_t used = (s->rbuf_head - s->rbuf_tail + TCP_BUF_SIZE) % TCP_BUF_SIZE;
        uint32_t free_space = TCP_BUF_SIZE - used - 1;
        tcp->window = NET_HTONS((uint16_t)free_space);
    }
    if (dlen) kmemcpy(pkt+14+20+20, data, dlen);
    tcp->checksum = 0;
    tcp->checksum = tcp_checksum(ip->src_ip, ip->dst_ip, tcp, tcp_len);
    net_send_raw(pkt,(uint16_t)(14+ip_len));

    if (flags&(TCP_SYN|TCP_FIN)) s->seq++;
    s->seq += dlen;
}

int tcp_connect(uint32_t dst_ip, uint16_t dst_port) {
    int s=-1;
    for (int i=0;i<TCP_MAX_SOCKETS;i++)
        if (!tcp_sockets[i].used) { s=i; break; }
    if (s<0) return -1;

    kmemset(&tcp_sockets[s],0,sizeof(tcp_socket_t));
    tcp_sockets[s].used=1;
    tcp_sockets[s].local_ip   = my_ip;
    tcp_sockets[s].remote_ip  = dst_ip;
    tcp_sockets[s].local_port = tcp_next_port++;
    tcp_sockets[s].remote_port= dst_port;
    tcp_sockets[s].seq        = 0x1000;
    tcp_sockets[s].state      = TCP_SYN_SENT;

    uint8_t mac[ETH_ALEN];
    resolve_mac(dst_ip, mac, 1);
    tcp_send_raw(&tcp_sockets[s], TCP_SYN, 0, 0);

    for (int tries=0; tries<500; tries++) {
        net_poll();
        if (tcp_sockets[s].state==TCP_ESTABLISHED) return s;
        timer_sleep(20);
    }
    tcp_sockets[s].used=0;
    return -1;
}

int tcp_send(int s, const void *data, uint16_t len) {
    if (s<0||s>=TCP_MAX_SOCKETS||!tcp_sockets[s].used) return -1;
    if (tcp_sockets[s].state!=TCP_ESTABLISHED) return -1;
    if (!len) { tcp_send_raw(&tcp_sockets[s], TCP_ACK, 0, 0); return 0; }
    const uint8_t *p = (const uint8_t *)data;
    uint16_t left = len;
    while (left) {
        uint16_t chunk = left > TCP_MSS ? TCP_MSS : left;
        tcp_send_raw(&tcp_sockets[s], TCP_ACK, p, chunk);
        p += chunk;
        left = (uint16_t)(left - chunk);
    }
    return len;
}

int tcp_recv(int s, void *buf, uint16_t len) {
    if (s<0||s>=TCP_MAX_SOCKETS||!tcp_sockets[s].used) return -1;
    tcp_socket_t *sock=&tcp_sockets[s];
    uint32_t avail=(sock->rbuf_head-sock->rbuf_tail+TCP_BUF_SIZE)%TCP_BUF_SIZE;
    if (avail==0) return 0;
    if (len>avail) len=(uint16_t)avail;
    for (uint16_t i=0;i<len;i++) {
        ((uint8_t*)buf)[i]=sock->rbuf[sock->rbuf_tail];
        sock->rbuf_tail=(sock->rbuf_tail+1)%TCP_BUF_SIZE;
    }
    return len;
}

void tcp_close(int s) {
    if (s<0||s>=TCP_MAX_SOCKETS||!tcp_sockets[s].used) return;
    tcp_send_raw(&tcp_sockets[s], TCP_FIN|TCP_ACK, 0, 0);
    tcp_sockets[s].state=TCP_FIN_WAIT1;
    timer_sleep(100);
    tcp_sockets[s].used=0;
}

void tcp_process(uint8_t *frame, uint16_t len) {
    if (len < 14+20+20) return;
    ip_hdr_t  *ip  = (ip_hdr_t*)(frame+14);
    tcp_hdr_t *tcp = (tcp_hdr_t*)(frame+14+20);

    uint32_t src_ip   = NET_HTONL(ip->src_ip);
    uint16_t src_port = NET_HTONS(tcp->src_port);
    uint16_t dst_port = NET_HTONS(tcp->dst_port);
    uint32_t seq      = NET_HTONL(tcp->seq);
    uint32_t ack_seq  = NET_HTONL(tcp->ack_seq);

    for (int i=0;i<TCP_MAX_SOCKETS;i++) {
        tcp_socket_t *s=&tcp_sockets[i];
        if (!s->used||s->remote_ip!=src_ip||s->remote_port!=src_port
            ||s->local_port!=dst_port) continue;

        if (s->state==TCP_SYN_SENT && (tcp->flags&(TCP_SYN|TCP_ACK))==(TCP_SYN|TCP_ACK)) {
            s->ack = seq+1;
            s->seq = ack_seq;
            s->state = TCP_ESTABLISHED;
            tcp_send_raw(s, TCP_ACK, 0, 0);
            return;
        }
        if (s->state==TCP_ESTABLISHED) {
            uint8_t doff = (uint8_t)((tcp->data_offset>>4)*4);

            uint16_t ip_total = NET_HTONS(ip->total_len);
            uint16_t hdrs = (uint16_t)(20 + doff);
            uint16_t data_len = (ip_total > hdrs) ? (uint16_t)(ip_total - hdrs) : 0;
            if (data_len>0) {
                uint32_t seg_end = seq + data_len;
                if (seq <= s->ack && seg_end > s->ack) {
                    uint8_t *data = frame+14+20+doff;
                    uint32_t skip = s->ack - seq;
                    uint32_t newlen = seg_end - s->ack;
                    uint32_t space = TCP_BUF_SIZE - ((s->rbuf_head-s->rbuf_tail+TCP_BUF_SIZE)%TCP_BUF_SIZE) - 1;
                    uint32_t take = (newlen > space) ? space : newlen;
                    for (uint32_t j=0;j<take;j++) {
                        s->rbuf[s->rbuf_head] = data[skip+j];
                        s->rbuf_head=(s->rbuf_head+1)%TCP_BUF_SIZE;
                    }
                    s->ack += take;
                }

                tcp_send_raw(s, TCP_ACK, 0, 0);
            }
            if (tcp->flags&TCP_FIN) {
                s->ack++;
                tcp_send_raw(s, TCP_ACK, 0, 0);
                s->state=TCP_CLOSE_WAIT;
                tcp_send_raw(s, TCP_FIN|TCP_ACK, 0, 0);
                s->state=TCP_LAST_ACK;
            }
        }
        return;
    }
}

void net_ip_to_str(uint32_t ip, char *out) {
    int p = 0;
    for (int i = 3; i >= 0; i--) {
        uint32_t o = (ip >> (i * 8)) & 0xFF;
        if (o >= 100) out[p++] = (char)('0' + o / 100);
        if (o >= 10)  out[p++] = (char)('0' + (o / 10) % 10);
        out[p++] = (char)('0' + o % 10);
        if (i) out[p++] = '.';
    }
    out[p] = 0;
}
