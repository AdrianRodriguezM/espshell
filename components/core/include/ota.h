/* SPDX-License-Identifier: GPL-3.0-or-later
 * ota.h — OTA receive state machine (Phase 3 placeholder; interface stable).
 */
#ifndef ESPSHELL_OTA_H
#define ESPSHELL_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ota_init(void);

/* Stub-friendly API: real impl lands in Phase 3. */
bool ota_begin(uint32_t size, const uint8_t sha256[32]);
bool ota_data(const uint8_t *chunk, size_t len);
bool ota_end(void);
void ota_abort(void);
bool ota_rollback(void);

#endif /* ESPSHELL_OTA_H */
