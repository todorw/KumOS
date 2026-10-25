#include "bignum.h"

void bn_zero(bignum_t *a) {
    for (int i = 0; i < BN_LIMBS; i++) a->d[i] = 0;
}

void bn_from_u32(bignum_t *a, uint32_t v) {
    bn_zero(a);
    a->d[0] = v;
}

void bn_from_be_bytes(bignum_t *a, const uint8_t *bytes, int len) {
    bn_zero(a);
    int limb = 0, shift = 0;
    for (int i = len - 1; i >= 0; i--) {
        a->d[limb] |= ((uint32_t)bytes[i]) << shift;
        shift += 8;
        if (shift == 32) { shift = 0; limb++; if (limb >= BN_LIMBS) break; }
    }
}

int bn_to_be_bytes(const bignum_t *a, uint8_t *out, int out_len) {
    for (int i = 0; i < out_len; i++) {
        int byte_index = out_len - 1 - i;
        int limb = byte_index / 4;
        int shift = (byte_index % 4) * 8;
        out[i] = (limb < BN_LIMBS) ? (uint8_t)(a->d[limb] >> shift) : 0;
    }
    return 0;
}

int bn_cmp(const bignum_t *a, const bignum_t *b) {
    for (int i = BN_LIMBS - 1; i >= 0; i--) {
        if (a->d[i] != b->d[i]) return (a->d[i] > b->d[i]) ? 1 : -1;
    }
    return 0;
}

int bn_is_zero(const bignum_t *a) {
    for (int i = 0; i < BN_LIMBS; i++) if (a->d[i]) return 0;
    return 1;
}

int bn_byte_len(const bignum_t *a) {
    for (int limb = BN_LIMBS - 1; limb >= 0; limb--) {
        if (a->d[limb]) {
            for (int b = 3; b >= 0; b--)
                if ((a->d[limb] >> (8*b)) & 0xFF) return limb*4 + b + 1;
        }
    }
    return 0;
}

static void bn_sub_inplace(bignum_t *out, const bignum_t *b) {
    int64_t borrow = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        int64_t v = (int64_t)out->d[i] - (int64_t)b->d[i] - borrow;
        if (v < 0) { v += 0x100000000LL; borrow = 1; } else borrow = 0;
        out->d[i] = (uint32_t)v;
    }
}

void bn_mul(const bignum_t *a, const bignum_t *b, bignum_wide_t *r) {
    for (int i = 0; i < 2*BN_LIMBS; i++) r->d[i] = 0;

    for (int i = 0; i < BN_LIMBS; i++) {
        if (a->d[i] == 0) continue;
        uint64_t carry = 0;
        int j;
        for (j = 0; j < BN_LIMBS; j++) {
            uint64_t p = (uint64_t)a->d[i] * (uint64_t)b->d[j] + r->d[i+j] + carry;
            r->d[i+j] = (uint32_t)p;
            carry = p >> 32;
        }
        int idx = i + j;
        while (carry) {
            uint64_t p = (uint64_t)r->d[idx] + carry;
            r->d[idx] = (uint32_t)p;
            carry = p >> 32;
            idx++;
        }
    }
}

void bn_mod_wide(const bignum_wide_t *val, const bignum_t *mod, bignum_t *rem) {
    bignum_t r;
    bn_zero(&r);

    for (int bit = 2*BN_LIMBS*32 - 1; bit >= 0; bit--) {
        uint32_t carry = 0;
        for (int i = 0; i < BN_LIMBS; i++) {
            uint32_t newcarry = r.d[i] >> 31;
            r.d[i] = (uint32_t)(r.d[i] << 1) | carry;
            carry = newcarry;
        }
        int limb = bit / 32, shift = bit % 32;
        uint32_t bitval = (val->d[limb] >> shift) & 1u;
        r.d[0] |= bitval;

        if (bn_cmp(&r, mod) >= 0) bn_sub_inplace(&r, mod);
    }

    for (int i = 0; i < BN_LIMBS; i++) rem->d[i] = r.d[i];
}

void bn_modmul(const bignum_t *a, const bignum_t *b, const bignum_t *mod, bignum_t *out) {
    bignum_wide_t p;
    bn_mul(a, b, &p);
    bn_mod_wide(&p, mod, out);
}

void bn_modexp_u32(const bignum_t *base, uint32_t exp, const bignum_t *mod, bignum_t *result) {
    bignum_t b;
    {
        bignum_wide_t bw;
        for (int i = 0; i < BN_LIMBS; i++) bw.d[i] = base->d[i];
        for (int i = BN_LIMBS; i < 2*BN_LIMBS; i++) bw.d[i] = 0;
        bn_mod_wide(&bw, mod, &b);
    }

    bignum_t r;
    bn_from_u32(&r, 1);

    while (exp) {
        if (exp & 1) bn_modmul(&r, &b, mod, &r);
        bn_modmul(&b, &b, mod, &b);
        exp >>= 1;
    }

    for (int i = 0; i < BN_LIMBS; i++) result->d[i] = r.d[i];
}
