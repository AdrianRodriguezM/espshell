/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host-side tests for the cmd tokenizer and dispatcher.
 * No hardware, no ESP-IDF — compiles with plain cc.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cmd.h"
#include "errors.h"

static int g_pass, g_fail;

#define CHECK(label, expr) do { \
    if (expr) { printf("  PASS  %s\n", label); g_pass++; } \
    else       { printf("  FAIL  %s  (line %d)\n", label, __LINE__); g_fail++; } \
} while (0)

/* ---- test handlers ---------------------------------------------------- */

static int   g_argc;
static char  g_argv0[128], g_argv1[128], g_argv2[128];

static bool h_echo(int argc, char **argv, char *r, size_t s)
{
    g_argc = argc;
    g_argv0[0] = g_argv1[0] = g_argv2[0] = '\0';
    if (argc > 0) snprintf(g_argv0, sizeof(g_argv0), "%s", argv[0]);
    if (argc > 1) snprintf(g_argv1, sizeof(g_argv1), "%s", argv[1]);
    if (argc > 2) snprintf(g_argv2, sizeof(g_argv2), "%s", argv[2]);
    snprintf(r, s, "argc=%d", argc);
    return true;
}

static bool h_fail(int argc, char **argv, char *r, size_t s)
{
    (void)argc; (void)argv;
    snprintf(r, s, "reason");
    cmd_set_err(ESPSHELL_E_BAD_ARGS);
    return false;
}

/* ---- tests ------------------------------------------------------------ */

static void test_empty_line(void)
{
    char line[8] = "";
    char out[64];
    int n = cmd_dispatch(line, out, sizeof(out));
    CHECK("empty line → OK", n > 0 && strncmp(out, "OK", 2) == 0);
}

static void test_whitespace_only(void)
{
    char line[8] = "   ";
    char out[64];
    int n = cmd_dispatch(line, out, sizeof(out));
    CHECK("whitespace-only → OK", n > 0 && strncmp(out, "OK", 2) == 0);
}

static void test_unknown_command(void)
{
    char line[16] = "NOPE";
    char out[64];
    int n = cmd_dispatch(line, out, sizeof(out));
    CHECK("unknown command → ERR 1", n > 0 && strncmp(out, "ERR 1", 5) == 0);
}

static void test_simple_args(void)
{
    cmd_register("ECHO", h_echo, "test");
    g_argc = -1;
    char line[32] = "ECHO hello world";
    char out[64];
    cmd_dispatch(line, out, sizeof(out));
    CHECK("simple args: argc=2",    g_argc == 2);
    CHECK("simple args: argv[0]",   strcmp(g_argv0, "hello") == 0);
    CHECK("simple args: argv[1]",   strcmp(g_argv1, "world") == 0);
    CHECK("simple args: OK prefix", strncmp(out, "OK", 2) == 0);
}

static void test_no_args(void)
{
    g_argc = -1;
    char line[8] = "ECHO";
    char out[64];
    cmd_dispatch(line, out, sizeof(out));
    CHECK("no args: argc=0", g_argc == 0);
}

static void test_quoted_arg(void)
{
    g_argc = -1; g_argv0[0] = '\0';
    char line[32] = "ECHO \"hello world\"";
    char out[64];
    cmd_dispatch(line, out, sizeof(out));
    CHECK("quoted arg: argc=1",   g_argc == 1);
    CHECK("quoted arg: value",    strcmp(g_argv0, "hello world") == 0);
}

static void test_escaped_quote(void)
{
    g_argc = -1; g_argv0[0] = '\0';
    char line[32] = "ECHO \"a\\\"b\"";
    char out[64];
    cmd_dispatch(line, out, sizeof(out));
    CHECK("escaped quote: argc=1", g_argc == 1);
    CHECK("escaped quote: value",  strcmp(g_argv0, "a\"b") == 0);
}

static void test_escaped_backslash(void)
{
    g_argc = -1; g_argv0[0] = '\0';
    char line[32] = "ECHO \"a\\\\b\"";
    char out[64];
    cmd_dispatch(line, out, sizeof(out));
    CHECK("escaped backslash: argc=1", g_argc == 1);
    CHECK("escaped backslash: value",  strcmp(g_argv0, "a\\b") == 0);
}

static void test_mixed_quoted_unquoted(void)
{
    g_argc = -1;
    char line[48] = "ECHO plain \"with spaces\" last";
    char out[64];
    cmd_dispatch(line, out, sizeof(out));
    CHECK("mixed args: argc=3",    g_argc == 3);
    CHECK("mixed args: argv[0]",   strcmp(g_argv0, "plain") == 0);
    CHECK("mixed args: argv[1]",   strcmp(g_argv1, "with spaces") == 0);
    CHECK("mixed args: argv[2]",   strcmp(g_argv2, "last") == 0);
}

static void test_handler_error(void)
{
    cmd_register("FAIL", h_fail, "test");
    char line[8] = "FAIL";
    char out[64];
    cmd_dispatch(line, out, sizeof(out));
    CHECK("handler error → ERR 2", strncmp(out, "ERR 2", 5) == 0);
}

static void test_duplicate_register(void)
{
    bool first  = cmd_register("UNIQ", h_echo, "first");
    bool second = cmd_register("UNIQ", h_echo, "second");
    CHECK("first register ok",      first  == true);
    CHECK("duplicate register fails", second == false);
}

/* ----------------------------------------------------------------------- */

int main(void)
{
    printf("=== tokenizer / dispatcher tests ===\n");
    test_empty_line();
    test_whitespace_only();
    test_unknown_command();
    test_simple_args();
    test_no_args();
    test_quoted_arg();
    test_escaped_quote();
    test_escaped_backslash();
    test_mixed_quoted_unquoted();
    test_handler_error();
    test_duplicate_register();
    printf("--- %d passed, %d failed ---\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
