/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ESPSHELL_PROJECT_APP_H
#define ESPSHELL_PROJECT_APP_H

/**
 * project_init() — entry point for project-specific code.
 *
 * Called once after the core (network, command dispatcher, logger, config,
 * health monitor) is fully initialized. Use it to register project-specific
 * commands via cmd_register() and to spawn any project-owned FreeRTOS tasks.
 *
 * MUST NOT block. Long-running work belongs in a task.
 */
void project_init(void);

#endif /* ESPSHELL_PROJECT_APP_H */
