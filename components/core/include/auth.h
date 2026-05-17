/* SPDX-License-Identifier: GPL-3.0-or-later
 * auth.h — token storage + HMAC challenge verification + HKDF session keying.
 *
 * The token is a binary secret (≥ 16 bytes recommended) stored in NVS. It
 * NEVER crosses the wire. The handshake proves possession via HMAC-SHA256
 * over a server-supplied nonce and the client-supplied nonce, after which
 * both sides derive a 32-byte ChaCha20-Poly1305 key by HKDF-SHA256.
 */
#ifndef ESPSHELL_AUTH_H
#define ESPSHELL_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESPSHELL_AUTH_NONCE_LEN     32   /* bytes — 256 bits */
#define ESPSHELL_AUTH_HMAC_LEN      32   /* HMAC-SHA256 truncated to full output */
#define ESPSHELL_SESSION_KEY_LEN    32   /* ChaCha20 key */
#define ESPSHELL_AUTH_MAX_TOKEN     128  /* upper bound on token length */

/* Initialise auth: load token from NVS, generating a random one on first
 * boot if the slot is empty. Logs the generated token via the local UART
 * (only the first boot) so it can be copied to the operator's wallet. */
void auth_init(void);

/**
 * auth_make_server_nonce — fill a buffer with cryptographically strong
 * random bytes for use as the server-side challenge.
 */
void auth_make_nonce(uint8_t out[ESPSHELL_AUTH_NONCE_LEN]);

/**
 * auth_verify — constant-time check of a client HMAC.
 *
 * Computes HMAC-SHA256(token, "espshell-auth-v1" || snonce || cnonce) and
 * compares against `client_hmac` in constant time. Returns true iff equal.
 */
bool auth_verify(const uint8_t snonce[ESPSHELL_AUTH_NONCE_LEN],
                 const uint8_t cnonce[ESPSHELL_AUTH_NONCE_LEN],
                 const uint8_t client_hmac[ESPSHELL_AUTH_HMAC_LEN]);

/**
 * auth_derive_session_key — HKDF-SHA256(extract=token, salt=snonce||cnonce,
 *                                       info="espshell-session-v1", L=32).
 *
 * Both server and client run the same derivation; result MUST match.
 */
bool auth_derive_session_key(const uint8_t snonce[ESPSHELL_AUTH_NONCE_LEN],
                             const uint8_t cnonce[ESPSHELL_AUTH_NONCE_LEN],
                             uint8_t out_key[ESPSHELL_SESSION_KEY_LEN]);

#endif /* ESPSHELL_AUTH_H */
