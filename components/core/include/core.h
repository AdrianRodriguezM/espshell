/* SPDX-License-Identifier: GPL-3.0-or-later
 * core.h — public umbrella header. Including this gets the dispatcher,
 * config store, logger, and event emitter.
 */
#ifndef ESPSHELL_CORE_H
#define ESPSHELL_CORE_H

#include "cmd.h"
#include "cfg.h"
#include "logger.h"
#include "errors.h"
#include "targets.h"

/**
 * core_init() — bring up the entire firmware base.
 *
 * Sequence: NVS → logger → cfg → wifi → tcp server task → health task.
 * Idempotent: calling twice is a no-op. Returns once tasks are spawned;
 * actual WiFi association and client acceptance happen asynchronously.
 */
void core_init(void);

#endif /* ESPSHELL_CORE_H */
