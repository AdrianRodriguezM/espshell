/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core.h"
#include "cfg.h"
#include "logger.h"
#include "auth.h"
#include "net.h"
#include "health.h"
#include "ota.h"

void cmd_register_builtins(void);   /* cmd_builtin.c */

/* Per-module registration entry points. Each module owns its own commands. */
void gpio_module_init(void);
void adc_module_init(void);
void dac_module_init(void);
void pwm_module_init(void);
void i2c_module_init(void);
void spi_module_init(void);
void uart_module_init(void);
void fs_module_init(void);
void wifi_mgmt_module_init(void);
void time_module_init(void);
void power_module_init(void);
void debug_module_init(void);
void ble_module_init(void);

static bool s_inited;

void core_init(void)
{
    if (s_inited) return;
    s_inited = true;

    logger_init();
    LOG_I("core", "espshell starting");

    cfg_init();
    auth_init();

    /* Register all commands BEFORE the network task can serve them. */
    cmd_register_builtins();
    gpio_module_init();
    adc_module_init();
    dac_module_init();
    pwm_module_init();
    i2c_module_init();
    spi_module_init();
    uart_module_init();
    fs_module_init();
    wifi_mgmt_module_init();
    time_module_init();
    power_module_init();
    debug_module_init();
    ble_module_init();
    ota_init();

    net_init();      /* spawns tcp server + log pump after wifi assoc */
    health_init();   /* spawns periodic stats emitter */
}
