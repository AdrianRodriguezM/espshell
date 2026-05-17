/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hex.h"
#include <string.h>

static const char HEX_CHARS[] = "0123456789abcdef";

int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void hex_encode(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = HEX_CHARS[in[i] >> 4];
        out[i * 2 + 1] = HEX_CHARS[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

int hex_to_bytes(const char *s, uint8_t *out, size_t cap)
{
    size_t slen = strlen(s);
    if (slen % 2) return -1;
    size_t need = slen / 2;
    if (need > cap) return -1;
    for (size_t i = 0; i < need; i++) {
        int hi = hex_nibble(s[i * 2]);
        int lo = hex_nibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)need;
}

bool hex_decode(const char *s, size_t expect_bytes, uint8_t *out)
{
    if (!s || strlen(s) != expect_bytes * 2) return false;
    return hex_to_bytes(s, out, expect_bytes) == (int)expect_bytes;
}
