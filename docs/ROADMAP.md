# espshell — roadmap

Notes on what could come next. Companion to `docs/protocol.md` (wire spec) and
the README command reference. Items are roughly ordered by how useful they seem
versus how much work they are, but it's not a commitment — just a list.

Each item notes whether it needs a wire-protocol bump. Flash estimates are
rough (release build, esp32).

---

## Done (v0.1.1)

Two protocol fixes that had to land before anything else:

- **Response framing limit.** `cmd_dispatch()` could produce up to 4096 bytes,
  but `send_frame()` capped plaintext at 1024 and tore the session down on a
  failed send, so any reply of 1025–4095 bytes silently killed the connection.
  Both ends now agree on `MAX_RESP`, and protocol.md documents the asymmetry
  (commands ≤ 1024, replies ≤ 4096).
- **Handshake timeout.** `recv_line()` blocked forever during the cleartext
  handshake, so an idle TCP connect could lock the device out under the
  single-client policy. Added `SO_RCVTIMEO` (~10 s) until the session is up.

## Done (v0.2.0)

- **mDNS advertisement + `esp-ctl discover`.** Device advertises
  `_espshell._tcp` on `IP_EVENT_STA_GOT_IP` with `proto`/`chip`/`fw` TXT
  records, so `devices.toml` no longer needs a fixed IP. `esp-ctl discover`
  does a one-shot multicast query and prints a ready-to-paste profile.

## Done (v0.3.0)

- **SoftAP first-boot provisioning.** When no `wifi_ssid` is configured in
  NVS or menuconfig, the device brings up an open WiFi AP
  (`espshell-XXYY`, where `XXYY` are the last two bytes of the SoftAP MAC)
  instead of refusing to start. The full encrypted shell runs on
  `192.168.4.1` at the configured TCP port — same protocol, same token auth,
  zero new attack surface. Connect to the AP, run `WIFI_SET <ssid> <pass>`
  then `REBOOT`; the device restarts in STA mode and announces itself via
  mDNS. Pairs with the existing mDNS flow to deliver the "flash it and find
  it" promise with no UART cable after the initial flash.
  Flash cost: ~384 bytes. No protocol bump. Kconfig-gated
  (`ESPSHELL_ENABLE_SOFTAP`, default y).

---

## Near-term

### Binary bulk frames (`type=2`)

Hex-over-command-line halves the usable payload, and the ACK-per-chunk OTA does
~3000 round-trips for a 1.5 MB image. A `type=2 BULK` frame (same AEAD
envelope, payload = `tag(u8) ‖ raw bytes`) doubles the effective chunk size and
opens the door to streaming FS transfer. With client-side pipelining (send N
frames, collect N ACKs — safe because the server is sequential and the AEAD seq
preserves order) OTA should get several times faster. Needs a proto bump to
`espshell/2`; batch it with the forward-secrecy work below so the version only
moves once.

### Forward secrecy

Full TLS 1.3 drags in certs and config weight that mostly duplicates what the
custom protocol already does. A lighter fit: an ephemeral X25519 exchange
authenticated by the existing token HMAC (Noise `NNpsk0` shape). HELLO/AUTH
carry ephemeral public keys alongside the nonces, the auth HMAC signs
`snonce ‖ cnonce ‖ epk_s ‖ epk_c`, and the session key becomes
`HKDF(ECDH_shared ‖ token, …)`. This gives forward secrecy and stops an
eavesdropper from verifying token guesses offline from a captured transcript.
mbedTLS already has X25519 via PSA, so no new dependency; the OpenSSL side is
small. Touches `auth.c`, `proto.c`, and `do_handshake()` on both ends. Once an
ephemeral secret exists, periodic rekey is a small follow-up.
Proto bump to `espshell/2` (same as bulk frames).

---

## Medium-term

### EVT subscriptions (`SUB`/`UNSUB`)

Right now every EVT source (LOG, HEALTH, GPIO, ADC, UART, PROJECT) goes to the
single client unconditionally, so `logs` mode and `shell` mode end up fighting
over the stream. A per-session bitmask filter checked in `net_send_event()`,
defaulting to all (so it stays backwards compatible), would allow quiet shells
and cheap dashboards. New commands only, no proto bump.

### Streaming FS transfer

`FS_READ`/`FS_WRITE` are capped at 8 KB and hex-encoded. On top of `type=2`
bulk frames: `FS_GET <path>` / `FS_PUT <path> <size>` with chunked binary
transfer and a SHA-256 trailer, mirroring the OTA state machine, then lift
`MAX_FILE_BYTES`. Depends on the bulk-frame work.

### esp-ctl shell improvements

Line editing + history + tab-completion fed from `CMDS`/`HELP` at session start
(linenoise vendored, no system deps). Plus a `--json` output mode for scripting
and reading commands from stdin when not a TTY, so `esp-ctl shell < script.txt`
works. Host-only.

### Fleet operations

`esp-ctl --all <cmd>` / `--devices a,b,c` iterating `devices.toml` profiles with
parallel connections and per-device prefixed output. `esp-ctl --all ota upload
fw.bin` is the obvious use. Host-side loop around existing code.

### NimBLE (BLE v2)

Replace the `ble.c` stubs with `BLE_SCAN`, `BLE_ADVERTISE`, `BLE_STOP`. This is
expensive: ~150 KB flash and ~40 KB RAM, which breaks the current
2×1.5 MB + 512 K SPIFFS layout on 4 MB flash. Options: shrink SPIFFS, gate BLE
to ≥8 MB targets (S3), or make it a Kconfig choice with two partition tables.
The partition story needs deciding before any code gets written. Large.

---

## Maybe someday

- **`INFER` — TFLite Micro bridge.** `INFER <model> <hex_input>` → `OK <hex>`,
  models loaded from SPIFFS. esp-tflite-micro is a managed component. Mostly
  interesting as an edge-AI experiment; flash cost depends on the model.
- **Thread / Zigbee on esp32c6.** `targets.h` already flags
  `ESPSHELL_HAS_THREAD`. An `OT_*` command family (form network, scan, send UDP)
  would make espshell a useful 802.15.4 lab tool. Big dependency (OpenThread),
  c6/h2 only — natural to pair with adding an esp32h2 target.
- **Ethernet (esp32 classic).** `ESPSHELL_HAS_ETHERNET` is flagged but unused.
  Only worth it with specific hardware (LAN8720) on the bench, and the `net.c`
  WiFi-event coupling would need an interface abstraction first. Parked.
- **Touch commands.** `ESPSHELL_HAS_TOUCH` exists for esp32/s3. A small driver
  mirroring the GPIO watch pattern (ISR → queue → EVT). Cheap once someone
  needs it.
- **Host-side metrics.** `esp-ctl metrics` mapping `EVT HEALTH` to Prometheus
  textfile / OpenMetrics. Keeps the firmware untouched.

---

## Decided against (for now)

- **Full TLS.** The forward-secrecy plan above gets the security properties
  with less code. Revisit only if a deployment actually needs PKI/cert rotation.
- **Multi-client sessions.** The single-client assumption is baked into the
  static dispatch buffer, per-command errno slot, and TX state. Fleet needs are
  better handled host-side.
- **HTTP/WebSocket gateway in firmware.** Anything web-shaped should be a
  host-side proxy speaking the native protocol.
- **Hex in new transfer paths.** New bulk transfer goes through `type=2` binary
  frames.

---

## Rough order

```
done            0.1.1 (framing + handshake fixes)
done            0.2.0 (mDNS discovery)
done            0.3.0 (SoftAP first-boot provisioning)
next            bulk frames + forward secrecy (one espshell/2 bump)
later           EVT subscriptions, shell improvements, fleet ops
                FS streaming (once type=2 exists)
someday         BLE / the exploratory stuff, by whim
```
