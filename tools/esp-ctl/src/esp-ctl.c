/* SPDX-License-Identifier: GPL-3.0-or-later
 * esp-ctl — Linux CLI for the espshell firmware.
 *
 * Usage:
 *   esp-ctl --host <ip> [--port 9000] (--token <s> | --token-file <path>) <subcmd> [args]
 *
 * Subcommands:
 *   ping
 *   info
 *   stats
 *   cmds
 *   send <CMD> [args...]
 *   shell                 interactive REPL
 *   logs                  follow EVT lines until Ctrl-C
 *
 * Exit codes: 0 success, 1 usage, 2 connect/auth, 3 protocol/IO, 4 remote ERR.
 */
#define _POSIX_C_SOURCE 200809L

#include "proto.h"
#include "profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/evp.h>

extern bool ec_rand(uint8_t *out, size_t n);
extern bool ec_hmac_sha256(const uint8_t *key, size_t key_len,
                           const uint8_t *parts[], const size_t lens[], size_t nparts,
                           uint8_t out[32]);
extern bool ec_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                           const uint8_t *salt, size_t salt_len,
                           const uint8_t *info, size_t info_len,
                           uint8_t *out, size_t out_len);

/* ---------- hex ---------- */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool hex_decode(const char *s, size_t expect, uint8_t *out)
{
    if (!s || strlen(s) != expect * 2) return false;
    for (size_t i = 0; i < expect; i++) {
        int hi = hex_nibble(s[i*2]), lo = hex_nibble(s[i*2+1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i*2]   = H[in[i] >> 4];
        out[i*2+1] = H[in[i] & 0x0f];
    }
    out[n*2] = '\0';
}

/* ---------- IO helpers ---------- */
static ssize_t read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, (char *)buf + got, n - got, 0);
        if (r == 0) return 0;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        got += r;
    }
    return (ssize_t)got;
}
static bool write_full(int fd, const void *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = send(fd, (const char *)buf + sent, n - sent, 0);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        sent += w;
    }
    return true;
}
static int read_line(int fd, char *out, size_t cap)
{
    size_t i = 0;
    while (i < cap - 1) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') {
            if (i && out[i-1] == '\r') i--;
            out[i] = '\0';
            return (int)i;
        }
        out[i++] = c;
    }
    return -1;
}
static bool send_line(int fd, const char *line)
{
    size_t n = strlen(line);
    return write_full(fd, line, n) && write_full(fd, "\n", 1);
}

/* ---------- frame IO ---------- */
static bool send_cmd(int fd, session_t *s, const char *cmd)
{
    size_t cmd_len = strlen(cmd);
    if (cmd_len > ESPSHELL_MAX_LINE) return false;
    uint8_t buf[ESPSHELL_FRAME_HEADER_LEN + ESPSHELL_MAX_LINE + ESPSHELL_FRAME_TAG_LEN];
    size_t  fl;
    if (!frame_encrypt(s, (const uint8_t *)cmd, cmd_len, buf, sizeof(buf), &fl))
        return false;
    return write_full(fd, buf, fl);
}

static int recv_reply(int fd, session_t *s, char *out, size_t out_cap)
{
    uint8_t frame[ESPSHELL_FRAME_HEADER_LEN + ESPSHELL_MAX_LINE + ESPSHELL_FRAME_TAG_LEN];
    if (read_full(fd, frame, 2) <= 0) return -1;
    uint16_t fl = ((uint16_t)frame[0] << 8) | frame[1];
    if (fl < ESPSHELL_FRAME_HEADER_LEN + ESPSHELL_FRAME_TAG_LEN || fl > sizeof(frame))
        return -1;
    if (read_full(fd, frame + 2, fl - 2) <= 0) return -1;

    size_t pt_len = 0;
    if (!frame_decrypt(s, frame, fl, (uint8_t *)out, out_cap - 1, &pt_len)) return -1;
    out[pt_len] = '\0';
    return (int)pt_len;
}

/* ---------- handshake ---------- */
static bool do_handshake(int fd, const char *token, session_t *out_sess)
{
    char hello[512];
    if (read_line(fd, hello, sizeof(hello)) < 0) {
        fprintf(stderr, "no HELLO from server\n"); return false;
    }
    if (strncmp(hello, ESPSHELL_PROTO_VERSION " HELLO ",
                strlen(ESPSHELL_PROTO_VERSION " HELLO ")) != 0) {
        fprintf(stderr, "bad HELLO: %s\n", hello); return false;
    }
    char *nonce_hex = strstr(hello, "nonce=");
    if (!nonce_hex) { fprintf(stderr, "no nonce in HELLO\n"); return false; }
    nonce_hex += 6;

    uint8_t snonce[ESPSHELL_AUTH_NONCE_LEN];
    if (!hex_decode(nonce_hex, ESPSHELL_AUTH_NONCE_LEN, snonce)) {
        fprintf(stderr, "bad nonce in HELLO\n"); return false;
    }

    uint8_t cnonce[ESPSHELL_AUTH_NONCE_LEN];
    if (!ec_rand(cnonce, sizeof(cnonce))) {
        fprintf(stderr, "rng failure\n"); return false;
    }

    /* HMAC = HMAC-SHA256(token, "espshell-auth-v1" || snonce || cnonce) */
    const uint8_t *parts[3] = {
        (const uint8_t *)ESPSHELL_PROTO_AUTH_INFO,
        snonce, cnonce,
    };
    const size_t lens[3] = {
        sizeof(ESPSHELL_PROTO_AUTH_INFO) - 1,
        ESPSHELL_AUTH_NONCE_LEN, ESPSHELL_AUTH_NONCE_LEN,
    };
    uint8_t hmac[ESPSHELL_AUTH_HMAC_LEN];
    if (!ec_hmac_sha256((const uint8_t *)token, strlen(token), parts, lens, 3, hmac)) {
        fprintf(stderr, "hmac failure\n"); return false;
    }

    char cnonce_hex[ESPSHELL_AUTH_NONCE_LEN * 2 + 1];
    char hmac_hex  [ESPSHELL_AUTH_HMAC_LEN  * 2 + 1];
    hex_encode(cnonce, sizeof(cnonce), cnonce_hex);
    hex_encode(hmac,   sizeof(hmac),   hmac_hex);
    char authline[256];
    snprintf(authline, sizeof(authline), "AUTH cnonce=%s hmac=%s", cnonce_hex, hmac_hex);
    if (!send_line(fd, authline)) { fprintf(stderr, "send AUTH failed\n"); return false; }

    char reply[128];
    if (read_line(fd, reply, sizeof(reply)) < 0) {
        fprintf(stderr, "no AUTH reply\n"); return false;
    }
    if (strncmp(reply, "OK", 2) != 0) {
        fprintf(stderr, "auth rejected: %s\n", reply); return false;
    }

    /* Derive session key. */
    uint8_t salt[ESPSHELL_AUTH_NONCE_LEN * 2];
    memcpy(salt,                            snonce, ESPSHELL_AUTH_NONCE_LEN);
    memcpy(salt + ESPSHELL_AUTH_NONCE_LEN,  cnonce, ESPSHELL_AUTH_NONCE_LEN);
    uint8_t key[ESPSHELL_SESSION_KEY_LEN];
    if (!ec_hkdf_sha256((const uint8_t *)token, strlen(token),
                        salt, sizeof(salt),
                        (const uint8_t *)ESPSHELL_PROTO_SESS_INFO,
                        sizeof(ESPSHELL_PROTO_SESS_INFO) - 1,
                        key, sizeof(key))) {
        fprintf(stderr, "hkdf failure\n"); return false;
    }
    session_init(out_sess, key);
    /* Wipe sensitive locals. */
    volatile uint8_t *v;
    v = key;  for (size_t i = 0; i < sizeof(key); i++) v[i] = 0;
    v = salt; for (size_t i = 0; i < sizeof(salt); i++) v[i] = 0;
    return true;
}

/* ---------- token loading ---------- */
static char *load_token_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }
    char buf[ESPSHELL_MAX_LINE];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return NULL; }
    fclose(f);
    size_t n = strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) buf[--n] = '\0';
    return strdup(buf);
}

/* ---------- connect ---------- */
static int do_connect(const char *host, int port)
{
    char ps[8]; snprintf(ps, sizeof(ps), "%d", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    int rc = getaddrinfo(host, ps, &hints, &res);
    if (rc) { fprintf(stderr, "resolve %s: %s\n", host, gai_strerror(rc)); return -1; }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { fprintf(stderr, "connect %s:%d failed: %s\n", host, port, strerror(errno)); return -1; }
    int yes = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return fd;
}

/* ---------- subcommands ---------- */
static int print_reply(const char *reply)
{
    if (!strncmp(reply, "OK", 2)) {
        const char *rest = reply + 2;
        while (*rest == ' ') rest++;
        if (*rest) puts(rest); else puts("OK");
        return 0;
    }
    if (!strncmp(reply, "ERR ", 4)) { fprintf(stderr, "%s\n", reply); return 4; }
    if (!strncmp(reply, "EVT ", 4)) { puts(reply); return 0; }
    puts(reply);
    return 0;
}

/* Commands expect a terminal OK/ERR reply, but async EVT lines may arrive
 * interleaved. Consume and print EVTs until we get a non-EVT line. */
static int recv_cmd_reply(int fd, session_t *s, char *out, size_t out_cap)
{
    for (;;) {
        int n = recv_reply(fd, s, out, out_cap);
        if (n < 0) return n;
        if (!strncmp(out, "EVT ", 4)) {
            puts(out);
            fflush(stdout);
            continue;
        }
        return n;
    }
}

static int do_one_shot(int fd, session_t *s, const char *cmd)
{
    if (!send_cmd(fd, s, cmd)) { fprintf(stderr, "send failed\n"); return 3; }
    char reply[8192];
    int n = recv_cmd_reply(fd, s, reply, sizeof(reply));
    if (n < 0) { fprintf(stderr, "recv/decrypt failed\n"); return 3; }
    return print_reply(reply);
}

static volatile sig_atomic_t g_stop;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static int do_logs(int fd, session_t *s)
{
    /* Subscribe (LOG_STREAM ON) then keep reading EVTs until Ctrl-C. */
    if (!send_cmd(fd, s, "LOG_STREAM ON")) return 3;
    char ack[256];
    if (recv_reply(fd, s, ack, sizeof(ack)) < 0) return 3;

    signal(SIGINT, on_sigint);
    char line[8192];
    while (!g_stop) {
        int n = recv_reply(fd, s, line, sizeof(line));
        if (n < 0) return 3;
        puts(line);
        fflush(stdout);
    }
    return 0;
}

static int do_shell(int fd, session_t *s)
{
    char line[ESPSHELL_MAX_LINE];
    char reply[8192];
    for (;;) {
        fputs("espshell> ", stdout); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) { putchar('\n'); return 0; }
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;
        if (!strcmp(line, "exit") || !strcmp(line, "quit")) return 0;

        if (!send_cmd(fd, s, line)) { fprintf(stderr, "send failed\n"); return 3; }
        if (recv_cmd_reply(fd, s, reply, sizeof(reply)) < 0) { fprintf(stderr, "recv failed\n"); return 3; }
        puts(reply);
    }
}

/* ---------- ota upload ---------- */
static int do_ota_upload(int fd, session_t *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 3; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) { fclose(f); fprintf(stderr, "empty file\n"); return 3; }
    rewind(f);

    /* Compute SHA-256 of the binary (EVP, works on 1.1 and 3.x). */
    uint8_t *buf = malloc((size_t)size);
    if (!buf) { fclose(f); return 3; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf); fclose(f); fprintf(stderr, "read short\n"); return 3;
    }
    fclose(f);
    uint8_t sha[32];
    unsigned sha_len = 0;
    EVP_MD_CTX *mc = EVP_MD_CTX_new();
    if (!mc ||
        EVP_DigestInit_ex(mc, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(mc, buf, (size_t)size) != 1 ||
        EVP_DigestFinal_ex(mc, sha, &sha_len) != 1 ||
        sha_len != 32) {
        EVP_MD_CTX_free(mc); free(buf); fprintf(stderr, "sha256 failed\n"); return 3;
    }
    EVP_MD_CTX_free(mc);
    char sha_hex[65];
    hex_encode(sha, 32, sha_hex);

    /* OTA_BEGIN */
    char line[ESPSHELL_MAX_LINE], reply[8192];
    snprintf(line, sizeof(line), "OTA_BEGIN %ld %s", size, sha_hex);
    if (!send_cmd(fd, s, line)) { free(buf); return 3; }
    if (recv_reply(fd, s, reply, sizeof(reply)) < 0) { free(buf); return 3; }
    if (strncmp(reply, "OK", 2) != 0) { free(buf); fprintf(stderr, "%s\n", reply); return 4; }

    /* Chunks: hex-encoded, fit into ESPSHELL_MAX_LINE minus the "OTA_DATA " prefix.
     * Max plaintext payload = ESPSHELL_MAX_LINE; prefix 9 bytes. */
    const size_t max_hex = (ESPSHELL_MAX_LINE - 16) & ~1u;
    const size_t chunk   = max_hex / 2;
    size_t sent = 0;
    while (sent < (size_t)size) {
        size_t n = (size_t)size - sent;
        if (n > chunk) n = chunk;
        size_t off = snprintf(line, sizeof(line), "OTA_DATA ");
        for (size_t i = 0; i < n && off + 2 < sizeof(line); i++) {
            static const char H[] = "0123456789abcdef";
            line[off++] = H[buf[sent+i] >> 4];
            line[off++] = H[buf[sent+i] & 0xf];
        }
        line[off] = '\0';
        if (!send_cmd(fd, s, line)) { free(buf); return 3; }
        if (recv_reply(fd, s, reply, sizeof(reply)) < 0) { free(buf); return 3; }
        if (strncmp(reply, "OK", 2) != 0) { free(buf); fprintf(stderr, "%s\n", reply); return 4; }
        sent += n;
        if ((sent % (32 * 1024)) < chunk) {
            fprintf(stderr, "\r  %zu / %ld bytes", sent, size); fflush(stderr);
        }
    }
    fprintf(stderr, "\r  %zu / %ld bytes\n", sent, size);
    free(buf);

    if (!send_cmd(fd, s, "OTA_END")) return 3;
    if (recv_reply(fd, s, reply, sizeof(reply)) < 0) return 3;
    return print_reply(reply);
}

/* ---------- main ---------- */
static void usage(void)
{
    fputs(
"Usage: esp-ctl [--device <name>] | [--host <ip> [--port 9000]\n"
"                                    (--token <s> | --token-file <path>)]\n"
"               <subcommand> [args]\n"
"\n"
"Profiles live in ~/.config/esp-ctl/devices.toml (mode 0600).\n"
"\n"
"Subcommands:\n"
"  ping | info | stats | cmds\n"
"  send <CMD> [args...]    one-shot, prints OK/ERR/EVT line\n"
"  shell                   interactive REPL\n"
"  logs                    enable LOG_STREAM and follow EVTs (Ctrl-C to stop)\n"
"  ota upload <file>       chunked firmware upload (SHA-256 verified)\n"
        , stderr);
}

int main(int argc, char **argv)
{
    const char *host = NULL;
    const char *token = NULL;
    const char *token_file = NULL;
    const char *device = NULL;
    int port = 9000;

    static struct option opts[] = {
        {"host",       required_argument, 0, 'H'},
        {"port",       required_argument, 0, 'p'},
        {"token",      required_argument, 0, 't'},
        {"token-file", required_argument, 0, 'f'},
        {"device",     required_argument, 0, 'd'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "H:p:t:f:d:h", opts, NULL)) != -1) {
        switch (c) {
        case 'H': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 't': token = optarg; break;
        case 'f': token_file = optarg; break;
        case 'd': device = optarg; break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }

    profile_t prof = {0};
    char *tok_mem = NULL;
    if (device) {
        if (profile_load(device, &prof) != 0) {
            fprintf(stderr, "profile '%s' not found or unreadable\n", device);
            return 1;
        }
        host  = prof.host;
        port  = prof.port;
        token = prof.token;
    }

    if (!host || (!token && !token_file) || optind >= argc) {
        usage(); profile_free(&prof); return 1;
    }

    if (!token) {
        tok_mem = load_token_file(token_file);
        if (!tok_mem) { profile_free(&prof); return 2; }
        token = tok_mem;
    }

    int fd = do_connect(host, port);
    if (fd < 0) { free(tok_mem); profile_free(&prof); return 2; }

    session_t sess;
    if (!do_handshake(fd, token, &sess)) {
        close(fd); free(tok_mem); profile_free(&prof); return 2;
    }
    /* Token no longer needed after key derivation. */
    if (tok_mem) { volatile char *p = tok_mem; while (*p) *p++ = 0; free(tok_mem); tok_mem = NULL; }
    profile_free(&prof);   /* also wipes prof.token */

    const char *sub = argv[optind];
    int rc = 0;
    if      (!strcmp(sub, "ping"))  rc = do_one_shot(fd, &sess, "PING");
    else if (!strcmp(sub, "info"))  rc = do_one_shot(fd, &sess, "INFO");
    else if (!strcmp(sub, "stats")) rc = do_one_shot(fd, &sess, "STATS");
    else if (!strcmp(sub, "cmds"))  rc = do_one_shot(fd, &sess, "CMDS");
    else if (!strcmp(sub, "logs"))  rc = do_logs(fd, &sess);
    else if (!strcmp(sub, "shell")) rc = do_shell(fd, &sess);
    else if (!strcmp(sub, "ota") && optind + 2 < argc && !strcmp(argv[optind+1], "upload")) {
        rc = do_ota_upload(fd, &sess, argv[optind+2]);
    }
    else if (!strcmp(sub, "send") && optind + 1 < argc) {
        char buf[ESPSHELL_MAX_LINE];
        size_t off = 0;
        for (int i = optind + 1; i < argc; i++) {
            int w = snprintf(buf + off, sizeof(buf) - off, "%s%s",
                             i == optind + 1 ? "" : " ", argv[i]);
            if (w < 0 || (size_t)w >= sizeof(buf) - off) {
                fprintf(stderr, "command too long\n"); rc = 1; goto done;
            }
            off += (size_t)w;
        }
        rc = do_one_shot(fd, &sess, buf);
    } else {
        usage(); rc = 1;
    }

done:
    session_wipe(&sess);
    close(fd);
    return rc;
}
