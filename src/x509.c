#include "x509.h"

typedef struct {
    uint8_t tag;
    uint32_t len;
    const uint8_t *content;
    const uint8_t *next;
} asn1_tlv_t;

static int asn1_parse_tlv(const uint8_t *p, const uint8_t *end, asn1_tlv_t *tlv) {
    if (p >= end) return -1;
    tlv->tag = *p++;
    if (p >= end) return -1;

    uint32_t len;
    uint8_t first = *p++;
    if (first & 0x80) {
        int nbytes = first & 0x7F;
        if (nbytes == 0 || nbytes > 4 || p + nbytes > end) return -1;
        len = 0;
        for (int i = 0; i < nbytes; i++) len = (len << 8) | *p++;
    } else {
        len = first;
    }
    if (p + len > end || p + len < p) return -1;

    tlv->len = len;
    tlv->content = p;
    tlv->next = p + len;
    return 0;
}

int x509_extract_rsa_pubkey(const uint8_t *cert_der, uint32_t cert_len,
                             bignum_t *modulus, uint32_t *exponent) {
    const uint8_t *end = cert_der + cert_len;
    asn1_tlv_t cert, tbs;

    if (asn1_parse_tlv(cert_der, end, &cert) != 0 || cert.tag != 0x30) return -1;
    if (asn1_parse_tlv(cert.content, cert.next, &tbs) != 0 || tbs.tag != 0x30) return -1;

    const uint8_t *p = tbs.content, *pend = tbs.next;
    asn1_tlv_t field;

    if (asn1_parse_tlv(p, pend, &field) != 0) return -1;
    if (field.tag == 0xA0) { p = field.next; if (asn1_parse_tlv(p, pend, &field) != 0) return -1; }
    p = field.next;
    if (asn1_parse_tlv(p, pend, &field) != 0) return -1; p = field.next;
    if (asn1_parse_tlv(p, pend, &field) != 0) return -1; p = field.next;
    if (asn1_parse_tlv(p, pend, &field) != 0) return -1; p = field.next;
    if (asn1_parse_tlv(p, pend, &field) != 0) return -1; p = field.next;
    if (asn1_parse_tlv(p, pend, &field) != 0 || field.tag != 0x30) return -1;

    asn1_tlv_t alg, pubkey_bits;
    if (asn1_parse_tlv(field.content, field.next, &alg) != 0) return -1;
    if (asn1_parse_tlv(alg.next, field.next, &pubkey_bits) != 0 || pubkey_bits.tag != 0x03) return -1;
    if (pubkey_bits.len < 1 || pubkey_bits.content[0] != 0x00) return -1;

    const uint8_t *rsa_der = pubkey_bits.content + 1;
    uint32_t rsa_der_len = pubkey_bits.len - 1;

    asn1_tlv_t rsa_seq;
    if (asn1_parse_tlv(rsa_der, rsa_der + rsa_der_len, &rsa_seq) != 0 || rsa_seq.tag != 0x30) return -1;

    asn1_tlv_t mod_tlv, exp_tlv;
    if (asn1_parse_tlv(rsa_seq.content, rsa_seq.next, &mod_tlv) != 0 || mod_tlv.tag != 0x02) return -1;
    if (asn1_parse_tlv(mod_tlv.next, rsa_seq.next, &exp_tlv) != 0 || exp_tlv.tag != 0x02) return -1;

    const uint8_t *mod_bytes = mod_tlv.content;
    uint32_t mod_len = mod_tlv.len;
    if (mod_len > 1 && mod_bytes[0] == 0x00) { mod_bytes++; mod_len--; }
    if (mod_len > BN_LIMBS * 4) return -1;

    bn_from_be_bytes(modulus, mod_bytes, (int)mod_len);

    uint32_t e = 0;
    for (uint32_t i = 0; i < exp_tlv.len; i++) e = (e << 8) | exp_tlv.content[i];
    *exponent = e;

    return 0;
}
