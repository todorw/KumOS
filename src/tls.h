#ifndef TLS_H
#define TLS_H
#include <stdint.h>
#include "bignum.h"
#include "aes.h"
#include "sha256.h"

#define TLS_RECVBUF_SIZE 16640

typedef struct {
    int      sock;
    int      established;
    int      read_encrypted;
    uint8_t  client_mac_key[20];
    uint8_t  server_mac_key[20];
    aes128_ctx_t client_enc;
    aes128_ctx_t server_enc;
    uint64_t client_seq;
    uint64_t server_seq;
    sha256_ctx_t transcript;

    uint8_t  iobuf[TLS_RECVBUF_SIZE];
    uint32_t io_start, io_len;
} tls_conn_t;

int  tls_connect(tls_conn_t *c, uint32_t dst_ip, uint16_t dst_port, const char *sni_hostname);
int  tls_send(tls_conn_t *c, const void *data, uint32_t len);
int  tls_recv(tls_conn_t *c, void *buf, uint32_t maxlen);
void tls_close(tls_conn_t *c);

#endif
