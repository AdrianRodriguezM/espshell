/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "proto.h"

#include <string.h>
#include <stdbool.h>

extern bool ec_chacha20_seal(const uint8_t key[32], const uint8_t nonce[12],
                             const uint8_t *aad, size_t aad_len,
                             const uint8_t *pt,  size_t pt_len,
                             uint8_t *ct, uint8_t tag[16]);
extern bool ec_chacha20_open(const uint8_t key[32], const uint8_t nonce[12],
                             const uint8_t *aad, size_t aad_len,
                             const uint8_t *ct,  size_t ct_len,
                             const uint8_t tag[16], uint8_t *pt);

static void be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static uint16_t rd16(const uint8_t *p)   { return ((uint16_t)p[0] << 8) | p[1]; }
static void be64(uint8_t *p, uint64_t v) { for (int i = 7; i >= 0; i--) { p[i] = v & 0xff; v >>= 8; } }
static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void nonce_for(uint8_t out[12], uint8_t dir, uint64_t seq)
{
    out[0] = dir;
    be64(out + 1, seq);
    out[9] = out[10] = out[11] = 0;
}

void session_init(session_t *s, const uint8_t key[ESPSHELL_SESSION_KEY_LEN])
{
    memset(s, 0, sizeof(*s));
    memcpy(s->key, key, ESPSHELL_SESSION_KEY_LEN);
    s->tx_dir = ESPSHELL_DIR_CLIENT;
    s->rx_dir = ESPSHELL_DIR_SERVER;
    s->established = true;
}

void session_wipe(session_t *s)
{
    if (!s) return;
    /* Best-effort wipe; volatile to discourage DCE. */
    volatile uint8_t *p = (volatile uint8_t *)s;
    for (size_t i = 0; i < sizeof(*s); i++) p[i] = 0;
}

bool frame_encrypt(session_t *s,
                   const uint8_t *pt, size_t pt_len,
                   uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!s || !s->established) return false;
    size_t fl = ESPSHELL_FRAME_HEADER_LEN + pt_len + ESPSHELL_FRAME_TAG_LEN;
    if (fl > out_cap || fl > 0xFFFF) return false;

    be16(out, (uint16_t)fl);
    out[2] = ESPSHELL_FRAME_TYPE_DATA;
    out[3] = 0;
    be64(out + 4, s->tx_seq);

    uint8_t nonce[12];
    nonce_for(nonce, s->tx_dir, s->tx_seq);

    uint8_t *ct  = out + ESPSHELL_FRAME_HEADER_LEN;
    uint8_t *tag = ct + pt_len;
    if (!ec_chacha20_seal(s->key, nonce, out, ESPSHELL_FRAME_HEADER_LEN,
                          pt, pt_len, ct, tag)) return false;
    s->tx_seq++;
    *out_len = fl;
    return true;
}

bool frame_decrypt(session_t *s,
                   const uint8_t *frame, size_t frame_len,
                   uint8_t *out, size_t out_cap, size_t *pt_len)
{
    if (!s || !s->established) return false;
    if (frame_len < ESPSHELL_FRAME_HEADER_LEN + ESPSHELL_FRAME_TAG_LEN) return false;
    if (rd16(frame) != frame_len) return false;
    if (frame[2] != ESPSHELL_FRAME_TYPE_DATA || frame[3] != 0) return false;
    uint64_t seq = rd64(frame + 4);
    if (seq != s->rx_seq) return false;

    size_t ct_len = frame_len - ESPSHELL_FRAME_HEADER_LEN - ESPSHELL_FRAME_TAG_LEN;
    if (ct_len > out_cap) return false;

    uint8_t nonce[12];
    nonce_for(nonce, s->rx_dir, seq);
    const uint8_t *ct  = frame + ESPSHELL_FRAME_HEADER_LEN;
    const uint8_t *tag = ct + ct_len;
    if (!ec_chacha20_open(s->key, nonce, frame, ESPSHELL_FRAME_HEADER_LEN,
                          ct, ct_len, tag, out)) return false;
    s->rx_seq++;
    *pt_len = ct_len;
    return true;
}
