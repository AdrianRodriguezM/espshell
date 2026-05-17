/* SPDX-License-Identifier: GPL-3.0-or-later
 * periph_gpio.c — GPIO mode/set/get/toggle + ISR-driven watch with EVT emit.
 */
#include "cmd.h"
#include "errors.h"
#include "net.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define TAG "gpio"
#define MAX_WATCH 8

typedef struct {
    int8_t  pin;            /* -1 = slot empty */
    uint8_t edge;           /* GPIO_INTR_* enum */
} watch_slot_t;

static watch_slot_t   s_watch[MAX_WATCH];
static QueueHandle_t  s_evt_q;
static bool           s_isr_svc;

typedef struct { int pin; int level; } gpio_evt_t;

static void IRAM_ATTR on_gpio_isr(void *arg)
{
    int pin = (int)(intptr_t)arg;
    gpio_evt_t e = { .pin = pin, .level = gpio_get_level(pin) };
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_evt_q, &e, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void watch_task(void *arg)
{
    (void)arg;
    gpio_evt_t e;
    char buf[64];
    for (;;) {
        if (xQueueReceive(s_evt_q, &e, portMAX_DELAY) == pdTRUE) {
            snprintf(buf, sizeof(buf), "EVT GPIO %d %d", e.pin, e.level);
            net_send_event(buf);
        }
    }
}

static bool parse_pin(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*end || v < 0 || v > 48) return false;
    *out = (int)v;
    return true;
}

static bool c_mode(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "pin mode"); return false; }
    int pin;
    if (!parse_pin(v[0], &pin)) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "bad pin"); return false; }

    gpio_config_t g = { .pin_bit_mask = 1ULL << pin, .intr_type = GPIO_INTR_DISABLE };
    if      (!strcasecmp(v[1], "OUTPUT"))         { g.mode = GPIO_MODE_OUTPUT; }
    else if (!strcasecmp(v[1], "INPUT"))          { g.mode = GPIO_MODE_INPUT; }
    else if (!strcasecmp(v[1], "INPUT_PULLUP"))   { g.mode = GPIO_MODE_INPUT; g.pull_up_en = 1; }
    else if (!strcasecmp(v[1], "INPUT_PULLDOWN")) { g.mode = GPIO_MODE_INPUT; g.pull_down_en = 1; }
    else { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "bad mode"); return false; }

    if (gpio_config(&g) != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "config failed"); return false; }
    return true;
}

static bool c_set(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "pin HIGH|LOW"); return false; }
    int pin;
    if (!parse_pin(v[0], &pin)) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "bad pin"); return false; }
    int level = !strcasecmp(v[1], "HIGH") ? 1 : !strcasecmp(v[1], "LOW") ? 0 : -1;
    if (level < 0) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "level"); return false; }
    if (gpio_set_level(pin, level) != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); return false; }
    return true;
}

static bool c_get(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "pin"); return false; }
    int pin;
    if (!parse_pin(v[0], &pin)) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "bad pin"); return false; }
    snprintf(r, s, "%d", gpio_get_level(pin));
    return true;
}

static bool c_toggle(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "pin"); return false; }
    int pin;
    if (!parse_pin(v[0], &pin)) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "bad pin"); return false; }
    gpio_set_level(pin, !gpio_get_level(pin));
    return true;
}

static bool c_watch(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "pin RISING|FALLING|ANY"); return false; }
    int pin;
    if (!parse_pin(v[0], &pin)) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "bad pin"); return false; }
    gpio_int_type_t edge = GPIO_INTR_DISABLE;
    if      (!strcasecmp(v[1], "RISING"))  edge = GPIO_INTR_POSEDGE;
    else if (!strcasecmp(v[1], "FALLING")) edge = GPIO_INTR_NEGEDGE;
    else if (!strcasecmp(v[1], "ANY"))     edge = GPIO_INTR_ANYEDGE;
    else { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "edge"); return false; }

    int slot = -1;
    for (int i = 0; i < MAX_WATCH; i++) {
        if (s_watch[i].pin == pin) { slot = i; break; }
        if (s_watch[i].pin == -1 && slot < 0) slot = i;
    }
    if (slot < 0) { cmd_set_err(ESPSHELL_E_NO_MEM); snprintf(r, s, "watch table full"); return false; }

    if (!s_isr_svc) { gpio_install_isr_service(0); s_isr_svc = true; }

    gpio_config_t g = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT, .intr_type = edge };
    gpio_config(&g);
    gpio_isr_handler_remove(pin);
    if (gpio_isr_handler_add(pin, on_gpio_isr, (void *)(intptr_t)pin) != ESP_OK) {
        cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "isr add"); return false;
    }
    s_watch[slot].pin = pin;
    s_watch[slot].edge = edge;
    return true;
}

static bool c_unwatch(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "pin"); return false; }
    int pin;
    if (!parse_pin(v[0], &pin)) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    gpio_isr_handler_remove(pin);
    gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
    for (int i = 0; i < MAX_WATCH; i++) if (s_watch[i].pin == pin) s_watch[i].pin = -1;
    return true;
}

void gpio_module_init(void)
{
    for (int i = 0; i < MAX_WATCH; i++) s_watch[i].pin = -1;
    s_evt_q = xQueueCreate(16, sizeof(gpio_evt_t));
    xTaskCreate(watch_task, "gpiowatch", 3072, NULL, 4, NULL);

    cmd_register("GPIO_MODE",    c_mode,    "GPIO_MODE <pin> <INPUT|OUTPUT|INPUT_PULLUP|INPUT_PULLDOWN>");
    cmd_register("GPIO_SET",     c_set,     "GPIO_SET <pin> <HIGH|LOW>");
    cmd_register("GPIO_GET",     c_get,     "GPIO_GET <pin>");
    cmd_register("GPIO_TOGGLE",  c_toggle,  "GPIO_TOGGLE <pin>");
    cmd_register("GPIO_WATCH",   c_watch,   "GPIO_WATCH <pin> <RISING|FALLING|ANY>");
    cmd_register("GPIO_UNWATCH", c_unwatch, "GPIO_UNWATCH <pin>");
}
