/* SPDX-License-Identifier: GPL-3.0-or-later
 * periph_i2c.c — master-mode I2C using the new IDF v5 i2c_master driver.
 */
#include "cmd.h"
#include "errors.h"
#include "hex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"

#define MAX_PORT  2
#define MAX_BYTES 64

static i2c_master_bus_handle_t s_bus[MAX_PORT];

static bool c_init(int c, char **v, char *r, size_t s)
{
    if (c != 4) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "port sda scl hz"); return false; }
    int port = atoi(v[0]);
    int sda  = atoi(v[1]);
    int scl  = atoi(v[2]);
    uint32_t hz = (uint32_t)strtoul(v[3], NULL, 10);
    if (port < 0 || port >= MAX_PORT || hz < 1000 || hz > 1000000) {
        cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "args"); return false;
    }
    if (s_bus[port]) { i2c_del_master_bus(s_bus[port]); s_bus[port] = NULL; }

    i2c_master_bus_config_t cfg = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&cfg, &s_bus[port]) != ESP_OK) {
        cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "bus init"); return false;
    }
    (void)hz; /* speed is per-device in the new API; kept as future hint */
    return true;
}

static bool ensure(int port)
{
    return port >= 0 && port < MAX_PORT && s_bus[port] != NULL;
}

static bool c_scan(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int port = atoi(v[0]);
    if (!ensure(port)) { cmd_set_err(ESPSHELL_E_NOT_FOUND); snprintf(r, s, "port"); return false; }

    int w = 0;
    for (uint8_t a = 0x08; a < 0x78 && w < (int)s - 8; a++) {
        if (i2c_master_probe(s_bus[port], a, 10) == ESP_OK) {
            w += snprintf(r + w, s - w, "%s0x%02x", w ? " " : "", a);
        }
    }
    return true;
}

static bool c_read(int c, char **v, char *r, size_t s)
{
    if (c != 4) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "port addr reg n"); return false; }
    int port = atoi(v[0]);
    uint8_t addr = (uint8_t)strtoul(v[1], NULL, 0);
    uint8_t reg  = (uint8_t)strtoul(v[2], NULL, 0);
    int n        = atoi(v[3]);
    if (!ensure(port) || n < 1 || n > MAX_BYTES) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "args"); return false; }

    i2c_device_config_t dcfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(s_bus[port], &dcfg, &dev) != ESP_OK) {
        cmd_set_err(ESPSHELL_E_HW_FAIL); return false;
    }

    uint8_t rx[MAX_BYTES];
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, rx, n, 1000);
    i2c_master_bus_rm_device(dev);
    if (err != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "xfer"); return false; }

    if ((size_t)(n * 2 + 1) > s) { cmd_set_err(ESPSHELL_E_NO_MEM); return false; }
    hex_encode(rx, n, r);
    return true;
}

static bool c_write(int c, char **v, char *r, size_t s)
{
    if (c != 4) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "port addr reg hex"); return false; }
    int port = atoi(v[0]);
    uint8_t addr = (uint8_t)strtoul(v[1], NULL, 0);
    uint8_t reg  = (uint8_t)strtoul(v[2], NULL, 0);
    uint8_t buf[MAX_BYTES + 1];
    buf[0] = reg;
    int n = hex_to_bytes(v[3], buf + 1, MAX_BYTES);
    if (!ensure(port) || n <= 0) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "args"); return false; }

    i2c_device_config_t dcfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(s_bus[port], &dcfg, &dev) != ESP_OK) {
        cmd_set_err(ESPSHELL_E_HW_FAIL); return false;
    }
    esp_err_t err = i2c_master_transmit(dev, buf, n + 1, 1000);
    i2c_master_bus_rm_device(dev);
    if (err != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "xfer"); return false; }
    return true;
}

void i2c_module_init(void)
{
    cmd_register("I2C_INIT",  c_init,  "I2C_INIT <port> <sda> <scl> <hz>");
    cmd_register("I2C_SCAN",  c_scan,  "I2C_SCAN <port>");
    cmd_register("I2C_READ",  c_read,  "I2C_READ <port> <addr> <reg> <n_bytes>");
    cmd_register("I2C_WRITE", c_write, "I2C_WRITE <port> <addr> <reg> <hex_bytes>");
}
