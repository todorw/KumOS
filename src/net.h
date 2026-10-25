#ifndef NET_H
#define NET_H

#include <stdint.h>

#define ETH_ALEN       6
#define ETH_P_IP       0x0800
#define ETH_P_ARP      0x0806

#define IPPROTO_ICMP   1
#define IPPROTO_UDP    17
#define IPPROTO_TCP    6

typedef struct {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    uint8_t  version_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed)) ip_hdr_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_hdr_t;

typedef struct {
    uint8_t  hw_type[2];
    uint8_t  proto_type[2];
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t operation;
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;
} __attribute__((packed)) arp_pkt_t;

int      net_init(void);
int      net_ready(void);
void     net_get_mac(uint8_t mac[ETH_ALEN]);
uint32_t net_get_ip(void);

int  net_send_udp(uint32_t dst_ip, uint16_t src_port,
                  uint16_t dst_port, const void *data, uint16_t len);
int  net_recv_udp(uint16_t port, void *buf, uint16_t bufsz,
                  uint32_t *src_ip, uint16_t *src_port);

int  net_send_icmp_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                        const void *payload, uint16_t plen);
int  net_icmp_poll_reply(uint32_t *from_ip, uint16_t *id, uint16_t *seq, uint8_t *ttl);

int  net_send_raw(const void *frame, uint16_t len);
void net_poll(void);
void net_print_info(void);
void net_get_stats(uint32_t *tx_packets, uint32_t *rx_packets);
void net_set_config(uint32_t ip, uint32_t gw, uint32_t mask);
uint32_t net_get_gateway(void);
void net_ip_to_str(uint32_t ip, char *out);

#define NET_IP(a,b,c,d)  (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))
#define NET_HTONS(x)     ((uint16_t)(((x)>>8)|((x)<<8)))
#define NET_HTONL(x)     ((uint32_t)(((uint32_t)(x)>>24)|(((uint32_t)(x)>>8)&0xFF00)|(((uint32_t)(x)<<8)&0xFF0000)|((uint32_t)(x)<<24)))

#endif

int  tcp_connect(uint32_t dst_ip, uint16_t dst_port);
int  tcp_send(int sock, const void *data, uint16_t len);
int  tcp_recv(int sock, void *buf, uint16_t len);
void tcp_close(int sock);
void tcp_process(uint8_t *frame, uint16_t len);
