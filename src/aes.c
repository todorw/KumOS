#include "aes.h"

static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint8_t inv_sbox[256];
static int     tables_ready = 0;

static const uint8_t rcon[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static void build_inv_sbox(void) {
    if (tables_ready) return;
    for (int i = 0; i < 256; i++) inv_sbox[sbox[i]] = (uint8_t)i;
    tables_ready = 1;
}

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi = (uint8_t)(a & 0x80);
        a = (uint8_t)(a << 1);
        if (hi) a = (uint8_t)(a ^ 0x1b);
        b >>= 1;
    }
    return p;
}

void aes128_init(aes128_ctx_t *ctx, const uint8_t key[16]) {
    build_inv_sbox();

    uint32_t *rk = ctx->round_key;
    for (int i = 0; i < 4; i++)
        rk[i] = ((uint32_t)key[4*i]<<24)|((uint32_t)key[4*i+1]<<16)|((uint32_t)key[4*i+2]<<8)|key[4*i+3];

    for (int i = 4; i < 44; i++) {
        uint32_t temp = rk[i-1];
        if (i % 4 == 0) {
            temp = (temp << 8) | (temp >> 24);
            uint8_t b0 = sbox[(temp>>24)&0xFF], b1 = sbox[(temp>>16)&0xFF];
            uint8_t b2 = sbox[(temp>>8)&0xFF],  b3 = sbox[temp&0xFF];
            temp = ((uint32_t)b0<<24)|((uint32_t)b1<<16)|((uint32_t)b2<<8)|b3;
            temp ^= ((uint32_t)rcon[i/4] << 24);
        }
        rk[i] = rk[i-4] ^ temp;
    }
}

static void add_round_key(uint8_t state[16], const uint32_t *rk) {
    for (int c = 0; c < 4; c++) {
        state[4*c+0] ^= (uint8_t)(rk[c] >> 24);
        state[4*c+1] ^= (uint8_t)(rk[c] >> 16);
        state[4*c+2] ^= (uint8_t)(rk[c] >> 8);
        state[4*c+3] ^= (uint8_t)(rk[c]);
    }
}

static void sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) state[i] = sbox[state[i]];
}
static void inv_sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) state[i] = inv_sbox[state[i]];
}

static void shift_rows(uint8_t s[16]) {
    uint8_t t;
    t=s[1]; s[1]=s[5]; s[5]=s[9]; s[9]=s[13]; s[13]=t;
    t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
    t=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3]; s[3]=t;
}
static void inv_shift_rows(uint8_t s[16]) {
    uint8_t t;
    t=s[13]; s[13]=s[9]; s[9]=s[5]; s[5]=s[1]; s[1]=t;
    t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
    t=s[3]; s[3]=s[7]; s[7]=s[11]; s[11]=s[15]; s[15]=t;
}

static void mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t a0=s[4*c],a1=s[4*c+1],a2=s[4*c+2],a3=s[4*c+3];
        s[4*c+0] = (uint8_t)(gmul(a0,2)^gmul(a1,3)^a2^a3);
        s[4*c+1] = (uint8_t)(a0^gmul(a1,2)^gmul(a2,3)^a3);
        s[4*c+2] = (uint8_t)(a0^a1^gmul(a2,2)^gmul(a3,3));
        s[4*c+3] = (uint8_t)(gmul(a0,3)^a1^a2^gmul(a3,2));
    }
}
static void inv_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t a0=s[4*c],a1=s[4*c+1],a2=s[4*c+2],a3=s[4*c+3];
        s[4*c+0] = (uint8_t)(gmul(a0,14)^gmul(a1,11)^gmul(a2,13)^gmul(a3,9));
        s[4*c+1] = (uint8_t)(gmul(a0,9)^gmul(a1,14)^gmul(a2,11)^gmul(a3,13));
        s[4*c+2] = (uint8_t)(gmul(a0,13)^gmul(a1,9)^gmul(a2,14)^gmul(a3,11));
        s[4*c+3] = (uint8_t)(gmul(a0,11)^gmul(a1,13)^gmul(a2,9)^gmul(a3,14));
    }
}

void aes128_encrypt_block(const aes128_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    for (int i = 0; i < 16; i++) state[i] = in[i];

    add_round_key(state, ctx->round_key);
    for (int round = 1; round <= 9; round++) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, ctx->round_key + round*4);
    }
    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, ctx->round_key + 40);

    for (int i = 0; i < 16; i++) out[i] = state[i];
}

void aes128_decrypt_block(const aes128_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    for (int i = 0; i < 16; i++) state[i] = in[i];

    add_round_key(state, ctx->round_key + 40);
    for (int round = 9; round >= 1; round--) {
        inv_shift_rows(state);
        inv_sub_bytes(state);
        add_round_key(state, ctx->round_key + round*4);
        inv_mix_columns(state);
    }
    inv_shift_rows(state);
    inv_sub_bytes(state);
    add_round_key(state, ctx->round_key);

    for (int i = 0; i < 16; i++) out[i] = state[i];
}

void aes128_cbc_encrypt(const aes128_ctx_t *ctx, const uint8_t iv[16],
                         const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t prev[16];
    for (int i = 0; i < 16; i++) prev[i] = iv[i];

    for (size_t off = 0; off < len; off += 16) {
        uint8_t block[16];
        for (int i = 0; i < 16; i++) block[i] = (uint8_t)(in[off+i] ^ prev[i]);
        aes128_encrypt_block(ctx, block, out+off);
        for (int i = 0; i < 16; i++) prev[i] = out[off+i];
    }
}

void aes128_cbc_decrypt(const aes128_ctx_t *ctx, const uint8_t iv[16],
                         const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t prev[16];
    for (int i = 0; i < 16; i++) prev[i] = iv[i];

    for (size_t off = 0; off < len; off += 16) {
        uint8_t dec[16];
        aes128_decrypt_block(ctx, in+off, dec);
        for (int i = 0; i < 16; i++) out[off+i] = (uint8_t)(dec[i] ^ prev[i]);
        for (int i = 0; i < 16; i++) prev[i] = in[off+i];
    }
}
