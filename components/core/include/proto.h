/* SPDX-License-Identifier: GPL-3.0-or-later
 * proto.h — espshell wire protocol (handshake + AEAD framing).
 *
 *  ╭───────────────────────── PROTOCOL `espshell/1` ─────────────────────────╮
 *  │                                                                          │
 *  │  Phase A — cleartext handshake (single \n-terminated line per side):     │
 *  │    S → C   "ESPSHELL/1 HELLO fw=<v> chip=<m> mac=<mac> nonce=<64hex>"    │
 *  │    C → S   "AUTH cnonce=<64hex> hmac=<64hex>"                            │
 *  │    S → C   "OK"        on success                                        │
 *  │    S → C   "ERR 3 ..." on failure  → connection closes                   │
 *  │                                                                          │
 *  │  Phase B — encrypted records (binary, big-endian):                       │
 *  │    ┌───────────────────────────────────────────────────────────────────┐ │
 *  │    │ u16 length  │ u8 type │ u8 flags │ u64 seq │ ciphertext+tag(16) │ │ │
 *  │    └───────────────────────────────────────────────────────────────────┘ │
 *  │    length   = total frame bytes including this header                    │
 *  │    type     = 1 (DATA)                                                   │
 *  │    flags    = 0                                                          │
 *  │    seq      = monotonic per direction, starts at 0                       │
 *  │    nonce    = dir_byte(1) || seq_be(8) || 0x000000 (3 zero bytes)        │
 *  │               dir_byte = 0x00 from server, 0x01 from client              │
 *  │    AAD      = first 12 bytes of header (length || type || flags || seq)  │
 *  │    cipher   = ChaCha20-Poly1305(key, nonce, AAD, plaintext)              │
 *  │    plaintext = one ASCII command line, NO trailing newline               │
 *  │                                                                          │
 *  │  Replay: receiver MUST drop frames whose seq != expected_next.           │
 *  │                                                                          │
 *  ╰──────────────────────────────────────────────────────────────────────────╯
 */
#ifndef ESPSHELL_PROTO_H
#define ESPSHELL_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth.h"

#define ESPSHELL_PROTO_VERSION   "espshell/1"
#define ESPSHELL_PROTO_AUTH_INFO "espshell-auth-v1"
#define ESPSHELL_PROTO_SESS_INFO "espshell-session-v1"

#define ESPSHELL_FRAME_HEADER_LEN  12   /* u16 + u8 + u8 + u64 */
#define ESPSHELL_FRAME_TAG_LEN     16   /* Poly1305 tag */
#define ESPSHELL_FRAME_TYPE_DATA   1

#define ESPSHELL_DIR_SERVER  0x00
#define ESPSHELL_DIR_CLIENT  0x01

/* Per-session crypto state. One instance per accepted TCP connection. */
typedef struct {
    uint8_t  key[ESPSHELL_SESSION_KEY_LEN];
    uint64_t rx_seq;     /* expected next sequence from peer */
    uint64_t tx_seq;     /* next sequence to send */
    uint8_t  rx_dir;     /* peer's direction byte (0x01 server-side) */
    uint8_t  tx_dir;     /* our direction byte    (0x00 server-side) */
    bool     established;
} proto_session_t;

/**
 * proto_session_init — zero out and load the session key. Sequence counters
 * reset to 0. rx_dir / tx_dir are set for the server side automatically.
 */
void proto_session_init(proto_session_t *s,
                        const uint8_t key[ESPSHELL_SESSION_KEY_LEN]);

/**
 * proto_session_wipe — best-effort zeroisation of the session key. Call
 * after a connection is torn down.
 */
void proto_session_wipe(proto_session_t *s);

/**
 * proto_frame_encrypt — build a complete on-the-wire frame.
 *
 * @param s          Session state. tx_seq is incremented on success.
 * @param plaintext  Bytes to encrypt (a single command line without \n).
 * @param pt_len     Length of plaintext.
 * @param out        Destination buffer for the full frame.
 * @param out_cap    Capacity of `out` in bytes. Must be
 *                   ≥ ESPSHELL_FRAME_HEADER_LEN + pt_len + ESPSHELL_FRAME_TAG_LEN.
 * @param out_len    On success, set to the frame length written.
 *
 * @return true on success, false on capacity/crypto error.
 */
bool proto_frame_encrypt(proto_session_t *s,
                         const uint8_t *plaintext, size_t pt_len,
                         uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * proto_frame_decrypt — inverse of encrypt. `frame` must point at a complete
 * frame already read from the socket; `frame_len` MUST equal the length field
 * in the header. Writes plaintext into `out` and returns its length via
 * `pt_len`. Verifies tag AND that frame.seq == s->rx_seq. Increments rx_seq
 * on success. Returns false on auth failure, replay, or capacity.
 */
bool proto_frame_decrypt(proto_session_t *s,
                         const uint8_t *frame, size_t frame_len,
                         uint8_t *out, size_t out_cap, size_t *pt_len);

/* Helpers — exposed for tests and net.c. */
uint16_t proto_frame_peek_length(const uint8_t header[ESPSHELL_FRAME_HEADER_LEN]);

#endif /* ESPSHELL_PROTO_H */
