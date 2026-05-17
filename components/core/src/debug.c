/* SPDX-License-Identifier: GPL-3.0-or-later
 * debug.c — CHIP_TEMP, HALL_READ (ESP32 only), MEM_READ/WRITE (Kconfig-gated).
 */
#include "cmd.h"
#include "errors.h"
#include "targets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#if ESPSHELL_CHIP_TEMP_OK
#include "driver/temperature_sensor.h"
static temperature_sensor_handle_t s_t;
#endif

/* HALL_READ: the internal Hall sensor was only accessible via the legacy
 * `driver/adc.h` interface, which was removed in IDF v6. We expose the
 * command for protocol stability but always return E_NOT_IMPL. */

static bool c_chip_temp(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
#if ESPSHELL_CHIP_TEMP_OK
    if (!s_t) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
        if (temperature_sensor_install(&cfg, &s_t) != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); return false; }
        temperature_sensor_enable(s_t);
    }
    float t = 0;
    if (temperature_sensor_get_celsius(s_t, &t) != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); return false; }
    snprintf(r, s, "%.2f", (double)t);
    return true;
#else
    (void)r; (void)s;
    snprintf(r, s, "no chip temp sensor");
    cmd_set_err(ESPSHELL_E_NOT_IMPL);
    return false;
#endif
}

static bool c_hall(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    snprintf(r, s, "removed: legacy ADC API gone in IDF v6");
    cmd_set_err(ESPSHELL_E_NOT_IMPL);
    return false;
}

#ifdef CONFIG_ESPSHELL_ENABLE_MEM_CMDS
static bool c_mem_read(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    uintptr_t addr = (uintptr_t)strtoull(v[0], NULL, 0);
    size_t    n    = (size_t)strtoull(v[1], NULL, 0);
    if (n == 0 || n > 256 || (n * 2 + 1) > s) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    static const char H[] = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)addr;
    for (size_t i = 0; i < n; i++) {
        r[i*2]   = H[p[i] >> 4];
        r[i*2+1] = H[p[i] & 0xf];
    }
    r[n*2] = '\0';
    return true;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool c_mem_write(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    uintptr_t addr = (uintptr_t)strtoull(v[0], NULL, 0);
    size_t hex_len = strlen(v[1]);
    if (hex_len == 0 || hex_len % 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    size_t n = hex_len / 2;
    if (n > 256) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    uint8_t *p = (uint8_t *)addr;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(v[1][i * 2]);
        int lo = hex_nibble(v[1][i * 2 + 1]);
        if (hi < 0 || lo < 0) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
        p[i] = (uint8_t)((hi << 4) | lo);
    }
    snprintf(r, s, "%u", (unsigned)n);
    return true;
}
#endif

void debug_module_init(void)
{
    cmd_register("CHIP_TEMP", c_chip_temp, "Internal temperature (°C)");
    cmd_register("HALL_READ", c_hall,      "Hall sensor (ESP32 only)");
#ifdef CONFIG_ESPSHELL_ENABLE_MEM_CMDS
    cmd_register("MEM_READ",  c_mem_read,  "MEM_READ <addr> <n_bytes> (DANGEROUS)");
    cmd_register("MEM_WRITE", c_mem_write, "MEM_WRITE <addr> <hex_bytes> (DANGEROUS)");
#endif
}
