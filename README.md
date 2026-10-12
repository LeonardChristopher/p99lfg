# p99lfg

A minimal-resource LFG (looking-for-group) companion for **Project 1999**.

- Scans your `eqlog_*.txt` files for LFG activity (`You are now LFG.`, `/who` output with `<LFG>`/`<LFM>` rows)
- Shows everyone currently LFG in one list
- Lets you mark any of your own characters LFG **even when you're not in game**
- Shares LFG listings between users over **Nostr** relays — no central server to run or pay for

Written in pure C with a native Win32 GUI: a single static exe, no runtime
dependencies, a few MB of RAM. Built for the old hardware P99 players love.

## How it works

1. **Log scan** — the app watches every `eqlog_<char>_project1999.txt` in your
   EQ folder (including the `%LOCALAPPDATA%\VirtualStore` redirect for
   Program Files installs, matching how EQTool detects installs) and picks up:
   - `You are now LFG.` / `You are no longer LFG.` (your own state)
   - `/who` rows like `[60 Archmage] Soandso (Dark Elf) ZONE: sebilis <LFG>`
2. **Manual LFG** — add any character name (+ zone/class/level) and it stays
   listed until you remove it or it goes stale.
3. **Nostr sync** — your LFG list is published as a signed kind-30078
   parameterized replaceable Nostr event tagged `#t p99lfg` with the server
   (`green`/`blue`) as the `d` tag. Everyone running the app subscribes to the
   same tag, so listings appear across all users without any central server.
   Relays are cheap, public, and interchangeable — you can point the app at
   any list of relays.

## Build

All tests and the POSIX CLI run on Linux:

```sh
./build.sh
```

Cross-compile the Windows exe (needs `mingw-w64`):

```sh
./build-windows.sh
```

Or just push to GitHub — `.github/workflows/build-windows.yml` builds and
attaches `p99lfg.exe` as an artifact on every push.

## Project layout

- `src/core.c` — log discovery, tailing, LFG line parsing, entry list (portable)
- `src/nostr.c` — NIP-01 event serialization + BIP340 schnorr signing
- `src/sha256.c` — self-contained SHA-256 for event ids
- `src/ws_client.c` — minimal RFC 6455 WebSocket client
- `src/sync.c` — publish/subscribe glue over relays
- `src/win32_main.c` — the Windows GUI
- `src/tls_schannel.c` — Windows TLS (SChannel) hooks for `wss://`
- `vendor/secp256k1` — vendored libsecp256k1 (schnorr signatures)
- `ios/` — iOS app (SwiftUI + the portable C core, see `ios/README.md`)
- `tests/` — unit + loopback WebSocket integration tests

## Status

- [x] Log scanner + parser (tested)
- [x] Nostr signing + serialization (BIP340 vectors, tested)
- [x] WebSocket client, incl. required client-frame masking (loopback tested)
- [x] wss:// TLS via Windows SChannel (built-in; needs on-Windows validation — CI builds it)
- [x] Sync layer: publish on Mark LFG, subscribe loop feeding remote entries into the list (parser unit-tested)
- [x] Win32 GUI wired to sync (background relay poll thread)
- [ ] On-Windows smoke test of SChannel path against a real relay
- [x] iOS app: SwiftUI + portable C core, SecureTransport TLS hooks (`ios/`)
