/* SPDX-License-Identifier: GPL-3.0-or-later
 * Crypto helpers for esp-ctl (HMAC-SHA256, HKDF-SHA256, ChaCha20-Poly1305).
 * Implemented on top of OpenSSL libcrypto.
 */
#include "proto.h"

#include <string.h>
#include <stdlib.h>

/* HMAC_* on OpenSSL 3.x is "deprecated but works"; the EVP_MAC replacement
 * isn't available on 1.1.x. Silencing keeps the code dual-compatible. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#pragma GCC diagnostic pop

bool ec_rand(uint8_t *out, size_t n) { return RAND_bytes(out, (int)n) == 1; }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
bool ec_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *parts[], const size_t lens[], size_t nparts,
                    uint8_t out[32])
{
    HMAC_CTX *ctx = HMAC_CTX_new();
    if (!ctx) return false;
    bool ok = HMAC_Init_ex(ctx, key, (int)key_len, EVP_sha256(), NULL) == 1;
    for (size_t i = 0; ok && i < nparts; i++) {
        ok = HMAC_Update(ctx, parts[i], lens[i]) == 1;
    }
    unsigned mlen = 32;
    if (ok) ok = HMAC_Final(ctx, out, &mlen) == 1;
    HMAC_CTX_free(ctx);
    return ok && mlen == 32;
}
#pragma GCC diagnostic pop

bool ec_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                    const uint8_t *salt, size_t salt_len,
                    const uint8_t *info, size_t info_len,
                    uint8_t *out, size_t out_len)
{
    EVP_PKEY_CTX *p = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    if (!p) return false;
    bool ok = EVP_PKEY_derive_init(p) > 0
           && EVP_PKEY_CTX_set_hkdf_md(p, EVP_sha256()) > 0
           && EVP_PKEY_CTX_set1_hkdf_salt(p, salt, (int)salt_len) > 0
           && EVP_PKEY_CTX_set1_hkdf_key(p,  ikm, (int)ikm_len)   > 0
           && EVP_PKEY_CTX_add1_hkdf_info(p, info, (int)info_len) > 0;
    size_t l = out_len;
    if (ok) ok = EVP_PKEY_derive(p, out, &l) > 0 && l == out_len;
    EVP_PKEY_CTX_free(p);
    return ok;
}

/* ChaCha20-Poly1305 helpers. */
bool ec_chacha20_seal(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *pt,  size_t pt_len,
                      uint8_t *ct, uint8_t tag[16])
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    if (!c) return false;
    int len = 0;
    bool ok = EVP_EncryptInit_ex(c, EVP_chacha20_poly1305(), NULL, NULL, NULL) == 1
           && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) == 1
           && EVP_EncryptInit_ex(c, NULL, NULL, key, nonce) == 1;
    if (ok && aad_len) ok = EVP_EncryptUpdate(c, NULL, &len, aad, (int)aad_len) == 1;
    if (ok)            ok = EVP_EncryptUpdate(c, ct, &len, pt, (int)pt_len) == 1;
    int flen = 0;
    if (ok) ok = EVP_EncryptFinal_ex(c, ct + len, &flen) == 1;
    if (ok) ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(c);
    return ok;
}

bool ec_chacha20_open(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *ct,  size_t ct_len,
                      const uint8_t tag[16], uint8_t *pt)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    if (!c) return false;
    int len = 0;
    bool ok = EVP_DecryptInit_ex(c, EVP_chacha20_poly1305(), NULL, NULL, NULL) == 1
           && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) == 1
           && EVP_DecryptInit_ex(c, NULL, NULL, key, nonce) == 1;
    if (ok && aad_len) ok = EVP_DecryptUpdate(c, NULL, &len, aad, (int)aad_len) == 1;
    if (ok)            ok = EVP_DecryptUpdate(c, pt, &len, ct, (int)ct_len) == 1;
    if (ok) ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, 16, (void *)tag) == 1;
    int flen = 0;
    if (ok) ok = EVP_DecryptFinal_ex(c, pt + len, &flen) == 1;
    EVP_CIPHER_CTX_free(c);
    return ok;
}
