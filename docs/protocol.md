# espshell wire protocol — version `espshell/1`

Authoritative spec. Both the firmware (`components/core/src/proto.c`) and the
CLI (`tools/esp-ctl/src/proto.c`) implement this document.

## Overview

A connection has two phases:

1. **Cleartext handshake.** Two lines, server-then-client-then-server. The
   server proves liveness and offers a challenge; the client proves
   possession of the shared token without revealing it.
2. **Encrypted records.** All subsequent traffic is binary,
   length-prefixed frames sealed with ChaCha20-Poly1305 using
   per-direction, per-frame nonces.

The token never crosses the wire.

## Phase A — handshake

Lines terminated with `\n` (server tolerates `\r\n` from the client). UTF-8.

### Server → Client (HELLO)

```
espshell/1 HELLO nonce=<64-hex>
```

- `nonce` is 32 cryptographically random bytes hex-encoded.
- Device identity (`fw`, `chip`, `mac`) is intentionally omitted from the
  cleartext HELLO to prevent passive enumeration. Use the `INFO` command
  after authentication to retrieve this information.
- Additional `<key>=<value>` pairs may appear in future versions; clients MUST
  ignore unknown keys.

### Client → Server (AUTH)

```
AUTH cnonce=<64-hex> hmac=<64-hex>
```

- `cnonce`: 32 random bytes generated client-side.
- `hmac` = `HMAC-SHA256(token, "espshell-auth-v1" || snonce || cnonce)`
  where `||` denotes byte concatenation.

### Server → Client (auth result)

- On success: `OK session established`
- On failure: `ERR 3 invalid auth`, followed by a **fresh HELLO with a new
  nonce**. The client may retry (up to 3 attempts total) but MUST answer the
  newest nonce — server nonces are single-use. After the last failure the
  server enforces a 10-second cool-down and drops the socket.

### Session key derivation

Both sides compute:

```
salt  = snonce || cnonce                  ( 64 bytes )
ikm   = token                             ( arbitrary length )
info  = "espshell-session-v1"
key   = HKDF-SHA256(ikm, salt, info, L = 32)
```

`key` is the ChaCha20-Poly1305 symmetric key used for the rest of the session.

## Phase B — encrypted frames

Big-endian, packed. No padding.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
┌───────────────┬───────┬───────┬─────────────────────────────────┐
│   length(u16) │type(u8)│flags(u8)│            seq(u64)            │
├───────────────┴───────┴───────┴─────────────────────────────────┤
│                    ciphertext (length − 28 bytes)                │
│                              ⋮                                   │
├─────────────────────────────────────────────────────────────────┤
│                        Poly1305 tag (16 B)                       │
└─────────────────────────────────────────────────────────────────┘
```

| field   | size | notes |
|---------|------|-------|
| length  | u16  | total bytes of the frame including this header & the tag |
| type    | u8   | 1 = DATA; reserved values MUST NOT be sent |
| flags   | u8   | reserved, must be 0 |
| seq     | u64  | monotonic per direction, starts at 0 |

- **AAD**: the first 12 bytes of the frame (length ‖ type ‖ flags ‖ seq).
- **Nonce** (12 bytes): `dir_byte (1) || seq_be (8) || 0x00 0x00 0x00`
  where `dir_byte = 0x00` for frames sent by the server and `0x01` for frames
  sent by the client.
- **Plaintext**: one ASCII line WITHOUT a trailing newline. Size limits are
  **asymmetric**: client → server (command lines) ≤ 1024 bytes
  (`ESPSHELL_MAX_LINE`); server → client (replies and events) ≤ 4096 bytes
  (`ESPSHELL_MAX_RESP`). Receivers MUST size their frame buffers for the
  limit of the direction they read.

### Receiver rules

1. Read 2 bytes → `length`.
2. Validate `length ≥ 28` and `length ≤ buffer_cap`.
3. Read the remaining `length − 2` bytes.
4. Validate `type == 1`, `flags == 0`.
5. Validate `seq == expected_next_seq` (anti-replay). MUST drop the frame and
   close the connection on mismatch — do not log details over the network.
6. Decrypt and authenticate with AEAD; on tag failure, close the connection.
7. Increment `expected_next_seq`.

A connection MUST be torn down on any AEAD failure or sequence mismatch.

## Message classes

The protocol carries three classes of plaintext line. Classes are conveyed by
prefix; no class byte is added.

| prefix | direction | meaning |
|--------|-----------|---------|
| `OK[ payload]` | server → client | success reply |
| `ERR <code> <message>` | server → client | error reply, code from `errors.h` |
| `EVT <type> <data...>` | server → client | unsolicited async event |
| anything else | client → server | command line, see `cmd.h` for syntax |

## Versioning

The version is set at the start of HELLO (`espshell/1`). Breaking changes bump
the integer. Clients MUST refuse to speak to a server reporting an unknown
major version.
