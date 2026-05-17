/* SPDX-License-Identifier: GPL-3.0-or-later
 * periph_spi.c — SPI master full-duplex TXRX.
 */
#include "cmd.h"
#include "errors.h"
#include "hex.h"
#include "targets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/spi_master.h"

#define MAX_BYTES 64
#define MAX_HOST  ESPSHELL_SPI_NUM_HOSTS

/* Ordered list of usable SPI hosts for this target. */
#if ESPSHELL_SPI_NUM_HOSTS >= 2
static const spi_host_device_t s_host_map[MAX_HOST] = { SPI2_HOST, SPI3_HOST };
#else
static const spi_host_device_t s_host_map[MAX_HOST] = { SPI2_HOST };
#endif

static spi_device_handle_t s_dev[MAX_HOST];
static bool                s_bus_inited[MAX_HOST];

static bool c_init(int c, char **v, char *r, size_t s)
{
    if (c != 7) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "host miso mosi sclk cs hz mode"); return false; }
    int host = atoi(v[0]);
    int miso = atoi(v[1]), mosi = atoi(v[2]), sclk = atoi(v[3]), cs = atoi(v[4]);
    uint32_t hz = (uint32_t)strtoul(v[5], NULL, 10);
    int mode = atoi(v[6]);
    if (host < 0 || host >= MAX_HOST || mode < 0 || mode > 3) {
        cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "args"); return false;
    }

    spi_host_device_t hd = s_host_map[host];

    if (!s_bus_inited[host]) {
        spi_bus_config_t bc = {
            .miso_io_num = miso, .mosi_io_num = mosi, .sclk_io_num = sclk,
            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = MAX_BYTES,
        };
        if (spi_bus_initialize(hd, &bc, SPI_DMA_CH_AUTO) != ESP_OK) {
            cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "bus init"); return false;
        }
        s_bus_inited[host] = true;
    }
    if (s_dev[host]) { spi_bus_remove_device(s_dev[host]); s_dev[host] = NULL; }
    spi_device_interface_config_t dc = {
        .clock_speed_hz = (int)hz, .mode = mode, .spics_io_num = cs, .queue_size = 1,
    };
    if (spi_bus_add_device(hd, &dc, &s_dev[host]) != ESP_OK) {
        cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "device"); return false;
    }
    return true;
}

static bool c_txrx(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int host = atoi(v[0]);
    if (host < 0 || host >= MAX_HOST || !s_dev[host]) { cmd_set_err(ESPSHELL_E_NOT_FOUND); snprintf(r, s, "not init"); return false; }

    uint8_t tx[MAX_BYTES], rx[MAX_BYTES];
    int n = hex_to_bytes(v[1], tx, sizeof(tx));
    if (n <= 0) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "hex"); return false; }

    spi_transaction_t t = { .length = n * 8, .tx_buffer = tx, .rx_buffer = rx };
    if (spi_device_transmit(s_dev[host], &t) != ESP_OK) {
        cmd_set_err(ESPSHELL_E_HW_FAIL); return false;
    }
    if ((size_t)(n * 2 + 1) > s) { cmd_set_err(ESPSHELL_E_NO_MEM); return false; }
    hex_encode(rx, n, r);
    return true;
}

void spi_module_init(void)
{
    cmd_register("SPI_INIT", c_init, "SPI_INIT <host> <miso> <mosi> <sclk> <cs> <hz> <mode>");
    cmd_register("SPI_TXRX", c_txrx, "SPI_TXRX <host> <hex>");
}
