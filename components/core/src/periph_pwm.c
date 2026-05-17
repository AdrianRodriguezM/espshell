/* SPDX-License-Identifier: GPL-3.0-or-later
 * periph_pwm.c — LEDC-based PWM. One timer per (resolution,freq) pair.
 */
#include "cmd.h"
#include "errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/ledc.h"

#define MAX_CH  8

typedef struct {
    bool     used;
    int      pin;
    uint32_t freq;
    uint8_t  res;
    ledc_timer_t tnum;
} pwm_ch_t;

static pwm_ch_t s_ch[MAX_CH];

static bool c_init(int c, char **v, char *r, size_t s)
{
    if (c != 4) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "ch pin freq res"); return false; }
    int ch  = atoi(v[0]);
    int pin = atoi(v[1]);
    uint32_t freq = (uint32_t)strtoul(v[2], NULL, 10);
    int res = atoi(v[3]);
    if (ch < 0 || ch >= MAX_CH || res < 1 || res > 14 || freq < 1) {
        cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "args"); return false;
    }

    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = (ledc_timer_t)(ch % LEDC_TIMER_MAX),
        .duty_resolution = (ledc_timer_bit_t)res,
        .freq_hz         = freq,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&t) != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "timer"); return false; }

    ledc_channel_config_t cc = {
        .gpio_num   = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = (ledc_channel_t)ch,
        .timer_sel  = t.timer_num,
        .duty       = 0,
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    if (ledc_channel_config(&cc) != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "channel"); return false; }

    s_ch[ch] = (pwm_ch_t){ .used = true, .pin = pin, .freq = freq, .res = (uint8_t)res, .tnum = t.timer_num };
    return true;
}

static bool c_set(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int ch = atoi(v[0]);
    uint32_t duty = (uint32_t)strtoul(v[1], NULL, 10);
    if (ch < 0 || ch >= MAX_CH || !s_ch[ch].used) { cmd_set_err(ESPSHELL_E_NOT_FOUND); snprintf(r, s, "ch not init"); return false; }
    uint32_t max = (1U << s_ch[ch].res) - 1U;
    if (duty > max) duty = max;
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty) != ESP_OK ||
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ch) != ESP_OK) {
        cmd_set_err(ESPSHELL_E_HW_FAIL); return false;
    }
    return true;
}

static bool c_stop(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int ch = atoi(v[0]);
    if (ch < 0 || ch >= MAX_CH) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    ledc_stop(LEDC_LOW_SPEED_MODE, ch, 0);
    s_ch[ch].used = false;
    (void)r; (void)s;
    return true;
}

void pwm_module_init(void)
{
    cmd_register("PWM_INIT", c_init, "PWM_INIT <ch> <pin> <freq_hz> <res_bits>");
    cmd_register("PWM_SET",  c_set,  "PWM_SET <ch> <duty>");
    cmd_register("PWM_STOP", c_stop, "PWM_STOP <ch>");
}
