/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ESPCTL_PROFILE_H
#define ESPCTL_PROFILE_H

typedef struct {
    char *host;
    int   port;
    char *token;
} profile_t;

/* Load profile `name` from ~/.config/esp-ctl/devices.toml. Returns 0 on hit.
 * The file is a minimal subset of TOML: sections `[name]` and key=value
 * (quoted strings). Owner read/write only; permissions are enforced. */
int profile_load(const char *name, profile_t *out);
void profile_free(profile_t *p);

#endif
