/* SPDX-License-Identifier: GPL-3.0-or-later
 * profile.c — tiny TOML-subset parser for ~/.config/esp-ctl/devices.toml.
 *
 * Supported syntax:
 *   # comment
 *   [name]
 *   host  = "1.2.3.4"
 *   port  = 9000
 *   token = "hex..."
 *
 * Anything else is ignored. The file must be mode 0600.
 */
#define _POSIX_C_SOURCE 200809L

#include "profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>

static char *home_dir(void)
{
    const char *h = getenv("HOME");
    if (h) return strdup(h);
    struct passwd *pw = getpwuid(getuid());
    return pw ? strdup(pw->pw_dir) : strdup("/tmp");
}

static char *path_for_config(void)
{
    char *h = home_dir();
    char *p = NULL;
    if (asprintf(&p, "%s/.config/esp-ctl/devices.toml", h) < 0) p = NULL;
    free(h);
    return p;
}

static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
}

static char *unquote(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '"') {
        s++;
        char *e = strchr(s, '"');   /* closing quote; ignore any trailing comment */
        if (e) *e = '\0';
    }
    return strdup(s);
}

int profile_load(const char *name, profile_t *out)
{
    if (!name || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->port = 9000;

    char *path = path_for_config();
    if (!path) return -1;

    struct stat st;
    if (stat(path, &st) != 0) { free(path); return -1; }
    if ((st.st_mode & 0077) != 0) {
        fprintf(stderr, "esp-ctl: refusing to read %s with mode %04o (need 0600)\n",
                path, st.st_mode & 0777);
        free(path);
        return -1;
    }

    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return -1;

    char line[1024];
    int in_section = 0;
    while (fgets(line, sizeof(line), f)) {
        rstrip(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        if (*p == '[') {
            char *e = strchr(p, ']');
            if (!e) continue;
            *e = '\0';
            in_section = (strcmp(p + 1, name) == 0);
            continue;
        }
        if (!in_section) continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = p;
        char *v = eq + 1;
        rstrip(k);
        while (*v == ' ' || *v == '\t') v++;

        if      (!strcmp(k, "host"))  { free(out->host);  out->host  = unquote(v); }
        else if (!strcmp(k, "port"))  { out->port = atoi(v); }
        else if (!strcmp(k, "token")) { free(out->token); out->token = unquote(v); }
    }
    fclose(f);

    if (!out->host || !out->token) { profile_free(out); return -1; }
    return 0;
}

void profile_free(profile_t *p)
{
    if (!p) return;
    free(p->host);  p->host  = NULL;
    free(p->token); p->token = NULL;
    p->port = 0;
}
