/* SPDX-License-Identifier: GPL-3.0-or-later
 * periph_dac.c — DAC_WRITE on GPIO 25/26 (ESP32 classic only).
 */
#include "cmd.h"
#include "errors.h"
#include "targets.h"

#include <stdio.h>
#include <stdlib.h>

#if ESPSHELL_HAS_DAC
#include "driver/dac_oneshot.h"

static dac_oneshot_handle_t s_dac[2];

static bool c_write(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int ch = atoi(v[0]);
    int val = atoi(v[1]);
    if (ch < 0 || ch > 1 || val < 0 || val > 255) {
        cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "ch 0..1, val 0..255"); return false;
    }
    if (!s_dac[ch]) {
        dac_oneshot_config_t cfg = { .chan_id = ch == 0 ? DAC_CHAN_0 : DAC_CHAN_1 };
        if (dac_oneshot_new_channel(&cfg, &s_dac[ch]) != ESP_OK) {
            cmd_set_err(ESPSHELL_E_HW_FAIL); return false;
        }
    }
    if (dac_oneshot_output_voltage(s_dac[ch], (uint8_t)val) != ESP_OK) {
        cmd_set_err(ESPSHELL_E_HW_FAIL); return false;
    }
    return true;
}
#else
static bool c_write(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v; (void)s;
    snprintf(r, s, "DAC not present on this chip");
    cmd_set_err(ESPSHELL_E_NOT_IMPL);
    return false;
}
#endif

void dac_module_init(void)
{
    cmd_register("DAC_WRITE", c_write, "DAC_WRITE <channel 0|1> <value 0..255> (ESP32 only)");
}
