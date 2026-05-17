/* SPDX-License-Identifier: GPL-3.0-or-later
 * periph_uart.c — secondary UART ports (UART1/UART2 on classic, UART1 on C3/C6).
 * UART0 is reserved for console logging and never touched here.
 */
#include "cmd.h"
#include "errors.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_PORT  3
#define MAX_BYTES 256
#define BUF_SZ    1024

static bool          s_inited[MAX_PORT];
static TaskHandle_t  s_stream_task[MAX_PORT];
static bool          s_stream_on[MAX_PORT];

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hex_to_bytes(const char *s, uint8_t *out, size_t cap)
{
    size_t n = strlen(s);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble(s[i*2]), lo = hex_nibble(s[i*2+1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}
static void bytes_to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i*2] = H[b[i] >> 4]; out[i*2+1] = H[b[i] & 0xf]; }
    out[n*2] = '\0';
}

static bool c_init(int c, char **v, char *r, size_t s)
{
    if (c != 4) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int port = atoi(v[0]);
    int tx   = atoi(v[1]);
    int rx   = atoi(v[2]);
    int baud = atoi(v[3]);
    if (port <= 0 || port >= MAX_PORT || baud < 300 || baud > 5000000) {
        cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "port 1..2, baud"); return false;
    }
    uart_config_t cfg = {
        .baud_rate = baud, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_delete(port);
    if (uart_param_config(port, &cfg) != ESP_OK ||
        uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK ||
        uart_driver_install(port, BUF_SZ, BUF_SZ, 0, NULL, 0) != ESP_OK) {
        cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "init"); return false;
    }
    s_inited[port] = true;
    return true;
}

static bool c_write(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int port = atoi(v[0]);
    if (port <= 0 || port >= MAX_PORT || !s_inited[port]) { cmd_set_err(ESPSHELL_E_NOT_FOUND); snprintf(r, s, "not init"); return false; }
    uint8_t buf[MAX_BYTES];
    int n = hex_to_bytes(v[1], buf, sizeof(buf));
    if (n <= 0) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "hex"); return false; }
    int w = uart_write_bytes(port, buf, n);
    if (w != n) { cmd_set_err(ESPSHELL_E_HW_FAIL); return false; }
    return true;
}

static bool c_read(int c, char **v, char *r, size_t s)
{
    if (c != 3) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int port = atoi(v[0]);
    int n    = atoi(v[1]);
    int to   = atoi(v[2]);
    if (port <= 0 || port >= MAX_PORT || !s_inited[port] || n < 1 || n > MAX_BYTES) {
        cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "args"); return false;
    }
    uint8_t buf[MAX_BYTES];
    int got = uart_read_bytes(port, buf, n, pdMS_TO_TICKS(to));
    if (got < 0) { cmd_set_err(ESPSHELL_E_HW_FAIL); return false; }
    if ((size_t)(got * 2 + 1) > s) { cmd_set_err(ESPSHELL_E_NO_MEM); return false; }
    bytes_to_hex(buf, got, r);
    return true;
}

static void stream_task_fn(void *arg)
{
    int port = (int)(intptr_t)arg;
    uint8_t buf[64];
    char line[160];
    while (s_stream_on[port]) {
        int got = uart_read_bytes(port, buf, sizeof(buf), pdMS_TO_TICKS(200));
        if (got > 0) {
            int off = snprintf(line, sizeof(line), "EVT UART %d ", port);
            for (int i = 0; i < got && off + 2 < (int)sizeof(line); i++) {
                off += snprintf(line + off, sizeof(line) - off, "%02x", buf[i]);
            }
            net_send_event(line);
        }
    }
    s_stream_task[port] = NULL;
    vTaskDelete(NULL);
}

static bool c_stream(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    int port = atoi(v[0]);
    if (port <= 0 || port >= MAX_PORT || !s_inited[port]) { cmd_set_err(ESPSHELL_E_NOT_FOUND); snprintf(r, s, "not init"); return false; }
    if (!strcasecmp(v[1], "ON")) {
        if (s_stream_task[port]) return true;
        s_stream_on[port] = true;
        char name[16]; snprintf(name, sizeof(name), "uart%d", port);
        xTaskCreate(stream_task_fn, name, 3072, (void *)(intptr_t)port, 3, &s_stream_task[port]);
    } else if (!strcasecmp(v[1], "OFF")) {
        s_stream_on[port] = false;
    } else { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "ON|OFF"); return false; }
    return true;
}

void uart_module_init(void)
{
    cmd_register("UART_INIT",   c_init,   "UART_INIT <port 1..2> <tx_pin> <rx_pin> <baud>");
    cmd_register("UART_WRITE",  c_write,  "UART_WRITE <port> <hex>");
    cmd_register("UART_READ",   c_read,   "UART_READ <port> <n_bytes> <timeout_ms>");
    cmd_register("UART_STREAM", c_stream, "UART_STREAM <port> ON|OFF");
}
