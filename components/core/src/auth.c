/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth.h"
#include "proto.h"
#include "cfg.h"
#include "logger.h"

#include <string.h>

#include "esp_random.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#define TAG "auth"
#define KEY_TOKEN "auth_token"

static uint8_t s_token[ESPSHELL_AUTH_MAX_TOKEN];
static size_t  s_token_len;

static void wipe(volatile void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--) *b++ = 0;
}

static void hexify(const uint8_t *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

void auth_init(void)
{
    psa_crypto_init();

#ifdef CONFIG_ESPSHELL_DEFAULT_AUTH_TOKEN
    const char *k_token = CONFIG_ESPSHELL_DEFAULT_AUTH_TOKEN;
#else
    const char *k_token = "";
#endif

    char buf[ESPSHELL_AUTH_MAX_TOKEN + 1] = {0};

    if (cfg_get_str(KEY_TOKEN, buf, sizeof(buf)) && buf[0] != '\0') {
        s_token_len = strlen(buf);
        memcpy(s_token, buf, s_token_len);
        LOG_I(TAG, "token loaded from NVS (%u bytes)", (unsigned)s_token_len);
        return;
    }

    if (k_token && k_token[0] != '\0') {
        s_token_len = strlen(k_token);
        if (s_token_len > ESPSHELL_AUTH_MAX_TOKEN) s_token_len = ESPSHELL_AUTH_MAX_TOKEN;
        memcpy(s_token, k_token, s_token_len);
        cfg_set_str(KEY_TOKEN, k_token);
        LOG_W(TAG, "using compile-time default token; rotate via CFG_SET auth_token <...>");
        return;
    }

    uint8_t raw[32];
    esp_fill_random(raw, sizeof(raw));
    char hex[sizeof(raw) * 2 + 1];
    hexify(raw, sizeof(raw), hex);
    s_token_len = strlen(hex);
    memcpy(s_token, hex, s_token_len);
    cfg_set_str(KEY_TOKEN, hex);

    printf("\n*** espshell first-boot token (copy this to your client) ***\n"
           "    %s\n"
           "*** rotate with CFG_SET auth_token <new> ***\n\n", hex);
    wipe(raw, sizeof(raw));
    wipe(hex, sizeof(hex));
}

void auth_make_nonce(uint8_t out[ESPSHELL_AUTH_NONCE_LEN])
{
    esp_fill_random(out, ESPSHELL_AUTH_NONCE_LEN);
}

bool auth_verify(const uint8_t snonce[ESPSHELL_AUTH_NONCE_LEN],
                 const uint8_t cnonce[ESPSHELL_AUTH_NONCE_LEN],
                 const uint8_t client_hmac[ESPSHELL_AUTH_HMAC_LEN])
{
    if (s_token_len == 0) return false;

    /* Import the token as an HMAC key. */
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&a, s_token_len * 8);

    psa_key_id_t kid = 0;
    if (psa_import_key(&a, s_token, s_token_len, &kid) != PSA_SUCCESS) return false;

    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    uint8_t expected[ESPSHELL_AUTH_HMAC_LEN];
    bool ok = false;

    if (psa_mac_sign_setup(&op, kid, PSA_ALG_HMAC(PSA_ALG_SHA_256)) != PSA_SUCCESS) goto out;
    if (psa_mac_update(&op, (const uint8_t *)ESPSHELL_PROTO_AUTH_INFO,
                       sizeof(ESPSHELL_PROTO_AUTH_INFO) - 1) != PSA_SUCCESS) goto out;
    if (psa_mac_update(&op, snonce, ESPSHELL_AUTH_NONCE_LEN) != PSA_SUCCESS) goto out;
    if (psa_mac_update(&op, cnonce, ESPSHELL_AUTH_NONCE_LEN) != PSA_SUCCESS) goto out;

    size_t mac_len = 0;
    if (psa_mac_sign_finish(&op, expected, sizeof(expected), &mac_len) != PSA_SUCCESS) goto out;
    if (mac_len != ESPSHELL_AUTH_HMAC_LEN) goto out;

    /* Constant-time compare. */
    uint8_t diff = 0;
    for (size_t i = 0; i < ESPSHELL_AUTH_HMAC_LEN; i++) diff |= expected[i] ^ client_hmac[i];
    ok = (diff == 0);

out:
    psa_mac_abort(&op);
    psa_destroy_key(kid);
    wipe(expected, sizeof(expected));
    return ok;
}

bool auth_derive_session_key(const uint8_t snonce[ESPSHELL_AUTH_NONCE_LEN],
                             const uint8_t cnonce[ESPSHELL_AUTH_NONCE_LEN],
                             uint8_t out_key[ESPSHELL_SESSION_KEY_LEN])
{
    if (s_token_len == 0) return false;

    uint8_t salt[ESPSHELL_AUTH_NONCE_LEN * 2];
    memcpy(salt,                            snonce, ESPSHELL_AUTH_NONCE_LEN);
    memcpy(salt + ESPSHELL_AUTH_NONCE_LEN,  cnonce, ESPSHELL_AUTH_NONCE_LEN);

    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    bool ok = false;

    if (psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) != PSA_SUCCESS) goto out;
    if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT,
                                       salt, sizeof(salt)) != PSA_SUCCESS) goto out;
    if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET,
                                       s_token, s_token_len) != PSA_SUCCESS) goto out;
    if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO,
                                       (const uint8_t *)ESPSHELL_PROTO_SESS_INFO,
                                       sizeof(ESPSHELL_PROTO_SESS_INFO) - 1) != PSA_SUCCESS) goto out;
    if (psa_key_derivation_output_bytes(&op, out_key, ESPSHELL_SESSION_KEY_LEN) != PSA_SUCCESS) goto out;
    ok = true;
out:
    psa_key_derivation_abort(&op);
    wipe(salt, sizeof(salt));
    return ok;
}
