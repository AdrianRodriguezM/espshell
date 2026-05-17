/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host-side tests for the espshell/1 framing layer (proto.c + crypto.c).
 * Uses the CLI implementation — identical protocol, no ESP-IDF dependency.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "proto.h"

static int g_pass, g_fail;

#define CHECK(label, expr) do { \
    if (expr) { printf("  PASS  %s\n", label); g_pass++; } \
    else       { printf("  FAIL  %s  (line %d)\n", label, __LINE__); g_fail++; } \
} while (0)

#define FRAME_CAP (ESPSHELL_FRAME_HEADER_LEN + ESPSHELL_MAX_LINE + ESPSHELL_FRAME_TAG_LEN)

static const uint8_t KEY[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
    0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20,
};

/* Initialise a server-side session (tx = server direction). */
static void server_session(session_t *s)
{
    session_init(s, KEY);
    s->tx_dir = ESPSHELL_DIR_SERVER;
    s->rx_dir = ESPSHELL_DIR_CLIENT;
}

/* Initialise a client-side session (default from session_init). */
static void client_session(session_t *s)
{
    session_init(s, KEY);
}

/* ---- tests ------------------------------------------------------------ */

static void test_roundtrip(void)
{
    session_t srv, cli;
    server_session(&srv);
    client_session(&cli);

    const char *pt = "OK PONG";
    uint8_t frame[FRAME_CAP];
    size_t fl;
    bool ok = frame_encrypt(&srv, (const uint8_t *)pt, strlen(pt),
                             frame, sizeof(frame), &fl);
    CHECK("roundtrip: encrypt succeeds", ok);

    uint8_t out[ESPSHELL_MAX_LINE];
    size_t pt_len;
    ok = frame_decrypt(&cli, frame, fl, out, sizeof(out), &pt_len);
    CHECK("roundtrip: decrypt succeeds", ok);
    out[pt_len] = '\0';
    CHECK("roundtrip: plaintext matches", strcmp((char *)out, pt) == 0);
}

static void test_tag_tamper(void)
{
    session_t srv, cli;
    server_session(&srv);
    client_session(&cli);

    const char *pt = "EVT HEALTH uptime=100";
    uint8_t frame[FRAME_CAP];
    size_t fl;
    frame_encrypt(&srv, (const uint8_t *)pt, strlen(pt), frame, sizeof(frame), &fl);

    frame[fl - 1] ^= 0xff;  /* corrupt last byte of Poly1305 tag */

    uint8_t out[ESPSHELL_MAX_LINE];
    size_t pt_len;
    bool ok = frame_decrypt(&cli, frame, fl, out, sizeof(out), &pt_len);
    CHECK("tag tamper → decrypt fails", !ok);
}

static void test_ciphertext_tamper(void)
{
    session_t srv, cli;
    server_session(&srv);
    client_session(&cli);

    const char *pt = "OK data=abc";
    uint8_t frame[FRAME_CAP];
    size_t fl;
    frame_encrypt(&srv, (const uint8_t *)pt, strlen(pt), frame, sizeof(frame), &fl);

    frame[ESPSHELL_FRAME_HEADER_LEN] ^= 0x01;  /* corrupt first ciphertext byte */

    uint8_t out[ESPSHELL_MAX_LINE];
    size_t pt_len;
    bool ok = frame_decrypt(&cli, frame, fl, out, sizeof(out), &pt_len);
    CHECK("ciphertext tamper → decrypt fails", !ok);
}

static void test_replay(void)
{
    session_t srv, cli;
    server_session(&srv);
    client_session(&cli);

    const char *pt = "INFO";
    uint8_t frame[FRAME_CAP];
    size_t fl;
    frame_encrypt(&srv, (const uint8_t *)pt, strlen(pt), frame, sizeof(frame), &fl);

    uint8_t out[ESPSHELL_MAX_LINE];
    size_t pt_len;
    frame_decrypt(&cli, frame, fl, out, sizeof(out), &pt_len);   /* first: accepted */
    bool ok = frame_decrypt(&cli, frame, fl, out, sizeof(out), &pt_len); /* replay */
    CHECK("replay → decrypt fails", !ok);
}

static void test_seq_advance(void)
{
    session_t srv, cli;
    server_session(&srv);
    client_session(&cli);

    uint8_t frame[FRAME_CAP];
    uint8_t out[ESPSHELL_MAX_LINE];
    size_t fl, pt_len;
    bool all_ok = true;

    for (int i = 0; i < 8; i++) {
        char msg[16];
        snprintf(msg, sizeof(msg), "MSG%d", i);
        if (!frame_encrypt(&srv, (const uint8_t *)msg, strlen(msg),
                           frame, sizeof(frame), &fl)) { all_ok = false; break; }
        if (!frame_decrypt(&cli, frame, fl, out, sizeof(out), &pt_len)) {
            all_ok = false; break;
        }
    }
    CHECK("8 sequential frames all accepted", all_ok);
    CHECK("server tx_seq == 8", srv.tx_seq == 8);
    CHECK("client rx_seq == 8", cli.rx_seq == 8);
}

static void test_direction_isolation(void)
{
    /* A frame encrypted with server direction must not decrypt with wrong rx_dir. */
    session_t srv, wrong;
    server_session(&srv);
    client_session(&wrong);
    wrong.rx_dir = ESPSHELL_DIR_CLIENT; /* wrong: expects client frames, not server */

    const char *pt = "STATS";
    uint8_t frame[FRAME_CAP];
    size_t fl;
    frame_encrypt(&srv, (const uint8_t *)pt, strlen(pt), frame, sizeof(frame), &fl);

    uint8_t out[ESPSHELL_MAX_LINE];
    size_t pt_len;
    bool ok = frame_decrypt(&wrong, frame, fl, out, sizeof(out), &pt_len);
    CHECK("wrong rx direction → decrypt fails", !ok);
}

static void test_empty_plaintext(void)
{
    session_t srv, cli;
    server_session(&srv);
    client_session(&cli);

    uint8_t frame[FRAME_CAP];
    size_t fl;
    bool ok = frame_encrypt(&srv, (const uint8_t *)"", 0, frame, sizeof(frame), &fl);
    CHECK("empty plaintext: encrypt ok", ok);

    uint8_t out[16];
    size_t pt_len;
    ok = frame_decrypt(&cli, frame, fl, out, sizeof(out), &pt_len);
    CHECK("empty plaintext: decrypt ok", ok);
    CHECK("empty plaintext: pt_len == 0", pt_len == 0);
}

static void test_session_wipe(void)
{
    session_t s;
    client_session(&s);
    session_wipe(&s);
    /* Key bytes must all be zero after wipe. */
    uint8_t zero[32] = {0};
    CHECK("session wipe zeroes key", memcmp(s.key, zero, 32) == 0);
    CHECK("session wipe clears established", s.established == false);
}

static void test_client_to_server(void)
{
    /* Client encrypts (tx_dir=CLIENT), server decrypts (rx_dir=CLIENT). */
    session_t cli, srv;
    client_session(&cli);
    server_session(&srv);

    const char *pt = "GPIO_SET 2 HIGH";
    uint8_t frame[FRAME_CAP];
    size_t fl;
    bool ok = frame_encrypt(&cli, (const uint8_t *)pt, strlen(pt),
                             frame, sizeof(frame), &fl);
    CHECK("client→server: encrypt ok", ok);

    uint8_t out[ESPSHELL_MAX_LINE];
    size_t pt_len;
    ok = frame_decrypt(&srv, frame, fl, out, sizeof(out), &pt_len);
    CHECK("client→server: decrypt ok", ok);
    out[pt_len] = '\0';
    CHECK("client→server: plaintext matches", strcmp((char *)out, pt) == 0);
}

/* ----------------------------------------------------------------------- */

int main(void)
{
    printf("=== proto framing tests ===\n");
    test_roundtrip();
    test_tag_tamper();
    test_ciphertext_tamper();
    test_replay();
    test_seq_advance();
    test_direction_isolation();
    test_empty_plaintext();
    test_session_wipe();
    test_client_to_server();
    printf("--- %d passed, %d failed ---\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
