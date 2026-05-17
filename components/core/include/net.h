/* SPDX-License-Identifier: GPL-3.0-or-later
 * net.h — WiFi STA bring-up + TCP server that speaks the espshell protocol.
 */
#ifndef ESPSHELL_NET_H
#define ESPSHELL_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void net_init(void);

/* True once WiFi is associated and we have an IP. */
bool net_is_online(void);

/* Snapshot of the WiFi/IP state. Returns false if not yet online. */
typedef struct {
    char     ssid[33];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint32_t ip;        /* host byte order */
    uint32_t gateway;
    uint32_t netmask;
} net_status_t;
bool net_get_status(net_status_t *out);

/* Force a reconnect to the configured AP. */
void net_reconnect(void);

/**
 * net_send_event — emit an asynchronous EVT line to the currently-connected
 * authenticated client, if any. Internally framed and encrypted. Drops
 * silently when no client is connected. Safe from any task.
 */
void net_send_event(const char *line);

#endif /* ESPSHELL_NET_H */
