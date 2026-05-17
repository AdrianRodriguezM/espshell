/* SPDX-License-Identifier: GPL-3.0-or-later
 * proto.c — AEAD frame I/O using the PSA Crypto API (mbedTLS 4 / IDF v6).
 */
#include "proto.h"

#include <string.h>

#include "psa/crypto.h"

static void be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static uint16_t rd16(const uint8_t *p)   { return ((uint16_t)p[0] << 8) | p[1]; }
static void be64(uint8_t *p, uint64_t v) { for (int i = 7; i >= 0; i--) { p[i] = v & 0xff; v >>= 8; } }
static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}
static void build_nonce(uint8_t out[12], uint8_t dir, uint64_t seq)
{
    out[0] = dir;
    be64(out + 1, seq);
    out[9] = out[10] = out[11] = 0;
}

/* Volatile zeroisation (no dependency on private platform_util.h). */
static void wipe(volatile void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--) *b++ = 0;
}

void proto_session_init(proto_session_t *s,
                        const uint8_t key[ESPSHELL_SESSION_KEY_LEN])
{
    memset(s, 0, sizeof(*s));
    memcpy(s->key, key, ESPSHELL_SESSION_KEY_LEN);
    s->tx_dir = ESPSHELL_DIR_SERVER;
    s->rx_dir = ESPSHELL_DIR_CLIENT;
    s->established = true;
}

void proto_session_wipe(proto_session_t *s)
{
    if (!s) return;
    wipe(s, sizeof(*s));
}

uint16_t proto_frame_peek_length(const uint8_t header[ESPSHELL_FRAME_HEADER_LEN])
{
    return rd16(header);
}

/* Import the session key into PSA, run the AEAD op, then destroy the key.
 * Per-frame import is a small (microsecond-scale) cost on an MCU but keeps
 * the API simple and avoids storing PSA handles in the session struct. */
static psa_key_id_t import_key(const uint8_t key[32], int for_encrypt)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&a, for_encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_CHACHA20_POLY1305);
    psa_set_key_type(&a, PSA_KEY_TYPE_CHACHA20);
    psa_set_key_bits(&a, 256);
    psa_key_id_t id = 0;
    if (psa_import_key(&a, key, 32, &id) != PSA_SUCCESS) return 0;
    return id;
}

bool proto_frame_encrypt(proto_session_t *s,
                         const uint8_t *plaintext, size_t pt_len,
                         uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!s || !s->established || !out || !out_len) return false;
    if (pt_len > 0xFFFF) return false;

    const size_t frame_len = ESPSHELL_FRAME_HEADER_LEN + pt_len + ESPSHELL_FRAME_TAG_LEN;
    if (frame_len > out_cap || frame_len > 0xFFFF) return false;

    be16(out, (uint16_t)frame_len);
    out[2] = ESPSHELL_FRAME_TYPE_DATA;
    out[3] = 0;
    be64(out + 4, s->tx_seq);

    uint8_t nonce[12];
    build_nonce(nonce, s->tx_dir, s->tx_seq);

    psa_key_id_t kid = import_key(s->key, 1);
    if (!kid) return false;

    size_t produced = 0;
    psa_status_t st = psa_aead_encrypt(
        kid, PSA_ALG_CHACHA20_POLY1305,
        nonce, sizeof(nonce),
        out, ESPSHELL_FRAME_HEADER_LEN,            /* AAD = header */
        plaintext, pt_len,
        out + ESPSHELL_FRAME_HEADER_LEN,            /* ciphertext+tag */
        pt_len + ESPSHELL_FRAME_TAG_LEN,
        &produced);
    psa_destroy_key(kid);
    wipe(nonce, sizeof(nonce));
    if (st != PSA_SUCCESS || produced != pt_len + ESPSHELL_FRAME_TAG_LEN) return false;

    s->tx_seq++;
    *out_len = frame_len;
    return true;
}

bool proto_frame_decrypt(proto_session_t *s,
                         const uint8_t *frame, size_t frame_len,
                         uint8_t *out, size_t out_cap, size_t *pt_len)
{
    if (!s || !s->established || !frame || !out || !pt_len) return false;
    if (frame_len < ESPSHELL_FRAME_HEADER_LEN + ESPSHELL_FRAME_TAG_LEN) return false;
    if (rd16(frame) != frame_len) return false;
    if (frame[2] != ESPSHELL_FRAME_TYPE_DATA) return false;
    if (frame[3] != 0) return false;

    const uint64_t seq = rd64(frame + 4);
    if (seq != s->rx_seq) return false;

    const size_t ct_tag_len = frame_len - ESPSHELL_FRAME_HEADER_LEN;
    const size_t ct_len     = ct_tag_len - ESPSHELL_FRAME_TAG_LEN;
    if (ct_len > out_cap) return false;

    uint8_t nonce[12];
    build_nonce(nonce, s->rx_dir, seq);

    psa_key_id_t kid = import_key(s->key, 0);
    if (!kid) return false;

    size_t produced = 0;
    psa_status_t st = psa_aead_decrypt(
        kid, PSA_ALG_CHACHA20_POLY1305,
        nonce, sizeof(nonce),
        frame, ESPSHELL_FRAME_HEADER_LEN,           /* AAD */
        frame + ESPSHELL_FRAME_HEADER_LEN, ct_tag_len,
        out, out_cap, &produced);
    psa_destroy_key(kid);
    wipe(nonce, sizeof(nonce));
    if (st != PSA_SUCCESS || produced != ct_len) return false;

    s->rx_seq++;
    *pt_len = produced;
    return true;
}
