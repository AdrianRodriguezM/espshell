/* SPDX-License-Identifier: GPL-3.0-or-later
 * fs.c — SPIFFS commands for the `storage` partition.
 */
#include "cmd.h"
#include "errors.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_spiffs.h"

#define MOUNT "/sp"
#define TAG   "fs"
#define MAX_FILE_BYTES 8192

static bool s_mounted;

static void ensure_mount(void)
{
    if (s_mounted) return;
    esp_vfs_spiffs_conf_t cfg = {
        .base_path = MOUNT, .partition_label = "storage",
        .max_files = 5, .format_if_mount_failed = true,
    };
    if (esp_vfs_spiffs_register(&cfg) == ESP_OK) s_mounted = true;
    else LOG_W(TAG, "spiffs mount failed");
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hex_to_bytes(const char *str, uint8_t *out, size_t cap)
{
    size_t n = strlen(str);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble(str[i*2]), lo = hex_nibble(str[i*2+1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}
static void bytes_to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i*2] = H[b[i] >> 4]; out[i*2+1] = H[b[i] & 0xf]; }
    out[n*2] = '\0';
}

static void join_path(char *out, size_t cap, const char *user)
{
    if (user[0] == '/') snprintf(out, cap, MOUNT "%s", user);
    else                snprintf(out, cap, MOUNT "/%s", user);
}

static bool c_info(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v;
    ensure_mount();
    size_t total, used;
    if (esp_spiffs_info("storage", &total, &used) != ESP_OK) {
        cmd_set_err(ESPSHELL_E_HW_FAIL); return false;
    }
    snprintf(r, s, "total=%u used=%u free=%u", (unsigned)total, (unsigned)used, (unsigned)(total - used));
    return true;
}

static bool c_list(int c, char **v, char *r, size_t s)
{
    ensure_mount();
    const char *path = c >= 1 ? v[0] : "/";
    char full[64];
    join_path(full, sizeof(full), path);
    DIR *d = opendir(full);
    if (!d) { cmd_set_err(ESPSHELL_E_NOT_FOUND); snprintf(r, s, "open"); return false; }
    int w = 0;
    struct dirent *de;
    while ((de = readdir(d)) && w < (int)s - 64) {
        struct stat st;
        char fp[320];
        snprintf(fp, sizeof(fp), "%s/%s", full, de->d_name);
        size_t sz = stat(fp, &st) == 0 ? (size_t)st.st_size : 0;
        w += snprintf(r + w, s - w, "%s%s:%u", w ? " " : "", de->d_name, (unsigned)sz);
    }
    closedir(d);
    return true;
}

static bool c_read(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    ensure_mount();
    char full[64];
    join_path(full, sizeof(full), v[0]);
    FILE *f = fopen(full, "rb");
    if (!f) { cmd_set_err(ESPSHELL_E_NOT_FOUND); snprintf(r, s, "open"); return false; }
    uint8_t buf[MAX_FILE_BYTES];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n * 2 + 1 > s) { cmd_set_err(ESPSHELL_E_NO_MEM); snprintf(r, s, "too big"); return false; }
    bytes_to_hex(buf, n, r);
    return true;
}

static bool c_write(int c, char **v, char *r, size_t s)
{
    if (c != 2) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    ensure_mount();
    uint8_t buf[MAX_FILE_BYTES];
    int n = hex_to_bytes(v[1], buf, sizeof(buf));
    if (n < 0) { cmd_set_err(ESPSHELL_E_BAD_ARGS); snprintf(r, s, "hex"); return false; }
    char full[64];
    join_path(full, sizeof(full), v[0]);
    FILE *f = fopen(full, "wb");
    if (!f) { cmd_set_err(ESPSHELL_E_HW_FAIL); snprintf(r, s, "open"); return false; }
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    if ((int)w != n) { cmd_set_err(ESPSHELL_E_HW_FAIL); return false; }
    snprintf(r, s, "%d", n);
    return true;
}

static bool c_del(int c, char **v, char *r, size_t s)
{
    if (c != 1) { cmd_set_err(ESPSHELL_E_BAD_ARGS); return false; }
    ensure_mount();
    char full[64];
    join_path(full, sizeof(full), v[0]);
    if (unlink(full) != 0) { cmd_set_err(ESPSHELL_E_NOT_FOUND); snprintf(r, s, "rm"); return false; }
    return true;
}

static bool c_format(int c, char **v, char *r, size_t s)
{
    (void)c; (void)v; (void)r; (void)s;
    if (esp_spiffs_format("storage") != ESP_OK) { cmd_set_err(ESPSHELL_E_HW_FAIL); return false; }
    s_mounted = false;
    return true;
}

void fs_module_init(void)
{
    cmd_register("FS_INFO",   c_info,   "Mount info: total/used/free bytes");
    cmd_register("FS_LIST",   c_list,   "FS_LIST [path]");
    cmd_register("FS_READ",   c_read,   "FS_READ <path> (returns hex)");
    cmd_register("FS_WRITE",  c_write,  "FS_WRITE <path> <hex>");
    cmd_register("FS_DEL",    c_del,    "FS_DEL <path>");
    cmd_register("FS_FORMAT", c_format, "FS_FORMAT (destructive)");
}
