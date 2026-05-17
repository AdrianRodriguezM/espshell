# espshell

[![CI](https://github.com/AdrianRodriguezM/espshell/actions/workflows/build.yml/badge.svg)](https://github.com/AdrianRodriguezM/espshell/actions/workflows/build.yml)

A reusable application-layer firmware on top of ESP-IDF that exposes the full
capability surface of any ESP32-family chip to a Linux host over an encrypted
TCP shell-style command protocol. Each downstream project builds on top of the
core and only adds its own command handlers.

> **Status:** Phases 1–4 implemented (WiFi, TCP, auth, AEAD, OTA, peripherals,
> filesystem, power). Phase 5 (TLS / forward secrecy) is not yet implemented —
> use on a trusted LAN or VPN.

## Supported targets

- `esp32`     (Xtensa LX6, dual-core, BT classic + BLE 4.2, DAC, Hall sensor)
- `esp32s3`   (LX7, dual-core, USB-OTG, BLE 5)
- `esp32c3`   (RISC-V single-core, BLE 5)
- `esp32c6`   (RISC-V, BLE 5 + Thread + Zigbee)

Capability matrix in `components/core/include/targets.h`.

## Security model

- Transport: TCP over WiFi, single active client per device.
- Handshake (cleartext): server emits `HELLO` with a 32-byte random nonce;
  client responds with `AUTH cnonce=… hmac=…` where
  `hmac = HMAC-SHA256(token, "espshell-auth-v1" || snonce || cnonce)`.
- Session keys: `HKDF-SHA256(token, salt = snonce ‖ cnonce, info = "espshell-session-v1")`.
- All subsequent frames are binary, length-prefixed, **ChaCha20-Poly1305 AEAD**.
  Per-direction monotonic sequence numbers serve as the AEAD nonce → strict
  anti-replay.
- Rate limit: 3 failed auth attempts → 10-second cool-down, then drop.
- The token never crosses the wire. `MEM_READ/WRITE` are gated behind
  `CONFIG_ESPSHELL_ENABLE_MEM_CMDS` (off by default).

Full spec: [`docs/protocol.md`](docs/protocol.md).

## Built-in commands

### System
`PING` · `INFO` · `STATS` · `UPTIME` · `HEAP` · `TASKS` · `RESET_REASON` ·
`REBOOT [ms]` · `FACTORY_RESET` · `CMDS` · `HELP <cmd>`

### Config (NVS)
`CFG_GET <key>` · `CFG_SET <key> <value>` · `CFG_DEL <key>` ·
`CFG_LIST [prefix]` · `CFG_COMMIT`
(`wifi_pass` and `auth_token` are redacted on read.)

### Logging
`LOG_LEVEL <0..5>` · `LOG_STREAM ON|OFF`

### WiFi
`WIFI_STATUS` · `WIFI_SCAN` · `WIFI_SET <ssid> <pass>` · `WIFI_RECONNECT`

### GPIO
`GPIO_MODE <pin> <INPUT|OUTPUT|INPUT_PULLUP|INPUT_PULLDOWN>` ·
`GPIO_SET <pin> <HIGH|LOW>` · `GPIO_GET <pin>` · `GPIO_TOGGLE <pin>` ·
`GPIO_WATCH <pin> <RISING|FALLING|ANY>` · `GPIO_UNWATCH <pin>`

### Analog
`ADC_READ <ch>` · `ADC_READ_MV <ch>` · `ADC_STREAM <ch> <ms>` ·
`ADC_STREAM_STOP <ch>` · `DAC_WRITE <ch> <0..255>` (ESP32 only)

### PWM
`PWM_INIT <ch> <pin> <freq> <res>` · `PWM_SET <ch> <duty>` · `PWM_STOP <ch>`

### Buses
`I2C_INIT <port> <sda> <scl> <hz>` · `I2C_SCAN <port>` ·
`I2C_READ <port> <addr> <reg> <n>` · `I2C_WRITE <port> <addr> <reg> <hex>`

`SPI_INIT <host> <miso> <mosi> <sclk> <cs> <hz> <mode>` · `SPI_TXRX <host> <hex>`

`UART_INIT <port> <tx> <rx> <baud>` · `UART_WRITE <port> <hex>` ·
`UART_READ <port> <n> <timeout_ms>` · `UART_STREAM <port> ON|OFF`

### Filesystem (SPIFFS @ `storage` partition)
`FS_INFO` · `FS_LIST [path]` · `FS_READ <path>` · `FS_WRITE <path> <hex>` ·
`FS_DEL <path>` · `FS_FORMAT`

### Time
`TIME_GET` · `TIME_SET <unix_ts>` · `SNTP_SYNC [server]`

### Power
`SLEEP_LIGHT <ms>` · `SLEEP_DEEP <ms>` · `CPU_FREQ <80|160|240>`

### OTA (chunked, SHA-256 verified)
`OTA_BEGIN <size> <sha256>` · `OTA_DATA <hex>` · `OTA_END` ·
`OTA_ABORT` · `OTA_ROLLBACK`

### Diagnostics
`CHIP_TEMP` · `HALL_READ` (ESP32 only) ·
`MEM_READ <addr> <n>` / `MEM_WRITE <addr> <hex>` (Kconfig-gated, off by default)

### BLE
`BLE_SCAN <s>` · `BLE_ADVERTISE <name>` · `BLE_STOP` — stubs in v1; full
NimBLE integration in v2 (adds ~150 KB flash, needs partition rebalance).

## Async events (`EVT <type> <data>`)

`LOG` · `HEALTH` · `GPIO` · `ADC` · `UART`,
plus arbitrary `EVT PROJECT <data>` emitted by downstream code via `net_send_event()`.

## Project extension

```c
#include "core.h"

static bool cmd_read_bme(int argc, char **argv, char *resp, size_t sz) {
    /* ... */
    snprintf(resp, sz, "t=%.2f h=%.2f", t, h);
    return true;
}

void project_init(void) {
    cmd_register("READ_BME", cmd_read_bme, "Read BME280 sensor");
}
```

## Tutorial

Full step-by-step walkthrough in [`docs/TUTORIAL.md`](docs/TUTORIAL.md) —
from `git clone` to running your own custom command over OTA.

## Build & flash

```sh
git clone https://github.com/AdrianRodriguezM/espshell && cd espshell
idf.py set-target esp32        # or esp32s3 / esp32c3 / esp32c6
idf.py menuconfig              # espshell core → set WiFi creds + token (or leave blank to auto-generate)
idf.py build flash monitor
```

On the very first boot the device prints a random hex token over UART unless
you set one in menuconfig or via `CFG_SET auth_token <hex>`.

## Build the CLI

```sh
cd tools/esp-ctl
make
cp devices.example.toml ~/.config/esp-ctl/devices.toml
$EDITOR ~/.config/esp-ctl/devices.toml
chmod 600 ~/.config/esp-ctl/devices.toml
./esp-ctl --device default shell
```

## License

Licensed under **GNU GPL v3.0 or later** (`GPL-3.0-or-later`, SPDX).
See [`LICENSE`](LICENSE).
