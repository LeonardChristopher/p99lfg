# p99lfg Mobile Port Guide (iOS / Android)

**Purpose: bring the mobile companion up to speed on everything the
Windows app now does, so the two platforms converge on one feature
set. Read this before writing mobile code. Companion to
`docs/SESSION-HANDOFF.md` (implementation state) and
`docs/ROADMAP-GROUPS-RAIDS-VOICE.md` (design decisions).**

The iOS companion repo: https://github.com/LeonardChristopher/LFG-iOS

---

## 1. Current Windows feature set (the parity target)

Core (predates mobile work):
- LFG characters: one replaceable Nostr event per character,
  kind 30078, `#t p99lfg`, d tag `<server>/<charname>`,
  content `{"v":2,"name","class","level","zone","lfg","user"}`
- Encrypted DMs: NIP-04 style, kind 4, AES-256-CBC under the
  ECDH shared secret; char name rides INSIDE the encrypted payload
- QR identity sharing (phone + PC = same Nostr identity)
- Favourites, ratings w/ anti-brigade consensus, log scanning

New since the mobile app last synced (Oct 2026 sessions):
1. **Groups**: 6 members + 2 waitlist, all 8 chat. Leader/officer
   invite model, encrypted invite DMs (`GROUPINV:<channel>:<role>`),
   kind-9 channel chat + join/leave control, kind-30081 replaceable
   roster state (`{"members":"Name:pubkey:officer,...","wait":...}`).
   Lowest-pubkey member consolidates joins; waitlist players have
   FULL chat (channel messages are not membership-gated).
2. **Relay resilience**: 5 default relays; every publish requires an
   explicit relay `["OK",...,true]` verdict; failed sends spool in an
   outbox (32 ring) and retry on every poll; dedup by event id.
3. **Delivery audit ("receipts")**: every client publishes its seen
   chat ids as `{"rcpt":"id,id,..."}` on the channel (~30s); senders
   rebroadcast any spooled message a peer did not acknowledge.
   Same-event rebroadcast is idempotent (relays dedupe by id).
4. **Persistent relay connections**: one streaming thread per relay,
   REQ with a `since` watermark (last seen created_at minus 2s),
   reconnect/backoff 1s..60s. The 5s poll remains as safety net.
5. **Voice**: full-mesh UDP, AES-256-CBC under the same pairwise
   ECDH secret, Opus 32kbps mono 20ms frames, push-to-talk
   (global key poll), presence `{"voice":"pubkey:ip:port"}` on the
   channel, greeting/keepalive pings (NAT punch + address
   learning), CRYPTO attribution of incoming frames (address match
   is only the fast path - multi-NIC/VPN machines break it).
6. **Embedded relay node** ("Host node"): the app can run a real
   NIP-01 relay for the group (store 512 events, fan-out, OK
   verdicts, `#t p99lfg` scope enforcement, since-filter on REQ).
7. **Presence hygiene**: 4-hour receiver-side LFG staleness filter
   (the app heartbeats every ~60s while running, so older = gone);
   7-day known-user prune; 20-minute stale-list sweep; Clear-list
   writes a persisted hidden-user blocklist (relay re-adds cannot
   resurrect cleared entries).

## 2. Wire protocol reference (all of it)

Every event the apps exchange:

| What | Kind | Tags | Notes |
|---|---|---|---|
| Character LFG | 30078 | `#t p99lfg`, `d:<server>/<char>` | replaceable; heartbeat ~60s |
| Profile/name | 30082 | `d:profile` | display name |
| DM (chat + invites) | 4 | `#t p99lfg`, `p:<recipient>` | NIP-04 encryption |
| Group chat | 9 | `#t p99lfg`, `d:p99lfg-g-<channel>` | content `{"from":"Char","msg":"text"}` |
| Group control | 9 | same d tag | `{"join":"Name:pubkey"}` / `{"leave":...}` |
| Voice presence | 9 | same d tag | `{"voice":"pubkey:ip:udpport"}` |
| Receipt | 9 | same d tag | `{"rcpt":"id,id,...","v":1}` |
| Roster state | 30081 | `d:p99lfg-g-<channel>` | replaceable, host-consolidated |

Channel tag charset: alnum + `-`/`_`. Groups are `grp%06x` tags.
Presence marks: `[ON]` <= 2 min, `[recent]` <= 30 min, else `[off]`.

**The subscribe filter** (both REQ variants):
`{"kinds":[30078,4,9,30079,30080,30081,30082],"#t":["p99lfg"]}`
plus `"since":<watermark>` on reconnect.

## 3. Lessons learned on Windows (do NOT relearn these)

Each of these cost a live-testing round:

1. **Kinds 30000-39999 are REPLACEABLE.** We originally used 30083
   for chat; same pubkey+kind+dtag REPLACES the event, so each new
   message DELETED the previous one and could delete pending join
   control events. Chat is kind 9 (regular). State (roster, LFG)
   correctly uses replaceable kinds - state SHOULD replace.
2. **`chat_try_decrypt` must return the RAW payload.** We once
   reformatted to display form ("[Name] text") before the GROUPINV:
   prefix check - invites silently never matched. Protocol checks
   belong on the raw bytes; the {"from":..,"msg":..} envelope is
   display-only.
3. **Relay verdicts are mandatory.** A quiet socket is NOT a
   published message. Only `["OK",<id>,true]` counts; ok:false,
   NOTICE, or silence = not sent. Everything else (spooling,
   receipts) is built on this honesty.
4. **Never touch UI controls from the sync/network thread.**
   SetWindowText from the wrong thread silently does nothing on
   real Windows builds (worked in tests, blank on user machines).
   Always marshal to the UI thread.
5. **Re-download loops need dedup, but state kinds must NOT be
   deduped**: a client may see the roster state BEFORE the invite
   DM that activates the group - suppressing it leaves the roster
   empty forever. Dedup applies to append-style kinds (4, 9,
   30079/80) only.
6. **Audio device handles: 0 is a valid socket/handle on Windows.**
   Initialize sentinels to -1 explicitly (voice_init).
7. **`"created_at":` is 13 bytes.** An off-by-one there silently
   mangles every timestamp's first digit - presence, staleness
   filters, reconnect watermarks all compare garbage. (We shipped
   this bug for weeks.) Write a timestamp round-trip test first.
8. **Voice frame attribution must be CRYPTO, not address matching.**
   The advertised IP can differ from the datagram source (VPN,
   multi-NIC) - only the true peer's frames decrypt under the
   pairwise secret. Address match is a fast path only.
9. **The first transmitted packet must be unconditional** (greeting
   pings): both sides waiting to "learn from traffic" is a deadlock.
10. **Raw PCM sounds robotic from jitter, not the codec.** Use a
    ~140ms playback cushion; Opus concealment papers over device
    starvation and compounds into artifacts.

## 4. Platform recommendations (the shortcuts you asked for)

The Windows app is dependency-free C because P99 players run toasters.
Mobile does NOT need that constraint - use the platform's batteries:

**Do NOT port from scratch. Use these:**

| Concern | iOS (Swift) | Android (Kotlin) |
|---|---|---|
| Language | Swift (UIKit or SwiftUI) | Kotlin + Jetpack Compose |
| Nostr protocol | nostr-sdk (rust-nostr bindings) or NostrKit | NostrPo / nostr-sdk (same rust core) |
| Schnorr/ECDH/AES | CryptoKit + Security framework - FREE, no vendoring | androidx.security + BouncyCastle or the NDK |
| WebSockets | URLSessionWebSocketTask - BUILT IN, no lib needed | OkHttp - de facto standard |
| Opus voice | libopus via the NDK/FFI, or AVAudioEngine + OpusX | libopus via JNI/FFI, or Oboe for audio I/O |
| Audio I/O | AVAudioEngine (built-in) | Oboe (Google's audio lib, low latency) |
| QR scan | AVFoundation (built-in) | ML Kit / ZXing |
| JSON | Codable (built-in) | kotlinx.serialization |

**The single biggest shortcut**: use `rust-nostr`'s mobile bindings
(nostrsdk.com) for BOTH platforms. It gives you: NIP-01 client,
NIP-04 (and NIP-44 - better, see below), kinds, filters, relay pool
with reconnect/backoff, event signing/verification - the entire
section-2 surface area, in one dependency, maintained by the Nostr
community. The Windows app hand-rolled all of this (secp256k1,
SHA-256, Schnorr, WebSocket client, TLS) because of the toaster
constraint - mobile should NOT replicate that work. Estimated saving:
the entire sessions 1-8 of the Windows project.

**What to keep hand-rolled**: the p99lfg wire format (the table in
section 2) - kinds/tags/content envelopes are OUR protocol, the
library only carries them.

**NIP-04 vs NIP-44**: NIP-04 (what we use) is deprecated by the
Nostr community but still universally relayed; NIP-44 is the
modern standard. Recommendation: mobile implements BOTH - speak
NIP-44 to other mobile clients, NIP-04 to interop with the
Windows app (port the Windows app to NIP-44 later; it's a
content-format swap, the keys are identical).

**Suggested mobile-specific wins:**
- Push notifications: Bells (iOS) / NostR-specific push services
  poll relays for kind-4 DMs to your key and wake the app. This is
  THE mobile feature - "invite received" while the phone is locked.
- Key storage: iOS Keychain / Android Keystore for the Nostr secret
  (the Windows app uses a plain file - fine there, wrong on mobile).
- QR identity import: the Windows app already emits the key QR -
  mobile scan-in is the zero-friction onboarding path.

## 5. Recommended port order

1. **Read-only parity**: subscribe (persistent connection, since
   watermark), parse LFG/roster/chat events, render the community
   list. (Library: rust-nostr. No crypto needed to READ public
   events.)
2. **Identity + DMs**: QR import (or fresh key in Keychain),
   NIP-04 decrypt, send DMs, GROUPINV invite handling. CryptoKit
   does the ECDH/AES.
3. **Groups**: channel chat kind-9, join/leave control, adopt
   kind-30081 roster state. This is where the iOS app earns
   feature parity with Windows.
4. **Voice LAST**: Opus + UDP mesh + PTT on mobile is a real
   project (audio session modes, mic permissions, backgrounding).
   Ship text parity first; voice can follow.

## 6. Testing seam

The Windows repo has a full headless harness the mobile app can
reuse without running the Windows app:
- `tests/test_group_flow.c` contains a scripted fake relay
  (stores events, answers REQ with per-frame replies, broadcasts
  new events) - port its behavior to a 50-line Kotlin/Swift test
  server, then run your mobile client against it on loopback.
- The wire cases to replicate: OK verdict handling, REQ reply
  ordering (one event per frame), EOSE, reconnect-since, and the
  sender-echo exclusion (fan-out must not echo the publisher).
- `tests/test_voice.c` shows the ping/crypto-attribution
  sequences if you port voice.

## 7. Open items both platforms should converge on

- NIP-44 migration (above)
- Guild channels: guild_relay/guild_channel ini keys exist on
  Windows (ini-only, no GUI yet); a shared "guild directory"
  event (kind 30082 style) is the agreed direction so users join
  guilds without hand-editing config
- Relay list sync: Windows hardcodes 5 defaults; a shared relay
  recommendation event would let mobile discover them
