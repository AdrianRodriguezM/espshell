/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cmd.h"
#include "errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_ESPSHELL_MAX_REGISTERED_CMDS
#define CONFIG_ESPSHELL_MAX_REGISTERED_CMDS 128
#endif

typedef struct {
    char            name[ESPSHELL_CMD_MAX_NAME + 1];
    const char     *help;
    cmd_handler_fn  fn;
} cmd_entry_t;

static cmd_entry_t s_tbl[CONFIG_ESPSHELL_MAX_REGISTERED_CMDS];
static size_t      s_n;

/* Single active client policy → a single static slot is safe. If we ever
 * add multi-client support, promote this to FreeRTOS task-local storage. */
static int s_last_err;

void cmd_set_err(int code)        { s_last_err = code; }
static int  cmd_get_err(void)     { return s_last_err; }
static void cmd_clear_err(void)   { s_last_err = 0; }

bool cmd_register(const char *name, cmd_handler_fn fn, const char *help)
{
    if (!name || !fn) return false;
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > ESPSHELL_CMD_MAX_NAME) return false;
    if (s_n >= CONFIG_ESPSHELL_MAX_REGISTERED_CMDS) return false;

    for (size_t i = 0; i < s_n; i++) {
        if (strcmp(s_tbl[i].name, name) == 0) return false;
    }
    memcpy(s_tbl[s_n].name, name, nlen + 1);
    s_tbl[s_n].fn   = fn;
    s_tbl[s_n].help = help ? help : "";
    s_n++;
    return true;
}

size_t cmd_count(void) { return s_n; }

bool cmd_get(size_t idx, cmd_entry_view_t *out)
{
    if (idx >= s_n || !out) return false;
    out->name = s_tbl[idx].name;
    out->help = s_tbl[idx].help;
    return true;
}

const char *cmd_help_for(const char *name)
{
    for (size_t i = 0; i < s_n; i++) {
        if (strcmp(s_tbl[i].name, name) == 0) return s_tbl[i].help;
    }
    return NULL;
}

/* Tokeniser: splits on ASCII whitespace; supports double-quoted args with
 * \" and \\ escapes. Mutates `line` in place. Returns argc, with argv[]
 * pointing into `line`. */
static int tokenize(char *line, char **argv, int max_argv)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max_argv) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        if (*p == '"') {
            p++;
            argv[argc++] = p;
            char *w = p;
            while (*p && *p != '"') {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
                *w++ = *p++;
            }
            if (*p == '"') p++;
            *w = '\0';
        } else {
            argv[argc++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

int cmd_dispatch(char *line, char *out, size_t out_sz)
{
    if (!line || !out || out_sz < 8) return -1;

    char *argv[ESPSHELL_CMD_MAX_ARGV];
    int argc = tokenize(line, argv, ESPSHELL_CMD_MAX_ARGV);
    if (argc == 0) {
        /* Empty line: respond with empty OK. */
        return snprintf(out, out_sz, "OK");
    }

    const char *name = argv[0];
    for (size_t i = 0; i < s_n; i++) {
        if (strcmp(s_tbl[i].name, name) != 0) continue;

        char payload[CONFIG_ESPSHELL_MAX_RESP];
        payload[0] = '\0';
        cmd_clear_err();

        bool ok = s_tbl[i].fn(argc - 1, &argv[1], payload, sizeof(payload));
        if (ok) {
            if (payload[0] == '\0') return snprintf(out, out_sz, "OK");
            return snprintf(out, out_sz, "OK %s", payload);
        }
        int code = cmd_get_err();
        if (code == 0) code = ESPSHELL_E_INTERNAL;
        return snprintf(out, out_sz, "ERR %d %s",
                        code, payload[0] ? payload : "handler failed");
    }

    return snprintf(out, out_sz, "ERR %d unknown command: %s",
                    ESPSHELL_E_UNKNOWN_CMD, name);
}
