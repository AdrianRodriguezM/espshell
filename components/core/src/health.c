/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "health.h"
#include "net.h"
#include "logger.h"
#include "targets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"

/* The internal temperature sensor handle is owned by debug.c; health
 * reports NaN here to avoid double-ownership conflicts. The CHIP_TEMP
 * command remains the canonical reader. */

#define TAG "health"

#ifndef CONFIG_ESPSHELL_HEALTH_INTERVAL_MS
#define CONFIG_ESPSHELL_HEALTH_INTERVAL_MS 10000
#endif

static void snapshot(health_snapshot_t *o)
{
    memset(o, 0, sizeof(*o));
    o->uptime_s     = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    o->ram_free     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    o->ram_min_free = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    o->largest_block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    o->chip_temp_c = NAN;
    o->cpu0_pct    = NAN;
    o->cpu1_pct    = NAN;

    /* RSSI: filled from net layer if connected. */
    net_status_t ns;
    if (net_get_status(&ns)) o->rssi = ns.rssi;
}

void health_get(health_snapshot_t *out) { if (out) snapshot(out); }

static void health_task(void *arg)
{
    (void)arg;
    const uint32_t period_ms = CONFIG_ESPSHELL_HEALTH_INTERVAL_MS;
    if (period_ms == 0) vTaskDelete(NULL);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(period_ms));
        health_snapshot_t s;
        snapshot(&s);
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "EVT HEALTH uptime=%lu ram_free=%lu ram_min=%lu rssi=%d",
                 (unsigned long)s.uptime_s,
                 (unsigned long)s.ram_free,
                 (unsigned long)s.ram_min_free,
                 (int)s.rssi);
        net_send_event(buf);
    }
}

void health_init(void)
{
    xTaskCreate(health_task, "health", 3072, NULL, 3, NULL);
}
