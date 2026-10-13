# p99lfg

A minimal-resource LFG (looking-for-group) and chat companion for **Project 1999**.

Pure C, native Win32, single static exe — no runtime dependencies, a few MB
of RAM, built for the old hardware P99 players love. Everything syncs over
**Nostr relays**: no central server to run or pay for, and the same network
the mobile companion ([LFG-iOS](https://github.com/LeonardChristopher/LFG-iOS))
uses.

## Features

- **My Characters** — your P99 roster (name / class / level), saved locally;
  toggle LFG per character; each character publishes as its own replaceable
  Nostr event (kind 30078, `#t p99lfg`, d tag `server/charname`)
- **Community LFG list** — everyone running the app, with last-seen presence
  marks: `[ON]` (≤2 min), `[recent]` (≤30 min), `[off]`
- **Encrypted chat** — NIP-04 style DMs (AES-256-CBC under an ECDH shared
  key; relays never read them). Send-as character picker; the speaking toon's
  name rides inside the encrypted payload. Per-peer histories.
- **Favourites** — star players; pinned to their own list section
- **Ratings** — competent / friendly / listens, 1–5, with anti-brigade
  consensus (a rating hides after two disagreeing active raters)
- **Show key QR** — share your identity with the iOS app so phone and PC
  publish as the same person
- **Log scan** (secondary) — watches `eqlog_*_P1999Green.txt` for
  `/who all lfg` output and `You are now LFG.` lines, using the log's own
  timestamps for freshness

## Install

1. Download `p99lfg-windows-x64` from the latest successful
   [Actions run](https://github.com/LeonardChristopher/p99lfg/actions)
   (artifact at the bottom of the run page)
2. Extract into its own folder (e.g. `C:\Games\p99lfg\` — not Program Files)
3. Run `p99lfg.exe`. Windows may warn about an unsigned exe —
   *More info → Run anyway*

Files created next to the exe:

| File | Purpose |
|---|---|
| `p99lfg.key` | your Nostr identity (back it up to keep your ratings/name) |
| `p99lfg.ini` | settings (log folder, server) |
| `p99lfg.chars` | your character roster |
| `p99lfg.favs` | favourites |
| `p99lfg.log` | diagnostics (create `p99lfg.debug` to make it verbose) |

## Usage notes

- Log into P99 and type `/log` once if you haven't — the scanner needs
  logging enabled
- Pick your EverQuest folder with **Change log folder...** — the choice
  persists between runs
- Both default relays (`wss://nos.lol`, `wss://relay.damus.io`) are public
  community relays; `relay.damus.io` intermittently returns 503 — harmless,
  `nos.lol` carries the load
- To run a second identity (e.g. a friend or an alt account), copy the exe
  into a *separate folder* — identity comes from the key file beside the exe

## Build

All tests and the POSIX CLI run on Linux/macOS:

```sh
./build.sh
```

Windows exe builds automatically on every push (GitHub Actions); to
cross-compile locally:

```sh
./build-windows.sh   # needs mingw-w64
```

## Layout

- `src/core.c` — log discovery, tailing, LFG parsing, staleness
- `src/chars.c` — local character roster
- `src/nostr.c` — NIP-01 events, BIP340 schnorr signing
- `src/chat.c`, `src/aes.c` — encrypted DMs
- `src/rating.c`, `src/fav.c` — ratings consensus, favourites
- `src/sync.c` — relay publish/subscribe glue
- `src/ws_client.c` — RFC 6455 WebSocket (ws:// hand-rolled;
  wss:// via Microsoft WinHTTP on Windows — zero external TLS libs)
- `src/win32_main.c` — the GUI
- `vendor/secp256k1`, `vendor/qrcodegen` — vendored
- `tests/` — unit + loopback WebSocket tests, run in CI on both platforms
- `docs/ROADMAP-GROUPS-RAIDS-VOICE.md` — the design doc for group/raid
  channels and voice (future work)

## Roadmap

See the [design doc](docs/ROADMAP-GROUPS-RAIDS-VOICE.md) for the planned
group/raid text channels and the voice architecture.

**Status:** v0.1 — core app working (LFG sync, chat, favourites, ratings,
identity sharing). Log-scan integration and the group/raid features are
in progress.
