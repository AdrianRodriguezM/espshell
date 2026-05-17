/* SPDX-License-Identifier: GPL-3.0-or-later
 * time_svc.c — TIME_GET / TIME_SET / SNTP_SYNC.
 */
#include "cmd.h"
#include "errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_sntp.h"

static bool c_get(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    time_t now;
    time(&now);
    snprintf(r, s, "%lld", (long long)now);
    return true;
}

static bool c_set(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    long long t = strtoll(v[0], NULL, 10);
    if (t <= 0) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "unix ts"); return false; }
    struct timeval tv = { .tv_sec = (time_t)t };
    settimeofday(&tv, NULL);
    return true;
}

static bool c_sntp(int c, char **v, char *r, size_t s)
{
    const char *server = (c >= 1) ? v[0] : "pool.ntp.org";
    if (esp_sntp_enabled()) esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server);
    esp_sntp_init();
    /* Don't block; SNTP runs in the background and updates the clock. */
    snprintf(r, s, "syncing with %s", server);
    return true;
}

void time_module_init(void)
{
    cmd_register("TIME_GET",  c_get,  "UNIX timestamp");
    cmd_register("TIME_SET",  c_set,  "TIME_SET <unix_ts>");
    cmd_register("SNTP_SYNC", c_sntp, "SNTP_SYNC [server]");
}
