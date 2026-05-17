/* SPDX-License-Identifier: GPL-3.0-or-later
 * power.c — SLEEP_LIGHT / SLEEP_DEEP / CPU_FREQ.
 */
#include "cmd.h"
#include "errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_sleep.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"

static bool c_light(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    uint64_t us = (uint64_t)strtoull(v[0], NULL, 10) * 1000ULL;
    if (us == 0) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    esp_sleep_enable_timer_wakeup(us);
    esp_light_sleep_start();
    snprintf(r, s, "woke");
    return true;
}

static void do_deep_sleep(void *arg)
{
    esp_deep_sleep_start();
}

static bool c_deep(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    uint64_t us = (uint64_t)strtoull(v[0], NULL, 10) * 1000ULL;
    esp_sleep_enable_timer_wakeup(us);
    /* Defer so session_loop can flush the OK reply before the chip sleeps. */
    static esp_timer_handle_t t;
    if (!t) {
        esp_timer_create_args_t a = { .callback = do_deep_sleep, .name = "deep-slp" };
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 50000); /* 50 ms — enough for one TCP round-trip */
    snprintf(r, s, "sleeping");
    return true;
}

static bool c_freq(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int mhz = atoi(v[0]);
    if (mhz != 80 && mhz != 160 && mhz != 240) {
        cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "80|160|240"); return false;
    }
#if CONFIG_PM_ENABLE
    esp_pm_config_t cfg = { .max_freq_mhz = mhz, .min_freq_mhz = mhz };
    if (esp_pm_configure(&cfg) != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); return false; }
#else
    snprintf(r, s, "PM disabled at build time");
    cmd_set_err(ESPSHELL_E_DISABLED);
    return false;
#endif
    return true;
}

void power_module_init(void)
{
    cmd_register("SLEEP_LIGHT", c_light, "SLEEP_LIGHT <ms>");
    cmd_register("SLEEP_DEEP",  c_deep,  "SLEEP_DEEP <ms> (reboots on wake)");
    cmd_register("CPU_FREQ",    c_freq,  "CPU_FREQ <80|160|240>");
}
