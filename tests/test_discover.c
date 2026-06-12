/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host-side tests for the esp-ctl mDNS discovery parser.
 *
 * Includes discover.c directly to reach its static functions; builds a
 * synthetic mDNS response (with RFC 1035 compression pointers, the usual
 * failure spot for hand-rolled DNS parsers) and feeds it to parse_response.
 */
#include "../tools/esp-ctl/src/discover.c"

static int g_pass, g_fail;

#define CHECK(label, expr) do { \
    if (expr) { printf("  PASS  %s\n", label); g_pass++; } \
    else       { printf("  FAIL  %s  (line %d)\n", label, __LINE__); g_fail++; } \
} while (0)

/* Append a DNS-encoded name ("\x05bench\x09_espshell..." style) from dotted
 * notation. Returns new offset. */
static size_t put_name(uint8_t *p, size_t off, const char *dotted)
{
    const char *s = dotted;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        p[off++] = (uint8_t)l;
        memcpy(p + off, s, l);
        off += l;
        s += l + (dot ? 1 : 0);
    }
    p[off++] = 0;
    return off;
}

static size_t put_u16(uint8_t *p, size_t off, uint16_t v)
{
    p[off++] = (uint8_t)(v >> 8);
    p[off++] = (uint8_t)v;
    return off;
}

/* Build a realistic response: PTR + SRV + TXT + A, with the SRV/TXT record
 * names given as compression pointers back to the PTR rdata. */
static size_t build_packet(uint8_t *p)
{
    size_t off = 0;
    memset(p, 0, 512);
    off = put_u16(p, off, 0);        /* id */
    off = put_u16(p, off, 0x8400);   /* flags: response, authoritative */
    off = put_u16(p, off, 0);        /* qdcount */
    off = put_u16(p, off, 4);        /* ancount */
    off = put_u16(p, off, 0);        /* nscount */
    off = put_u16(p, off, 0);        /* arcount */

    /* PTR: _espshell._tcp.local → bench._espshell._tcp.local */
    size_t svc_name_off = off;
    off = put_name(p, off, "_espshell._tcp.local");
    off = put_u16(p, off, TYPE_PTR);
    off = put_u16(p, off, 1);
    off += 4;                                  /* ttl = 0 (ignored) */
    size_t rdlen_at = off; off += 2;
    size_t inst_off = off;                     /* rdata: instance name */
    p[off++] = 5; memcpy(p + off, "bench", 5); off += 5;
    off = put_u16(p, off, 0xc000 | (uint16_t)svc_name_off);  /* ptr to svc */
    put_u16(p, rdlen_at, (uint16_t)(off - rdlen_at - 2));

    /* SRV: bench._espshell._tcp.local → port 9000, target bench.local */
    off = put_u16(p, off, 0xc000 | (uint16_t)inst_off);      /* name = ptr */
    off = put_u16(p, off, TYPE_SRV);
    off = put_u16(p, off, 1);
    off += 4;
    rdlen_at = off; off += 2;
    off = put_u16(p, off, 0);                  /* priority */
    off = put_u16(p, off, 0);                  /* weight */
    off = put_u16(p, off, 9000);               /* port */
    size_t host_off = off;
    off = put_name(p, off, "bench.local");
    put_u16(p, rdlen_at, (uint16_t)(off - rdlen_at - 2));

    /* TXT: proto=1 chip=esp32 */
    off = put_u16(p, off, 0xc000 | (uint16_t)inst_off);
    off = put_u16(p, off, TYPE_TXT);
    off = put_u16(p, off, 1);
    off += 4;
    rdlen_at = off; off += 2;
    p[off++] = 7;  memcpy(p + off, "proto=1", 7);     off += 7;
    p[off++] = 10; memcpy(p + off, "chip=esp32", 10); off += 10;
    put_u16(p, rdlen_at, (uint16_t)(off - rdlen_at - 2));

    /* A: bench.local → 192.168.1.42 */
    off = put_u16(p, off, 0xc000 | (uint16_t)host_off);
    off = put_u16(p, off, TYPE_A);
    off = put_u16(p, off, 1);
    off += 4;
    off = put_u16(p, off, 4);
    p[off++] = 192; p[off++] = 168; p[off++] = 1; p[off++] = 42;
    return off;
}

int main(void)
{
    printf("=== discover (mDNS parser) tests ===\n");

    uint8_t pkt[512];
    size_t len = build_packet(pkt);
    struct in_addr src;
    inet_pton(AF_INET, "192.168.1.42", &src);

    device_t d;
    bool ok = parse_response(pkt, len, &src, &d);
    CHECK("full response: parsed", ok);
    CHECK("instance == bench",      strcmp(d.instance, "bench") == 0);
    CHECK("hostname == bench.local", strcmp(d.hostname, "bench.local") == 0);
    CHECK("ip == 192.168.1.42",     strcmp(d.ip, "192.168.1.42") == 0);
    CHECK("port == 9000",           d.port == 9000);
    CHECK("txt joined",             strcmp(d.txt, "proto=1 chip=esp32") == 0);

    /* Foreign service must be ignored. */
    uint8_t other[512];
    size_t olen = 12;
    memset(other, 0, sizeof(other));
    other[7] = 1;                                   /* ancount = 1 */
    olen = put_name(other, olen, "_http._tcp.local");
    olen = put_u16(other, olen, TYPE_PTR);
    olen = put_u16(other, olen, 1);
    olen += 4;
    olen = put_u16(other, olen, 7);
    other[olen++] = 5; memcpy(other + olen, "webby", 5); olen += 5;
    other[olen++] = 0;
    CHECK("foreign service: ignored", !parse_response(other, olen, &src, &d));

    /* Truncated/garbage input must not crash or match. */
    CHECK("truncated header: rejected", !parse_response(pkt, 8, &src, &d));
    uint8_t loop[16] = {0,0,0x84,0,0,0,0,1,0,0,0,0, 0xc0,12, 0,0};
    CHECK("pointer loop: rejected", !parse_response(loop, sizeof(loop), &src, &d));

    printf("--- %d passed, %d failed ---\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
