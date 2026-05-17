/* SPDX-License-Identifier: GPL-3.0-or-later
 * periph_adc.c — ADC1 oneshot read + per-channel streaming via EVT ADC.
 *
 * Uses the IDF v5 oneshot API. Calibration: curve-fitting where supported.
 */
#include "cmd.h"
#include "errors.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ADC_UNIT  ADC_UNIT_1
#define MAX_CH    10

static adc_oneshot_unit_handle_t s_h;
static adc_cali_handle_t          s_cali[MAX_CH];
static bool                        s_ch_inited[MAX_CH];
static TaskHandle_t                s_stream_task[MAX_CH];
static uint32_t                    s_stream_interval[MAX_CH];

static bool parse_ch(const char *s, int *out)
{
    char *e; long v = strtol(s, &e, 10);
    if (*e || v < 0 || v >= MAX_CH) return false;
    *out = (int)v; return true;
}

static bool ensure_channel(int ch)
{
    if (s_ch_inited[ch]) return true;
    if (!s_h) {
        adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT };
        if (adc_oneshot_new_unit(&u, &s_h) != ESP_OK) return false;
    }
    adc_oneshot_chan_cfg_t cfg = { .bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12 };
    if (adc_oneshot_config_channel(s_h, ch, &cfg) != ESP_OK) return false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cc = {
        .unit_id = ADC_UNIT, .chan = ch, .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_create_scheme_curve_fitting(&cc, &s_cali[ch]);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t lc = {
        .unit_id = ADC_UNIT, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_create_scheme_line_fitting(&lc, &s_cali[ch]);
#endif
    s_ch_inited[ch] = true;
    return true;
}

static bool c_read(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int ch;
    if (!parse_ch(v[0], &ch) || !ensure_channel(ch)) {
        cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "bad channel"); return false;
    }
    int raw;
    if (adc_oneshot_read(s_h, ch, &raw) != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); return false; }
    snprintf(r, s, "%d", raw);
    return true;
}

static bool c_read_mv(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int ch;
    if (!parse_ch(v[0], &ch) || !ensure_channel(ch)) {
        cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "bad channel"); return false;
    }
    int raw, mv = 0;
    if (adc_oneshot_read(s_h, ch, &raw) != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); return false; }
    if (s_cali[ch] && adc_cali_raw_to_voltage(s_cali[ch], raw, &mv) == ESP_OK) {
        snprintf(r, s, "%d", mv);
    } else {
        snprintf(r, s, "%d", raw); /* uncalibrated fallback */
    }
    return true;
}

static void stream_task_fn(void *arg)
{
    int ch = (int)(intptr_t)arg;
    char buf[48];
    while (s_stream_interval[ch] > 0) {
        int raw;
        if (s_h && adc_oneshot_read(s_h, ch, &raw) == ESP_OK) {
            snprintf(buf, sizeof(buf), "EVT ADC %d %d", ch, raw);
            net_send_event(buf);
        }
        vTaskDelay(pdMS_TO_TICKS(s_stream_interval[ch]));
    }
    s_stream_task[ch] = NULL;
    vTaskDelete(NULL);
}

static bool c_stream(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int ch;
    uint32_t ms = (uint32_t)strtoul(v[1], NULL, 10);
    if (!parse_ch(v[0], &ch) || ms < 10 || ms > 3600000) {
        cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "bad args"); return false;
    }
    if (!ensure_channel(ch)) { cmd_set_err(ESPSHELL_E_HW_FAIL); return false; }
    if (s_stream_task[ch]) { cmd_set_err(ESPSHELL_E_BUSY); snprintf(r, s, "already streaming"); return false; }
    s_stream_interval[ch] = ms;
    char name[16]; snprintf(name, sizeof(name), "adc%d", ch);
    xTaskCreate(stream_task_fn, name, 3072, (void *)(intptr_t)ch, 3, &s_stream_task[ch]);
    return true;
}

static bool c_stream_stop(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int ch;
    if (!parse_ch(v[0], &ch)) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    s_stream_interval[ch] = 0;  /* task exits next tick */
    (void)r; (void)s;
    return true;
}

void adc_module_init(void)
{
    cmd_register("ADC_READ",        c_read,        "ADC_READ <channel> — raw (0..4095)");
    cmd_register("ADC_READ_MV",     c_read_mv,     "ADC_READ_MV <channel> — calibrated mV");
    cmd_register("ADC_STREAM",      c_stream,      "ADC_STREAM <channel> <interval_ms>");
    cmd_register("ADC_STREAM_STOP", c_stream_stop, "ADC_STREAM_STOP <channel>");
}
