/* SPDX-License-Identifier: GPL-3.0-or-later
 * test_profile.c - regression tests for the devices.toml parser.
 *
 * A quoted value followed by an inline comment must parse to just the quoted
 * content. `esp-ctl discover` prints exactly such lines
 *   host = "1.2.3.4"   # or "name.local" with nss-mdns
 * and the earlier strrchr-based unquote() swallowed the comment, yielding a
 * bogus hostname that hung name resolution.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>

static int run, pass;
#define CHECK(c) do { run++; if (c) pass++; \
    else printf("  FAIL: %s (line %d)\n", #c, __LINE__); } while (0)

static char home[] = "/tmp/espctl_test_XXXXXX";

static void write_toml(const char *body)
{
    char dir[600], path[700], cmd[800];
    snprintf(dir, sizeof(dir), "%s/.config/esp-ctl", home);
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    if (system(cmd) != 0) { perror("mkdir"); exit(2); }
    snprintf(path, sizeof(path), "%s/devices.toml", dir);
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
    chmod(path, 0600);
}

int main(void)
{
    if (!mkdtemp(home)) { perror("mkdtemp"); return 2; }
    setenv("HOME", home, 1);

    /* Regression: discover's own output shape (quoted host + inline comment). */
    write_toml(
        "[espshell]\n"
        "host = \"192.168.1.160\"   # or \"espshell.local\" with nss-mdns\n"
        "port = 9000\n"
        "token = \"aa7f19672357ef16964d224148c67929c8713583508e88a2bb7e9981319552ae\"\n");
    profile_t p;
    int rc = profile_load("espshell", &p);
    CHECK(rc == 0);
    CHECK(rc == 0 && strcmp(p.host, "192.168.1.160") == 0);
    CHECK(rc == 0 && p.port == 9000);
    CHECK(rc == 0 && strcmp(p.token,
        "aa7f19672357ef16964d224148c67929c8713583508e88a2bb7e9981319552ae") == 0);
    if (rc == 0) profile_free(&p);

    /* Plain quoted values (no trailing comment) still parse. */
    write_toml(
        "[default]\n"
        "host  = \"192.168.1.148\"\n"
        "port  = 9000\n"
        "token = \"5bfa511a7b5074304c939354e9ef3015209ce599be17c02e2a54cf0fb25a213c\"\n");
    rc = profile_load("default", &p);
    CHECK(rc == 0);
    CHECK(rc == 0 && strcmp(p.host, "192.168.1.148") == 0);
    if (rc == 0) profile_free(&p);

    printf("test_profile: %d/%d passed\n", pass, run);
    return pass == run ? 0 : 1;
}
