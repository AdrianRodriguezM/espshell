# esp-ctl

Linux CLI for the **espshell** firmware. Speaks the `espshell/1` protocol:
HMAC-SHA256 challenge over a TCP connection, HKDF-derived session key,
ChaCha20-Poly1305 AEAD framing.

## Build

```sh
make            # produces ./esp-ctl
make install    # installs to /usr/local/bin (PREFIX=, DESTDIR= supported)
```

Requires `libcrypto` (OpenSSL ≥ 1.1).

## Quick start

First boot of the ESP32 prints a random hex token over UART — copy it.

```sh
mkdir -p ~/.config/esp-ctl
cp devices.example.toml ~/.config/esp-ctl/devices.toml
$EDITOR ~/.config/esp-ctl/devices.toml   # set host + token
chmod 600 ~/.config/esp-ctl/devices.toml
```

## Usage

```sh
# explicit args
esp-ctl --host 192.168.1.50 --token-file ~/.espshell/token ping

# via profile
esp-ctl --device default stats
esp-ctl --device default send GPIO_SET 2 HIGH
esp-ctl --device default send I2C_INIT 0 21 22 400000
esp-ctl --device default send I2C_SCAN 0

# interactive REPL
esp-ctl --device default shell

# follow logs / async events
esp-ctl --device default logs

# OTA firmware update (SHA-256 verified end-to-end)
esp-ctl --device default ota upload build/espshell.bin
```

## Exit codes

| code | meaning |
|------|---------|
| 0    | success |
| 1    | usage error |
| 2    | connect or auth failure |
| 3    | protocol / IO error |
| 4    | remote returned `ERR` |

## Security notes

- The shared token never crosses the wire. The handshake uses an HMAC over
  fresh nonces, and the session key is derived via HKDF-SHA256.
- `~/.config/esp-ctl/devices.toml` MUST be mode `0600`; the CLI refuses to
  read it otherwise.
- Tokens are wiped from process memory after the session key is derived.
