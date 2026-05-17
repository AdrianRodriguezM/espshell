/* SPDX-License-Identifier: GPL-3.0-or-later
 * ble.c — BLE commands. v1 ships stubs so the wire surface is stable; full
 * NimBLE integration is a v2 task because it adds ~150 KB of flash and
 * forces a partition rebalance on 4 MB targets.
 */
#include "cmd.h"
#include "errors.h"
#include "targets.h"

#include <stdio.h>

#if ESPSHELL_HAS_BLE
static bool stub(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    snprintf(r, s, "BLE pending v2");
    cmd_set_err(ESPSHELL_E_NOT_IMPL);
    return false;
}
#else
static bool stub(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    snprintf(r, s, "no BLE on this chip");
    cmd_set_err(ESPSHELL_E_NOT_IMPL);
    return false;
}
#endif

void ble_module_init(void)
{
    cmd_register("BLE_SCAN",      stub, "BLE_SCAN <duration_s> (stub)");
    cmd_register("BLE_ADVERTISE", stub, "BLE_ADVERTISE <name> (stub)");
    cmd_register("BLE_STOP",      stub, "BLE_STOP (stub)");
}
