# espshell — feature roadmap

Forward-looking feature analysis. Companion to `docs/protocol.md` (wire spec)
and the README command reference. Ordered by value/effort, grounded in the
current codebase — each entry says what it reuses and what it forces.

Conventions: **flash** deltas are rough (release build, esp32); **proto** says
whether the wire protocol version must bump.

---

## Tier 0 — protocol fixes that gate everything below

These are prerequisites, not features. Shipping new surface on top of them
avoids doing the migration twice.

### 0.1 Response framing limit (`MAX_RESP` end-to-end)

`cmd_dispatch()` produces up to `CONFIG_ESPSHELL_MAX_RESP` (4096) bytes, but
`send_frame()` (net.c) caps plaintext at `CONFIG_ESPSHELL_MAX_LINE` (1024) and
`session_loop` tears the session down when the send fails — any reply of
1025–4095 bytes silently kills the connection. The CLI RX buffer is sized to
1024 too. Fix both sides to `MAX_RESP + header + tag`, document the asymmetry
in protocol.md (commands ≤ 1024, replies ≤ 4096), add a round-trip test at
2048 bytes. **Effort: small. Proto: clarification only.**

### 0.2 Handshake timeout

`recv_line()` blocks forever during the cleartext handshake; combined with the
single-client policy, an idle TCP connect locks the device out. `SO_RCVTIMEO`
(~10 s) on the client socket until the session is established.
**Effort: trivial. Proto: none.**

---

## Tier 1 — high value, low effort

### 1.1 mDNS advertisement + `esp-ctl discover`

Remove the fixed-IP requirement from `devices.toml`.

- **Firmware:** add the `espressif/mdns` managed component (`main/idf_component.yml`
  does not exist yet — create it). On `IP_EVENT_STA_GOT_IP`, advertise:
  - hostname: `device_name` from NVS/Kconfig — note `ESPSHELL_DEFAULT_DEVICE_NAME`
    already exists in Kconfig but is currently **referenced nowhere in the code**;
    this feature finally consumes it (and a `CFG_SET device_name` override).
  - service `_espshell._tcp` on `CONFIG_ESPSHELL_TCP_PORT`, TXT records:
    `proto=1`, `chip=<target>`, `fw=<version>`. Do **not** put anything secret
    or unique-identifying beyond what a port scan reveals anyway; keep parity
    with the deliberately anonymous HELLO.
  - ~40 lines in a new `mdns_svc.c` module + one line in `core_init()`.
- **Host:** `esp-ctl discover` — one-shot multicast PTR query + 2 s collect,
  printed as a ready-to-paste `devices.toml` section. Either ~150 lines of raw
  mDNS (no new deps) or shell out to `avahi-browse` when present. Also accept
  `host = "name.local"` in profiles (works only where nss-mdns is configured —
  document that; the built-in discover path must not depend on it).
- **Flash:** ~10 KB. **Proto:** none. **Effort: small.**

### 1.2 SoftAP first-boot provisioning

Kill the UART-only first-boot flow. If `wifi_ssid` is absent in NVS, start
SoftAP `espshell-<mac4>` (open or with the printed token as WPA2 pass),
run the *same* TCP shell on 192.168.4.1, accept `WIFI_SET`, reboot into STA.

- Reuses 100 % of the existing server/auth/protocol stack — the only new code
  is the AP-mode branch in `wifi_start()` and a `provisioned` check.
- Pairs naturally with 1.1: after the reboot the device announces itself over
  mDNS, so the user never needs to learn its DHCP address.
- **Flash:** ~0. **Proto:** none. **Effort: small-medium** (the WiFi init
  paths in `net.c` assume STA throughout; refactor `wifi_start()` first).

### 1.3 Binary bulk frames (`type=2`) + pipelined OTA

The hex-over-command-line transport halves payload and the synchronous
ACK-per-chunk makes OTA ~3000 round-trips for a 1.5 MB image.

- New frame `type=2 BULK`: same AEAD envelope, payload = `tag(u8) ‖ raw bytes`
  (tag: 1 = OTA data, 2 = FS write, 3 = FS read reply, …). Doubles effective
  chunk size immediately and unlocks streaming FS transfer (Tier 2).
- Client-side pipelining: send N bulk frames, then collect N ACKs (the server
  is sequential and AEAD seq preserves order, so windowing is safe with no
  firmware change beyond `type=2` support). Expect 5–10× OTA throughput.
- **Proto: bump to `espshell/2`** — batch this with the Tier-1 security work
  below so the version only bumps once.

### 1.4 Forward secrecy (redefining "Phase 5: TLS")

Full TLS 1.3 brings certificates and config weight that duplicates what the
custom protocol already does well. Better fit: an ephemeral **X25519 exchange
authenticated by the existing token HMAC** (Noise-`NNpsk0` shape):

- HELLO/AUTH carry ephemeral public keys alongside the nonces; the auth HMAC
  signs `snonce ‖ cnonce ‖ epk_s ‖ epk_c`; session key =
  `HKDF(ECDH_shared ‖ token, …)`.
- Buys forward secrecy **and** kills passive offline dictionary attacks on
  the token (an eavesdropper can no longer verify token guesses from a
  transcript). mbedTLS already provides X25519 via PSA (`PSA_ALG_ECDH`,
  Montgomery family) — no new dependency; OpenSSL side is ~30 lines.
- Touches `auth.c`, `proto.c`, `do_handshake()` on both ends. With an
  ephemeral secret in place, periodic rekey (every N frames) becomes a
  10-line addition.
- **Proto: `espshell/2`** (same bump as 1.3). **Effort: medium.**

---

## Tier 2 — medium effort, clear payoff

### 2.1 EVT subscriptions (`SUB <type>` / `UNSUB <type>`)

Today every EVT source (LOG, HEALTH, GPIO, ADC, UART, PROJECT) ships to the
single client unconditionally; `logs` mode and `shell` mode fight over the
stream. Per-session bitmask filter checked in `net_send_event()`, default =
all (backwards compatible). Enables quiet shells and cheap dashboards.
**Effort: small-medium. Proto: none** (new commands only).

### 2.2 Streaming FS transfer

`FS_READ`/`FS_WRITE` are capped at 8 KB and hex-encoded (and >512 B reads
currently trip bug 0.1). On top of `type=2` bulk frames: `FS_GET <path>` /
`FS_PUT <path> <size>` with chunked binary transfer and a SHA-256 trailer,
mirroring the OTA state machine. Then lift `MAX_FILE_BYTES`.
**Depends on 1.3. Effort: medium.**

### 2.3 esp-ctl interactive shell upgrade

readline (or linenoise, vendored, zero system deps) + history +
tab-completion fed from `CMDS`/`HELP` at session start. Also `--json` output
mode for scripting (`{"ok":true,"payload":"…"}`), and read commands from
stdin when not a TTY so `esp-ctl shell < script.txt` works.
**Effort: medium, host-only.**

### 2.4 Fleet operations

`esp-ctl --all <cmd>` / `--devices a,b,c` iterating `devices.toml` profiles
with parallel connections and per-device prefixed output; `esp-ctl --all ota
upload fw.bin` is the killer use. Pure host-side loop around existing code.
**Effort: small-medium, host-only.**

### 2.5 NimBLE (BLE v2)

Replace the `ble.c` stubs: `BLE_SCAN <s>` (active scan → `EVT BLE` lines),
`BLE_ADVERTISE <name>`, `BLE_STOP`. Honest costs: **~150 KB flash + ~40 KB
RAM**, which breaks the current 2×1.5 MB + 512 K SPIFFS layout on 4 MB flash.
Options: shrink SPIFFS to 256 K, or gate BLE to ≥8 MB targets (S3 modules),
or make it a Kconfig choice with two partition tables. Decide the partition
story *before* writing code. **Effort: large.**

---

## Tier 3 — strategic / exploratory

### 3.1 `INFER` — TFLite Micro bridge

`INFER <model> <hex_input>` → `OK <hex_output>`, models loaded from SPIFFS
(via 2.2). esp-tflite-micro is a managed component. Start with a keyword
classifier or anomaly detector on ADC streams. High learning value
(edge-AI track); flash cost depends entirely on the model (+~80 KB runtime).

### 3.2 Thread / Zigbee on esp32c6

`targets.h` already flags `ESPSHELL_HAS_THREAD`. An `OT_*` command family
(form network, scan, send UDP over Thread) would make espshell a useful
802.15.4 lab tool. Big dependency (OpenThread stack), c6/h2 only — natural
to pair with adding the **esp32h2 target**, which is mostly a `targets.h`
entry + `sdkconfig.defaults.esp32h2` + CI matrix line.

### 3.3 Ethernet (esp32 classic)

`ESPSHELL_HAS_ETHERNET` is flagged but unused. Only worth it with specific
hardware (LAN8720 boards) on the bench; the `net.c` WiFi-event coupling would
need an interface abstraction first. Park until there's a concrete need.

### 3.4 Touch commands (`TOUCH_READ`, `TOUCH_WATCH`)

`ESPSHELL_HAS_TOUCH` exists for esp32/s3. Small driver module mirroring the
GPIO watch pattern (ISR → queue → EVT). Nice-to-have; cheap once someone
actually needs it.

### 3.5 Host-side metrics bridge

`esp-ctl metrics` mapping `EVT HEALTH` to Prometheus textfile / OpenMetrics
stdout. Keeps the firmware untouched — observability belongs on the host.

---

## Non-goals (decided against, revisit only with new evidence)

- **Full TLS** — 1.4 achieves the security properties with less code and an
  auditable design; revisit only if a deployment needs PKI/cert rotation.
- **Multi-client sessions** — the single-client invariant is load-bearing
  (static dispatch buffer, per-command errno slot, TX state). Fleet needs are
  better served host-side (2.4).
- **HTTP/WebSocket gateway in firmware** — anything web-shaped should be a
  host-side proxy speaking the native protocol.
- **Hex in new bulk paths** — all new transfer surface goes through `type=2`
  binary frames (1.3).

## Suggested sequencing

```
0.1, 0.2            (one small PR — correctness)
1.1 + 1.2           (discovery + provisioning: "flash it and find it")
1.3 + 1.4           (one espshell/2 protocol bump: binary frames + PFS)
2.1 → 2.3 → 2.4     (UX wave, mostly host-side)
2.2                 (FS streaming, once type=2 exists)
2.5 / 3.x           (pick by current learning goal)
```
