# p99lfg for iOS

Mobile companion for [p99lfg](../README.md) — see who's LFG on Project 1999
and mark your own characters LFG even when you're not in game. Shares
listings with other players (Windows and mobile alike) over Nostr relays,
exactly like the Windows app.

## Features

- **LFG list** — everyone currently LFG on the selected server, synced over
  the same `#t p99lfg` Nostr tag and kind-30078 events as the Windows app.
- **Mark LFG** — add any character name (+ optional zone, class/level); it
  publishes to the relays and stays listed until removed or stale (30 min).
- **Server picker** — Green or Blue, published as the event `d` tag.

The log scanner is Windows-only (it reads `eqlog_*.txt`); the mobile app
relies on manual marking + Nostr sync, which is the mobile use case.

## Architecture

Reuses the portable C core unchanged:

- `src/core.c` — entry list, manual add/remove, staleness (no log scan on iOS)
- `src/nostr.c` — NIP-01 serialization + BIP340 signing
- `src/sync.c` — publish/subscribe over relays
- `src/ws_client.c` — minimal WebSocket client
- `ios/src/tls_apple.m` — **new**: TLS hooks for `wss://` using the system
  Security framework (SecureTransport) over the raw socket, mirroring the
  SChannel implementation on Windows — no OpenSSL shipped.

The Swift layer is a thin bridge:

- `LfgModel.swift` — `ObservableObject` wrapping `LfgState`/`SyncState`,
  background sync thread polling every 5s (same cadence as the Win32 GUI)
- `ContentView.swift` — SwiftUI list + add-character sheet

secp256k1 is compiled from source with
`ENABLE_MODULE_SCHNORRSIG`/`ENABLE_MODULE_EXTRAKEYS` defines only — no cmake
step needed inside Xcode (verified against the BIP340 test vectors).

## Build

```sh
brew install xcodegen
cd ios
xcodegen generate
open p99lfg.xcodeproj
```

Then select your device/simulator and build (iOS 15+, Swift 5). Code signing
is disabled in CI; enable automatic signing with your own team in Xcode.

## CI

`.github/workflows/build-ios.yml` generates the project with xcodegen and
builds the simulator target on every push.
