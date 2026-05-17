/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net.h"
#include "cfg.h"
#include "logger.h"
#include "cmd.h"
#include "auth.h"
#include "proto.h"
#include "errors.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "sdkconfig.h"
#include "targets.h"

#define TAG "net"

#ifndef CONFIG_ESPSHELL_TCP_PORT
#define CONFIG_ESPSHELL_TCP_PORT 9000
#endif
#ifndef CONFIG_ESPSHELL_MAX_LINE
#define CONFIG_ESPSHELL_MAX_LINE 1024
#endif
#ifndef CONFIG_ESPSHELL_MAX_RESP
#define CONFIG_ESPSHELL_MAX_RESP 4096
#endif
#ifndef CONFIG_ESPSHELL_AUTH_MAX_ATTEMPTS
#define CONFIG_ESPSHELL_AUTH_MAX_ATTEMPTS 3
#endif

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_evts;
static esp_netif_t       *s_netif;
static volatile bool      s_online;
static net_status_t       s_status;

/* === single-client connection state ====================================== */
static int                 s_client_fd = -1;
static SemaphoreHandle_t   s_tx_mtx;             /* serialise frame writes */
static proto_session_t     s_sess;
static volatile bool       s_authed;

/* ------------------------------------------------------------------------- */
/* Hex helpers                                                               */
/* ------------------------------------------------------------------------- */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool hex_decode(const char *s, size_t expect, uint8_t *out)
{
    if (!s || strlen(s) != expect * 2) return false;
    for (size_t i = 0; i < expect; i++) {
        int hi = hex_nibble(s[i * 2]), lo = hex_nibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

/* ------------------------------------------------------------------------- */
/* WiFi event handling                                                       */
/* ------------------------------------------------------------------------- */
static void on_wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_online = false;
        xEventGroupClearBits(s_wifi_evts, WIFI_CONNECTED_BIT);
        LOG_W(TAG, "wifi disconnected; retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_status.ip      = e->ip_info.ip.addr;
        s_status.gateway = e->ip_info.gw.addr;
        s_status.netmask = e->ip_info.netmask.addr;
        s_online = true;
        xEventGroupSetBits(s_wifi_evts, WIFI_CONNECTED_BIT);
        LOG_I(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static void wifi_start(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};
#ifdef CONFIG_ESPSHELL_DEFAULT_WIFI_SSID
    cfg_get_str_or_default("wifi_ssid", ssid, sizeof(ssid),
                           CONFIG_ESPSHELL_DEFAULT_WIFI_SSID);
    cfg_get_str_or_default("wifi_pass", pass, sizeof(pass),
                           CONFIG_ESPSHELL_DEFAULT_WIFI_PASS);
#else
    cfg_get_str("wifi_ssid", ssid, sizeof(ssid));
    cfg_get_str("wifi_pass", pass, sizeof(pass));
#endif

    if (ssid[0] == '\0') {
        LOG_W(TAG, "no wifi_ssid configured; set via CFG_SET or menuconfig");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       on_wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       on_wifi_evt, NULL, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid,     ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    strncpy((char *)s_status.ssid, ssid, sizeof(s_status.ssid) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Stability-first default: some ESP32 boards/IDF combos can panic in
     * modem-sleep paths under socket traffic. Keep WiFi power-save off. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

bool net_is_online(void) { return s_online; }

bool net_get_status(net_status_t *out)
{
    if (!out || !s_online) return false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_status.rssi    = ap.rssi;
        s_status.channel = ap.primary;
        memcpy(s_status.bssid, ap.bssid, 6);
    }
    *out = s_status;
    return true;
}

void net_reconnect(void)
{
    esp_wifi_disconnect();
    esp_wifi_connect();
}

/* ------------------------------------------------------------------------- */
/* TCP / framing                                                             */
/* ------------------------------------------------------------------------- */
static ssize_t read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, (char *)buf + got, n - got, 0);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += r;
    }
    return (ssize_t)got;
}

static bool write_full(int fd, const void *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = send(fd, (const char *)buf + sent, n - sent, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += w;
    }
    return true;
}

/* Send one frame, protected by mutex (multiple producers: cmd handler + logger). */
static bool send_frame(const char *plaintext)
{
    if (s_client_fd < 0 || !s_authed) return false;
    size_t pt_len = strlen(plaintext);
    if (pt_len > CONFIG_ESPSHELL_MAX_LINE) return false;

    uint8_t buf[ESPSHELL_FRAME_HEADER_LEN + CONFIG_ESPSHELL_MAX_LINE + ESPSHELL_FRAME_TAG_LEN];
    size_t  fl;
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    bool ok = proto_frame_encrypt(&s_sess, (const uint8_t *)plaintext, pt_len,
                                  buf, sizeof(buf), &fl);
    if (ok) ok = write_full(s_client_fd, buf, fl);
    xSemaphoreGive(s_tx_mtx);
    return ok;
}

void net_send_event(const char *line)
{
    if (!line) return;
    (void)send_frame(line);
}

/* Receive one full frame: u16 length first, then the remainder. */
static int recv_frame(int fd, uint8_t *out, size_t out_cap)
{
    uint8_t hdr[ESPSHELL_FRAME_HEADER_LEN];
    ssize_t r = read_full(fd, hdr, 2);
    if (r <= 0) return -1;
    uint16_t flen = ((uint16_t)hdr[0] << 8) | hdr[1];
    if (flen < ESPSHELL_FRAME_HEADER_LEN + ESPSHELL_FRAME_TAG_LEN) return -1;
    if (flen > out_cap) return -1;

    /* We already have the first 2 bytes; copy them and read the rest. */
    memcpy(out, hdr, 2);
    if (read_full(fd, out + 2, flen - 2) <= 0) return -1;
    return flen;
}

/* Cleartext line read with strict length cap + CRLF tolerance. */
static int recv_line(int fd, char *out, size_t cap)
{
    size_t i = 0;
    while (i < cap - 1) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') {
            if (i && out[i - 1] == '\r') i--;
            out[i] = '\0';
            return (int)i;
        }
        out[i++] = c;
    }
    return -1;  /* line too long */
}

static bool send_line(int fd, const char *line)
{
    size_t n = strlen(line);
    if (!write_full(fd, line, n)) return false;
    return write_full(fd, "\n", 1);
}

/* ------------------------------------------------------------------------- */
/* Handshake                                                                 */
/* ------------------------------------------------------------------------- */
static bool do_handshake(int fd)
{
    /* 1. Send HELLO with server nonce. */
    uint8_t snonce[ESPSHELL_AUTH_NONCE_LEN];
    auth_make_nonce(snonce);
    char snonce_hex[ESPSHELL_AUTH_NONCE_LEN * 2 + 1];
    hex_encode(snonce, sizeof(snonce), snonce_hex);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    const esp_app_desc_t *desc = esp_app_get_description();

    char hello[256];
    snprintf(hello, sizeof(hello),
             ESPSHELL_PROTO_VERSION
             " HELLO fw=%s chip=%s mac=%02x:%02x:%02x:%02x:%02x:%02x nonce=%s",
             desc ? desc->version : "?", ESPSHELL_TARGET_NAME,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             snonce_hex);
    if (!send_line(fd, hello)) return false;

    /* 2. Receive AUTH cnonce=<hex> hmac=<hex>, with retry budget. */
    char line[512];
    uint8_t cnonce[ESPSHELL_AUTH_NONCE_LEN];
    uint8_t chmac[ESPSHELL_AUTH_HMAC_LEN];

    for (int attempt = 0; attempt < CONFIG_ESPSHELL_AUTH_MAX_ATTEMPTS; attempt++) {
        if (recv_line(fd, line, sizeof(line)) < 0) return false;

        char *cnonce_hex = NULL, *hmac_hex = NULL;
        char *tok = strtok(line, " ");
        if (!tok || strcmp(tok, "AUTH") != 0) goto bad;
        while ((tok = strtok(NULL, " ")) != NULL) {
            if      (!strncmp(tok, "cnonce=", 7)) cnonce_hex = tok + 7;
            else if (!strncmp(tok, "hmac=",   5)) hmac_hex   = tok + 5;
        }
        if (!cnonce_hex || !hmac_hex) goto bad;
        if (!hex_decode(cnonce_hex, ESPSHELL_AUTH_NONCE_LEN, cnonce)) goto bad;
        if (!hex_decode(hmac_hex,   ESPSHELL_AUTH_HMAC_LEN,  chmac))  goto bad;

        if (!auth_verify(snonce, cnonce, chmac)) goto bad;

        /* Success. Derive session key. */
        uint8_t key[ESPSHELL_SESSION_KEY_LEN];
        if (!auth_derive_session_key(snonce, cnonce, key)) {
            send_line(fd, "ERR 9 keygen failed");
            return false;
        }
        proto_session_init(&s_sess, key);
        memset(key, 0, sizeof(key));
        if (!send_line(fd, "OK session established")) return false;
        return true;

    bad:
        send_line(fd, "ERR 3 invalid auth");
        LOG_W(TAG, "auth failure (attempt %d)", attempt + 1);
    }

    /* Cool-down: hold the socket for a few seconds to slow brute force. */
    vTaskDelay(pdMS_TO_TICKS(10000));
    return false;
}

/* ------------------------------------------------------------------------- */
/* Per-client session loop                                                   */
/* ------------------------------------------------------------------------- */
static void session_loop(int fd)
{
    s_client_fd = fd;
    s_authed    = false;

    /* 1. Cleartext handshake. */
    if (!do_handshake(fd)) {
        LOG_W(TAG, "handshake failed; dropping");
        goto out;
    }
    s_authed = true;
    LOG_I(TAG, "client authenticated");

    /* 2. Encrypted command loop. */
    uint8_t frame[ESPSHELL_FRAME_HEADER_LEN + CONFIG_ESPSHELL_MAX_LINE + ESPSHELL_FRAME_TAG_LEN];
    char    line[CONFIG_ESPSHELL_MAX_LINE + 1];
    char    resp[CONFIG_ESPSHELL_MAX_RESP];

    for (;;) {
        int flen = recv_frame(fd, frame, sizeof(frame));
        if (flen <= 0) break;
        size_t pt_len = 0;
        if (!proto_frame_decrypt(&s_sess, frame, flen,
                                 (uint8_t *)line, sizeof(line) - 1, &pt_len)) {
            LOG_W(TAG, "frame auth failed; closing");
            break;
        }
        line[pt_len] = '\0';

        int n = cmd_dispatch(line, resp, sizeof(resp));
        if (n < 0) {
            snprintf(resp, sizeof(resp), "ERR %d response overflow", ESPSHELL_E_NO_MEM);
        }
        if (!send_frame(resp)) break;
    }

out:
    s_authed = false;
    s_client_fd = -1;
    proto_session_wipe(&s_sess);
    close(fd);
    LOG_I(TAG, "client disconnected");
}

static void tcp_server_task(void *arg)
{
    (void)arg;
    /* Wait for IP before listening. */
    xEventGroupWaitBits(s_wifi_evts, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    int lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lsock < 0) { LOG_E(TAG, "socket() failed"); vTaskDelete(NULL); return; }

    int yes = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(CONFIG_ESPSHELL_TCP_PORT),
    };
    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_E(TAG, "bind() failed errno=%d", errno);
        close(lsock); vTaskDelete(NULL); return;
    }
    if (listen(lsock, 1) < 0) {
        LOG_E(TAG, "listen() failed errno=%d", errno);
        close(lsock); vTaskDelete(NULL); return;
    }
    LOG_I(TAG, "listening on tcp/%d", CONFIG_ESPSHELL_TCP_PORT);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(lsock, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            LOG_E(TAG, "accept() failed errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        /* Single client policy: refuse new connections while one is active. */
        if (s_client_fd >= 0) {
            const char *busy = "ERR 8 busy\n";
            send(cfd, busy, strlen(busy), 0);
            close(cfd);
            continue;
        }
        int ka = 1, idle = 30, intvl = 10, cnt = 3;
        setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
        setsockopt(cfd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle, sizeof(idle));
        setsockopt(cfd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(cfd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt, sizeof(cnt));

        LOG_I(TAG, "accepted client from %s", inet_ntoa(peer.sin_addr));
        session_loop(cfd);
    }
}

/* Logger pump: drains the queue and ships lines over the encrypted channel. */
static void logger_pump_task(void *arg)
{
    (void)arg;
    char line[256];
    for (;;) {
        if (logger_drain(line, sizeof(line), 1000)) {
            if (s_authed) (void)send_frame(line);
        }
    }
}

void net_init(void)
{
    s_wifi_evts = xEventGroupCreate();
    s_tx_mtx    = xSemaphoreCreateMutex();
    wifi_start();
    /* session_loop() uses ~6KB+ of local buffers; keep margin to avoid
     * stack corruption (symptoms: TCP connect succeeds but no HELLO / hangs). */
    xTaskCreate(tcp_server_task,  "tcpsrv", 12288, NULL, 4, NULL);
    xTaskCreate(logger_pump_task, "logpump", 3072, NULL, 3, NULL);
}
