/* SPDX-License-Identifier: GPL-3.0-or-later
 * health.h — periodic emitter of `EVT HEALTH ...`.
 */
#ifndef ESPSHELL_HEALTH_H
#define ESPSHELL_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t uptime_s;
    uint32_t ram_free;
    uint32_t ram_min_free;
    uint32_t largest_block;
    float    chip_temp_c;     /* NAN if unsupported */
    int8_t   rssi;
    float    cpu0_pct;
    float    cpu1_pct;        /* 0 on single-core targets */
} health_snapshot_t;

void health_init(void);
void health_get(health_snapshot_t *out);

#endif /* ESPSHELL_HEALTH_H */
