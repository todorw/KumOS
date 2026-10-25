#ifndef X509_H
#define X509_H
#include <stdint.h>
#include "bignum.h"

int x509_extract_rsa_pubkey(const uint8_t *cert_der, uint32_t cert_len,
                             bignum_t *modulus, uint32_t *exponent);

#endif
