/* SPDX-License-Identifier: GPL-3.0-or-later
 * wifi_mgmt.c — WIFI_SCAN and WIFI_SET (credentials → NVS, then reconnect).
 */
#include "cmd.h"
#include "cfg.h"
#include "errors.h"
#include "net.h"

#include <stdio.h>
#include <string.h>

#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_timer.h"

static bool c_scan(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    wifi_scan_config_t cfg = {0};
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); return false; }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 12) ap_count = 12;   /* cap output */
    wifi_ap_record_t recs[12];
    esp_wifi_scan_get_ap_records(&ap_count, recs);

    int w = 0;
    for (int i = 0; i < ap_count && w < (int)s - 80; i++) {
        const char *auth =
            recs[i].authmode == WIFI_AUTH_OPEN          ? "OPEN" :
            recs[i].authmode == WIFI_AUTH_WPA2_PSK      ? "WPA2" :
            recs[i].authmode == WIFI_AUTH_WPA3_PSK      ? "WPA3" :
            recs[i].authmode == WIFI_AUTH_WPA_WPA2_PSK  ? "WPA/2" : "OTHER";
        w += snprintf(r + w, s - w, "%s%s:%d:%s",
                      w ? " " : "", (const char *)recs[i].ssid, (int)recs[i].rssi, auth);
    }
    return true;
}

static void reboot_cb(void *arg) { (void)arg; esp_restart(); }

static bool c_set(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "ssid pass"); return false; }
    if (strlen(v[0]) >= 33 || strlen(v[1]) >= 65) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    if (!cfg_set_str("wifi_ssid", v[0]) || !cfg_set_str("wifi_pass", v[1])) {
        cmd_set_err(ESPSHELL_E_INTERNAL); return false;
    }
    if (net_is_softap_mode()) {
        /* In provisioning mode we can't reconnect (no STA interface exists).
         * Schedule a reboot so the response frame is sent before we restart. */
        snprintf(r, s, "saved; rebooting into STA mode in 1s");
        esp_timer_handle_t t;
        esp_timer_create_args_t ta = { .callback = reboot_cb, .name = "prov_reboot" };
        if (esp_timer_create(&ta, &t) == ESP_OK)
            esp_timer_start_once(t, 1000000);
        return true;
    }
    snprintf(r, s, "saved; reconnecting");
    net_reconnect();
    return true;
}

void wifi_mgmt_module_init(void)
{
    cmd_register("WIFI_SCAN", c_scan, "Active scan; returns ssid:rssi:auth list");
    cmd_register("WIFI_SET",  c_set,  "WIFI_SET <ssid> <pass>");
}
