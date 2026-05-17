# espshell — getting started tutorial

End-to-end walkthrough: from a freshly cloned repo to running custom commands
on the device over an encrypted shell.

---

## 0. Prerequisites

- ESP-IDF v6.0+ installed and sourced (`. $IDF_PATH/export.sh`)
- An ESP32 family board (esp32 / esp32s3 / esp32c3 / esp32c6) on USB
- A serial driver in the host kernel for your board's USB-to-UART bridge
  (cp210x for CP210x, ftdi_sio for FTDI, ch341 for cheap clones)
- OpenSSL libs for the Linux CLI (`libssl-dev` / `openssl-devel`)

---

## 1. Configure WiFi and token

```sh
cd espshell
idf.py set-target esp32          # or esp32s3 / esp32c3 / esp32c6
idf.py menuconfig
```

Navigate to **`espshell core`** and set:

| Option | Recommended value |
|---|---|
| Default WiFi SSID | your network |
| Default WiFi password | the password |
| Default auth token | leave empty (auto-generated on first boot) |

Save (`S` → enter → `Q`).

Alternative without menuconfig — set it via `CFG_SET` from the CLI after the
device is online by some other means (UART console, USB-CDC, etc).

---

## 2. Build and flash

```sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Adjust the port (`ls /dev/ttyUSB* /dev/ttyACM*`). Press the BOOT button if
esptool is stuck on "Connecting…".

---

## 3. Capture the first-boot token

The monitor will show, once, something like:

```
*** espshell first-boot token (copy this to your client) ***
    7e2a91d8b4...64 hex chars total...
*** rotate with CFG_SET auth_token <new> ***

I (1234) net: got ip 192.168.1.123
I (1235) net: listening on tcp/9000
```

**Write down the token and the IP.** Exit the monitor with `Ctrl+]`.

If you ever lose the token, factory-reset the device:

```sh
# from the IDF console:
idf.py -p /dev/ttyUSB0 erase-flash
idf.py -p /dev/ttyUSB0 flash monitor
```

A fresh token is generated.

---

## 4. Build and configure the CLI

```sh
cd tools/esp-ctl
make                               # builds ./esp-ctl

mkdir -p ~/.config/esp-ctl
cp devices.example.toml ~/.config/esp-ctl/devices.toml
$EDITOR ~/.config/esp-ctl/devices.toml
chmod 600 ~/.config/esp-ctl/devices.toml
```

Edit the `[default]` block with your IP and token:

```toml
[default]
host  = "192.168.1.123"
port  = 9000
token = "7e2a91d8b4..."
```

---

## 5. First contact

```sh
./esp-ctl --device default ping
# → PONG

./esp-ctl --device default info
# → fw=1 chip=esp32 cores=2 rev=… mac=… idf=…

./esp-ctl --device default stats
# → uptime=… ram_free=… rssi=-58 …

./esp-ctl --device default cmds
# → CMDS HELP PING INFO STATS UPTIME … (about 50 commands)
```

If those four succeed: the firmware is healthy, the AEAD session was
established, and the HMAC handshake authenticated correctly.

---

## 6. Talk to hardware

Assuming a dev board with an LED on GPIO 2:

```sh
./esp-ctl --device default send GPIO_MODE 2 OUTPUT
./esp-ctl --device default send GPIO_SET  2 HIGH      # LED on
./esp-ctl --device default send GPIO_SET  2 LOW       # LED off
./esp-ctl --device default send GPIO_TOGGLE 2
```

I2C bus scan (SDA=21, SCL=22 on classic ESP32):

```sh
./esp-ctl --device default send I2C_INIT 0 21 22 400000
./esp-ctl --device default send I2C_SCAN 0
# → 0x68 0x76     (your device addresses)
```

ADC read in millivolts (channel 0 = GPIO 36 on classic ESP32):

```sh
./esp-ctl --device default send ADC_READ_MV 0
# → 1234
```

---

## 7. Interactive REPL

```sh
./esp-ctl --device default shell
espshell> PING
OK PONG
espshell> WIFI_STATUS
OK ssid=MyNet ip=192.168.1.123 rssi=-58 ch=6
espshell> CFG_LIST
OK wifi_ssid auth_token wifi_pass
espshell> exit
```

---

## 8. Stream async events

Some commands produce `EVT` lines (logs, GPIO transitions, ADC streams, BLE
scans). The `logs` subcommand subscribes and prints them as they arrive.

Terminal A:

```sh
./esp-ctl --device default logs
```

Terminal B:

```sh
./esp-ctl --device default send GPIO_MODE 4 INPUT_PULLUP
./esp-ctl --device default send GPIO_WATCH 4 ANY
```

Now every transition on pin 4 prints in Terminal A:

```
EVT GPIO 4 0
EVT GPIO 4 1
EVT GPIO 4 0
```

ADC streaming (one sample every 200 ms):

```sh
./esp-ctl --device default send ADC_STREAM 0 200
# → EVT ADC 0 <raw>  every 200ms
./esp-ctl --device default send ADC_STREAM_STOP 0
```

---

## 9. OTA — never reflash via USB again

After changing code (`main/project_app.c` for example):

```sh
idf.py build
./esp-ctl --device default ota upload build/espshell.bin
#   1010816 / 1010816 bytes
# OK committed, rebooting
```

End-to-end SHA-256 is verified by the device before swapping the boot
partition. If the new image misbehaves you can roll back without USB:

```sh
./esp-ctl --device default send OTA_ROLLBACK
./esp-ctl --device default send REBOOT
```

---

## 10. Add your own command

Edit `main/project_app.c`:

```c
#include "project_app.h"
#include "core.h"
#include <stdio.h>

static bool cmd_hello(int argc, char **argv, char *resp, size_t resp_sz) {
    snprintf(resp, resp_sz, "hola %s", argc > 0 ? argv[0] : "mundo");
    return true;
}

void project_init(void) {
    cmd_register("HELLO", cmd_hello, "HELLO [name]");
}
```

```sh
idf.py build
./esp-ctl --device default ota upload build/espshell.bin
./esp-ctl --device default send HELLO Adrian
# → OK hola Adrian
```

That is the full development loop: write code → build → OTA → use, with no
USB cable involved after the first flash.

---

## Troubleshooting

| symptom | likely cause | fix |
|---|---|---|
| `connect failed` | device offline or IP changed | `idf.py monitor` to see current IP |
| `auth rejected` | wrong token | re-copy from UART logs, or rotate via `CFG_SET auth_token <new_hex>` |
| `refusing to read with mode 0644` | toml perms | `chmod 600 ~/.config/esp-ctl/devices.toml` |
| `ERR 8 busy` | another client connected | only one TCP session allowed; close the other |
| `recv/decrypt failed` after token change | stale session | reconnect (kill CLI, start again) |
| `partition_table` build error “does not fit in 4MB” | bigger flash needed | edit `partitions.csv` or change `CONFIG_ESPTOOLPY_FLASHSIZE` |
| `mbedtls/chachapoly.h: No such file` | mbedTLS 4 / IDF v6 | already handled — we use PSA Crypto |

---

## Where to go from here

- `docs/protocol.md` — the wire-level spec.
- `README.md` — full command catalog.
- `tools/esp-ctl/README.md` — CLI reference.
- `main/project_app.c` — your starting point for project-specific code.
- `components/core/src/cmd_builtin.c` — example of how to write commands.
