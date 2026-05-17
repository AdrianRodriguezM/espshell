/* SPDX-License-Identifier: GPL-3.0-or-later
 * cmd.h — command registration and dispatch API.
 */
#ifndef ESPSHELL_CMD_H
#define ESPSHELL_CMD_H

#include <stdbool.h>
#include <stddef.h>

#define ESPSHELL_CMD_MAX_NAME    31
#define ESPSHELL_CMD_MAX_ARGV    16

/**
 * cmd_handler_fn — callback signature for a command.
 *
 * @param argc      Number of arguments AFTER the command itself (so for
 *                  `GPIO_SET 2 HIGH`, argc=2 and argv = {"2","HIGH"}).
 * @param argv      Pointer to argument vector. NUL-terminated strings.
 *                  Lifetime is the duration of the call only.
 * @param resp      Output buffer for the response payload. Write a single
 *                  line (no trailing \n) of length ≤ resp_sz-1.
 * @param resp_sz   Capacity of `resp` in bytes (>= 64).
 *
 * @return true  → wire response is `OK <resp>\n` (or just `OK\n` if resp is empty).
 *         false → wire response is `ERR <code> <resp>\n`; set the global error
 *                 code with cmd_set_err() before returning, or it defaults to
 *                 ESPSHELL_E_INTERNAL.
 */
typedef bool (*cmd_handler_fn)(int argc, char **argv, char *resp, size_t resp_sz);

/**
 * cmd_register() — add a command to the dispatcher.
 *
 * Names are matched case-sensitive, in uppercase by convention. `help` is a
 * one-line description (≤ 80 chars). Returns false if the table is full or
 * the name is already taken.
 *
 * MUST be called from core_init() or project_init(), never from a command
 * handler — the table is unlocked only at boot.
 */
bool cmd_register(const char *name, cmd_handler_fn fn, const char *help);

/**
 * cmd_set_err() — set the error code reported when a handler returns false.
 * Stored in task-local storage. Defaults to ESPSHELL_E_INTERNAL.
 */
void cmd_set_err(int code);

/**
 * cmd_dispatch() — parse one line, look up, run, and format the response.
 *
 * Called by net.c after AEAD decryption. `line` may be mutated (in-place
 * tokenization). `out` receives a full wire-format response WITHOUT trailing
 * newline. Returns the number of bytes written to `out`, or -1 on overflow.
 */
int cmd_dispatch(char *line, char *out, size_t out_sz);

/* Iteration — used by CMDS / HELP built-ins. */
typedef struct {
    const char *name;
    const char *help;
} cmd_entry_view_t;

size_t cmd_count(void);
bool   cmd_get(size_t idx, cmd_entry_view_t *out);
const char *cmd_help_for(const char *name);  /* NULL if not found */

#endif /* ESPSHELL_CMD_H */
