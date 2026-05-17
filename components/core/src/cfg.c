/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cfg.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"

#define NS  "espshell"
#define TAG "cfg"

static nvs_handle_t s_h;
static bool s_open;

void cfg_init(void)
{
    if (s_open) return;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &s_h));
    s_open = true;
}

bool cfg_get_str(const char *key, char *out, size_t out_sz)
{
    if (!s_open || !key || !out || out_sz == 0) return false;
    size_t sz = out_sz;
    esp_err_t err = nvs_get_str(s_h, key, out, &sz);
    return err == ESP_OK;
}

bool cfg_set_str(const char *key, const char *value)
{
    if (!s_open || !key || !value) return false;
    if (strlen(value) >= ESPSHELL_CFG_MAX_VALUE) return false;
    return nvs_set_str(s_h, key, value) == ESP_OK && nvs_commit(s_h) == ESP_OK;
}

bool cfg_get_u32(const char *key, uint32_t *out)
{
    if (!s_open || !key || !out) return false;
    return nvs_get_u32(s_h, key, out) == ESP_OK;
}

bool cfg_set_u32(const char *key, uint32_t value)
{
    if (!s_open || !key) return false;
    return nvs_set_u32(s_h, key, value) == ESP_OK && nvs_commit(s_h) == ESP_OK;
}

bool cfg_get_blob(const char *key, void *out, size_t *inout_sz)
{
    if (!s_open || !key || !out || !inout_sz) return false;
    return nvs_get_blob(s_h, key, out, inout_sz) == ESP_OK;
}

bool cfg_set_blob(const char *key, const void *data, size_t sz)
{
    if (!s_open || !key || !data) return false;
    return nvs_set_blob(s_h, key, data, sz) == ESP_OK && nvs_commit(s_h) == ESP_OK;
}

bool cfg_del(const char *key)
{
    if (!s_open || !key) return false;
    esp_err_t err = nvs_erase_key(s_h, key);
    if (err == ESP_OK) (void)nvs_commit(s_h);
    return err == ESP_OK;
}

bool cfg_get_str_or_default(const char *key, char *out, size_t out_sz,
                            const char *dflt)
{
    if (cfg_get_str(key, out, out_sz) && out[0] != '\0') return true;
    strncpy(out, dflt ? dflt : "", out_sz - 1);
    out[out_sz - 1] = '\0';
    return false;
}

void cfg_iterate(const char *prefix, cfg_iter_fn fn, void *ud)
{
    if (!s_open || !fn) return;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NS, NVS_TYPE_ANY, &it);
    while (err == ESP_OK && it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        bool match = !prefix || prefix[0] == '\0' ||
                     strncmp(info.key, prefix, strlen(prefix)) == 0;
        if (match && !fn(info.key, ud)) break;
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
}

void cfg_commit(void)
{
    if (s_open) (void)nvs_commit(s_h);
}
