/* SPDX-License-Identifier: GPL-3.0-or-later
 * mdns_svc.c — advertise the shell over mDNS so clients can find the device
 * without a fixed IP. Consumed by `esp-ctl discover`.
 */
#include "sdkconfig.h"
#include "logger.h"

#define TAG "mdns"

void mdns_svc_init(void);

#ifdef CONFIG_ESPSHELL_ENABLE_MDNS

#include "cfg.h"
#include "mdns.h"
#include "esp_app_desc.h"

#ifndef CONFIG_ESPSHELL_TCP_PORT
#define CONFIG_ESPSHELL_TCP_PORT 9000
#endif

void mdns_svc_init(void)
{
    /* Hostname: device_name from NVS, Kconfig default otherwise. Static so
     * the mdns component may keep referring to it. */
    static char name[33];
    cfg_get_str_or_default("device_name", name, sizeof(name),
                           CONFIG_ESPSHELL_DEFAULT_DEVICE_NAME);

    if (mdns_init() != ESP_OK) {
        /* No netif/default event loop yet (WiFi unconfigured) — not fatal,
         * the device just stays undiscoverable until provisioned. */
        LOG_W(TAG, "mdns init skipped (no network)");
        return;
    }
    mdns_hostname_set(name);
    mdns_instance_name_set(name);

    /* Deliberately anonymous, same policy as the cleartext HELLO: nothing
     * beyond what a port scan of the device would reveal anyway. */
    mdns_txt_item_t txt[] = {
        { "proto", "1" },
        { "chip",  CONFIG_IDF_TARGET },
        { "fw",    esp_app_get_description()->version },
    };
    mdns_service_add(NULL, "_espshell", "_tcp", CONFIG_ESPSHELL_TCP_PORT,
                     txt, sizeof(txt) / sizeof(txt[0]));
    LOG_I(TAG, "advertising %s.local _espshell._tcp port %d",
          name, CONFIG_ESPSHELL_TCP_PORT);
}

#else  /* !CONFIG_ESPSHELL_ENABLE_MDNS */

void mdns_svc_init(void) { }

#endif
