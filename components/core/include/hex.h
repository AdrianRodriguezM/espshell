/* SPDX-License-Identifier: GPL-3.0-or-later
 * hex.h — shared hex encode/decode helpers used across all components.
 */
#ifndef ESPSHELL_HEX_H
#define ESPSHELL_HEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Returns 0-15 for valid hex digit, -1 otherwise. */
int hex_nibble(char c);

/* Encode `n` bytes from `in` into `out` (must hold 2*n+1 bytes). NUL-terminates. */
void hex_encode(const uint8_t *in, size_t n, char *out);

/* Decode a hex string of arbitrary even length into `out` (capacity `cap`).
 * Returns the number of decoded bytes, or -1 on error (odd length, invalid
 * digit, or result exceeds cap). */
int hex_to_bytes(const char *s, uint8_t *out, size_t cap);

/* Decode a hex string of exactly `expect_bytes` bytes. Returns false if the
 * string length is wrong or contains an invalid digit. */
bool hex_decode(const char *s, size_t expect_bytes, uint8_t *out);

#endif /* ESPSHELL_HEX_H */
