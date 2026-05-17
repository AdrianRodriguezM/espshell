/* SPDX-License-Identifier: GPL-3.0-or-later
 * cmd_builtin.c — Phase 1 built-in commands. Phases 2+ extend this file or
 * add new compilation units that register on init.
 */
#include "cmd.h"
#include "cfg.h"
#include "logger.h"
#include "health.h"
#include "errors.h"
#include "net.h"
#include "targets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs_flash.h"

/* ------------------------------------------------------------------------ */
/* helpers                                                                  */
/* ------------------------------------------------------------------------ */
static bool need_argc(int got, int want, char *resp, size_t sz)
{
    if (got == want) return true;
    snprintf(resp, sz, "expected %d args, got %d", want, got);
    cmd_set_err(ESPSHELL_E_BAD_ARGS);
    return false;
}

/* ------------------------------------------------------------------------ */
/* System                                                                   */
/* ------------------------------------------------------------------------ */
static bool c_ping(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    snprintf(r, s, "PONG");
    return true;
}

static bool c_info(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const esp_app_desc_t *desc = esp_app_get_description();
    snprintf(r, s,
             "fw=%s chip=%s cores=%d rev=%d mac=%02x:%02x:%02x:%02x:%02x:%02x idf=%s",
             desc ? desc->version : "?",
             ESPSHELL_TARGET_NAME, ci.cores, ci.revision,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             esp_get_idf_version());
    return true;
}

static bool c_stats(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    health_snapshot_t h;
    health_get(&h);
    snprintf(r, s,
             "uptime=%" PRIu32 " ram_free=%" PRIu32 " ram_min=%" PRIu32 " rssi=%d",
             h.uptime_s, h.ram_free, h.ram_min_free, (int)h.rssi);
    return true;
}

static bool c_uptime(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    snprintf(r, s, "%" PRIu64, esp_timer_get_time() / 1000000ULL);
    return true;
}

static bool c_heap(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    snprintf(r, s, "total=%u free=%u min_free=%u largest=%u",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

static bool c_reset_reason(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    static const char *names[] = {
        "UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT", "TASK_WDT",
        "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO", "USB", "JTAG", "EFUSE",
        "PWR_GLITCH", "CPU_LOCKUP"
    };
    int n = esp_reset_reason();
    const char *nm = (n >= 0 && n < (int)(sizeof(names) / sizeof(names[0])))
                     ? names[n] : "OTHER";
    snprintf(r, s, "%s", nm);
    return true;
}

static bool c_reboot(int c, char **v, char *r, size_t s)
{
    uint32_t delay = 100;
    if (c == 1) delay = (uint32_t)strtoul(v[0], NULL, 10);
    snprintf(r, s, "rebooting in %" PRIu32 "ms", delay);
    /* Schedule restart on a one-shot timer so we have time to flush the reply. */
    static esp_timer_handle_t t;
    if (!t) {
        esp_timer_create_args_t a = {
            .callback = (esp_timer_cb_t)esp_restart,
            .name = "reboot",
        };
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, (uint64_t)delay * 1000);
    return true;
}

static bool c_factory_reset(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v; (void)r; (void)s;
    /* Real impl: erase NVS partition + restart. */
    nvs_flash_erase();   /* declared in nvs_flash.h, included transitively via cfg.h chain */
    esp_restart();
    return true;
}

static bool c_tasks(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
#if (configUSE_TRACE_FACILITY == 1)
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = calloc(n, sizeof(TaskStatus_t));
    if (!ts) { cmd_set_err(ESPSHELL_E_NO_MEM); return false; }
    n = uxTaskGetSystemState(ts, n, NULL);
    int w = 0;
    for (UBaseType_t i = 0; i < n && w < (int)s - 80; i++) {
        w += snprintf(r + w, s - w, "%s:p%u:sf%u:st%u ",
                      ts[i].pcTaskName,
                      (unsigned)ts[i].uxCurrentPriority,
                      (unsigned)ts[i].usStackHighWaterMark,
                      (unsigned)ts[i].eCurrentState);
    }
    free(ts);
    return true;
#else
    snprintf(r, s, "trace facility disabled");
    cmd_set_err(ESPSHELL_E_DISABLED);
    return false;
#endif
}

/* ------------------------------------------------------------------------ */
/* Discovery                                                                */
/* ------------------------------------------------------------------------ */
static bool c_cmds(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    int w = 0;
    size_t n = cmd_count();
    for (size_t i = 0; i < n && w < (int)s - 40; i++) {
        cmd_entry_view_t e;
        if (cmd_get(i, &e)) {
            w += snprintf(r + w, s - w, "%s%s", i ? " " : "", e.name);
        }
    }
    return true;
}

static bool c_help(int c, char **v, char *r, size_t s)
{
    if (!need_argc(c, 1, r, s)) return false;
    const char *h = cmd_help_for(v[0]);
    if (!h) {
        snprintf(r, s, "no such command: %s", v[0]);
        cmd_set_err(ESPSHELL_E_NOT_FOUND);
        return false;
    }
    snprintf(r, s, "%s", h);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Config                                                                   */
/* ------------------------------------------------------------------------ */
/* Keys we refuse to read back over the wire because they're secret. */
static bool key_is_secret(const char *k)
{
    return strcmp(k, "auth_token") == 0 ||
           strcmp(k, "wifi_pass")  == 0;
}

static bool c_cfg_get(int c, char **v, char *r, size_t s)
{
    if (!need_argc(c, 1, r, s)) return false;
    if (key_is_secret(v[0])) {
        snprintf(r, s, "redacted");
        cmd_set_err(ESPSHELL_E_NOT_AUTH);
        return false;
    }
    char buf[ESPSHELL_CFG_MAX_VALUE];
    if (!cfg_get_str(v[0], buf, sizeof(buf))) {
        snprintf(r, s, "no such key");
        cmd_set_err(ESPSHELL_E_NOT_FOUND);
        return false;
    }
    snprintf(r, s, "%s", buf);
    return true;
}

static bool c_cfg_set(int c, char **v, char *r, size_t s)
{
    if (!need_argc(c, 2, r, s)) return false;
    if (strcmp(v[0], "auth_token") == 0 && strlen(v[1]) < 16) {
        snprintf(r, s, "auth_token must be at least 16 characters");
        cmd_set_err(ESPSHELL_E_BAD_ARGS);
        return false;
    }
    if (!cfg_set_str(v[0], v[1])) {
        snprintf(r, s, "write failed");
        cmd_set_err(ESPSHELL_E_INTERNAL);
        return false;
    }
    return true;
}

static bool c_cfg_del(int c, char **v, char *r, size_t s)
{
    if (!need_argc(c, 1, r, s)) return false;
    if (!cfg_del(v[0])) {
        cmd_set_err(ESPSHELL_E_NOT_FOUND);
        snprintf(r, s, "no such key");
        return false;
    }
    return true;
}

typedef struct { char *buf; size_t cap; size_t used; bool first; } list_ctx_t;
static bool list_cb(const char *key, void *ud)
{
    list_ctx_t *ctx = ud;
    if (ctx->used + strlen(key) + 2 >= ctx->cap) return false;
    int w = snprintf(ctx->buf + ctx->used, ctx->cap - ctx->used, "%s%s",
                     ctx->first ? "" : " ", key);
    if (w < 0) return false;
    ctx->used += (size_t)w;
    ctx->first = false;
    return true;
}
static bool c_cfg_list(int c, char **v, char *r, size_t s)
{
    list_ctx_t ctx = { r, s, 0, true };
    cfg_iterate(c >= 1 ? v[0] : "", list_cb, &ctx);
    return true;
}

static bool c_cfg_commit(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v; (void)r; (void)s;
    cfg_commit();
    return true;
}

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */
/* ------------------------------------------------------------------------ */
static bool c_log_level(int c, char **v, char *r, size_t s)
{
    if (!need_argc(c, 1, r, s)) return false;
    int lvl = atoi(v[0]);
    if (lvl < 0 || lvl > 5) {
        cmd_set_err(ESPSHELL_E_BAD_ARGS);
        snprintf(r, s, "level must be 0..5");
        return false;
    }
    logger_set_level((log_level_t)lvl);
    return true;
}

static bool c_log_stream(int c, char **v, char *r, size_t s)
{
    if (!need_argc(c, 1, r, s)) return false;
    if      (!strcasecmp(v[0], "ON"))  logger_set_stream(true);
    else if (!strcasecmp(v[0], "OFF")) logger_set_stream(false);
    else {
        cmd_set_err(ESPSHELL_E_BAD_ARGS);
        snprintf(r, s, "expected ON|OFF");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* WiFi                                                                     */
/* ------------------------------------------------------------------------ */
static bool c_wifi_status(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    net_status_t st;
    if (!net_get_status(&st)) { snprintf(r, s, "offline"); cmd_set_err(ESPSHELL_E_NOT_FOUND); return false; }
    uint32_t ip = st.ip;
    snprintf(r, s,
             "ssid=%s ip=%u.%u.%u.%u rssi=%d ch=%u",
             st.ssid,
             (unsigned)(ip & 0xff),
             (unsigned)((ip >> 8) & 0xff),
             (unsigned)((ip >> 16) & 0xff),
             (unsigned)((ip >> 24) & 0xff),
             (int)st.rssi, (unsigned)st.channel);
    return true;
}

static bool c_wifi_reconnect(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v; (void)r; (void)s;
    net_reconnect();
    return true;
}

/* ------------------------------------------------------------------------ */
/* Registration                                                             */
/* ------------------------------------------------------------------------ */
void cmd_register_builtins(void)
{
    /* discovery (register CMDS/HELP first so they appear at the top) */
    cmd_register("CMDS",        c_cmds,         "List all registered commands");
    cmd_register("HELP",        c_help,         "HELP <cmd> — print description");

    /* system */
    cmd_register("PING",        c_ping,         "Liveness check");
    cmd_register("INFO",        c_info,         "Static chip/firmware info");
    cmd_register("STATS",       c_stats,        "Dynamic runtime metrics");
    cmd_register("UPTIME",      c_uptime,       "Seconds since boot");
    cmd_register("HEAP",        c_heap,         "Heap totals/free/min/largest");
    cmd_register("RESET_REASON",c_reset_reason, "Why the chip last reset");
    cmd_register("REBOOT",      c_reboot,       "REBOOT [delay_ms] — restart the chip");
    cmd_register("FACTORY_RESET", c_factory_reset, "Erase NVS and reboot");
    cmd_register("TASKS",       c_tasks,        "List FreeRTOS tasks");

    /* config */
    cmd_register("CFG_GET",     c_cfg_get,      "CFG_GET <key>");
    cmd_register("CFG_SET",     c_cfg_set,      "CFG_SET <key> <value>");
    cmd_register("CFG_DEL",     c_cfg_del,      "CFG_DEL <key>");
    cmd_register("CFG_LIST",    c_cfg_list,     "CFG_LIST [prefix]");
    cmd_register("CFG_COMMIT",  c_cfg_commit,   "Flush NVS");

    /* logging */
    cmd_register("LOG_LEVEL",   c_log_level,    "LOG_LEVEL <0..5>");
    cmd_register("LOG_STREAM",  c_log_stream,   "LOG_STREAM ON|OFF");

    /* wifi */
    cmd_register("WIFI_STATUS", c_wifi_status,  "Current WiFi/IP state");
    cmd_register("WIFI_RECONNECT", c_wifi_reconnect, "Force reconnect");
}
