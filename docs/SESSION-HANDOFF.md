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

## 2. Done in this session (update as work completes)

- [x] Wrote this handoff file (crash-resume point = this file).
- [ ] Create group button wired, joinq dangling call fixed
- [ ] Officer enforcement in on_invite + context menu
- [ ] Roster markers + leader toggle (right-click on roster)
- [ ] Tests for group state CSV round-trip incl. officer flag
- [ ] Full `./build.sh` green
- [ ] Committed + pushed to `main` (user has been committing to main
      directly in this repo - previous session pushed b7d7686 to main)

## 3. Next after this feature

See ROADMAP doc: raids (multi-group), voice indication, mobile parity.
