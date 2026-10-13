# p99lfg Design Doc: Groups, Raids & Voice

**Status: draft — design agreed, not yet built. Captures the product/architecture
decisions so they survive between sessions.**

Date of design sessions: Oct 2026 (worked out live between the user and the
build agent). This doc records *why*, not just *what*.

---

## 0. Where the project stands (prerequisite context)

Shipped and verified working:

- Win32 app, pure C, single static exe, tiny footprint (P99 players = old
  hardware, keep it that way)
- Nostr identity per install (BIP340 keys), relays: nos.lol, relay.damus.io
- Character roster (`chars.c`) with per-character LFG publishing
  (kind-30078, d tag = `server/charname`, content v2 with class/level)
- Community LFG list + presence (last-seen freshness: ON <=2 min,
  recent <=30 min, off beyond)
- Encrypted chat (NIP-04 style, AES-256-CBC under ECDH), send-as character
  ("from" inside the encrypted payload), per-peer history, relay verdicts
  read and logged
- Ratings (competent/friendly/listens, kind-30079/30080, anti-brigade
  consensus), favourites, QR identity sharing
- WinHTTP WebSocket transport for wss:// (after the great bug-chain saga:
  WSAStartup -> SOCKET truncation -> SChannel ABI -> double-framing;
  all now regression-tested in CI)
- Log-scanner exists (eqlog parsing) but the game-integration UX is
  unfinished — finish before or alongside Phase 1 below.

Lessons from that saga that constrain this design:

1. Windows-only failure modes are the project's dominant risk. New network
   code MUST ship with stage-by-stage diagnostics from day one (we have
   ws_dbg_public / log_printf machinery — reuse it).
2. Verify on real Windows early, not in sandbox. The sandbox cannot run
   the audio path at all (no sound hardware), so design voice for
   loopback-testability.
3. Keep dependencies at zero. Voice will follow the same rule: Opus +
   our own UDP, no libwebrtc.

---

## 1. Group & raid TEXT channels (the easy, high-value phase)

### Design

- A "channel" is just an agreed Nostr tag: `#t p99lfg-g-<id>` (group of 6)
  or `#t p99lfg-r-<id>` (raid). No new infrastructure — same REQ filter
  pattern the LFG list already uses.
- NIP-28-style public chat events (kind 42 or our own kind with the channel
  tag + t tag). Messages are plain text, no encryption needed for raid/
  group text (P99 culture: channels are semi-public; revisit if users ask).
- UI: tabs across the top of the window — `LFG | Group | Raid` — each tab
  is a list + (for chat) a message pane. Group tab doubles as the roster
  for group voice in phase 2.
- Group formation: for v1, a group is created by one member who publishes
  the channel tag via DM invite to the other 5; invitees join by tag.
  (Future: one-click "invite to group" from the LFG list using existing
    DM machinery + the char picker.)

### Constraints & honesty

- Relays store everything forever: raid text channels should carry a
  "created" event and clients show history only since join (or last N
  events) to avoid re-downloading a raid's full history.
- No membership enforcement exists in Nostr. Anyone who learns the tag
  can read/join. For P99 this is acceptable (same as /ooc); note it in
  the UI ("semi-public channel").
- 6-person group chat: trivial bandwidth. 100-person raid chat: also
  trivial (few KB/s total). No concern.

---

## 2. Group VOICE (6 people, free-talk mesh)

The simplest useful voice: validates the whole audio stack with zero
tree/election machinery.

### Design

- **Topology:** full mesh over UDP. Each of 6 members sends Opus-encoded
  audio directly to the other 5. No relays, no roles, no floor control —
  just talk, like a group hangout.
- **Codec:** Opus at 16–24 kbps, mono, 20 ms frames. Library: libopus
  (tiny, C, MIT — consistent with our dependency philosophy; it's the
  one justified addition). Capture via the classic WaveIn/WinMM APIs
  (present since Win95 — perfect for our audience), not WASAPI.
- **Bandwidth math:** each member uploads 5 x ~24 kbps = ~120 kbps, down
  is the same. Fine even on rural DSL. CPU: encoding 1 + decoding 5 Opus
  streams = negligible on 2008 hardware.
- **Signaling (who's in the group voice + their UDP address):** rides on
  existing Nostr DMs / group channel. Voice join = publish presence in
  the group channel ("I'm in voice, my public IP/port, my Nostr key").
  Members exchange SDP-lite info (just IP:port + Opus params) via DM.
- **NAT:** try direct UDP; if hole-punch fails (common on consumer NAT),
  fall back to a relay candidate — the one group member with the best
  uplink (self-reported `up_kbps` in presence, ties broken by lowest
  pubkey) volunteers as pass-through for the failed pair. Pull-based:
  both ends dial the relay.
- **UI:** a "Voice" button in the Group tab; speaking indicator dots;
  in v1 no mute/deafen niceties beyond a mute toggle.

### Constraints & honesty

- Two members behind symmetric NATs can't hole-punch and must relay
  through a third — acceptable for 6; the relay election is trivial.
- No encryption of audio in v1? NO — decision: **encrypt audio with the
  same ECDH keys** (DTLS is heavy; instead: AES-256-CBC per-packet with
  the pairwise shared secret we already derive for chat). Audio privacy
  should match chat privacy.
- Loopback testing: build a `voice_test` mode that records 5 seconds,
  encodes, decodes, plays back — CI can verify encode/decode on Linux;
  real-device testing needs the user (sandbox has no audio).

---

## 3. Raid VOICE — "stage mode" (the signature feature)

**The key insight (user's, and it's a good one):** constrain simultaneous
TALKERS, not listeners. 100 listeners are cheap; 100 potential streams
are impossible. This converts an unsolved P2P problem into a buildable
one.

### 3.1 Roles (user's extension)

Voice rights attach to roles defined per-raid:

- **Raid Leader (RL)** — permanent voice slot; preempts all claims
  instantly (his voice must never wait for ceremony)
- **Pull Team** — permanent slots (2–4 flagged members)
- **Raid Coordinator(s)** — permanent slots
- **Open MIC slots** — claimable by anyone; count is bandwidth-scaled,
  not fixed

The role assignment itself is a Nostr replaceable event (kind-30078
pattern we already use): `{channel tag, RL pubkey, pull team, coords,
slot count, relay expectations}` published by the raid creator. Any
leader re-publish updates roles mid-raid. "Who may assign roles" =
"whose key created the raid event" — cryptographic raid leadership,
no server, no accounts. Free elegance from our existing identity layer.

Priority order for open-slot contention: **RL > pull team > coordinators
> credit-priority claims > first-come.** Deterministic on every client
because it derives from published events.

### 3.2 Slots scale with bandwidth, not a magic number

Every active talker adds ~24 kbps to each relay's load. Relays self-
report `up_kbps` in presence. Rule of thumb for defaults:
- 2 volunteer relays at 5+ Mbps up -> 8–12 total voice slots
- 1 relay -> 4–6 slots
- 0 relays -> voice unavailable in raid mode; app says so plainly

The UI displays it: "Voice slots: 10 (3 roles, 7 open)" — the system
documents itself.

### 3.3 Transport topology

- **Talkers** encode once, send to each relay (pull-based: relays dial
  the talker's published address; talkers also dial up to the RL as
  anchor).
- **Relays** (2–4 volunteers by self-reported uplink, tiebreak lowest
  pubkey) each carry a subset of listeners, and carry only the talker
  streams their capacity supports — relays *share* the talker load
  (each relay takes 1–2 streams, not all; bookkeeping: raid event lists
  relay->listener assignments, maintained by the "top relay" acting as
  coordinator, republished on change).
- **Listeners** dial up to their assigned relay, receive the mixed or
  forwarded set of talker streams. Pull-based dialing everywhere: no
  inbound ports required on anyone, NAT-friendly.
- **Audio:** forwarded (not mixed) in v1 — relaying each talker's
  stream untouched. Mixing (decode+re-encode one combined stream) saves
  listener bandwidth but burns relay CPU; defer until relay behavior is
  known in the wild.
- **Encryption:** same per-pairwise AES key scheme as group voice.

### 3.4 Floor control (all over Nostr, no new machinery)

- Claim/release/open-slot state = small replaceable events in the raid
  channel (one event: "current slot holders").
- RL push-to-talk *auto-claims with preemption* — the emergency path
  never waits on round-trips; others' claims are best-effort with ~1s
  latency tolerance.
- Everyone can always TYPE in the raid channel; voice is the scarce,
  high-value channel — matches raid culture (97 people type "r", 3
  people talk).

### 3.5 Churn (the known boss)

Raids churn constantly (zones = loading screens = drops). Mitigations:

- Slot holders who vanish: slot state event auto-expires them
  (freshness window like LFG staleness — reuse the pattern), next in
  queue claims.
- Relay death: listeners re-request assignment from the coordinator;
  coordinator rebalances to surviving relays; if the coordinator dies,
  the lowest-pubkey relay takes over (deterministic failover, same
  election principle everywhere).
- Design decision: accept and make churn *graceful* rather than trying
  to prevent it. Small blast radius per failure (1 slot or 1 relay's
  listeners) is the win.

---

## 4. Contribution & priority (user's incentive question)

**Decision: contribution gates PRIORITY, never ACCESS.** A hard
"no relay, no voice" rule would punish the weak-connection users who
are half of P99's population — they have nothing to contribute but
their presence, and they're not freeloaders.

### The mic-credit layer (defer to Phase 3+)

1. Open MIC slots claimable by anyone — most raids won't be contended.
2. Contended claims: priority goes to contributors.
   "Relay one raid = priority voice for the rest of the night."
3. Roles always outrank credits; credits outrank luck; nobody muted.

### Verification (why naive pledges don't work)

"I pledge 5 Mbps" is only testable mid-raid when your relay chokes —
so credit derives from **witnessed performance**: relays report
(listeners, sustained kbps, uptime); listeners confirm via the existing
ratings/consensus machinery (anti-brigade logic already shipped —
extend it to service reports). Persistent keys make history meaningful.
Sybil note: contribution is witnessed by dozens of real clients, and
raid communities are small and local — the shipped consensus is
proportionate to the actual threat ("someone cranky wants a MIC slot"),
so do NOT build a blockchain about it.

---

## 5. Build order (each phase useful alone)

- **Phase 0 — refinement:** finish log-folder persistence; chat
  persistent-connection pass (kill connect-per-message); diagnostics
  behind a debug flag; README release pass. (Chat verdict logging stays.)
- **Phase 1 — group/raid text tabs:** channels-as-tags, invites via DM,
  tab UI. Pure Nostr, no new tech. Ship it, watch relay behavior with
  60–100 person raid chat (validates relay appetite for our traffic).
- **Phase 2 — group voice mesh (6):** libopus + WinMM capture + UDP
  mesh + pairwise AES. Loopback test harness in CI. Real-device test
  by user. The stack validator.
- **Phase 3 — raid stage mode:** role events, floor control, relay tier
  with assignment events, forwarding relays. The signature feature.
- **Phase 4 — credit/priority layer:** witnessed relay reputation ->
  claim priority. Only after Phase 3 shows real relay behavior.

## 6. Explicitly rejected alternatives (so we don't relitigate)

- **Voice through Nostr relays:** relays are text-event storage;
  latency, storage costs, and rate limits forbid it. Nostr = signaling,
  UDP = media. (Nobody in the Nostr ecosystem has shipped voice
  transport; our design would be novel.)
- **Full-mesh raid voice:** 1.6 Mbps+ upload per member — impossible
  for the P99 demographic.
- **SFU server for raids:** works, but it's a server; rejected while
  the serverless property holds. Graceful fallback acknowledged: if
  tree/stage voice ever hits a wall, a community Mumble hand-off for
  100-person nights does not invalidate the app (and the tree design
  degrades to it).
- **Mandatory contribution for voice:** punishes the weak-connection
  half of the community; priority-gating instead of access-gating.
- **NIP-29 group chats:** requires a dedicated group relay = server.
  Tag-based channels (NIP-28 pattern) instead.
- **libwebrtc:** browser-sized dependency; violates the project's
  zero-dependency/minimal-footprint charter.

## 7. Open questions (flagged, not solved)

- Talkback/half-duplex vs full-duplex for open MIC slots (PTT default
  is presumed — matches raid discipline; confirm with community).
- Do relay assignment events need anti-replay (someone republishing a
  stale assignment)? Freshness windows probably suffice; confirm.
- Should raid text channels persist history across restarts (useful for
  "what did I miss") vs join-point-only (privacy/lighter)? Default to
  last N events; ask users.
- libopus static-link size impact on the exe (~700KB dynamic? measure;
  acceptable given the feature).
- Group voice when two members are both symmetric-NAT: relay through
  a third is designed but unproven; needs a real-device test.
