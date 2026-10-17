# Session Handoff Notes

**Purpose: crash-resume file. The build agent writes this BEFORE doing any
work and updates it after each unit of work, so that if a session dies
mid-task (has happened), the next session can pick up with zero context
loss. Read this file first.**

Companion to `docs/ROADMAP-GROUPS-RAIDS-VOICE.md` (product/design decisions).
This file tracks *current implementation state*, not design.

---

## 1. Feature in flight: officer-style invite rights + group GUI wiring

Design (agreed with user, Oct 2026):

- Groups are leader-created, invite-only. Leader gets a unique channel tag
  (`grpXXXXXX`) — no shared-name guessing.
- Invites ride as encrypted DMs: plaintext `GROUPINV:<channel>:<role>`.
  Recipient auto-adopts the channel and gets a popup.
- Invite permission, guild-rank style: leader can invite by default; leader
  can grant members the **officer flag** (can invite). The roster state
  event (kind 30081) carries the permission so ALL clients enforce it
  consistently — CSV per member is `name:pubkey:officer`.
- Roster shows `(can invite)` markers; leader toggles the flag per member
  via right-click in the roster.

### State as of handoff writing (commit b7d7686, all on `main`, WORKS)

Backend / plumbing — DONE and pushed:

- `src/channels.h`: `GroupMember` has `int officer` field.
  `GROUP_MEMBERS_MAX 6`, `GROUP_WAIT_MAX 2`, `GROUP_STATE_KIND 30081`.
- `src/channels.c`: CSV round-trip includes officer flag
  (`group_members_to_csv` / `group_csv_to_members`, third field `:1`/`:0`).
  `group_state_publish` sends kind 30081 with dtag `p99lfg-g-<channel>`,
  content `{"members":"...","wait":"...","v":1}`.
- `src/win32_main.c` `on_dm` (~line 328): DM starting `GROUPINV:` parses
  `<channel>:<role>`, sets `g_group_name`, resets log, sets
  `g_group_is_leader = 0`, logs `--- invited to group ... ---`, popup,
  refreshes group window. WORKS.
- `on_group_create` (~line 1266): fresh `grp%06x` tag, seeds self as
  member #1, `g_group_is_leader = 1`, publishes state. Defined but
  see "missing" below.
- `on_invite(name, wait)` (~line 1303): sends `GROUPINV:` DM, adds invitee
  to local roster, republishes state. WORKS but permission check is
  leader-only (see missing #2).
- Group control messages (join/leave requests) on kind 30083 handled in
  `on_generic`; host (lowest pubkey) consolidates; waitlist promotion on
  leave. WORKS.
- LFG-list right-click context menu has `Invite to my group` /
  `Invite to waitlist` items — but only shown `if (g_group_is_leader)`.

### MISSING (the in-flight work that was lost in the crash) — DO NEXT

1. **Create group button not wired.** The Group window
   (`on_group_window`, ~line 1390) still has a `Join group` button
   (`ID_GROUP_JOINQ`). Per design, replace it with a **Create group**
   button that calls `on_group_create(dlg)`. Keep the manual channel
   Join box at top (used to join semi-public channels); the roster-side
   button becomes Create group. Need new `ID_GROUP_CREATE 262`.
   NOTE: `ID_GROUP_JOINQ` handler at ~line 1381 calls
   `on_group_joinq(dlg)` — **that function does not exist** (grep shows
   only the call site). Either remove the call/joinq button or implement
   it. Current build compiles because win32_main.c is only compiled on
   Windows (build.sh does not compile it; CI builds it - VERIFY).
2. **Officer permission enforcement.** `on_invite` rejects unless
   `g_group_is_leader`. Change to: leader OR (self has officer flag in
   current roster). Find self by `g_sync.pubkey` in `g_group_members[]`,
   check `.officer`. Context-menu gate in `WM_CONTEXTMENU` (~line 1788)
   must use the same predicate (extract a helper, e.g.
   `group_can_invite()`).
3. **Roster `(can invite)` markers + leader toggle.**
   `group_roster_refresh` (~line 407) prints an empty marker
   (`group_am_host() ? "" : ""` — leftover stub). Should append
   ` (can invite)` for officers; also mark leader. Add right-click menu
   on `g_group_roster`: for the leader, per selected member:
   "Allow invites" / "Disallow invites" -> set `officer` field,
   `group_state_publish`, `group_roster_refresh`.
4. **Tests.** `tests/test_all.c` has no group-state coverage. Add:
   - `group_state_parse(group_state_publish` round-trip of the CSV/JSON
     with officer flags (0 and 1, waitlist too)
   - malformed CSV robustness (missing third field => officer 0)
   The old session died when its first assert on `on_dm` text failed
   because `on_dm` had changed — **do not test win32_main.c internals**;
   it is not compiled into the Linux test binary. Test channels.c only.
5. Verify Windows CI still compiles win32_main.c after the
   ID_GROUP_JOINQ fix (see .github/workflows/build-windows.yml,
   uses build-windows.sh).

### Notes / gotchas

- Linux tests: `./build.sh` (builds secp256k1 first time, slow).
  win32_main.c is NOT in the Linux test build — GUI code can't be
  unit-tested there; keep all testable logic in src/channels.c etc.
- Officer flag rides ONLY in the members CSV third field. Waitlist CSV
  also has the field but it is meaningless for waitlisted players.
- `group_am_host()` = lowest pubkey among members. Distinct from
  `g_group_is_leader` (who created the group locally).
- Channel charset: alnum + `-`/`_` (see `on_group_join` sanitize).

---

## 2. Done in this session (update as work completed)

- [x] DELIVERY-AUDIT RECONCILIATION (round 10, receipts - built BEFORE
      the persistent connection per user decision, so its bugs surface
      before the bigger change lands):
      - channels.[ch]: ReconState - seen-id ledger (64/group), sent
        spool (16 raw ["EVENT",...] msgs, ring), receipt control msg
        {"rcpt":"id,id,.."} on the channel (kind 9, same family as
        join/leave control), throttled 15s.
      - channel_send_ex returns the signed event id + raw wire msg
        (channel_send wraps it, no behavior change).
      - SyncOnGeneric now carries the event id (signature change - all
        three implementations updated: win32 on_generic, test_all
        capture cb, harness generic_cb).
      - Flow: sender spools each chat; receivers note every chat id as
        seen; each client publishes its seen-id receipt ~every 30s in
        the sync thread; on receiving a PEER receipt, the client
        rebroadcasts any spooled msg the peer did NOT ack (identical
        event -> relay + dedup make it a no-op for those who have it,
        a delivery for the one missing it). Group log notes
        "re-sent N undelivered message(s)".
      - Tests: test_recon (ledger/spool/prune/rebroadcast semantics) +
        harness phase 9 END-TO-END: chat sent while all relays down
        (never reaches the store) -> peer receipt gap -> rebroadcast ->
        delivered + spool pruned on ack. ALL GREEN.
- [x] GUILD CHANNELS GROUNDWORK (user request): a guild hosting a
      website runs any Nostr relay endpoint at e.g.
      wss://theirsite/relay; p99lfg.ini gains guild_relay= (added to
      the relay list at startup - guild traffic uses the guild site
      as a hop, same chat flow) and guild_channel= (auto-joins the
      first at startup; group window opens into it like the manual
      semi-public join). Settings round-trip tested.
      NOTE: no GUI editor for these keys yet - ini-only. A guild
      "directory" event (kind 30082 profile-style announce of
      guild->relay mapping) is the natural next step so users can
      join a guild without hand-editing the ini.
- [x] CI CHASE for this round (3 fixes, ALL GREEN on 0cda426, run
      38971293116): (1) g_recon declared at use-site - moved to the
      globals block (the same declaration-order trap as last round:
      win32_main.c is only compiled by CI); (2) workflow test_ws
      build line was missing ws_winhttp.c (undefined refs on
      mingw); (3) test_guild_settings wrote to /tmp which does not
      exist on Windows - relative path now. LESSON reconfirmed: any
      test or code touching filesystem paths or GUI globals must be
      Windows-portable; CI is the only place win32_main.c compiles.
- [x] USER-AUTHORIZED DIRECTION (this session): keep making
      iterative improvements autonomously. Receipts use dedicated
      control messages (option 1: own event content shape, not
      piggybacking) - DONE. Guild plugin direction confirmed as a
      keep: guild-hosted relay + guild channels through the same
      chat flow.

ROUND 11 (user live-test feedback, Oct 16):
- [x] Invite popup is now Accept/Decline (MB_YESNO). Accept opens
      the group window AND sends the join control (previously it only
      opened the window - the invitee never announced the join, so
      the host never added them and state never republished - likely
      a major contributor to the empty-roster reports) AND registers
      self locally so the roster is instantly non-empty. Decline
      reverts the channel state (g_group_name cleared etc).
      Popup now shows the inviter's display name.
- [x] Roster thread bug FIXED: sync thread called group_roster_refresh
      (SetWindowTextA on the roster EDIT) directly from state adoption
      and join/leave handling - cross-thread UI access that silently
      failed on live builds. All roster painting now happens on the UI
      thread via WM_GROUPREFRESH (posted everywhere state changes).

- [x] VOICE OPTIONS DIALOG (user request): "Voice options" button
      under Voice in the group window. Mic + headset/speaker dropdowns
      (WinMM waveIn/waveOut enumeration, "(System default)" first),
      Push-to-talk checkbox, and a Set-key button (click it, press any
      key - captured via WM_KEYDOWN; a click captures mouse-button PTT).
      Persisted in p99lfg.ini: voice_mic/voice_output/voice_ptt/
      voice_ptt_key/voice_ptt_mouse/voice_ptt_button. Round-trip
      tested. NOTE: settings are STORED and preselected but not yet
      APPLIED - no audio capture exists yet; the codec layer
      (WinMM WaveIn) will read these when it lands. build-windows.sh
      was stale (missing ws_winhttp.c) - synced with the CI line.

- [x] EASY WINS round (user-sanctioned): (1) leader can PROMOTE a
      waitlisted player via roster right-click (auto-promote on leave
      still exists; manual UI was the known gap); (2) group window
      title now shows "Group: <name> (n/6)" plus "- N sending" while
      outbox events await a relay (outbox visibility was a known gap);
      (3) Voice button relabeled "Voice (setup)" until the codec
      layer lands - it only does transport/presence.

- [x] VOICE AUDIO LAYER (round 12 - mic/speakers/PTT feedback, user
      reported "mic or speakers not working" + no way to verify PTT):
      - src/voice_audio.[ch]: WinMM capture (4 rotating buffers, 16kHz
        mono 16-bit 20ms frames) + playback (3 buffers, drop-when-
        lagging), device-by-name matching with system-default
        fallback. Portable pure logic: vptt_gate (PTT), vptt_level.
      - Presence now carries the sender's IP (pubkey:ip:port;
        back-compat parse for pubkey:port) - fixes the chicken-and-egg
        where peers never learned addresses because "traffic learns
        host" but nobody could send first.
      - voice_peer_register(): upsert peer from presence; GUI presence
        handler now actually REGISTERS peers (before: only logged
        "started voice" - frames had no destination).
      - PTT: global key state via GetAsyncKeyState polled every 30ms
        in the sync thread's reworked wait loop (was a flat 5s sleep -
        far too coarse for 20ms audio; the loop now services voice at
        30ms between relay polls). Mouse4 (XBUTTON1) for mouse PTT.
      - MIC INDICATOR in the group window: "MIC: off" / "MIC: push
        your key" / "MIC: LIVE [####  ]" with a live loudness bar,
        repainted ~15fps via WM_MICUPDATE. This is the verify-your-mic
        feature the user asked for.
      - Voice button now opens mic+speakers on toggle; failure paths
        message clearly (mic fail blocks, speaker fail warns).
      - Tests: presence-with-ip parse, peer upsert, PTT gate, level
        meter (all portable). WinMM paths are Windows-only, exercised
        by live use.
      - STILL RAW PCM (640B/20ms = ~32kB/s) - fine on LAN, Opus
        encode/decode is the next layer for internet use.

- [x] OPUS CODEC (user: "pointless to test without it"):
      - vendored xiph/opus v1.5.2 at vendor/opus (git clone, .git
        removed; built by build.sh + CI + build-windows.sh like
        secp256k1). ~17MB source, built static, needs -lm.
      - src/voice_codec.[ch]: VOIP application, 16kHz mono, 20ms
        frames, 24kbps VBR. ~50-90B/frame vs 640B raw (~10x less
        wire). GUI encode path: capture -> vcodec_encode ->
        voice_send_frame (encrypted); receive: voice_frame_cb ->
        vcodec_decode -> va_play_frame.
      - Codec opens/closes with the voice toggle.
      - Tests: encode-in-bounds, size<25% raw, 20ms decode frame,
        ENERGY preservation over a 20-frame stream (>0.8; skip
        first 15 warm-up frames - opus needs stream history; a
        single isolated frame decodes misaligned, that's the codec,
        not a bug), silence encodes. Also vptt gate/level, presence
        with ip, peer upsert - all green.

ROUND 13 (review + UX, while user away ~40min):
- [x] Code-review fixes: (1) waitlist invite Accept registered self
      as a FULL MEMBER locally (role was ignored) - now role-aware,
      waitlisted self goes to g_group_wait, message confirms
      "waitlist - full chat access"; (2) connectivity strip built
      with the same cross-thread UI bug the roster had - caught
      in-review, posts WM_CONNUPDATE like the roster fix.
- [x] 6+2 group semantics per user: roster shows "Group (n/6 + m
      waiting)", title "Group: x (n/6+mw)". Waitlisted players
      already had full chat (kind-9 channel messages are not
      membership-gated) - verified, no change needed there.
- [x] Create group from the dashboard: the Group button now pops a
      menu (Open group window / Create a new group) - creating opens
      the group window AND seeds the new group in one flow.
- [x] Connectivity strip on the main window: "Relays: N/M connected
      (OFFLINE - messages will queue) + pending count", staged by
      the sync thread, painted via WM_CONNUPDATE.
- [x] UI friendliness: fonts -13/-15 -> -16/-19 (bigger controls),
      Dark mode toggle (dark_mode= in ini, dark brush + light text on
      statics/listbox, restart for full effect).
NOTE: leader roster right-click still reads name-only lines - if the
      new "(can invite)" markers confuse member vs waitlist
      right-clicks, promote finds waitlist names first (widx).

NEXT UP (in order):
1. PERSISTENT RELAY CONNECTION
2. Guild directory event + GUI editor for guild keys
3. NAT traversal / hole punching for internet voice (LAN works)
4. Ease-of-install pass
1. PERSISTENT RELAY CONNECTION (the big approved item; receipts are
   its safety net, now in place). Phased: persistent subscribe +
   reconnect/backoff alongside the poll, then retire the poll. Add a
   since filter at the same time - then receipts/rebroadcast become
   load-bearing (a missed event between reconnects is detectable and
   recoverable, which is exactly what round 10 built).
2. Guild directory event so users join guilds without ini editing,
   plus a small GUI editor for guild_relay/guild_channel keys.
3. Voice codec layer (libopus + WinMM capture, 50ms timer, roster
   speaking dots, voice_poll in sync thread).
4. Ease-of-install pass (approved, deferred until groups solid).

KNOWN GAPS (unchanged): outbox is in-memory only (lost on app exit);
waitlist has no manual promote UI; invite popup has no Decline;
Voice button produces no audio yet (transport only). The recon
spool (16) and ledger (64) are per-process, not persisted.

- [x] RELAY EXPANSION + OUTBOX/SPOOL (round 9, "chats cut off" fix):
      - DEFAULT_RELAYS now nos.lol, relay.primal.net, nostr.wine,
        offchain.pub, relay.damus.io (damus was timing out live on
        Oct 14 - probed healthy alternates with a kind-9 #t p99lfg REQ;
        damus kept as 5th since the spooler now tolerates flaky ones).
      - SYNC_MAX_RELAYS 4 -> 6.
      - Outbox in SyncState (32 msgs, ring): sync_send_event queues the
        raw ["EVENT",...] when NO relay gives an explicit OK verdict;
        returns 0 (= not lost). sync_poll flushes oldest-first, one
        retry per pending event per poll; relays dedupe by event id so
        retries are idempotent. sync_outbox_pending() for UI status.
      - Tests: test_all test_outbox (queue semantics, stays queued
        while relay down); test_group_flow phase 8 end-to-end (send
        with all relays down -> queued -> flush on recovery -> other
        client receives it). ALL GREEN.

- [x] Wrote this handoff file (crash-resume point = this file).
- [x] Create group button wired (ID_GROUP_CREATE 262), dangling
      `on_group_joinq` call removed (it never existed — crash damage;
      CI run 38454243809 on b7d7686 failed link for exactly this).
- [x] Officer enforcement: `group_can_invite()` helper; used by
      `on_invite` and the LFG-list context-menu gate.
- [x] Roster shows ` (you)` and ` (can invite)` markers; leader can
      toggle invite rights per member via roster right-click
      (ID_ROSTER_INV_ALLOW 263 / ID_ROSTER_INV_DISALLOW 264) in
      `group_wnd_proc` WM_CONTEXTMENU; republishes state on change.
- [x] Tests: `test_group_state` in tests/test_all.c — CSV/JSON
      round-trip with officer flags, malformed-CSV (missing third
      field -> officer 0), empty state. Linux `./build.sh` ALL PASS.
- [x] Crash-breakage scan (user-requested): verified every on_* /
      group_* handler resolves; globals all declared; REQ filter
      already includes kinds 30081/30082/30083 (group traffic flows);
      cross-module chat/sync signatures match; braces balanced (240/240).
      Only crash damage found was the joinq dangling call (fixed).
- [x] Committed + pushed to `main` (user commits to main directly).
- [ ] Windows CI on the new commit — VERIFY GREEN (mingw not available
      in Linux sandbox; win32_main.c is compiled only by CI). If red,
      read `gh run view --log-failed` and fix.

## 2b. Crash-breakage scan results (full sweep, Oct 13 2026)

Scanned for: undefined handler calls, unresolved globals, mismatched
cross-module signatures, stale REQ filters, leftover stubs. Findings:

- `on_group_joinq` call with no definition — THE crash casualty, had
  broken Windows CI on commit b7d7686. Fixed (button became Create group).
- `group_roster_refresh` printed an empty ternary `"" : ""` stub —
  replaced with real (you)/(can invite) markers.
- Everything else intact: all `on_*` handlers defined; all group globals
  declared (win32_main.c:54-61); `nostr_req_message` filter covers
  30081/30082/30083; `chat_send_message_from`/`chat_try_decrypt`
  signatures match chat.h; `channels.c` CSV code round-trips the
  officer field (regression-tested now).

## 3. Bug round 1 (live test adam+steve, Oct 13 2026) — FIXED

Symptoms: invitee had to manually join after being invited; group chat
window stale until closed and reopened; leader saw stale state too.

Root causes (all in win32_main.c):

1. **Thread bug**: sync callbacks (on_dm, on_generic) run on the
   background sync thread. They posted `WM_USER+4` to the MAIN window,
   which had no handler for it — the group window never learned about
   new state/messages. (`WM_USER+4` only handled in group_wnd_proc.)
2. **g_group_wnd never assigned** — could not post to the group window
   at all; opening Group twice stacked two windows.
3. **Blocking MessageBox on the sync thread** (invite popup + new-DM
   notice): relay processing froze until the user clicked OK.
4. **State adoption (kind 30081) posted no refresh at all.**
5. Group window open didn't repaint the roster from current state.

Fixes: new `WM_GROUPREFRESH` posted to main window (handles by repainting
group window + roster); new `WM_INVITEPOPUP` — sync thread stages text in
`g_pending_popup`, UI thread shows it and (for invites) opens the group
window; `g_group_wnd` now assigned/cleared, duplicate-open brings to
front; 30081 adoption posts refresh; roster refreshed on window open.

The manual-join workaround for invitees: the invite auto-joins by name
only; the roster fills in when the leader's state event arrives (<=5s
poll) and now repaints live.

## 3b. Bug round 2 (live test with reese/witherspoon) — FIXED

Symptoms: occasional lost chat messages (rapid sends replaced each
other; one user's message visible on one client but not the other);
flaky join notifications/join flow.

Root cause: channel chat AND join/leave control both used kind 30083
with dtag p99lfg-g-<channel>. Kinds 30000-39999 are REPLACEABLE in
Nostr: same pubkey+kind+dtag => the relay DELETES the older event.
So a user's new chat message deleted their previous one, and a chat
message could delete a pending join control event.

Fixes:
- chat messages: kind 9 (regular, immutable) — channels.c
  CHANNEL_KIND 9
- join/leave control: kind 9 as well, CHANNEL_CTL_KIND 9 (separate
  define so it can diverge later) — same kind is fine now since both
  are regular events, replaced-by nothing
- REQ filter + sync_handle_message kind lists updated (nostr.c,
  sync.c): kind 9 in, kind 30083 out
- win32_main.c on_generic: kind 9 branch (was 30083)
- NEW dedup: SyncState.seen_ids ring (256 ids, sync.h/sync.c) —
  every poll re-downloads stored events; without dedup chat history
  re-appended every 5s poll. Test fixture updated (ids must be
  unique) + new dup-suppression test.
- NOTE: group STATE stays kind 30081 replaceable — correct there,
  state SHOULD replace.
- channel_parse() in channels.c is dead code (GUI parses inline);
  its kind check still says CHANNEL_KIND — harmless, unused.

## 3c. Bug round 3 (able/cain: receiver saw nothing) — FIXED

Symptom: sender saw own messages (LOCAL append — proves nothing about
the relay); receiver never got anything. Relay probe (node script,
kinds:[9] #t p99lfg) showed both users' messages STORED on nos.lol and
relay.damus.io — so kind 9 publishes fine; the receive side dropped it.

Root cause: MY earlier fix fb3da51 was INCOMPLETE. The kind-9 dispatch
in sync.c sync_handle_message never landed (edit errored on a mismatch
and only the nostr.c REQ half was retried). sync.c still forwarded only
kinds 30079/30080/30083/30081/30082 -> kind 9 silently dropped before
on_generic ever fired. LESSON: a failed multi-block edit must be
re-verified block-by-block, not assumed.

Fix: sync.c kind list now includes "9". Regression test added
(test_sync_messages): a kind-9 wire event must reach the generic
callback with its d tag — fails on old code, passes now.

Debugging trick used: probe relays directly with node + built-in
WebSocket: REQ {"kinds":[9],"#t":["p99lfg"],"limit":20} on wss://nos.lol
/ wss://relay.damus.io — separates publish-side from receive-side bugs.

## 3d. Bug round 4 (invitee empty roster; msgs lost on rapid exchange;
no invite popup) — FIXED

1. Invitee saw no members: the dedup ring (round 3) suppressed the
   kind-30081 state event, which the invitee's client had already
   received BEFORE the invite DM activated the group. Dedup is now
   scoped: only append-style kinds (9 chat, 4 DM, 30079/30080) are
   deduped; state kinds (30081 roster, 30082 profile, 30078 LFG)
   are re-delivered every poll (latest-wins).
2. Message lost when sent right after someone else: g_group_log_text
   was appended by BOTH the UI thread (own sends) and the sync thread
   (received msgs) with unsynchronized strncat (read-modify-write
   race). Added CRITICAL_SECTION g_group_lock + group_log_append();
   state adoption and roster refresh take the lock too (refresh
   snapshots under lock, renders outside).
3. No "you've been invited" popup: single g_pending_popup slot was
   overwritten by any DM notice in the same poll batch. Invites now
   have a sticky g_pending_invite flag + their own popup that opens
   the group window on OK; DM notices keep the single slot.

Tests updated: kind-30078 is state (re-delivered), kind-9 dup still
suppressed, kind-30081 re-delivered every poll.

## 3e. Bug round 5 (wire audit) — FIXED / NOTES

Full relay probe (node, all kinds #t p99lfg) revealed:

- Messages sometimes landed on only ONE relay (charsurname msgs on
  nos.lol, missing on damus). Root cause: sync_send_event counted
  "quiet socket" as published. Now requires explicit relay verdict;
  ok:false / NOTICE / silence = not counted, and the group chat send
  shows "send failed" when no relay accepted.
- Enter-to-send added for group + DM chat inputs (subclassed EDIT
  controls; g_chat_wnd was never assigned - fixed).
- REQ has no since/limit: every 5s poll re-downloads the ENTIRE p99lfg
  feed (fine at current volume; growth risk later - see roadmap).
- USER REPORTED "cannot see roster" with the 20:11 exe - the roster
  fix (dedup scoping) landed in the 20:58 build 68db02f. Users MUST
  update BOTH clients; old+new mix reproduces old bugs.

## 5. Code review findings (Oct 13, post round-5)

QUICK WINS LANDED (round 6, while user resting - commit log has details):

- [x] #1 content truncation: NostrEvent.content 512 -> 2048; escape
      buffers 1024 -> 4096; event-message buffers 2048 -> 4200;
      sync.c receive content[600] -> 2200; channels.c csv buffers
      600 -> 1200/400. Regression test: full 6-member roster round-trips
      untruncated.
- [x] #2 self-echo: kind-9 group chat handler now skips own pubkey
      (was appending your own messages a second time on the next poll).
- [x] auto-scroll: chat_refresh + group_refresh now EM_SETSEL/EM_SCROLLCARET
      to bottom (SetWindowTextA used to snap the view to the top every
      refresh - user had to scroll down after every message).
- [x] input caps: EM_SETLIMITTEXT 500 on both chat inputs (matches what
      can actually be sent).
- [x] #3 DM IV randomness: chat.c random_bytes now uses
      CryptGenRandom on Windows (was rand()^GetTickCount - predictable).

REMAINING from review (not yet fixed):

Bugs found, by severity:

1. **Content truncation** (channels.c/nostr.c): NostrEvent.content is
   512 bytes. Worst-case group STATE csv (6 members + 2 wait, 64-char
   names, 65-char pubkeys) is ~1092 bytes -> snprintf truncates the
   roster JSON mid-way -> parse produces garbage/short rosters at >2-3
   members with long names. Chat worst case (32+512) = 564 > 512 too.
   FIX: enlarge content (e.g. 2048) AND cap message input to ~400
   chars, cap roster names, or both. Also NostrEvent.content limit vs
   relay limits should be checked.
2. **Self-echo** (win32_main.c ~620): the kind-9 group chat handler
   has NO `pubkey == g_sync.pubkey` check. Your own messages come back
   on the next poll and append AGAIN ("you see your message twice",
   ~5s later). Dedup can't help (sent ids were never received). FIX:
   skip own pubkey in the chat branch (or mark sent ids as seen).
3. **rand()-based IVs on Windows** (chat.c random_bytes): DM IVs use
   `rand() ^ GetTickCount()` — predictable; Nostr identity correctly
   uses CryptGenRandom, chat should too. Crypto hygiene, not the
   current bugs' cause.
4. **chat_peer_get recycles slot 0** when CHAT_PEERS_MAX is full —
   silently corrupts the conversation log of peer 0. Should evict
   least-recently-used instead.
5. **group_create tag collision**: grp%06x from time^tick-seeded
   rand(); two users creating in the same second can collide (1 in
   16M per seed, but seeds are correlated). Low priority.
6. **Send failures are silent for state**: group_state_publish return
   is ignored at call sites — a failed roster publish desyncs clients
   until the next change. Consider retry/status line.
7. **WSA/EM_CHARFROMPOS roster right-click**: HIWORD of the result is
   the line index — code treats it correctly, but EM_CHARFROMPOS
   returns a packed value; on very old edit controls without
   ES_MULTILINE range checks this can overflow. Verify on real use.
8. presence_set uses `g_presence[g_npresence]` after the bounds check
   returns without else-guard in one path — verified OK (185 checks).

UX improvements suggested (ease of use):

- Group + DM chat logs don't auto-scroll to the bottom on new content
  (SetWindowTextA resets view to top every refresh — user must scroll
  down manually EVERY new message. FIX: after SetWindowText, do
  EM_SETSEL -1,-1 + EM_SCROLLCARET, or switch to EM_REPLACESEL
  append). This is probably the single most annoying live bug.
- No visual feedback of "connecting/syncing" state (a status text in
  the group window title like 'synced 5s ago').
- Group window: roster and log are fixed-size; at 640x460 they fit,
  but no resizing behavior. Low priority.
- Input length unbounded visually: user can type 5000 chars, only 512
  are used; cap the input control (EM_SETLIMITTEXT).
- The invite popup has no Decline path (auto-joins on OK).
- No timestamps in group chat lines; helpful for 'missing message'
  debugging AND for users.
- Window title for group window should include the member count.

## 6. USER DECISIONS (Oct 13 2026, while user away from testing)

Direction confirmed by the user for the next phase of work:

1. **Sync architecture: PERSISTENT CONNECTION.** The 5s poll model
   (connect/REQ/download-all/close) is the root of several bug classes
   fought in rounds 1-5. Replace with one persistent relay connection
   + real-time EVENT stream (with reconnect/backoff). This is the
   primary structural fix to do next. Keep a `since` filter regardless
   so reconnects don't re-download the world.
2. **Chat UX: TIMESTAMPS on messages** (user pick). Add HH:MM to group
   + DM chat lines.
3. **Roadmap priority: EASE OF INSTALL/USE** once groups are solid
   (over raids/mobile parity): onboarding polish, first-run flow,
   clearer windows, setup instructions.
4. **Testing: AUTOMATED HARNESS.** User cannot test for a while; I
   build a self-test that exercises the group flow end-to-end without
   two humans (local fake relay already exists in test_ws.c pattern:
   spin a loopback WS server, run two sync instances against it,
   create/invite/join/chat and assert on delivered events).

## 7. Work order while user is away

ROUND 8 (aesthetics + voice groundwork):
- [x] Timestamps (HH:MM) on group chat + DM lines (user-picked UX item).
- [x] VOICE GROUNDWORK (ROADMAP section 2, phase 1):
      - src/voice.[ch]: full-mesh UDP transport, VoicePacket framing
        (magic/seq/stamp/len + AES-256-CBC-encrypted payload under the
        pairwise ECDH secret - same key material as chat, per design
        doc), per-peer dedup, loopback-testable (no audio hardware).
      - voice presence signaling rides the group channel as a kind-9
        control message {"voice":"pubkey:udpport"}.
      - tests/test_voice.c: two instances exchange encrypted frames on
        loopback UDP, assert integrity/attribution/dedup/drop-unknown;
        presence parse round-trip. 16 checks green, in build.sh.
      - GUI: Voice button in the group window - binds an ephemeral UDP
        port, publishes voice presence, logs who else started voice.
        (Video: NO. Voice capture/codec - libopus + WinMM - is the next
        layer and NOT wired yet; the button currently only establishes
        the transport + signaling. Don't promise audio to the user yet.)
      - derive_shared_key un-staticed from chat.c into chat.h (shared
        ECDH utility, used by chat AND voice).
NEXT for voice (when continuing): libopus encode/decode at 16-24kbps
mono 20ms; WinMM WaveIn capture + WaveOut playback on a 50ms timer;
speaking-indicator dots in the roster; wire voice_poll into the sync
thread loop when g_voice_on.

- [x] Automated two-instance group-flow harness - DONE (round 7):
      tests/test_group_flow.c, wired into build.sh. Persistent loopback
      fake relay (stores events, per-frame REQ replies), two SyncState
      clients run create/invite/join/chat/full-roster end-to-end.
      16 checks, all green. **IT FOUND A REAL BUG ON FIRST RUN**:
      chat_try_decrypt rewrote the payload into "[Name] text" display
      form BEFORE on_dm's GROUPINV: prefix check -> invites sent with a
      character name (the normal leader flow!) never matched, so invite
      popups/joins were flaky. Fix: decrypt returns the raw payload;
      on_dm unwraps the {"from":..,"msg":..} envelope itself and checks
      GROUPINV on the inner msg; DM history display uses the unwrapped
      pieces. LESSON: the invite flow can now be regression-tested
      headlessly - every future change to it gets caught here.
- [ ] Timestamps in group + DM chat lines
- [ ] Persistent relay connection (phased: keep poll as fallback,
      add persistent subscribe + reconnect/backoff; then retire poll)
- [ ] Ease-of-install pass (deferred until groups verified by harness)

## 8. Next after this feature

See ROADMAP doc: raids (multi-group), voice indication, mobile parity.
Known rough edges to consider: group window layout is fixed-position;
waitlist has no UI for the leader to promote manually (auto-promotes on
leave only); `g_pending_popup` is a single slot (two popups in the same
poll cycle overwrite each other - acceptable).
