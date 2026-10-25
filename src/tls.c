#include "tls.h"
#include "net.h"
#include "kstring.h"
#include "timer.h"
#include "rtc.h"
#include "sha1.h"
#include "x509.h"
#include <stdint.h>

#define TLS_CIPHER_SUITE 0x002F

#if defined(TLS_DEBUG_HOST)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#elif defined(TLS_DEBUG)
#include "serial.h"
#define DBG(...) serial_printf(__VA_ARGS__)
#else
#define DBG(...)
#endif

static uint8_t  prng_pool[32];
static int      prng_ready = 0;
static uint32_t prng_counter = 0;

static void tls_random_bytes(uint8_t *out, uint32_t len) {
    if (!prng_ready) {
        uint32_t seed1 = timer_ticks();
        rtc_time_t t = rtc_read();
        uint32_t seed2 = ((uint32_t)t.hour*3600u + (uint32_t)t.minute*60u + t.second) ^ (uint32_t)t.year*31u;
        kmemcpy(prng_pool, &seed1, 4);
        kmemcpy(prng_pool+4, &seed2, 4);
        for (int i = 8; i < 32; i++) prng_pool[i] = (uint8_t)(i*31 + 7);
        prng_ready = 1;
    }
    while (len > 0) {
        prng_counter++;
        uint8_t buf[36];
        kmemcpy(buf, prng_pool, 32);
        kmemcpy(buf+32, &prng_counter, 4);
        uint8_t digest[32];
        sha256(buf, 36, digest);
        kmemcpy(prng_pool, digest, 32);
        uint32_t take = (len < 32) ? len : 32;
        kmemcpy(out, digest, take);
        out += take; len -= take;
    }
}

static void tls_prf(const uint8_t *secret, uint32_t secret_len,
                     const char *label, const uint8_t *seed, uint32_t seed_len,
                     uint8_t *out, uint32_t out_len) {
    uint8_t label_seed[256];
    uint32_t label_len = (uint32_t)kstrlen(label);
    kmemcpy(label_seed, label, label_len);
    kmemcpy(label_seed + label_len, seed, seed_len);
    uint32_t ls_len = label_len + seed_len;

    uint8_t a[32];
    hmac_sha256(secret, secret_len, label_seed, ls_len, a);

    uint32_t produced = 0;
    while (produced < out_len) {
        uint8_t buf[256+32];
        kmemcpy(buf, a, 32);
        kmemcpy(buf+32, label_seed, ls_len);
        uint8_t chunk[32];
        hmac_sha256(secret, secret_len, buf, 32+ls_len, chunk);

        uint32_t take = (out_len - produced < 32) ? (out_len - produced) : 32;
        kmemcpy(out+produced, chunk, take);
        produced += take;

        uint8_t next_a[32];
        hmac_sha256(secret, secret_len, a, 32, next_a);
        kmemcpy(a, next_a, 32);
    }
}

static int tls_io_read(tls_conn_t *c, uint8_t *out, uint32_t len) {
    while (c->io_len < len) {
        if (c->io_start > 0) {
            for (uint32_t i = 0; i < c->io_len; i++) c->iobuf[i] = c->iobuf[c->io_start+i];
            c->io_start = 0;
        }
        int idle = 0;
        int n;
        for (;;) {
            net_poll();
            n = tcp_recv(c->sock, c->iobuf + c->io_len, (uint16_t)(TLS_RECVBUF_SIZE - c->io_len));
            if (n > 0) break;
            idle++;
            if (idle > 500) return -1;
            timer_sleep(10);
        }
        DBG("[tls] io_read got n=%d io_len now=%u first8: %x %x %x %x %x %x %x %x\n",
            n, c->io_len + (uint32_t)n,
            c->iobuf[c->io_len+0], c->iobuf[c->io_len+1], c->iobuf[c->io_len+2], c->iobuf[c->io_len+3],
            c->iobuf[c->io_len+4], c->iobuf[c->io_len+5], c->iobuf[c->io_len+6], c->iobuf[c->io_len+7]);
        c->io_len += (uint32_t)n;

        tcp_send(c->sock, 0, 0);
    }
    kmemcpy(out, c->iobuf + c->io_start, len);
    c->io_start += len;
    c->io_len -= len;
    return 0;
}

static int tls_send_record(tls_conn_t *c, uint8_t content_type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[5] = { content_type, 0x03, 0x03, (uint8_t)(len>>8), (uint8_t)len };
    if (tcp_send(c->sock, hdr, 5) < 0) return -1;
    if (len && tcp_send(c->sock, data, (uint16_t)len) < 0) return -1;
    return 0;
}

static int tls_send_handshake(tls_conn_t *c, uint8_t msg_type, const uint8_t *body, uint32_t body_len) {
    uint8_t hdr[4] = { msg_type, (uint8_t)(body_len>>16), (uint8_t)(body_len>>8), (uint8_t)body_len };
    sha256_update(&c->transcript, hdr, 4);
    sha256_update(&c->transcript, body, body_len);

    static uint8_t msg[4096];
    kmemcpy(msg, hdr, 4);
    kmemcpy(msg+4, body, body_len);
    return tls_send_record(c, 22, msg, 4+body_len);
}

static int tls_read_record_raw(tls_conn_t *c, uint8_t *content_type, uint8_t *out, uint32_t *out_len, uint32_t max_len) {
    uint8_t hdr[5];
    if (tls_io_read(c, hdr, 5) != 0) { DBG("[tls] io_read hdr failed\n"); return -1; }
    DBG("[tls] raw hdr bytes: %x %x %x %x %x\n", hdr[0], hdr[1], hdr[2], hdr[3], hdr[4]);
    *content_type = hdr[0];
    uint32_t rec_len = ((uint32_t)hdr[3]<<8) | hdr[4];
    DBG("[tls] record hdr: type=%d ver=%d.%d len=%u\n", hdr[0], hdr[1], hdr[2], rec_len);
    if (rec_len > TLS_RECVBUF_SIZE - 64) { DBG("[tls] rec_len too big\n"); return -1; }

    static uint8_t raw[TLS_RECVBUF_SIZE];
    if (tls_io_read(c, raw, rec_len) != 0) { DBG("[tls] io_read body failed (rec_len=%u)\n", rec_len); return -1; }

    if (!c->read_encrypted || *content_type == 20) {
        if (rec_len > max_len) return -1;
        kmemcpy(out, raw, rec_len);
        *out_len = rec_len;
        return 0;
    }

    if (rec_len < 16 + 20 + 1) return -1;
    uint8_t iv[16];
    kmemcpy(iv, raw, 16);
    uint32_t enc_len = rec_len - 16;

    static uint8_t plain[TLS_RECVBUF_SIZE];
    aes128_cbc_decrypt(&c->server_enc, iv, raw+16, plain, enc_len);

    uint8_t pad = plain[enc_len-1];
    if ((uint32_t)pad + 1 > enc_len) return -1;
    uint32_t unpadded_len = enc_len - (pad+1);
    if (unpadded_len < 20) return -1;
    uint32_t data_len = unpadded_len - 20;
    uint8_t *mac_recv = plain + data_len;

    static uint8_t macbuf[TLS_RECVBUF_SIZE];
    uint32_t mp = 0;
    for (int i = 7; i >= 0; i--) macbuf[mp++] = (uint8_t)(c->server_seq >> (8*i));
    macbuf[mp++] = *content_type;
    macbuf[mp++] = 0x03; macbuf[mp++] = 0x03;
    macbuf[mp++] = (uint8_t)(data_len>>8); macbuf[mp++] = (uint8_t)data_len;
    kmemcpy(macbuf+mp, plain, data_len); mp += data_len;

    uint8_t mac_calc[20];
    hmac_sha1(c->server_mac_key, 20, macbuf, mp, mac_calc);

    int match = 1;
    for (int i = 0; i < 20; i++) if (mac_calc[i] != mac_recv[i]) match = 0;
    if (!match) return -1;

    c->server_seq++;

    if (data_len > max_len) return -1;
    kmemcpy(out, plain, data_len);
    *out_len = data_len;
    return 0;
}

static int tls_send_encrypted(tls_conn_t *c, uint8_t content_type, const uint8_t *data, uint32_t len) {
    static uint8_t macbuf[TLS_RECVBUF_SIZE];
    uint32_t mp = 0;
    for (int i = 7; i >= 0; i--) macbuf[mp++] = (uint8_t)(c->client_seq >> (8*i));
    macbuf[mp++] = content_type;
    macbuf[mp++] = 0x03; macbuf[mp++] = 0x03;
    macbuf[mp++] = (uint8_t)(len>>8); macbuf[mp++] = (uint8_t)len;
    kmemcpy(macbuf+mp, data, len); mp += len;

    uint8_t mac[20];
    hmac_sha1(c->client_mac_key, 20, macbuf, mp, mac);

    static uint8_t plain[TLS_RECVBUF_SIZE];
    kmemcpy(plain, data, len);
    kmemcpy(plain+len, mac, 20);
    uint32_t base = len + 20;
    uint8_t pad_value = (uint8_t)(15 - (base % 16));
    uint32_t total_pad = (uint32_t)pad_value + 1;
    for (uint32_t i = 0; i < total_pad; i++) plain[base+i] = pad_value;
    uint32_t plain_len = base + total_pad;

    uint8_t iv[16];
    tls_random_bytes(iv, 16);

    static uint8_t record[TLS_RECVBUF_SIZE];
    kmemcpy(record, iv, 16);
    aes128_cbc_encrypt(&c->client_enc, iv, plain, record+16, plain_len);

    uint32_t rec_total = 16 + plain_len;
    if (tls_send_record(c, content_type, record, rec_total) != 0) return -1;
    c->client_seq++;
    return 0;
}

typedef struct { uint8_t buf[16384]; uint32_t len, pos; } hs_reader_t;

static int hs_reader_read_msg(tls_conn_t *c, hs_reader_t *r, uint8_t *msg_type,
                               uint8_t *body, uint32_t *body_len, uint32_t max_body) {
    if (r->pos >= r->len) {
        uint8_t ctype;
        if (tls_read_record_raw(c, &ctype, r->buf, &r->len, sizeof(r->buf)) != 0) return -1;
        if (ctype != 22) return -1;
        r->pos = 0;
    }
    if (r->pos + 4 > r->len) return -1;
    *msg_type = r->buf[r->pos];
    uint32_t blen = ((uint32_t)r->buf[r->pos+1]<<16) | ((uint32_t)r->buf[r->pos+2]<<8) | r->buf[r->pos+3];
    if (blen > max_body || r->pos + 4 + blen > r->len) return -1;

    kmemcpy(body, r->buf + r->pos + 4, blen);
    *body_len = blen;

    sha256_update(&c->transcript, r->buf + r->pos, 4 + blen);

    r->pos += 4 + blen;
    return 0;
}

int tls_connect(tls_conn_t *c, uint32_t dst_ip, uint16_t dst_port, const char *sni_hostname) {
    kmemset(c, 0, sizeof(*c));
    c->sock = tcp_connect(dst_ip, dst_port);
    DBG("tcp_connect -> %d\n", c->sock);
    if (c->sock < 0) return -1;

    sha256_init(&c->transcript);

    uint8_t client_random[32];
    tls_random_bytes(client_random, 32);

    static uint8_t body[1024];
    uint32_t bp = 0;
    body[bp++] = 0x03; body[bp++] = 0x03;
    kmemcpy(body+bp, client_random, 32); bp += 32;
    body[bp++] = 0x00;
    body[bp++] = 0x00; body[bp++] = 0x02;
    body[bp++] = (uint8_t)(TLS_CIPHER_SUITE>>8); body[bp++] = (uint8_t)TLS_CIPHER_SUITE;
    body[bp++] = 0x01;
    body[bp++] = 0x00;

    uint32_t ext_len_pos = bp;
    body[bp++] = 0; body[bp++] = 0;
    uint32_t ext_start = bp;

    if (sni_hostname && *sni_hostname) {
        uint32_t hn_len = (uint32_t)kstrlen(sni_hostname);
        body[bp++] = 0x00; body[bp++] = 0x00;
        uint32_t ext_body_len = 2+1+2+hn_len;
        body[bp++] = (uint8_t)(ext_body_len>>8); body[bp++] = (uint8_t)ext_body_len;
        uint32_t list_len = 1+2+hn_len;
        body[bp++] = (uint8_t)(list_len>>8); body[bp++] = (uint8_t)list_len;
        body[bp++] = 0x00;
        body[bp++] = (uint8_t)(hn_len>>8); body[bp++] = (uint8_t)hn_len;
        kmemcpy(body+bp, sni_hostname, hn_len); bp += hn_len;
    }

    uint32_t ext_total = bp - ext_start;
    if (ext_total == 0) bp = ext_len_pos;
    else { body[ext_len_pos] = (uint8_t)(ext_total>>8); body[ext_len_pos+1] = (uint8_t)ext_total; }

    if (tls_send_handshake(c, 0x01, body, bp) != 0) { tcp_close(c->sock); return -1; }
    DBG("sent ClientHello (%u bytes)\n", bp);

    static hs_reader_t r; r.len = 0; r.pos = 0;
    uint8_t msg_type;
    static uint8_t msgbuf[16384];
    uint32_t msg_len;

    uint8_t server_random[32];

    if (hs_reader_read_msg(c, &r, &msg_type, msgbuf, &msg_len, sizeof(msgbuf)) != 0) { DBG("failed reading ServerHello\n"); tcp_close(c->sock); return -1; }
    DBG("got msg type=%d len=%u (expect 2=ServerHello)\n", msg_type, msg_len);
    if (msg_type != 0x02 || msg_len < 2+32+1+2+1) { tcp_close(c->sock); return -1; }
    kmemcpy(server_random, msgbuf+2, 32);
    uint32_t sp = 2+32;
    uint8_t sid_len = msgbuf[sp]; sp += 1 + sid_len;
    uint16_t chosen_cipher = (uint16_t)(((uint16_t)msgbuf[sp]<<8) | msgbuf[sp+1]);
    DBG("chosen_cipher=%x expect %x\n", chosen_cipher, TLS_CIPHER_SUITE);
    if (chosen_cipher != TLS_CIPHER_SUITE) { tcp_close(c->sock); return -1; }

    if (hs_reader_read_msg(c, &r, &msg_type, msgbuf, &msg_len, sizeof(msgbuf)) != 0) { DBG("failed reading Certificate\n"); tcp_close(c->sock); return -1; }
    DBG("got msg type=%d len=%u (expect 11=Certificate)\n", msg_type, msg_len);
    if (msg_type != 0x0B || msg_len < 6) { tcp_close(c->sock); return -1; }
    uint32_t first_cert_len = ((uint32_t)msgbuf[3]<<16) | ((uint32_t)msgbuf[4]<<8) | msgbuf[5];
    DBG("first_cert_len=%u\n", first_cert_len);
    if (6 + first_cert_len > msg_len) { tcp_close(c->sock); return -1; }

    bignum_t server_mod; uint32_t server_exp;
    if (x509_extract_rsa_pubkey(msgbuf+6, first_cert_len, &server_mod, &server_exp) != 0) {
        DBG("x509 parse failed\n");
        tcp_close(c->sock); return -1;
    }
    DBG("x509 parsed ok, exponent=%u\n", server_exp);

    if (hs_reader_read_msg(c, &r, &msg_type, msgbuf, &msg_len, sizeof(msgbuf)) != 0) { DBG("failed reading ServerHelloDone\n"); tcp_close(c->sock); return -1; }
    DBG("got msg type=%d len=%u (expect 14=ServerHelloDone)\n", msg_type, msg_len);
    if (msg_type != 0x0E) { tcp_close(c->sock); return -1; }

    uint8_t pre_master[48];
    pre_master[0] = 0x03; pre_master[1] = 0x03;
    tls_random_bytes(pre_master+2, 46);

    int mod_len = bn_byte_len(&server_mod);
    if (mod_len < 64 || mod_len > 512) { tcp_close(c->sock); return -1; }

    static uint8_t em[512];
    em[0] = 0x00; em[1] = 0x02;
    int ps_len = mod_len - 3 - 48;
    if (ps_len < 8) { tcp_close(c->sock); return -1; }
    for (int i = 0; i < ps_len; i++) {
        uint8_t b;
        do { tls_random_bytes(&b, 1); } while (b == 0);
        em[2+i] = b;
    }
    em[2+ps_len] = 0x00;
    kmemcpy(em+3+ps_len, pre_master, 48);

    bignum_t em_bn, enc_bn;
    bn_from_be_bytes(&em_bn, em, mod_len);
    bn_modexp_u32(&em_bn, server_exp, &server_mod, &enc_bn);

    static uint8_t cke_body[2+512];
    cke_body[0] = (uint8_t)(mod_len>>8); cke_body[1] = (uint8_t)mod_len;
    bn_to_be_bytes(&enc_bn, cke_body+2, mod_len);

    DBG("mod_len=%d, sending ClientKeyExchange\n", mod_len);
    if (tls_send_handshake(c, 0x10, cke_body, (uint32_t)(2+mod_len)) != 0) { tcp_close(c->sock); return -1; }
    DBG("sent ClientKeyExchange\n");

    uint8_t seed_cs[64];
    kmemcpy(seed_cs, client_random, 32);
    kmemcpy(seed_cs+32, server_random, 32);

    uint8_t master_secret[48];
    tls_prf(pre_master, 48, "master secret", seed_cs, 64, master_secret, 48);

    uint8_t seed_sc[64];
    kmemcpy(seed_sc, server_random, 32);
    kmemcpy(seed_sc+32, client_random, 32);

    uint8_t key_block[2*20 + 2*16];
    tls_prf(master_secret, 48, "key expansion", seed_sc, 64, key_block, sizeof(key_block));

    kmemcpy(c->client_mac_key, key_block, 20);
    kmemcpy(c->server_mac_key, key_block+20, 20);
    aes128_init(&c->client_enc, key_block+40);
    aes128_init(&c->server_enc, key_block+56);

    uint8_t ccs = 0x01;
    if (tls_send_record(c, 20, &ccs, 1) != 0) { tcp_close(c->sock); return -1; }
    c->established = 1;

    uint8_t transcript_hash[32];
    {
        sha256_ctx_t snapshot;
        kmemcpy(&snapshot, &c->transcript, sizeof(snapshot));
        sha256_final(&snapshot, transcript_hash);
    }
    uint8_t verify_data[12];
    tls_prf(master_secret, 48, "client finished", transcript_hash, 32, verify_data, 12);

    uint8_t fin_hdr[4] = { 0x14, 0, 0, 12 };
    sha256_update(&c->transcript, fin_hdr, 4);
    sha256_update(&c->transcript, verify_data, 12);

    uint8_t fin_msg[16];
    kmemcpy(fin_msg, fin_hdr, 4);
    kmemcpy(fin_msg+4, verify_data, 12);
    if (tls_send_encrypted(c, 22, fin_msg, 16) != 0) { tcp_close(c->sock); return -1; }
    DBG("sent client Finished\n");

    uint8_t ctype;
    uint32_t n;
    static uint8_t tmp[512];
    if (tls_read_record_raw(c, &ctype, tmp, &n, sizeof(tmp)) != 0) { DBG("failed reading server CCS\n"); tcp_close(c->sock); return -1; }
    DBG("got record ctype=%d n=%u (expect 20=CCS, n=1)\n", ctype, n);
    if (ctype != 20 || n != 1 || tmp[0] != 0x01) { tcp_close(c->sock); return -1; }
    c->read_encrypted = 1;

    uint8_t expected_hash[32];
    {
        sha256_ctx_t snapshot;
        kmemcpy(&snapshot, &c->transcript, sizeof(snapshot));
        sha256_final(&snapshot, expected_hash);
    }
    uint8_t expected_verify[12];
    tls_prf(master_secret, 48, "server finished", expected_hash, 32, expected_verify, 12);

    if (tls_read_record_raw(c, &ctype, tmp, &n, sizeof(tmp)) != 0) { DBG("failed reading server Finished\n"); tcp_close(c->sock); return -1; }
    DBG("got record ctype=%d n=%u (expect 22=handshake, n=16)\n", ctype, n);
    if (ctype != 22 || n != 16 || tmp[0] != 0x14) { tcp_close(c->sock); return -1; }

    int fin_match = 1;
    for (int i = 0; i < 12; i++) if (tmp[4+i] != expected_verify[i]) fin_match = 0;
    DBG("finished verify match: %d\n", fin_match);
    if (!fin_match) { tcp_close(c->sock); return -1; }

    return 0;
}

int tls_send(tls_conn_t *c, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t*)data;
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > 16000) chunk = 16000;
        if (tls_send_encrypted(c, 23, p+sent, chunk) != 0) return -1;
        sent += chunk;
    }
    return (int)sent;
}

int tls_recv(tls_conn_t *c, void *buf, uint32_t maxlen) {
    uint8_t ctype;
    uint32_t n;
    for (;;) {
        if (tls_read_record_raw(c, &ctype, (uint8_t*)buf, &n, maxlen) != 0) return -1;
        if (ctype == 23) return (int)n;
        if (ctype == 21) return -1;
    }
}

void tls_close(tls_conn_t *c) {
    if (c->sock >= 0) tcp_close(c->sock);
}
