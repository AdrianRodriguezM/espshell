/* SPDX-License-Identifier: GPL-3.0-or-later
 * espshell client-side protocol — mirrors components/core/include/proto.h.
 */
#ifndef ESPCTL_PROTO_H
#define ESPCTL_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESPSHELL_PROTO_VERSION   "espshell/1"
#define ESPSHELL_PROTO_AUTH_INFO "espshell-auth-v1"
#define ESPSHELL_PROTO_SESS_INFO "espshell-session-v1"

#define ESPSHELL_AUTH_NONCE_LEN     32
#define ESPSHELL_AUTH_HMAC_LEN      32
#define ESPSHELL_SESSION_KEY_LEN    32
#define ESPSHELL_FRAME_HEADER_LEN   12
#define ESPSHELL_FRAME_TAG_LEN      16
#define ESPSHELL_FRAME_TYPE_DATA    1
#define ESPSHELL_DIR_SERVER         0x00
#define ESPSHELL_DIR_CLIENT         0x01
/* Payload limits are asymmetric: command lines (client → server) are capped
 * at MAX_LINE; replies and events (server → client) at MAX_RESP. */
#define ESPSHELL_MAX_LINE           1024
#define ESPSHELL_MAX_RESP           4096

typedef struct {
    uint8_t  key[ESPSHELL_SESSION_KEY_LEN];
    uint64_t rx_seq;
    uint64_t tx_seq;
    uint8_t  rx_dir;
    uint8_t  tx_dir;
    bool     established;
} session_t;

void session_init(session_t *s, const uint8_t key[ESPSHELL_SESSION_KEY_LEN]);
void session_wipe(session_t *s);

bool frame_encrypt(session_t *s,
                   const uint8_t *pt, size_t pt_len,
                   uint8_t *out, size_t out_cap, size_t *out_len);
bool frame_decrypt(session_t *s,
                   const uint8_t *frame, size_t frame_len,
                   uint8_t *out, size_t out_cap, size_t *pt_len);

#endif
