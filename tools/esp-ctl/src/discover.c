/* SPDX-License-Identifier: GPL-3.0-or-later
 * discover.c — one-shot mDNS discovery of _espshell._tcp devices.
 *
 * Sends a single multicast PTR query from an ephemeral port (RFC 6762 §5.1
 * "one-shot" / legacy-unicast mode: responders reply unicast to our port),
 * collects answers for a fixed window and prints them as a ready-to-paste
 * devices.toml section. No dependency on avahi/nss-mdns.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define MDNS_GROUP   "224.0.0.251"
#define MDNS_PORT    5353
#define SERVICE      "\x09_espshell\x04_tcp\x05local"   /* DNS-encoded */
#define TYPE_PTR     12
#define TYPE_SRV     33
#define TYPE_TXT     16
#define TYPE_A       1
#define MAX_DEVICES  16

typedef struct {
    char     instance[64];   /* "bench" in bench._espshell._tcp.local */
    char     hostname[64];   /* SRV target, "bench.local" */
    char     ip[INET_ADDRSTRLEN];
    uint16_t port;
    char     txt[128];       /* "proto=1 chip=esp32 fw=v0.1.1" */
} device_t;

/* ---- DNS name handling (with RFC 1035 compression pointers) ------------- */

/* Expand the name at `off` into out ("a.b.c", no trailing dot). Returns the
 * offset just past the name *as stored* (not past pointer targets), -1 on
 * malformed input. */
static int name_read(const uint8_t *pkt, size_t len, size_t off,
                     char *out, size_t cap)
{
    size_t o = off, w = 0;
    int end = -1;            /* set when we follow the first pointer */
    int hops = 0;
    while (o < len) {
        uint8_t l = pkt[o];
        if (l == 0) {
            if (w < cap) out[w] = '\0'; else out[cap - 1] = '\0';
            return end >= 0 ? end : (int)(o + 1);
        }
        if ((l & 0xc0) == 0xc0) {            /* compression pointer */
            if (o + 1 >= len || ++hops > 16) return -1;
            if (end < 0) end = (int)(o + 2);
            o = ((size_t)(l & 0x3f) << 8) | pkt[o + 1];
            continue;
        }
        if (o + 1 + l > len) return -1;
        if (w && w < cap) out[w++] = '.';
        for (uint8_t i = 0; i < l; i++) {
            if (w < cap - 1) out[w++] = (char)pkt[o + 1 + i];
        }
        o += 1 + l;
    }
    return -1;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* ---- response parsing ---------------------------------------------------- */

/* Parse one response packet; fill dev. Returns true if it described an
 * _espshell._tcp instance. The device's IP falls back to the packet source
 * if no A record is present. */
static bool parse_response(const uint8_t *pkt, size_t len,
                           const struct in_addr *src, device_t *dev)
{
    if (len < 12) return false;
    size_t qd = rd16(pkt + 4), an = rd16(pkt + 6) + rd16(pkt + 8) + rd16(pkt + 10);
    size_t off = 12;
    bool found = false;
    char a_host[64] = "", a_ip[INET_ADDRSTRLEN] = "";

    memset(dev, 0, sizeof(*dev));

    /* Skip echoed questions: name + type(2) + class(2). */
    for (size_t i = 0; i < qd; i++) {
        char tmp[256];
        int n = name_read(pkt, len, off, tmp, sizeof(tmp));
        if (n < 0 || (size_t)n + 4 > len) return false;
        off = (size_t)n + 4;
    }

    for (size_t i = 0; i < an && off < len; i++) {
        char name[256];
        int n = name_read(pkt, len, off, name, sizeof(name));
        if (n < 0 || (size_t)n + 10 > len) break;
        const uint8_t *rr = pkt + n;
        uint16_t type  = rd16(rr);
        uint16_t rdlen = rd16(rr + 8);
        size_t   rdoff = (size_t)n + 10;
        if (rdoff + rdlen > len) break;
        off = rdoff + rdlen;

        if (type == TYPE_PTR && strstr(name, "_espshell._tcp")) {
            char inst[256];
            if (name_read(pkt, len, rdoff, inst, sizeof(inst)) < 0) continue;
            char *dot = strchr(inst, '.');     /* keep instance label only */
            if (dot) *dot = '\0';
            snprintf(dev->instance, sizeof(dev->instance), "%.63s", inst);
            found = true;
        } else if (type == TYPE_SRV && strstr(name, "_espshell._tcp")) {
            if (rdlen < 7) continue;
            dev->port = rd16(pkt + rdoff + 4);
            name_read(pkt, len, rdoff + 6, dev->hostname, sizeof(dev->hostname));
        } else if (type == TYPE_TXT && strstr(name, "_espshell._tcp")) {
            size_t w = 0, p = rdoff;
            while (p < rdoff + rdlen && pkt[p]) {
                uint8_t l = pkt[p];
                if (p + 1 + l > rdoff + rdlen) break;
                if (w && w < sizeof(dev->txt) - 1) dev->txt[w++] = ' ';
                for (uint8_t j = 0; j < l && w < sizeof(dev->txt) - 1; j++)
                    dev->txt[w++] = (char)pkt[p + 1 + j];
                p += 1 + l;
            }
            dev->txt[w] = '\0';
        } else if (type == TYPE_A && rdlen == 4) {
            snprintf(a_host, sizeof(a_host), "%.63s", name);
            inet_ntop(AF_INET, pkt + rdoff, a_ip, sizeof(a_ip));
        }
    }

    if (!found) return false;
    /* Prefer the A record matching the SRV target; else the packet source. */
    if (a_ip[0] && (!dev->hostname[0] || strcmp(a_host, dev->hostname) == 0))
        snprintf(dev->ip, sizeof(dev->ip), "%s", a_ip);
    else
        inet_ntop(AF_INET, src, dev->ip, sizeof(dev->ip));
    if (!dev->instance[0]) snprintf(dev->instance, sizeof(dev->instance), "espshell");
    if (!dev->port) dev->port = 9000;
    return true;
}

/* ---- public entry --------------------------------------------------------- */

int do_discover(int wait_ms)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 3; }

    /* Query: header (id 0, flags 0, qdcount 1) + QNAME + PTR + IN. */
    uint8_t q[64];
    size_t  qn = 0;
    memset(q, 0, 12);
    q[5] = 1;                                   /* qdcount = 1 */
    qn = 12;
    memcpy(q + qn, SERVICE, sizeof(SERVICE));   /* includes trailing \0 */
    qn += sizeof(SERVICE);
    q[qn++] = 0; q[qn++] = TYPE_PTR;
    q[qn++] = 0; q[qn++] = 1;                   /* class IN */

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port   = htons(MDNS_PORT),
    };
    inet_pton(AF_INET, MDNS_GROUP, &dst.sin_addr);
    if (sendto(fd, q, qn, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        perror("sendto 224.0.0.251:5353");
        close(fd);
        return 3;
    }

    device_t devs[MAX_DEVICES];
    int ndev = 0;

    struct timeval deadline, now;
    gettimeofday(&deadline, NULL);
    deadline.tv_sec  += wait_ms / 1000;
    deadline.tv_usec += (wait_ms % 1000) * 1000;

    for (;;) {
        gettimeofday(&now, NULL);
        long left_us = (deadline.tv_sec - now.tv_sec) * 1000000L
                     + (deadline.tv_usec - now.tv_usec);
        if (left_us <= 0) break;
        struct timeval tv = { left_us / 1000000L, left_us % 1000000L };

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        int r = select(fd + 1, &rf, NULL, NULL, &tv);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            break;
        }

        uint8_t pkt[1500];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(fd, pkt, sizeof(pkt), 0,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;

        device_t d;
        if (!parse_response(pkt, (size_t)n, &from.sin_addr, &d)) continue;

        bool dup = false;
        for (int i = 0; i < ndev; i++)
            if (strcmp(devs[i].instance, d.instance) == 0) dup = true;
        if (!dup && ndev < MAX_DEVICES) devs[ndev++] = d;
    }
    close(fd);

    if (ndev == 0) {
        fprintf(stderr, "no espshell devices found in %d ms\n", wait_ms);
        return 4;
    }

    printf("# %d device(s) found — paste into ~/.config/esp-ctl/devices.toml\n", ndev);
    for (int i = 0; i < ndev; i++) {
        printf("\n[%s]\n", devs[i].instance);
        printf("host = \"%s\"", devs[i].ip);
        if (devs[i].hostname[0]) printf("   # or \"%s\" with nss-mdns", devs[i].hostname);
        printf("\nport = %u\n", devs[i].port);
        if (devs[i].txt[0]) printf("# %s\n", devs[i].txt);
        printf("# token = \"<paste the device token here>\"\n");
    }
    return 0;
}
