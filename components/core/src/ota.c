/* SPDX-License-Identifier: GPL-3.0-or-later
 * ota.c — chunked OTA over the encrypted channel.
 *
 * Flow:
 *   OTA_BEGIN <size> <sha256_hex64>     → reserve next partition
 *   OTA_DATA  <hex_chunk>                → write next chunk (sequence enforced)
 *   OTA_END                              → verify sha256, mark valid, reboot
 *   OTA_ABORT                            → discard in-progress upload
 *   OTA_ROLLBACK                         → switch back to previous partition
 *
 * The sha256 is computed over the plaintext binary as it would be written
 * to flash, NOT over the hex-encoded transport. Verification is mandatory.
 */
#include "ota.h"
#include "cmd.h"
#include "errors.h"
#include "logger.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "psa/crypto.h"

#define TAG "ota"

#ifdef CONFIG_ESPSHELL_ENABLE_OTA
#define OTA_ENABLED 1
#else
#define OTA_ENABLED 0
#endif

#if OTA_ENABLED

typedef struct {
    bool                       active;
    esp_ota_handle_t           h;
    const esp_partition_t     *part;
    uint32_t                   total;
    uint32_t                   received;
    uint8_t                    expected_sha[32];
    psa_hash_operation_t       sha;
} ota_state_t;
static ota_state_t s;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hex_to_bytes(const char *str, uint8_t *out, size_t cap)
{
    size_t n = strlen(str);
    if (n % 2) return -1;
    size_t need = n / 2;
    if (need > cap) return -1;
    for (size_t i = 0; i < need; i++) {
        int hi = hex_nibble(str[i*2]), lo = hex_nibble(str[i*2+1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)need;
}

static void abort_locked(void)
{
    if (!s.active) return;
    if (s.h) esp_ota_abort(s.h);
    psa_hash_abort(&s.sha);
    memset(&s, 0, sizeof(s));
}

bool ota_begin(uint32_t size, const uint8_t sha256[32])
{
    if (s.active) abort_locked();
    s.part = esp_ota_get_next_update_partition(NULL);
    if (!s.part) return false;
    if (esp_ota_begin(s.part, size, &s.h) != ESP_OK) return false;
    s.total = size;
    s.received = 0;
    memcpy(s.expected_sha, sha256, 32);
    s.sha = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&s.sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        esp_ota_abort(s.h);
        return false;
    }
    s.active = true;
    LOG_I(TAG, "begin size=%u part=%s", (unsigned)size, s.part->label);
    return true;
}

bool ota_data(const uint8_t *chunk, size_t len)
{
    if (!s.active || len == 0) return false;
    if (s.received + len > s.total) return false;
    if (esp_ota_write(s.h, chunk, len) != ESP_OK) {
        abort_locked();
        return false;
    }
    if (psa_hash_update(&s.sha, chunk, len) != PSA_SUCCESS) {
        abort_locked();
        return false;
    }
    s.received += len;
    return true;
}

bool ota_end(void)
{
    if (!s.active) return false;
    if (s.received != s.total) { abort_locked(); return false; }

    uint8_t got[32];
    size_t got_len = 0;
    if (psa_hash_finish(&s.sha, got, sizeof(got), &got_len) != PSA_SUCCESS ||
        got_len != 32 || memcmp(got, s.expected_sha, 32) != 0) {
        LOG_E(TAG, "sha256 mismatch — aborting");
        abort_locked();
        return false;
    }
    if (esp_ota_end(s.h) != ESP_OK) { abort_locked(); return false; }
    if (esp_ota_set_boot_partition(s.part) != ESP_OK) return false;
    memset(&s, 0, sizeof(s));
    LOG_I(TAG, "ota committed; rebooting");
    /* Caller is expected to reboot from the command handler. */
    return true;
}

void ota_abort(void)
{
    abort_locked();
}

bool ota_rollback(void)
{
    const esp_partition_t *p = esp_ota_get_last_invalid_partition();
    if (!p) p = esp_ota_get_next_update_partition(NULL);
    if (!p) return false;
    return esp_ota_set_boot_partition(p) == ESP_OK;
}

/* ---- command handlers ---- */
static bool c_begin(int c, char **v, char *r, size_t sz)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, sz, "size sha256"); return false; }
    uint32_t size = (uint32_t)strtoul(v[0], NULL, 10);
    uint8_t sha[32];
    if (hex_to_bytes(v[1], sha, 32) != 32) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, sz, "sha256"); return false; }
    if (!ota_begin(size, sha)) { cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, sz, "begin failed"); return false; }
    snprintf(r, sz, "ready chunk_max=%u", (unsigned)((1024 - 20) / 2)); /* hex doubles size */
    return true;
}

static bool c_data(int c, char **v, char *r, size_t sz)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    uint8_t buf[1024];
    int n = hex_to_bytes(v[0], buf, sizeof(buf));
    if (n <= 0) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, sz, "hex"); return false; }
    if (!ota_data(buf, (size_t)n)) { cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, sz, "write"); return false; }
    snprintf(r, sz, "%u/%u", (unsigned)s.received, (unsigned)s.total);
    return true;
}

static bool c_end(int c, char **v, char *r, size_t sz)
{
    (void)c; (void)v;
    if (!ota_end()) { cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, sz, "verify/commit failed"); return false; }
    snprintf(r, sz, "committed, rebooting");
    /* Defer restart so the OK reply can flush. */
    static esp_timer_handle_t t;
    if (!t) {
        esp_timer_create_args_t a = { .callback = (esp_timer_cb_t)esp_restart, .name = "ota-rb" };
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 200000);
    return true;
}

static bool c_abort(int c, char **v, char *r, size_t sz)
{
    (void)c; (void)v; (void)r; (void)sz;
    ota_abort();
    return true;
}

static bool c_rollback(int c, char **v, char *r, size_t sz)
{
    (void)c; (void)v;
    if (!ota_rollback()) { cmd_set_err(ESPSHELL_E_NOT_FOUND); snprintf(r, sz, "no prior partition"); return false; }
    snprintf(r, sz, "rolled back, reboot to apply");
    return true;
}
#endif /* OTA_ENABLED */

void ota_init(void)
{
#if OTA_ENABLED
    cmd_register("OTA_BEGIN",    c_begin,    "OTA_BEGIN <size> <sha256_hex>");
    cmd_register("OTA_DATA",     c_data,     "OTA_DATA <hex_chunk>");
    cmd_register("OTA_END",      c_end,      "OTA_END");
    cmd_register("OTA_ABORT",    c_abort,    "OTA_ABORT");
    cmd_register("OTA_ROLLBACK", c_rollback, "OTA_ROLLBACK");
#endif
}

#if !OTA_ENABLED
bool ota_begin(uint32_t s, const uint8_t h[32]) { (void)s; (void)h; return false; }
bool ota_data(const uint8_t *c, size_t n)        { (void)c; (void)n; return false; }
bool ota_end(void)                               { return false; }
void ota_abort(void)                             { }
bool ota_rollback(void)                          { return false; }
#endif
