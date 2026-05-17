/* SPDX-License-Identifier: GPL-3.0-or-later
 * cfg.h — typed wrapper around NVS for the espshell config namespace.
 *
 * All keys live under the NVS namespace "espshell". Values are strings on the
 * wire and persisted as either u32 (integers) or blob/string depending on
 * the typed accessor used.
 */
#ifndef ESPSHELL_CFG_H
#define ESPSHELL_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESPSHELL_CFG_MAX_KEY     15   /* NVS limit */
#define ESPSHELL_CFG_MAX_VALUE   256

void cfg_init(void);

/* String getters/setters. cfg_get_str returns false if missing; on hit, copies
 * up to `out_sz`-1 bytes and NUL-terminates. */
bool cfg_get_str(const char *key, char *out, size_t out_sz);
bool cfg_set_str(const char *key, const char *value);

bool cfg_get_u32(const char *key, uint32_t *out);
bool cfg_set_u32(const char *key, uint32_t value);

bool cfg_get_blob(const char *key, void *out, size_t *inout_sz);
bool cfg_set_blob(const char *key, const void *data, size_t sz);

bool cfg_del(const char *key);

/**
 * cfg_get_str_or_default — convenience for boot-time reads. If the key is
 * missing OR contains an empty string, copies `dflt` to `out` and returns
 * false. Otherwise returns true.
 */
bool cfg_get_str_or_default(const char *key, char *out, size_t out_sz,
                            const char *dflt);

/* Iteration for CFG_LIST. Callback returns false to stop. */
typedef bool (*cfg_iter_fn)(const char *key, void *ud);
void cfg_iterate(const char *prefix, cfg_iter_fn fn, void *ud);

void cfg_commit(void);

#endif /* ESPSHELL_CFG_H */
