/* p99lfg - minimal Win32 LFG companion for Project 1999.
 * Single-file GUI. Build with mingw-w64 (see build-windows.sh or CI).
 */
#include "core.h"
#include "sync.h"
#include "chat.h"
#include "chars.h"
#include "settings.h"
#include "channels.h"
#include "voice.h"
#include "voice_audio.h"
#include "voice_codec.h"
#include "fav.h"
#include "rating.h"
#include "qrcodegen.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <shlobj.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_POLL (WM_USER + 1)
#define WM_REMOTE (WM_USER + 2)

#define ID_LIST 100
#define ID_BTN_REFRESH 101
#define ID_BTN_DIR 102
#define ID_BTN_ADD 103
#define ID_BTN_REMOVE 104
#define ID_ED_NAME 105
#define ID_ED_ZONE 106
#define ID_ST_DIR 106
#define ID_ST_DIR_LBL 107

static LfgState *g_state;
static HWND g_hwnd, g_list, g_ed_name, g_ed_zone, g_st_dir;
static UINT g_timer;
static HFONT g_font_bold;
static SyncState g_sync;
static int g_sync_thread_run;
static HANDLE g_sync_thread;
static FavList *g_favs;
static RatingStore *g_ratings;
static CharList *g_chars;
static char g_keypath[LFG_PATH_MAX];
static HWND g_char_list;
static Settings g_settings;
static char g_inipath[LFG_PATH_MAX];
static HWND g_group_log, g_group_input, g_group_channel, g_group_roster;
static HWND g_group_wnd;
static char g_group_name[CHANNEL_NAME_MAX];
static char g_group_log_text[8192];
static GroupMember g_group_members[GROUP_MEMBERS_MAX];
static int g_group_nmembers;
static GroupMember g_group_wait[GROUP_WAIT_MAX];
static int g_group_nwait;
static int g_group_is_leader;
static ReconState g_recon;
static int g_voice_on;
static int g_mic_level;        /* 0-100, posted to UI for the indicator */
static int g_ptt_down;        /* current push-to-talk key state */
static HWND g_voice_indicator; /* "MIC: [####  ] hot/off" label */
static int g_mic_live;          /* 1 while transmitting: red indicator */
static VoiceCodec g_vcodec;

/* My LAN address for voice presence: whatever the OS picks for an
 * outbound datagram (works even with no DNS/network). */
static void voice_local_ip(char *out, size_t osz)
{
    out[0] = '\0';
    SOCKET tmp = socket(AF_INET, SOCK_DGRAM, 0);
    if (tmp == INVALID_SOCKET)
        return;
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(9); /* discard port */
    inet_pton(AF_INET, "8.8.8.8", &dest.sin_addr);
    if (connect(tmp, (struct sockaddr *)&dest, sizeof(dest)) == 0) {
        struct sockaddr_in local;
        int sl = sizeof(local);
        if (getsockname(tmp, (struct sockaddr *)&local, &sl) == 0)
            inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)osz);
    }
    closesocket(tmp);
}
/* voice groundwork: UDP transport is bound and presence published;
 * capture/codec (libopus + WinMM) is the next layer, not wired yet */
static VoiceState g_voice;
/* g_group_log_text and the roster arrays are touched by BOTH the UI
 * thread (own sends, create/invite) and the sync thread (received
 * messages, adopted state). Unsynchronized strncat from two threads is
 * a read-modify-write race: one append overwrites the other ("message
 * lost when sent right after someone else"). Every mutation goes
 * through group_log_append / takes g_group_lock. */
static CRITICAL_SECTION g_group_lock;
static int g_group_lock_init;
static void group_lock(void)
{
    if (!g_group_lock_init) {
        /* first caller initialises; both threads enter here before any
         * group state exists, so the race on the init flag is benign
         * (InitializeCriticalSection on an already-initialised CS is a
         * no-op in practice on Windows). */
        InitializeCriticalSection(&g_group_lock);
        g_group_lock_init = 1;
    }
    EnterCriticalSection(&g_group_lock);
}
static void group_unlock(void)
{
    LeaveCriticalSection(&g_group_lock);
}
static void voice_frame_cb(const char *sender_pubkey, const uint8_t *frame,
                           size_t len, uint32_t stamp_ms, void *ud);
static void group_log_append(const char *text)
{
    group_lock();
    strncat(g_group_log_text, text,
            sizeof(g_group_log_text) - strlen(g_group_log_text) - 1);
    group_unlock();
}
/* popup text staged by the sync thread, shown by the UI thread on
 * WM_INVITEPOPUP (MessageBox on the sync thread would block relays) */
static char g_pending_popup[256];
/* invites get their own sticky flag: a burst of DMs in one poll batch
 * must never overwrite a pending invite notification before the UI
 * thread shows it. */
static int g_pending_invite;
static char g_pending_invite_channel[CHANNEL_NAME_MAX];
static char g_pending_invite_by[64];
static char g_inv_channel[CHANNEL_NAME_MAX];
static char g_inv_role[16];
static char g_inv_sender[64];

/* last DM per sender pubkey: simple single-conversation chat window */
#define CHAT_LOG_MAX 4096
#define CHAT_PEERS_MAX 32

typedef struct {
    char pubkey[NOSTR_PUBKEY_HEX];
    char name[64];
    char log[CHAT_LOG_MAX];
} ChatPeer;

static ChatPeer g_chat_peers[CHAT_PEERS_MAX];
static int g_nchatpeers;
static char g_chat_with[NOSTR_PUBKEY_HEX];
static char g_chat_name[64];
static char *g_chat_log;   /* points at the active peer's log */
static HWND g_chat_wnd, g_chat_edit, g_chat_input;
/* HH:MM stamp for chat lines */
static void chat_time(char *out, size_t outsz)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, outsz, "%02d:%02d ", st.wHour, st.wMinute);
}
/* Enter-to-send: subclass procs defined near the group window (they
 * need g_group_wnd + the ID_ defines); declared here for on_chat(). */
static WNDPROC g_group_input_proc;
static WNDPROC g_chat_input_proc;
static LRESULT CALLBACK group_input_subclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK chat_input_subclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);


static ChatPeer *chat_peer_get(const char *pubkey, const char *name)
{
    for (int i = 0; i < g_nchatpeers; i++)
        if (strcmp(g_chat_peers[i].pubkey, pubkey) == 0)
            return &g_chat_peers[i];
    if (g_nchatpeers >= CHAT_PEERS_MAX)
        return &g_chat_peers[0]; /* recycle the oldest slot */
    ChatPeer *p = &g_chat_peers[g_nchatpeers++];
    snprintf(p->pubkey, sizeof(p->pubkey), "%s", pubkey);
    snprintf(p->name, sizeof(p->name), "%s", name ? name : "");
    p->log[0] = '\0';
    return p;
}

static ChatPeer *chat_peer_active(void)
{
    for (int i = 0; i < g_nchatpeers; i++)
        if (strcmp(g_chat_peers[i].pubkey, g_chat_with) == 0)
            return &g_chat_peers[i];
    return NULL;
}

#define WM_DMARRIVED (WM_USER + 3)
#define WM_GROUPREFRESH (WM_USER + 5)
#define WM_INVITEPOPUP (WM_USER + 6)
#define WM_MICUPDATE (WM_USER + 7)

/* ---- last-seen presence (option 1: freshness from event timestamps) ---- */
#define PRESENCE_MAX 512
#define PRESENCE_ONLINE_SECS 120
#define PRESENCE_RECENT_SECS (30 * 60)

typedef struct {
    char name[64];
    char pubkey[NOSTR_PUBKEY_HEX];
    uint64_t last_seen;
    char cls[32];
    int level;
    char user[64];   /* display name of the human behind the key */
} PresenceEntry;

static PresenceEntry g_presence[PRESENCE_MAX];
static int g_npresence;

static void presence_touch_key(const char *name, const char *pubkey, uint64_t when)
{
    if (!name[0])
        return;
    for (int i = 0; i < g_npresence; i++) {
        if (strcmp(g_presence[i].name, name) == 0) {
            if (when > g_presence[i].last_seen)
                g_presence[i].last_seen = when;
            if (pubkey && pubkey[0])
                snprintf(g_presence[i].pubkey, NOSTR_PUBKEY_HEX, "%s", pubkey);
            return;
        }
    }
    if (g_npresence >= PRESENCE_MAX)
        return;
    snprintf(g_presence[g_npresence].name, 64, "%s", name);
    snprintf(g_presence[g_npresence].pubkey, NOSTR_PUBKEY_HEX, "%s",
             pubkey ? pubkey : "");
    g_presence[g_npresence].last_seen = when;
    g_npresence++;
}

static int json_get_like(const char *content, const char *key, char *out, size_t outsz)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(content, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= outsz)
        return 0;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = '\0';
    return 1;
}

static void presence_set_user(const char *name, const char *user)
{
    if (!user || !user[0])
        return;
    for (int i = 0; i < g_npresence; i++)
        if (strcmp(g_presence[i].name, name) == 0) {
            snprintf(g_presence[i].user, 64, "%s", user);
            return;
        }
}

static const char *presence_pubkey(const char *name)
{
    for (int i = 0; i < g_npresence; i++)
        if (strcmp(g_presence[i].name, name) == 0)
            return g_presence[i].pubkey;
    return "";
}

static void presence_touch(const char *name, uint64_t when)
{
    presence_touch_key(name, NULL, when);
}

static const char *presence_mark(const char *name)
{
    uint64_t now = (uint64_t)time(NULL);
    for (int i = 0; i < g_npresence; i++) {
        if (strcmp(g_presence[i].name, name) == 0) {
            if (now <= g_presence[i].last_seen + PRESENCE_ONLINE_SECS)
                return "[ON] ";
            if (now <= g_presence[i].last_seen + PRESENCE_RECENT_SECS)
                return "[recent] ";
            return "[off] ";
        }
    }
    return "[off] ";
}

#define ID_BTN_FAV 110
#define ID_BTN_RATE 111
#define ID_BTN_CHAT 112
#define ID_BTN_QR 113
#define ID_RATE_DLG 200
#define ID_RATE_OK 201
#define ID_RATE_CANCEL 202
#define ID_CHAT_SEND 210
#define ID_CHAT_INPUT 211
#define ID_CHAT_LOG 212
#define ID_CHAR_LIST 220
#define ID_BTN_CHAR_ADD 221
#define ID_BTN_CHAR_DEL 222
#define ID_BTN_CHAR_LFG 223
#define ID_CHAR_NAME 224
#define ID_CHAR_CLASS 225
#define ID_CHAR_LEVEL 226
#define ID_CHAT_FROM 230
#define ID_GROUP_TAB 240
#define ID_GROUP_LOG 241
#define ID_GROUP_INPUT 242
#define ID_GROUP_SEND 243
#define ID_GROUP_CHANNEL 244
#define ID_GROUP_JOIN 245
#define ID_GROUP_JOINQ 246
#define ID_GROUP_LEAVE 247
#define ID_GROUP_ROSTER 248
#define ID_BTN_SETNAME 249
#define ID_INV_MEMBER 260
#define ID_INV_WAIT 261
#define ID_GROUP_CREATE 262
#define ID_GROUP_VOICE 265
#define ID_GROUP_VOICEOPT 266
#define ID_VO_MIC 270
#define ID_VO_OUTPUT 271
#define ID_VO_PTT 272
#define ID_VO_KEY 273
#define ID_VO_OK 274
#define ID_VO_CANCEL 275
#define ID_ROSTER_INV_ALLOW 263
#define ID_ROSTER_INV_DISALLOW 264
#define ID_ROSTER_PROMOTE 267
#define ID_BTN_CLEAR 250
#define ID_BTN_HISTORY 251

static FILE *g_log;
static int g_debug;

static void log_open(void)
{
    char path[LFG_PATH_MAX];
    GetModuleFileNameA(NULL, path, sizeof(path));
    char *slash = strrchr(path, '\\');
    if (slash)
        strcpy(slash + 1, "p99lfg.log");
    else
        strcat(path, ".log");
    g_log = fopen(path, "w");
}

static void log_printf(const char *fmt, ...)
{
    if (!g_log)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static void log_line(const char *msg)
{
    if (g_log) {
        long pos = ftell(g_log);
        if (pos > 1024 * 1024) {
            freopen(NULL, "w", g_log); /* truncate at 1MB */
            log_printf("log truncated\n");
        }
    }
    log_printf("%s\n", msg);
}

static void dbg_line(const char *msg)
{
    if (!g_debug)
        return;
    log_line(msg);
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep)
{
    if (g_log) {
        fprintf(g_log, "CRASH code=%08lx addr=%p\n",
                (unsigned long)ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress);
        fflush(g_log);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
static const char *DEFAULT_RELAYS[] = {
    "wss://nos.lol",
    "wss://relay.primal.net",
    "wss://nostr.wine",
    "wss://offchain.pub",
    "wss://relay.damus.io",
};

static void upsert_remote(const SyncRemoteEntry *e)
{
    if (e->lfg)
        lfg_add_manual(g_state, e->name, e->zone, e->detail);
    else
        lfg_remove(g_state, e->name);
}

static void on_remote_event(const SyncRemoteEntry *entry, const char *pubkey, void *ud)
{
    (void)pubkey; (void)ud;
    if (!g_hwnd)
        return;
    upsert_remote(entry);
    presence_touch_key(entry->name, pubkey,
                       entry->created_at ? entry->created_at
                                         : (uint64_t)time(NULL));
    if (entry->cls[0] || entry->level) {
        /* remember class/level for the history display */
        for (int i = 0; i < g_npresence; i++) {
            if (strcmp(g_presence[i].name, entry->name) == 0) {
                if (entry->cls[0])
                    snprintf(g_presence[i].cls, 32, "%s", entry->cls);
                if (entry->level > 0)
                    g_presence[i].level = entry->level;
                if (entry->user[0])
                    snprintf(g_presence[i].user, 64, "%s", entry->user);
                break;
            }
        }
    }
    PostMessageA(g_hwnd, WM_REMOTE, 0, 0);
}

static void on_dm(const char *sender_pubkey, const char *content, void *ud)
{
    (void)ud;
    if (!g_hwnd)
        return;
    char plain[1024];
    if (chat_try_decrypt(g_sync.privkey, sender_pubkey, content, plain, sizeof(plain)) != 0)
        return;
    /* unwrap {"from":"Name","msg":"text"} to get the raw message:
     * protocol prefixes like GROUPINV: live in msg (chat.c returns
     * the payload raw; the envelope is display-only) */
    char msgbuf[1024] = "";
    const char *msg = plain;
    char frombuf[64] = "";
    if (strncmp(plain, "{\"from\":", 8) == 0) {
        const char *f = plain + 8;
        const char *fe = strchr(f, '"');
        if (fe && (size_t)(fe - f) < sizeof(frombuf)) {
            memcpy(frombuf, f, (size_t)(fe - f));
            const char *m = strstr(fe, ",\"msg\":\"");
            if (m) {
                m += 8;
                const char *me = strrchr(m, '"');
                if (me && (size_t)(me - m) < sizeof(msgbuf)) {
                    memcpy(msgbuf, m, (size_t)(me - m));
                    msg = msgbuf;
                }
            }
        }
    }
    /* find the sender's display name from the roster */
    char sender_name[64] = "";
    for (int i = 0; i < g_npresence; i++)
        if (strcmp(g_presence[i].pubkey, sender_pubkey) == 0) {
            snprintf(sender_name, sizeof(sender_name), "%s", g_presence[i].name);
            break;
        }
    if (!sender_name[0])
        snprintf(sender_name, sizeof(sender_name), "%.12s", sender_pubkey);

    if (strncmp(msg, "GROUPINV:", 9) == 0) {
        /* group invite via DM: GROUPINV:<channel>:<role> */
        char ch[CHANNEL_NAME_MAX] = "";
        char role[16] = "member";
        const char *p = msg + 9;
        const char *c1 = strchr(p, ':');
        if (c1) {
            size_t n = (size_t)(c1 - p);
            if (n >= sizeof(ch)) n = sizeof(ch) - 1;
            memcpy(ch, p, n);
            snprintf(role, sizeof(role), "%s", c1 + 1);
        } else {
            snprintf(ch, sizeof(ch), "%s", p);
        }
        snprintf(g_group_name, sizeof(g_group_name), "%s", ch);
        g_group_log_text[0] = '\0';
        g_group_is_leader = 0;
        g_group_nmembers = 0;
        g_group_nwait = 0;
        char note[160];
        snprintf(note, sizeof(note),
                 "--- invited to group %s (%s) by %s ---\r\n", ch, role, sender_name);
        group_log_append(note);
        /* runs on the sync thread: no blocking MessageBox here, it would
         * freeze all relay processing until clicked. Stash + post. */
        snprintf(g_pending_invite_channel, sizeof(g_pending_invite_channel),
                 "%s", ch);
        snprintf(g_pending_invite_by, sizeof(g_pending_invite_by), "%s",
                 sender_name);
        g_pending_invite = 1;
        PostMessageA(g_hwnd, WM_INVITEPOPUP, 1, 0);
        return;
    }
    /* always append to the sender's own history; the envelope was
     * unwrapped above so msg/frombuf carry the display pieces */
    ChatPeer *peer = chat_peer_get(sender_pubkey, sender_name);
    char dispbuf[1100];
    {
        char ts[8];
        chat_time(ts, sizeof(ts));
        if (frombuf[0])
            snprintf(dispbuf, sizeof(dispbuf), "%sThem [%s]: %s", ts, frombuf, msg);
        else
            snprintf(dispbuf, sizeof(dispbuf), "%sThem: %s", ts, msg);
    }
    strncat(peer->log, dispbuf, sizeof(peer->log) - strlen(peer->log) - 1);
    strncat(peer->log, "\r\n", sizeof(peer->log) - strlen(peer->log) - 1);

    if (strcmp(g_chat_with, sender_pubkey) != 0) {
        /* DM from someone we're not currently viewing: notify on the UI
         * thread (a MessageBox here would block the sync thread). */
        snprintf(g_pending_popup, sizeof(g_pending_popup),
                 "New message from %s (open Chat to read it).", sender_name);
        PostMessageA(g_hwnd, WM_INVITEPOPUP, 0, 0);
    }
    PostMessageA(g_hwnd, WM_DMARRIVED, 0, 0);
}

static int group_am_host(void)
{
    /* lowest pubkey among members is host. An EMPTY group has no host:
     * only the seeding joiner publishes initial state (checked at the
     * call site) - bystanders must never seed state or competing
     * state events collide. */
    if (g_group_nmembers == 0)
        return 0;
    char lowest[NOSTR_PUBKEY_HEX];
    snprintf(lowest, sizeof(lowest), "%s", g_group_members[0].pubkey);
    for (int i = 1; i < g_group_nmembers; i++)
        if (strcmp(g_group_members[i].pubkey, lowest) < 0)
            snprintf(lowest, sizeof(lowest), "%s", g_group_members[i].pubkey);
    return strcmp(g_sync.pubkey, lowest) == 0;
}
static int group_can_invite(void)
{
    /* leader can always invite; officers (flag granted by the leader)
     * can too. The officer flag comes from the roster state event, so
     * every client enforces the same permission. */
    if (g_group_is_leader)
        return 1;
    if (!g_group_name[0])
        return 0;
    for (int i = 0; i < g_group_nmembers; i++)
        if (strcmp(g_group_members[i].pubkey, g_sync.pubkey) == 0)
            return g_group_members[i].officer;
    return 0;
}

static void group_roster_refresh(void)
{
    if (!g_group_roster)
        return;
    group_lock();
    GroupMember snap_m[GROUP_MEMBERS_MAX];
    GroupMember snap_w[GROUP_WAIT_MAX];
    int snap_nm = g_group_nmembers, snap_nw = g_group_nwait;
    memcpy(snap_m, g_group_members, sizeof(snap_m[0]) * (size_t)snap_nm);
    memcpy(snap_w, g_group_wait, sizeof(snap_w[0]) * (size_t)snap_nw);
    group_unlock();
    char text[1024];
    size_t o = 0;
    o += (size_t)snprintf(text + o, sizeof(text) - o, "Group (%d/6):\r\n",
                          snap_nm);
    for (int i = 0; i < snap_nm; i++)
        o += (size_t)snprintf(text + o, sizeof(text) - o, "  %s%s%s\r\n",
                              snap_m[i].name,
                              strcmp(snap_m[i].pubkey, g_sync.pubkey) == 0
                                  ? " (you)" : "",
                              snap_m[i].officer ? " (can invite)" : "");
    if (snap_nw > 0) {
        o += (size_t)snprintf(text + o, sizeof(text) - o,
                              "Waiting (%d/2):\r\n", snap_nw);
        for (int i = 0; i < snap_nw; i++)
            o += (size_t)snprintf(text + o, sizeof(text) - o, "  %s\r\n",
                                  snap_w[i].name);
    }
    SetWindowTextA(g_group_roster, text);
    /* live title: member count + undelivered-sends indicator so a
     * relay outage is visible instead of silent */
    if (g_group_wnd) {
        char title[128];
        int pending = sync_outbox_pending(&g_sync);
        if (pending > 0)
            snprintf(title, sizeof(title), "Group: %s (%d/6) - %d sending",
                     g_group_name, snap_nm, pending);
        else
            snprintf(title, sizeof(title), "Group: %s (%d/6)",
                     g_group_name, snap_nm);
        SetWindowTextA(g_group_wnd, title);
    }
}

static void on_generic(int kind, const char *pubkey, const char *dtag,
                       const char *content, uint64_t created_at,
                       const char *event_id, void *ud)
{
    (void)ud;
    if (kind == 30082) {
        /* profile announce: display name for the sender's key */
        char user[64];
        if (json_get_like(content, "user", user, sizeof(user))) {
            for (int i = 0; i < g_npresence; i++)
                if (strcmp(g_presence[i].pubkey, pubkey) == 0) {
                    snprintf(g_presence[i].user, 64, "%s", user);
                    break;
                }
        }
        return;
    }
    if (kind == 30081) {
        /* group state event: adopt it (state is consolidated by the host) */
        if (!g_group_name[0])
            return;
        const char *chsuffix = strstr(dtag, "p99lfg-g-");
        if (!chsuffix)
            return;
        chsuffix += strlen("p99lfg-g-");
        if (strcmp(g_group_name, chsuffix) != 0)
            return;
        group_lock();
        group_state_parse(content, g_group_members, &g_group_nmembers,
                          g_group_wait, &g_group_nwait);
        group_unlock();
        /* roster painting happens on the UI thread via WM_GROUPREFRESH;
         * touching EDIT controls from the sync thread is unsafe and
         * left the roster blank on live builds */
        if (g_hwnd)
            PostMessageA(g_hwnd, WM_GROUPREFRESH, 0, 0);
        return;
    }
    if (kind == 9) {
        /* control messages first: {"join":"name:pubkey"} / {"leave":...} */
        const char *chctl = strstr(dtag, "p99lfg-g-");
        if (chctl && g_group_name[0] &&
            strcmp(g_group_name, chctl + strlen("p99lfg-g-")) == 0) {
            /* delivery-audit receipts: {"rcpt":"id,id,..."}. A peer's
             * receipt both prunes our spool (they have it) and triggers a
             * rebroadcast of anything they're missing (they'll dedupe
             * on arrival if another member already forwarded it). */
            if (strstr(content, "\"rcpt\":\"")) {
                if (strcmp(pubkey, g_sync.pubkey) == 0)
                    return; /* own receipt echo */
                char rcpt_ids[RECON_SEEN_MAX * (NOSTR_EVENT_ID_HEX + 1) + 8];
                {
                    /* extract the id list from the content */
                    const char *rp = strstr(content, "\"rcpt\":\"") + 8;
                    const char *re = strchr(rp, '"');
                    size_t rl = re ? (size_t)(re - rp) : 0;
                    if (rl >= sizeof(rcpt_ids))
                        rl = sizeof(rcpt_ids) - 1;
                    memcpy(rcpt_ids, rp, rl);
                    rcpt_ids[rl] = '\0';
                }
                recon_on_message(&g_recon, content);
                int missed = recon_rebroadcast_missing(&g_sync, &g_recon,
                                                       rcpt_ids);
                if (missed > 0) {
                    char note[96];
                    snprintf(note, sizeof(note),
                             "--- re-sent %d undelivered message(s) ---\r\n",
                             missed);
                    group_log_append(note);
                    PostMessageA(g_hwnd, WM_GROUPREFRESH, 0, 0);
                }
                return;
            }
            /* voice presence: {"voice":"pubkey:udpport"} */
            {
                char vpk[NOSTR_PUBKEY_HEX], vhost[64];
                int vport = 0;
                if (g_voice_on &&
                    voice_presence_parse(content, vpk, sizeof(vpk),
                                         vhost, sizeof(vhost), &vport) == 1) {
                    if (strcmp(vpk, g_sync.pubkey) == 0)
                        return; /* own presence echo */
                    /* register/refresh the peer so frames can be routed
                     * to them immediately (presence now carries ip) */
                    int was_new = 1;
                    for (int i = 0; i < g_voice.npeers; i++)
                        if (strcmp(g_voice.peers[i].pubkey, vpk) == 0)
                            was_new = 0;
                    voice_peer_register(&g_voice, vpk,
                                        vhost[0] ? vhost : NULL, vport);
                    if (was_new) {
                        char note[160];
                        snprintf(note, sizeof(note),
                                 "--- %s started voice (UDP %d) ---\r\n",
                                 strcmp(pubkey, g_sync.pubkey) == 0
                                     ? "you" : pubkey, vport);
                        group_log_append(note);
                        PostMessageA(g_hwnd, WM_GROUPREFRESH, 0, 0);
                    }
                    return;
                }
            }
            char who[600] = "";
            const char *j = strstr(content, "\"join\":\"");
            const char *l = strstr(content, "\"leave\":\"");
            const char *ctl = j ? j + 7 : (l ? l + 8 : NULL);
            if (ctl) {
                const char *ce = strchr(ctl, '"');
                if (ce && (size_t)(ce - ctl) < sizeof(who)) {
                    memcpy(who, ctl, (size_t)(ce - ctl));
                    who[ce - ctl] = '\0';
                    int is_seed = (j && g_group_nmembers == 0 &&
                                  strcmp(pubkey, g_sync.pubkey) == 0);
                    if (group_am_host() || is_seed) {
                        GroupMember m;
                        memset(&m, 0, sizeof(m));
                        char *colon = strchr(who, ':');
                        if (colon) {
                            *colon = '\0';
                            snprintf(m.name, sizeof(m.name), "%s", who);
                            snprintf(m.pubkey, sizeof(m.pubkey), "%s", colon + 1);
                            int already = 0;
                            for (int i = 0; i < g_group_nmembers; i++)
                                if (strcmp(g_group_members[i].pubkey, m.pubkey) == 0)
                                    already = 1;
                            if (j && !already) {
                                if (g_group_nmembers < GROUP_MEMBERS_MAX) {
                                    g_group_members[g_group_nmembers++] = m;
                                } else if (g_group_nwait < GROUP_WAIT_MAX) {
                                    g_group_wait[g_group_nwait++] = m;
                                }
                                group_state_publish(&g_sync, g_group_name,
                                                    g_group_members, g_group_nmembers,
                                                    g_group_wait, g_group_nwait);
                            } else if (l) {
                                for (int i = 0; i < g_group_nmembers; i++)
                                    if (strcmp(g_group_members[i].pubkey, m.pubkey) == 0) {
                                        memmove(&g_group_members[i], &g_group_members[i+1],
                                                (size_t)(g_group_nmembers - i - 1) * sizeof(GroupMember));
                                        g_group_nmembers--;
                                        break;
                                    }
                                for (int i = 0; i < g_group_nwait; i++)
                                    if (strcmp(g_group_wait[i].pubkey, m.pubkey) == 0) {
                                        memmove(&g_group_wait[i], &g_group_wait[i+1],
                                                (size_t)(g_group_nwait - i - 1) * sizeof(GroupMember));
                                        g_group_nwait--;
                                        break;
                                    }
                                /* promote from waitlist */
                                if (g_group_nmembers < GROUP_MEMBERS_MAX && g_group_nwait > 0) {
                                    g_group_members[g_group_nmembers++] = g_group_wait[0];
                                    memmove(&g_group_wait[0], &g_group_wait[1],
                                            (size_t)(g_group_nwait - 1) * sizeof(GroupMember));
                                    g_group_nwait--;
                                }
                                group_state_publish(&g_sync, g_group_name,
                                                    g_group_members, g_group_nmembers,
                                                    g_group_wait, g_group_nwait);
                            }
                            /* UI repaint via WM_GROUPREFRESH (posted by
                             * state adoption above); sync thread must not
                             * touch the roster control directly */
                        }
                    }
                    return; /* control message, not chat */
                }
            }
        }
        /* group/raid channel message. dtag = "p99lfg-g-<channel>",
         * content = {"from":"Char","msg":"text"} (from may be absent).
         * Skip our own messages: they were appended locally at send
         * time; the relay copy comes back on the next poll and would
         * double them. */
        if (event_id && event_id[0])
            recon_note_seen(&g_recon, event_id);
        if (strcmp(pubkey, g_sync.pubkey) == 0)
            return;
        if (!g_group_name[0])
            return;
        const char *chsuffix = strstr(dtag, "p99lfg-g-");
        if (!chsuffix)
            return;
        chsuffix += strlen("p99lfg-g-");
        if (strcmp(g_group_name, chsuffix) != 0)
            return;
        char frombuf[64] = "";
        const char *f = strstr(content, "\"from\":\"");
        if (f) {
            f += 8;
            const char *fe = strchr(f, '"');
            if (fe && (size_t)(fe - f) < sizeof(frombuf)) {
                memcpy(frombuf, f, (size_t)(fe - f));
                frombuf[fe - f] = '\0';
            }
        }
        const char *m = strstr(content, "\"msg\":\"");
        if (!m)
            return;
        m += 7;
        const char *me = strrchr(m, '"');
        if (!me)
            return;
        char disp[700];
        char ts[8];
        chat_time(ts, sizeof(ts));
        char sendername[64] = "";
        char username[64] = "";
        /* prefer the human's display name; fall back to char name, then key */
        for (int i = 0; i < g_npresence; i++)
            if (strcmp(g_presence[i].pubkey, pubkey) == 0) {
                snprintf(sendername, sizeof(sendername), "%s", g_presence[i].name);
                snprintf(username, sizeof(username), "%s", g_presence[i].user);
                break;
            }
        if (!sendername[0])
            snprintf(sendername, sizeof(sendername), "%.8s", pubkey);
        if (username[0])
            snprintf(sendername, sizeof(sendername), "%s", username);
        if (frombuf[0])
            snprintf(disp, sizeof(disp), "%s%s [%s]: %.*s\r\n", ts,
                     sendername, frombuf, (int)(me - m), m);
        else
            snprintf(disp, sizeof(disp), "%s%s: %.*s\r\n", ts,
                     sendername, (int)(me - m), m);
        group_log_append(disp);
        if (g_hwnd)
            PostMessageA(g_hwnd, WM_GROUPREFRESH, 0, 0);
        return;
    }
    if (!g_ratings)
        return;
    if (kind == 30079) {
        rating_record(g_ratings, pubkey, dtag, content, created_at);
        rating_store_finalize(g_ratings);
        presence_touch(dtag, created_at);
    } else if (kind == 30080) {
        int agree = strstr(content, "\"agree\":true") != NULL;
        vote_record(g_ratings, pubkey, dtag, agree, created_at);
        rating_store_finalize(g_ratings);
        presence_touch(pubkey, created_at);
    }
}

static DWORD WINAPI sync_thread(LPVOID arg)
{
    (void)arg;
    int round = 0;
    int last_pending = -1;
    while (g_sync_thread_run) {
        sync_poll(&g_sync);
        {
            /* title shows pending-sends count; repaint when it changes */
            int pending = sync_outbox_pending(&g_sync);
            if (pending != last_pending && g_group_wnd && g_hwnd) {
                last_pending = pending;
                PostMessageA(g_hwnd, WM_GROUPREFRESH, 0, 0);
            }
        }
        if (g_log && g_debug && (round % 12 == 0)) {
            log_printf("sync poll round %d: ", round);
            for (int i = 0; i < g_sync.nrelays; i++)
                log_printf("[%s connected=%d] ", g_sync.relays[i].url, g_sync.relays[i].connected);
            log_printf("\n");
        }
        /* heartbeat: refresh the timestamps of our active LFG characters
         * so other users see us as [ON] while the app runs (replaceable
         * events: republishing just updates created_at) */
        if (g_chars && (round % 12 == 0)) {
            for (int i = 0; i < g_chars->count; i++)
                if (g_chars->chars[i].lfg)
                    sync_publish_char(&g_sync, g_chars->chars[i].name,
                                      g_chars->chars[i].cls,
                                      g_chars->chars[i].level, "", 1);
        }
        /* delivery audit: publish a receipt every ~30s while in a
         * group (throttled to 15s minimum inside recon_send_receipt),
         * so peers can detect our gaps and we detect theirs. */
        if (g_group_name[0] && round % 6 == 5)
            recon_send_receipt(&g_sync, &g_recon, g_group_name);
        round++;
        /* ~5s between relay polls, but voice needs ~20ms service:
         * poll PTT + incoming audio every 30ms inside the wait. */
        for (int i = 0; i < 166 && g_sync_thread_run; i++) {
            if (g_voice_on) {
                voice_poll(&g_voice, g_sync.privkey, voice_frame_cb, NULL);
                if (g_settings.voice_ptt) {
                    if (g_settings.voice_ptt_mouse)
                        g_ptt_down = (GetKeyState(VK_XBUTTON1) & 0x8000) != 0;
                    else if (g_settings.voice_ptt_key)
                        g_ptt_down = (GetAsyncKeyState(g_settings.voice_ptt_key)
                                      & 0x8000) != 0;
                    else
                        g_ptt_down = 0;
                }
                /* repaint the indicator at ~15fps: level while held,
                 * "push your key" prompt while not */
                PostMessageA(g_hwnd, WM_MICUPDATE, 0, 0);
            }
            Sleep(30);
        }
    }
    return 0;
}

static void refresh_dir_label(HWND hwnd)
{
    char buf[LFG_PATH_MAX + 64];
    snprintf(buf, sizeof(buf), "Log dir: %s", lfg_get_dir(g_state));
    SetWindowTextA(g_st_dir, buf);
}

static void populate_list(void)
{
    /* remember the selected row's name so refreshes keep the highlight */
    char sel_name[64] = "";
    LRESULT sel = SendMessageA(g_list, LB_GETCURSEL, 0, 0);
    if (sel != LB_ERR) {
        char buf[256] = {0};
        SendMessageA(g_list, LB_GETTEXT, (WPARAM)sel, (LPARAM)buf);
        char *name = buf;
        if (strncmp(name, "---", 3) != 0) {
            if (strncmp(name, "* ", 2) == 0)
                name += 2;
            while (*name == '[') {
                char *close = strchr(name, ']');
                if (!close) { name = NULL; break; }
                name = close + 1;
                if (*name == ' ')
                    name++;
            }
            if (name) {
                char *sp = strchr(name, ' ');
                if (sp)
                    *sp = '\0';
                snprintf(sel_name, sizeof(sel_name), "%s", name);
            }
        }
    }

    SendMessageA(g_list, LB_RESETCONTENT, 0, 0);
    if (g_favs && fav_count(g_favs) > 0) {
        SendMessageA(g_list, LB_ADDSTRING, 0, (LPARAM)"--- Favourites ---");
        for (int i = 0; i < fav_count(g_favs); i++) {
            const char *name = fav_get(g_favs, i);
            const LfgEntry *e = NULL;
            for (int j = 0; j < lfg_count(g_state); j++) {
                const LfgEntry *c = lfg_get(g_state, j);
                if (strcmp(c->name, name) == 0) {
                    e = c;
                    break;
                }
            }
            char row[256];
            if (e)
                snprintf(row, sizeof(row), "* %s%s  [%s]  %s",
                         presence_mark(name), e->name,
                         e->detail[0] ? e->detail : "?",
                         e->zone[0] ? e->zone : "-");
            else
                snprintf(row, sizeof(row), "* %s%s  (offline)", presence_mark(name), name);
            SendMessageA(g_list, LB_ADDSTRING, 0, (LPARAM)row);
        }
        SendMessageA(g_list, LB_ADDSTRING, 0, (LPARAM)"--- All LFG ---");
    }
    int reselect = LB_ERR;
    for (int i = 0; i < lfg_count(g_state); i++) {
        const LfgEntry *e = lfg_get(g_state, i);
        if (g_favs && fav_has(g_favs, e->name))
            continue;
        char row[256];
        snprintf(row, sizeof(row), "%s%s  [%s]  %s%s%s",
                 presence_mark(e->name), e->name,
                 e->detail[0] ? e->detail : "?",
                 e->zone[0] ? e->zone : "-",
                 e->source == LFG_SRC_MANUAL ? "  (manual)" : "",
                 "");
        SendMessageA(g_list, LB_ADDSTRING, 0, (LPARAM)row);
        if (sel_name[0] && strcmp(e->name, sel_name) == 0)
            reselect = (int)SendMessageA(g_list, LB_GETCOUNT, 0, 0) - 1;
    }
    if (reselect == LB_ERR && sel_name[0]) {
        /* maybe it was in the favourites section */
        int count = (int)SendMessageA(g_list, LB_GETCOUNT, 0, 0);
        for (int i = 0; i < count; i++) {
            char buf[256] = {0};
            SendMessageA(g_list, LB_GETTEXT, (WPARAM)i, (LPARAM)buf);
            if (strstr(buf, sel_name)) {
                reselect = i;
                break;
            }
        }
    }
    if (reselect != LB_ERR)
        SendMessageA(g_list, LB_SETCURSEL, (WPARAM)reselect, 0);
}

static HWND g_status;

static void status_refresh(void)
{
    if (!g_status)
        return;
    char text[256];
    int online = 0;
    for (int i = 0; i < g_sync.nrelays; i++)
        if (g_sync.relays[i].connected)
            online++;
    char who[80] = "";
    if (g_settings.displayname[0])
        snprintf(who, sizeof(who), "  |  %s", g_settings.displayname);
    snprintf(text, sizeof(text),
             "Relays: %d/%d online%s  |  LFG listed: %d",
             online, g_sync.nrelays, who, lfg_count(g_state));
    SetWindowTextA(g_status, text);
}

static void do_poll(void)
{
    lfg_poll(g_state);
    populate_list();
    status_refresh();
}

static void on_add(void)
{
    char name[LFG_NAME_MAX], zone[LFG_ZONE_MAX];
    GetWindowTextA(g_ed_name, name, sizeof(name));
    GetWindowTextA(g_ed_zone, zone, sizeof(zone));
    if (!name[0])
        return;
    lfg_add_manual(g_state, name, zone, "");
    SetWindowTextA(g_ed_name, "");
    SetWindowTextA(g_ed_zone, "");
    populate_list();
    /* publish to relays so other players see this character LFG */
    char content[512];
    sync_build_content(content, sizeof(content), name, zone, "", 1);
    sync_publish(&g_sync, content);
}

static void on_remove(void)
{
    LRESULT sel = SendMessageA(g_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR)
        return;
    const LfgEntry *e = lfg_get(g_state, (int)sel);
    if (!e)
        return;
    lfg_remove(g_state, e->name);
    populate_list();
    /* publish the cleared state */
    char content[512];
    sync_build_content(content, sizeof(content), e->name, e->zone, e->detail, 0);
    sync_publish(&g_sync, content);
}

static void on_pick_dir(HWND hwnd)
{
    char dir[LFG_PATH_MAX];
    strncpy(dir, lfg_get_dir(g_state), sizeof(dir) - 1);
    BROWSEINFOA bi = {0};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = "Select your Project 1999 EverQuest folder (contains eqlog_*.txt)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
    if (!pidl)
        return;
    char path[MAX_PATH];
    if (SHGetPathFromIDListA(pidl, path)) {
        lfg_set_dir(g_state, path);
        snprintf(g_settings.logdir, sizeof(g_settings.logdir), "%s", path);
        settings_save(&g_settings, g_inipath);
        lfg_rescan(g_state);
        refresh_dir_label(hwnd);
        do_poll();
    }
    CoTaskMemFree(pidl);
}

static void populate_chars(void)
{
    SendMessageA(g_char_list, LB_RESETCONTENT, 0, 0);
    if (!g_chars)
        return;
    for (int i = 0; i < g_chars->count; i++) {
        Character *c = &g_chars->chars[i];
        char row[160];
        snprintf(row, sizeof(row), "%s  %d %s  %s",
                 c->name, c->level, c->cls,
                 c->lfg ? "[LFG]" : "");
        SendMessageA(g_char_list, LB_ADDSTRING, 0, (LPARAM)row);
    }
}

static void on_char_add(void)
{
    char name[CHAR_NAME_MAX], cls[CHAR_CLASS_MAX], lvl[8];
    GetWindowTextA(GetDlgItem(g_hwnd, ID_CHAR_NAME), name, sizeof(name));
    GetWindowTextA(GetDlgItem(g_hwnd, ID_CHAR_CLASS), cls, sizeof(cls));
    GetWindowTextA(GetDlgItem(g_hwnd, ID_CHAR_LEVEL), lvl, sizeof(lvl));
    int level = atoi(lvl);
    if (!name[0] || !cls[0] || level < 1 || level > 65) {
        MessageBoxA(g_hwnd,
            "Fill in name, class, and level (1-65) for the character.",
            "p99lfg", MB_ICONINFORMATION);
        return;
    }
    if (chars_upsert(g_chars, name, cls, level) != 0)
        return;
    SetWindowTextA(GetDlgItem(g_hwnd, ID_CHAR_NAME), "");
    populate_chars();
}

static void on_char_lfg_toggle(void)
{
    LRESULT sel = SendMessageA(g_char_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || !g_chars)
        return;
    Character *c = &g_chars->chars[sel];
    chars_toggle_lfg(g_chars, c->name);
    populate_chars();
    /* publish this character's new state */
    sync_publish_char(&g_sync, c->name, c->cls, c->level, "", c->lfg);
}

static void on_char_del(void)
{
    LRESULT sel = SendMessageA(g_char_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || !g_chars)
        return;
    Character *c = &g_chars->chars[sel];
    char wasname[CHAR_NAME_MAX];
    char wascls[CHAR_CLASS_MAX];
    int waslvl = c->level;
    snprintf(wasname, sizeof(wasname), "%s", c->name);
    snprintf(wascls, sizeof(wascls), "%s", c->cls);
    int waslfg = c->lfg;
    chars_remove(g_chars, c->name);
    populate_chars();
    /* publish cleared state so others stop seeing them */
    if (waslfg)
        sync_publish_char(&g_sync, wasname, wascls, waslvl, "", 0);
}

static void create_controls(HWND hwnd, HFONT hf)
{
    g_status = CreateWindowA("STATIC", "Relays: connecting...",
                            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                            10, 10, 590, 24, hwnd, (HMENU)254, NULL, NULL);
    g_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                             10, 40, 590, 390, hwnd, (HMENU)ID_LIST, NULL, NULL);

    /* ---- my characters panel (right) ---- */
    CreateWindowA("STATIC", "My Characters", WS_CHILD | WS_VISIBLE,
                 610, 10, 130, 22, hwnd, (HMENU)253, NULL, NULL);
    g_char_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                             610, 30, 140, 160, hwnd, (HMENU)ID_CHAR_LIST, NULL, NULL);
    CreateWindowA("STATIC", "Name:", WS_CHILD | WS_VISIBLE,
                 610, 198, 34, 16, hwnd, NULL, NULL, NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                 646, 195, 104, 20, hwnd, (HMENU)ID_CHAR_NAME, NULL, NULL);
    CreateWindowA("STATIC", "Class:", WS_CHILD | WS_VISIBLE,
                 610, 222, 34, 16, hwnd, NULL, NULL, NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                 646, 219, 104, 20, hwnd, (HMENU)ID_CHAR_CLASS, NULL, NULL);
    CreateWindowA("STATIC", "Level:", WS_CHILD | WS_VISIBLE,
                 610, 246, 34, 16, hwnd, NULL, NULL, NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                 646, 243, 40, 20, hwnd, (HMENU)ID_CHAR_LEVEL, NULL, NULL);
    CreateWindowA("BUTTON", "Add", WS_CHILD | WS_VISIBLE,
                 690, 242, 60, 22, hwnd, (HMENU)ID_BTN_CHAR_ADD, NULL, NULL);
    CreateWindowA("BUTTON", "Toggle LFG", WS_CHILD | WS_VISIBLE,
                 610, 270, 68, 22, hwnd, (HMENU)ID_BTN_CHAR_LFG, NULL, NULL);
    CreateWindowA("BUTTON", "Delete", WS_CHILD | WS_VISIBLE,
                 682, 270, 68, 22, hwnd, (HMENU)ID_BTN_CHAR_DEL, NULL, NULL);
    CreateWindowA("BUTTON", "Refresh",
                  WS_CHILD | WS_VISIBLE, 10, 440, 70, 24, hwnd, (HMENU)ID_BTN_REFRESH, NULL, NULL);
    CreateWindowA("BUTTON", "Change log folder...",
                  WS_CHILD | WS_VISIBLE, 90, 440, 140, 24, hwnd, (HMENU)ID_BTN_DIR, NULL, NULL);
    CreateWindowA("STATIC", "Name:", WS_CHILD | WS_VISIBLE,
                  240, 445, 40, 20, hwnd, NULL, NULL, NULL);
    g_ed_name = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                285, 442, 110, 22, hwnd, (HMENU)ID_ED_NAME, NULL, NULL);
    CreateWindowA("STATIC", "Zone:", WS_CHILD | WS_VISIBLE,
                  405, 445, 34, 20, hwnd, NULL, NULL, NULL);
    g_ed_zone = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                443, 442, 90, 22, hwnd, (HMENU)ID_ED_ZONE, NULL, NULL);
    CreateWindowA("BUTTON", "Mark LFG",
                  WS_CHILD | WS_VISIBLE, 540, 440, 80, 24, hwnd, (HMENU)ID_BTN_ADD, NULL, NULL);
    CreateWindowA("BUTTON", "Remove",
                  WS_CHILD | WS_VISIBLE, 625, 440, 75, 24, hwnd, (HMENU)ID_BTN_REMOVE, NULL, NULL);
    CreateWindowA("BUTTON", "Fav",
                  WS_CHILD | WS_VISIBLE, 10, 470, 45, 22, hwnd, (HMENU)ID_BTN_FAV, NULL, NULL);
    CreateWindowA("BUTTON", "Rate",
                  WS_CHILD | WS_VISIBLE, 60, 470, 45, 22, hwnd, (HMENU)ID_BTN_RATE, NULL, NULL);
    CreateWindowA("BUTTON", "Chat",
                  WS_CHILD | WS_VISIBLE, 110, 470, 45, 22, hwnd, (HMENU)ID_BTN_CHAT, NULL, NULL);
    CreateWindowA("BUTTON", "Group",
                  WS_CHILD | WS_VISIBLE, 160, 470, 55, 22, hwnd, (HMENU)ID_GROUP_TAB, NULL, NULL);
    CreateWindowA("BUTTON", "Show key QR",
                  WS_CHILD | WS_VISIBLE, 220, 470, 90, 22, hwnd, (HMENU)ID_BTN_QR, NULL, NULL);
    CreateWindowA("BUTTON", "Set name",
                  WS_CHILD | WS_VISIBLE, 315, 470, 75, 22, hwnd, (HMENU)ID_BTN_SETNAME, NULL, NULL);
    CreateWindowA("BUTTON", "Clear list",
                  WS_CHILD | WS_VISIBLE, 395, 470, 75, 22, hwnd, (HMENU)ID_BTN_CLEAR, NULL, NULL);
    CreateWindowA("BUTTON", "History",
                  WS_CHILD | WS_VISIBLE, 475, 470, 70, 22, hwnd, (HMENU)ID_BTN_HISTORY, NULL, NULL);
    g_st_dir = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE,
                             10, 500, 690, 20, hwnd, (HMENU)ID_ST_DIR, NULL, NULL);

    HWND ctrl = GetWindow(hwnd, GW_CHILD);
    while (ctrl) {
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)hf, TRUE);
        ctrl = GetWindow(ctrl, GW_HWNDNEXT);
    }
    SendMessageA(GetDlgItem(hwnd, 253), WM_SETFONT, (WPARAM)g_font_bold, TRUE);
}

/* ---- favourites ---- */
static void on_fav_toggle(void)
{
    LRESULT sel = SendMessageA(g_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || !g_favs)
        return;
    char buf[256] = {0};
    SendMessageA(g_list, LB_GETTEXT, (WPARAM)sel, (LPARAM)buf);
    char *name = buf;
    if (strncmp(name, "---", 3) == 0)
        return;
    if (strncmp(name, "* ", 2) == 0)
        name += 2;
    while (*name == '[') {           /* skip presence prefix [ON] etc. */
        name = strchr(name, ']');
        if (!name)
            return;
        name++;
        if (*name == ' ')
            name++;
    }
    char *sp = strchr(name, ' ');
    if (sp)
        *sp = '\0';
    fav_toggle(g_favs, name);
    populate_list();
}

/* ---- ratings ---- */
typedef struct {
    HWND competent, friendly, listens;
} RateDlgState;

static RateDlgState g_rate;
static char g_rate_target[NOSTR_PUBKEY_HEX];

static LRESULT CALLBACK rate_wnd_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND && (LOWORD(wp) == ID_RATE_OK || LOWORD(wp) == ID_RATE_CANCEL)) {
        if (LOWORD(wp) == ID_RATE_OK) {
            char cbuf[8], fbuf[8], lbuf[8];
            GetWindowTextA(g_rate.competent, cbuf, sizeof(cbuf));
            GetWindowTextA(g_rate.friendly, fbuf, sizeof(fbuf));
            GetWindowTextA(g_rate.listens, lbuf, sizeof(lbuf));
            int comp = atoi(cbuf), fri = atoi(fbuf), lis = atoi(lbuf);
            if (comp >= 1 && comp <= 5 && fri >= 1 && fri <= 5 && lis >= 1 && lis <= 5 &&
                g_rate_target[0])
                rating_publish(&g_sync, g_rate_target, comp, fri, lis);
        }
        DestroyWindow(dlg);
        return 0;
    }
    return DefWindowProcA(dlg, msg, wp, lp);
}

static void on_rate(void)
{
    LRESULT sel = SendMessageA(g_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR)
        return;
    char buf[256] = {0};
    SendMessageA(g_list, LB_GETTEXT, (WPARAM)sel, (LPARAM)buf);
    char *name = buf;
    if (strncmp(name, "---", 3) == 0)
        return;
    if (strncmp(name, "* ", 2) == 0)
        name += 2;
    while (*name == '[') {           /* skip presence prefix [ON] etc. */
        name = strchr(name, ']');
        if (!name)
            return;
        name++;
        if (*name == ' ')
            name++;
    }
    char *sp = strchr(name, ' ');
    if (sp)
        *sp = '\0';
    snprintf(g_rate_target, sizeof(g_rate_target), "%s", name);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = rate_wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "p99lfg_rate";
    RegisterClassA(&wc);
    RECT pr;
    GetWindowRect(g_hwnd, &pr);
    HWND dlg = CreateWindowExA(WS_EX_DLGMODALFRAME, "p99lfg_rate", "Rate player",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              pr.left + 160, pr.top + 120, 230, 190,
                              g_hwnd, NULL, NULL, NULL);
    CreateWindowA("STATIC", "Competent (1-5):", WS_CHILD | WS_VISIBLE,
                 10, 14, 100, 20, dlg, NULL, NULL, NULL);
    g_rate.competent = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "3",
                 WS_CHILD | WS_VISIBLE | ES_NUMBER, 115, 12, 30, 22, dlg, NULL, NULL, NULL);
    CreateWindowA("STATIC", "Friendly (1-5):", WS_CHILD | WS_VISIBLE,
                 10, 44, 100, 20, dlg, NULL, NULL, NULL);
    g_rate.friendly = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "3",
                 WS_CHILD | WS_VISIBLE | ES_NUMBER, 115, 42, 30, 22, dlg, NULL, NULL, NULL);
    CreateWindowA("STATIC", "Listens (1-5):", WS_CHILD | WS_VISIBLE,
                 10, 74, 100, 20, dlg, NULL, NULL, NULL);
    g_rate.listens = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "3",
                 WS_CHILD | WS_VISIBLE | ES_NUMBER, 115, 72, 30, 22, dlg, NULL, NULL, NULL);
    CreateWindowA("BUTTON", "Submit", WS_CHILD | WS_VISIBLE,
                 30, 110, 70, 24, dlg, (HMENU)ID_RATE_OK, NULL, NULL);
    CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE,
                 110, 110, 70, 24, dlg, (HMENU)ID_RATE_CANCEL, NULL, NULL);
    HFONT hf = (HFONT)SendMessageA(g_hwnd, WM_GETFONT, 0, 0);
    HWND c = GetWindow(dlg, GW_CHILD);
    while (c) {
        SendMessageA(c, WM_SETFONT, (WPARAM)hf, TRUE);
        c = GetWindow(c, GW_HWNDNEXT);
    }
    ShowWindow(dlg, SW_SHOW);
}

/* ---- chat ---- */
static void chat_refresh(void)
{
    if (g_chat_edit) {
        ChatPeer *peer = chat_peer_active();
        SetWindowTextA(g_chat_edit, peer ? peer->log : "");
        /* SetWindowText resets the view to the top; keep the newest
         * line visible without the user scrolling every message. */
        SendMessageA(g_chat_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageA(g_chat_edit, EM_SCROLLCARET, 0, 0);
    }
}

static LRESULT CALLBACK chat_wnd_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND && LOWORD(wp) == ID_CHAT_SEND && lp) {
        if (((HWND)lp) == GetDlgItem(dlg, ID_CHAT_SEND)) {
            char m[512];
            GetWindowTextA(g_chat_input, m, sizeof(m));
            if (m[0] && g_chat_with[0]) {
                ChatPeer *peer = chat_peer_active();
                char frombuf[CHAR_NAME_MAX] = "";
                {
                    HWND combo = GetDlgItem(dlg, ID_CHAT_FROM);
                    LRESULT ci = SendMessageA(combo, CB_GETCURSEL, 0, 0);
                    if (ci > 0 && g_chars && (int)ci <= g_chars->count)
                        snprintf(frombuf, sizeof(frombuf), "%s",
                                 g_chars->chars[ci - 1].name);
                }
                int send_rc = frombuf[0]
                    ? chat_send_message_from(&g_sync, g_chat_with, frombuf, m)
                    : chat_send_message(&g_sync, g_chat_with, m);
                if (send_rc == 0) {
                    if (peer) {
                        char ts[8];
                        chat_time(ts, sizeof(ts));
                        strncat(peer->log, ts, sizeof(peer->log) - strlen(peer->log) - 1);
                        strncat(peer->log, "You: ", sizeof(peer->log) - strlen(peer->log) - 1);
                        strncat(peer->log, m, sizeof(peer->log) - strlen(peer->log) - 1);
                        strncat(peer->log, "\r\n", sizeof(peer->log) - strlen(peer->log) - 1);
                    }
                    SetWindowTextA(g_chat_input, "");
                    chat_refresh();
                } else {
                    if (peer)
                        strncat(peer->log, "(send failed - no relay reachable)\r\n",
                                sizeof(peer->log) - strlen(peer->log) - 1);
                    chat_refresh();
                }
            } else if (!g_chat_with[0]) {
                MessageBoxA(dlg,
                    "No conversation yet. You can chat once someone DMs you, or after\n"
                    "identity sharing is configured between both apps.",
                    "p99lfg", MB_ICONINFORMATION);
            }
        }
        return 0;
    }
    if (msg == WM_DMARRIVED)
        chat_refresh();
    return DefWindowProcA(dlg, msg, wp, lp);
}

static void on_chat(void)
{
    /* start (or resume) a conversation with the selected player */
    LRESULT sel = SendMessageA(g_list, LB_GETCURSEL, 0, 0);
    if (sel != LB_ERR) {
        char buf[256] = {0};
        SendMessageA(g_list, LB_GETTEXT, (WPARAM)sel, (LPARAM)buf);
        char *name = buf;
        if (strncmp(name, "---", 3) == 0)
            return;
        if (strncmp(name, "* ", 2) == 0)
            name += 2;
        while (*name == '[') {
            name = strchr(name, ']');
            if (!name)
                return;
            name++;
            if (*name == ' ')
                name++;
        }
        char *sp = strchr(name, ' ');
        if (sp)
            *sp = '\0';
        const char *pk = presence_pubkey(name);
        if (pk && pk[0]) {
            ChatPeer *peer = chat_peer_get(pk, name);
            snprintf(g_chat_with, sizeof(g_chat_with), "%s", pk);
            snprintf(g_chat_name, sizeof(g_chat_name), "%s", peer->name[0] ? peer->name : name);
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "No Nostr identity known for '%s'.\n\n"
                     "They appear to be from your local log scan and don't run\n"
                     "the app (or their entry hasn't arrived yet). Encrypted\n"
                     "DMs need their app to have published an event.", name);
            MessageBoxA(g_hwnd, msg, "p99lfg", MB_ICONINFORMATION);
            return;
        }
    }
    if (!g_chat_with[0]) {
        MessageBoxA(g_hwnd,
                    "Select a player in the list first, then click Chat.",
                    "p99lfg", MB_ICONINFORMATION);
        return;
    }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = chat_wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "p99lfg_chat";
    RegisterClassA(&wc);
    RECT pr;
    GetWindowRect(g_hwnd, &pr);
    char title[96];
    snprintf(title, sizeof(title), "Chat with %s",
             g_chat_name[0] ? g_chat_name : "(nobody yet)");
    HWND dlg = CreateWindowExA(0, "p99lfg_chat", title,
                              WS_OVERLAPPEDWINDOW,
                              pr.left + 80, pr.top + 60, 420, 370,
                              g_hwnd, NULL, NULL, NULL);
    g_chat_wnd = dlg;
    g_chat_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                 WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                 10, 10, 380, 220, dlg, (HMENU)ID_CHAT_LOG, NULL, NULL);
    g_chat_input = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                 10, 240, 300, 24, dlg, (HMENU)ID_CHAT_INPUT, NULL, NULL);
    g_chat_input_proc = (WNDPROC)SetWindowLongPtrA(g_chat_input, GWLP_WNDPROC,
                                                   (LONG_PTR)chat_input_subclassProc);
    SendMessageA(g_chat_input, EM_SETLIMITTEXT, 500, 0);
    CreateWindowA("BUTTON", "Send", WS_CHILD | WS_VISIBLE,
                 315, 238, 75, 26, dlg, (HMENU)ID_CHAT_SEND, NULL, NULL);
    CreateWindowA("STATIC", "Send as:", WS_CHILD | WS_VISIBLE,
                 10, 270, 45, 18, dlg, NULL, NULL, NULL);
    HWND combo = CreateWindowA("COMBOBOX", NULL,
                 WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                 60, 268, 150, 200, dlg, (HMENU)ID_CHAT_FROM, NULL, NULL);
    if (g_chars) {
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"(default)");
        for (int i = 0; i < g_chars->count; i++)
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)g_chars->chars[i].name);
    }
    SendMessageA(combo, CB_SETCURSEL, 0, 0);
    HFONT hf = (HFONT)SendMessageA(g_hwnd, WM_GETFONT, 0, 0);
    HWND c = GetWindow(dlg, GW_CHILD);
    while (c) {
        SendMessageA(c, WM_SETFONT, (WPARAM)hf, TRUE);
        c = GetWindow(c, GW_HWNDNEXT);
    }
    ShowWindow(dlg, SW_SHOW);
    chat_refresh();
    SetFocus(g_chat_input);
}

/* ---- group channel window ---- */
/* Enter-to-send: single-line EDIT controls in frame windows don't get
 * WM_CHAR Enter by default. Subclass the input boxes and forward Enter
 * as a click on their Send button. */
static LRESULT CALLBACK group_input_subclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (g_group_wnd)
            SendMessageA(g_group_wnd, WM_COMMAND, MAKEWPARAM(ID_GROUP_SEND, BN_CLICKED),
                          (LPARAM)GetDlgItem(g_group_wnd, ID_GROUP_SEND));
        return 0;
    }
    return CallWindowProcA(g_group_input_proc, h, msg, wp, lp);
}
static LRESULT CALLBACK chat_input_subclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (g_chat_wnd)
            SendMessageA(g_chat_wnd, WM_COMMAND, MAKEWPARAM(ID_CHAT_SEND, BN_CLICKED),
                          (LPARAM)GetDlgItem(g_chat_wnd, ID_CHAT_SEND));
        return 0;
    }
    return CallWindowProcA(g_chat_input_proc, h, msg, wp, lp);
}

static void group_refresh(void)
{
    if (g_group_log) {
        SetWindowTextA(g_group_log, g_group_log_text);
        SendMessageA(g_group_log, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageA(g_group_log, EM_SCROLLCARET, 0, 0);
    }
}

static void on_group_send(HWND dlg)
{
    char m[512];
    GetWindowTextA(g_group_input, m, sizeof(m));
    if (!m[0] || !g_group_name[0])
        return;
    char frombuf[CHAR_NAME_MAX] = "";
    if (g_chars && g_chars->count > 0)
        snprintf(frombuf, sizeof(frombuf), "%s", g_chars->chars[0].name);
    char ev_id[NOSTR_EVENT_ID_HEX];
    char raw[RECON_SPOOL_MSG];
    if (channel_send_ex(&g_sync, g_group_name, frombuf, m,
                        ev_id, sizeof(ev_id), raw, sizeof(raw)) == 0) {
        /* spool for delivery audit: kept until every member's receipt
         * acknowledges the id (recon_on_message prunes it) */
        recon_note_seen(&g_recon, ev_id);
        recon_note_sent(&g_recon, ev_id, raw);
        char disp[700];
        char ts[8];
        chat_time(ts, sizeof(ts));
        snprintf(disp, sizeof(disp), "%sYou [%s]: %s\r\n", ts,
                 frombuf[0] ? frombuf : "?", m);
        group_log_append(disp);
        SetWindowTextA(g_group_input, "");
        group_refresh();
    } else {
        MessageBoxA(dlg, "Send failed (no relay reachable).", "p99lfg",
                    MB_ICONINFORMATION);
    }
}

static void on_group_join(HWND dlg)
{
    char ch[CHANNEL_NAME_MAX];
    GetWindowTextA(g_group_channel, ch, sizeof(ch));
    if (!ch[0])
        return;
    /* sanitize: the channel becomes a tag */
    for (char *p = ch; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
            *p = '_';
    snprintf(g_group_name, sizeof(g_group_name), "%s", ch);
    g_group_log_text[0] = '\0';
    /* fresh channel: reset any previous channel's roster state */
    g_group_nmembers = 0;
    g_group_nwait = 0;
    group_roster_refresh();
    char title[96];
    snprintf(title, sizeof(title), "Group: %s (semi-public channel)", ch);
    SetWindowTextA(dlg, title);
    char welcome[128];
    snprintf(welcome, sizeof(welcome),
             "--- joined #%s ---\r\n", ch);
    group_log_append(welcome);
    group_refresh();
}

static void on_group_create(HWND dlg)
{
    /* leader flow: create a fresh group with a unique channel tag */
    char tag[16];
    srand((unsigned)time(NULL) ^ (unsigned)GetTickCount());
    snprintf(tag, sizeof(tag), "grp%06x", (unsigned)(rand() & 0xffffff));
    snprintf(g_group_name, sizeof(g_group_name), "%s", tag);
    g_group_log_text[0] = '\0';
    g_group_nmembers = 0;
    g_group_nwait = 0;
    g_group_is_leader = 1;
    char myname[64] = "Player";
    if (g_chars && g_chars->count > 0)
        snprintf(myname, sizeof(myname), "%s", g_chars->chars[0].name);
    GroupMember m;
    memset(&m, 0, sizeof(m));
    snprintf(m.name, sizeof(m.name), "%s", myname);
    snprintf(m.pubkey, sizeof(m.pubkey), "%s", g_sync.pubkey);
    g_group_members[g_group_nmembers++] = m;
    group_state_publish(&g_sync, g_group_name, g_group_members,
                        g_group_nmembers, g_group_wait, g_group_nwait);
    char title[128];
    snprintf(title, sizeof(title), "Group: %s (you are the leader)", tag);
    SetWindowTextA(dlg, title);
    char welcome[160];
    snprintf(welcome, sizeof(welcome),
             "--- group %s created ---\r\nRight-click players in the LFG list to invite them.\r\n",
             tag);
    group_log_append(welcome);
    if (g_group_channel)
        SetWindowTextA(g_group_channel, tag);
    group_roster_refresh();
    group_refresh();
}

/* invite a player (from the LFG list) as member or waitlisted */
static void on_invite(const char *name, int wait)
{
    if (!g_group_name[0] || !group_can_invite()) {
        MessageBoxA(g_hwnd,
                    "Create a group first (Group button -> Create group),\nor ask the leader to grant you invite rights.",
                    "p99lfg", MB_ICONINFORMATION);
        return;
    }
    const char *pk = presence_pubkey(name);
    if (!pk || !pk[0]) {
        MessageBoxA(g_hwnd,
                    "That player doesn't use the app (no Nostr identity known) -\n"
                    "only app users can be invited.",
                    "p99lfg", MB_ICONINFORMATION);
        return;
    }
    char myname[64] = "Player";
    if (g_chars && g_chars->count > 0)
        snprintf(myname, sizeof(myname), "%s", g_chars->chars[0].name);
    /* the invite rides as an encrypted DM */
    char payload[160];
    snprintf(payload, sizeof(payload), "GROUPINV:%s:%s",
             g_group_name, wait ? "wait" : "member");
    chat_send_message_from(&g_sync, pk, myname, payload);
    /* leader adds them to the roster immediately */
    GroupMember m;
    memset(&m, 0, sizeof(m));
    snprintf(m.name, sizeof(m.name), "%s", name);
    snprintf(m.pubkey, sizeof(m.pubkey), "%s", pk);
    int already = 0;
    for (int i = 0; i < g_group_nmembers; i++)
        if (strcmp(g_group_members[i].pubkey, pk) == 0)
            already = 1;
    if (!already) {
        if (g_group_nmembers < GROUP_MEMBERS_MAX)
            g_group_members[g_group_nmembers++] = m;
        else if (g_group_nwait < GROUP_WAIT_MAX)
            g_group_wait[g_group_nwait++] = m;
        group_state_publish(&g_sync, g_group_name, g_group_members,
                            g_group_nmembers, g_group_wait, g_group_nwait);
    }
    char note[160];
    snprintf(note, sizeof(note), "--- invited %s as %s ---\r\n", name,
             (g_group_nmembers <= GROUP_MEMBERS_MAX && !wait) ? "member" : "waitlisted");
    group_log_append(note);
    group_roster_refresh();
    group_refresh();
}

static void on_group_leave(HWND dlg)
{
    if (!g_group_name[0])
        return;
    char myname[64] = "Player";
    if (g_chars && g_chars->count > 0)
        snprintf(myname, sizeof(myname), "%s", g_chars->chars[0].name);
    group_control_send(&g_sync, g_group_name, myname, g_sync.pubkey, 1);
    if (group_am_host()) {
        /* host leaving: rebuild state without self and republish */
        int w = 0;
        for (int i = 0; i < g_group_nmembers; i++)
            if (strcmp(g_group_members[i].pubkey, g_sync.pubkey) != 0 &&
                w < GROUP_MEMBERS_MAX)
                g_group_members[w++] = g_group_members[i];
        g_group_nmembers = w;
        group_state_publish(&g_sync, g_group_name, g_group_members,
                            g_group_nmembers, g_group_wait, g_group_nwait);
    }
    group_roster_refresh();
}

/* ---- Voice options dialog: mic/headset selection + push-to-talk ----
 * Device lists come from WinMM (waveIn/waveOut enumeration). PTT key
 * is captured live: click "Set key", press any key, it sticks. */
static HWND g_vo_mic, g_vo_out, g_vo_ptt, g_vo_keybtn;
static int g_vo_capturing;

static void vo_key_label(char *out, size_t osz)
{
    if (g_settings.voice_ptt_mouse)
        snprintf(out, osz, "Mouse button %d", g_settings.voice_ptt_button);
    else if (g_settings.voice_ptt_key)
        snprintf(out, osz, "Key code %d", g_settings.voice_ptt_key);
    else
        snprintf(out, osz, "(click Set, then press a key)");
}

static LRESULT CALLBACK vo_wnd_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && g_vo_capturing) {
        g_settings.voice_ptt_key = (int)wp;
        g_settings.voice_ptt_mouse = 0;
        g_vo_capturing = 0;
        char lbl[64];
        vo_key_label(lbl, sizeof(lbl));
        SetWindowTextA(g_vo_keybtn, lbl);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN && g_vo_capturing) {
        g_settings.voice_ptt_mouse = 1;
        g_settings.voice_ptt_button = 1;
        g_vo_capturing = 0;
        char lbl[64];
        vo_key_label(lbl, sizeof(lbl));
        SetWindowTextA(g_vo_keybtn, lbl);
        return 0;
    }
    if (msg == WM_COMMAND) {
        int id = LOWORD(wp);
        if (id == ID_VO_KEY) {
            g_vo_capturing = 1;
            SetWindowTextA(g_vo_keybtn, "press any key / click...");
            SetFocus(dlg);
            return 0;
        }
        if (id == ID_VO_OK || id == ID_VO_CANCEL) {
            if (id == ID_VO_OK) {
                char mic[160] = "", outp[160] = "";
                LRESULT mi = SendMessageA(g_vo_mic, CB_GETCURSEL, 0, 0);
                LRESULT oi = SendMessageA(g_vo_out, CB_GETCURSEL, 0, 0);
                if (mi != CB_ERR && mi > 0)
                    SendMessageA(g_vo_mic, CB_GETLBTEXT, mi, (LPARAM)mic);
                if (oi != CB_ERR && oi > 0)
                    SendMessageA(g_vo_out, CB_GETLBTEXT, oi, (LPARAM)outp);
                snprintf(g_settings.voice_mic, sizeof(g_settings.voice_mic),
                         "%s", mic);
                snprintf(g_settings.voice_output,
                         sizeof(g_settings.voice_output), "%s", outp);
                g_settings.voice_ptt =
                    SendMessageA(g_vo_ptt, BM_GETCHECK, 0, 0) == BST_CHECKED;
                settings_save(&g_settings, g_inipath);
            }
            DestroyWindow(dlg);
            return 0;
        }
    }
    return DefWindowProcA(dlg, msg, wp, lp);
}

static void on_voice_options(HWND parent)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = vo_wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "p99lfg_voiceopt";
    RegisterClassA(&wc);
    RECT pr;
    GetWindowRect(parent ? parent : g_hwnd, &pr);
    HWND dlg = CreateWindowExA(WS_EX_DLGMODALFRAME, "p99lfg_voiceopt",
                              "Voice options",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              pr.left + 120, pr.top + 90, 330, 220,
                              g_hwnd, NULL, NULL, NULL);
    CreateWindowA("STATIC", "Microphone:", WS_CHILD | WS_VISIBLE,
                  10, 14, 80, 20, dlg, NULL, NULL, NULL);
    g_vo_mic = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                             95, 12, 220, 200, dlg, (HMENU)ID_VO_MIC, NULL, NULL);
    CreateWindowA("STATIC", "Headset / speakers:", WS_CHILD | WS_VISIBLE,
                  10, 44, 80, 20, dlg, NULL, NULL, NULL);
    g_vo_out = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                             95, 42, 220, 200, dlg, (HMENU)ID_VO_OUTPUT, NULL, NULL);
    g_vo_ptt = CreateWindowA("BUTTON", "Push to talk",
                             WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             10, 78, 110, 22, dlg, (HMENU)ID_VO_PTT, NULL, NULL);
    g_vo_keybtn = CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE,
                                130, 76, 120, 24, dlg, (HMENU)ID_VO_KEY, NULL, NULL);
    CreateWindowA("BUTTON", "OK", WS_CHILD | WS_VISIBLE,
                  80, 130, 70, 24, dlg, (HMENU)ID_VO_OK, NULL, NULL);
    CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE,
                  160, 130, 70, 24, dlg, (HMENU)ID_VO_CANCEL, NULL, NULL);
    HFONT hf = (HFONT)SendMessageA(g_hwnd, WM_GETFONT, 0, 0);
    HWND c = GetWindow(dlg, GW_CHILD);
    while (c) {
        SendMessageA(c, WM_SETFONT, (WPARAM)hf, TRUE);
        c = GetWindow(c, GW_HWNDNEXT);
    }
    /* enumerate capture devices: entry 0 = system default */
    SendMessageA(g_vo_mic, CB_ADDSTRING, 0, (LPARAM)"(System default)");
    {
        UINT n = waveInGetNumDevs();
        WAVEINCAPSA caps;
        for (UINT i = 0; i < n && i < 20; i++) {
            if (waveInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
                caps.szPname[0])
                SendMessageA(g_vo_mic, CB_ADDSTRING, 0, (LPARAM)caps.szPname);
        }
    }
    SendMessageA(g_vo_out, CB_ADDSTRING, 0, (LPARAM)"(System default)");
    {
        UINT n = waveOutGetNumDevs();
        WAVEOUTCAPSA caps;
        for (UINT i = 0; i < n && i < 20; i++) {
            if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
                caps.szPname[0])
                SendMessageA(g_vo_out, CB_ADDSTRING, 0, (LPARAM)caps.szPname);
        }
    }
    /* preselect saved devices (fallback: system default) */
    {
        int mi = 0, oi = 0;
        char buf[160];
        int cnt = (int)SendMessageA(g_vo_mic, CB_GETCOUNT, 0, 0);
        for (int i = 1; i < cnt; i++) {
            SendMessageA(g_vo_mic, CB_GETLBTEXT, i, (LPARAM)buf);
            if (strcmp(buf, g_settings.voice_mic) == 0) { mi = i; break; }
        }
        cnt = (int)SendMessageA(g_vo_out, CB_GETCOUNT, 0, 0);
        for (int i = 1; i < cnt; i++) {
            SendMessageA(g_vo_out, CB_GETLBTEXT, i, (LPARAM)buf);
            if (strcmp(buf, g_settings.voice_output) == 0) { oi = i; break; }
        }
        SendMessageA(g_vo_mic, CB_SETCURSEL, mi, 0);
        SendMessageA(g_vo_out, CB_SETCURSEL, oi, 0);
    }
    SendMessageA(g_vo_ptt, BM_SETCHECK,
                 g_settings.voice_ptt ? BST_CHECKED : BST_UNCHECKED, 0);
    char lbl[64];
    vo_key_label(lbl, sizeof(lbl));
    SetWindowTextA(g_vo_keybtn, lbl);
    g_vo_capturing = 0;
    ShowWindow(dlg, SW_SHOW);
    SetFocus(dlg);
}

/* Called on the WinMM audio thread: gate on push-to-talk, ship the
 * frame to every voice peer, and publish the mic level for the UI
 * indicator (atomic-ish int, read on the UI thread). */
static void va_capture_cb(const int16_t *pcm, size_t samples, void *ud)
{
    (void)ud;
    int transmit = vptt_gate(g_settings.voice_ptt, g_ptt_down);
    g_mic_level = transmit ? vptt_level(pcm, samples) : 0;
    if (!transmit)
        return;
    /* Opus-encode the 20ms frame, then ship the compressed bytes */
    uint8_t opusbuf[VCODEC_MAX_BYTES];
    int n = vcodec_encode(&g_vcodec, pcm, (int)samples,
                          opusbuf, sizeof(opusbuf));
    if (n <= 0)
        return;
    uint32_t stamp = (uint32_t)GetTickCount();
    for (int i = 0; i < g_voice.npeers; i++)
        if (g_voice.peers[i].active && g_voice.peers[i].port > 0)
            voice_send_frame(&g_voice, g_voice.peers[i].pubkey,
                             g_sync.privkey, opusbuf, (size_t)n, stamp);
}

/* Decrypted incoming voice frame -> speakers (sync-thread poll). */
static void voice_frame_cb(const char *sender_pubkey, const uint8_t *frame,
                           size_t len, uint32_t stamp_ms, void *ud)
{
    (void)sender_pubkey; (void)stamp_ms; (void)ud;
    /* decode opus -> PCM -> speakers */
    int16_t pcm[VCODEC_SAMPLES * 2];
    int n = vcodec_decode(&g_vcodec, frame, len, pcm,
                          sizeof(pcm) / sizeof(pcm[0]));
    if (n > 0)
        va_play_frame(pcm, (size_t)n);
}

static void on_group_voice_toggle(HWND dlg)
{
    if (!g_group_name[0]) {
        MessageBoxA(dlg, "Create or join a group first.",
                    "p99lfg", MB_ICONINFORMATION);
        return;
    }
    if (!g_voice_on) {
        if (voice_open(&g_voice) != 0) {
            MessageBoxA(dlg, "Could not bind a UDP port for voice.",
                    "p99lfg", MB_ICONINFORMATION);
            return;
        }
        if (va_capture_open(g_settings.voice_mic, va_capture_cb, NULL) != 0) {
            voice_close(&g_voice);
            MessageBoxA(dlg, "Could not open the microphone.\n"
                        "Check Voice options (device selection).",
                        "p99lfg", MB_ICONINFORMATION);
            return;
        }
        if (vcodec_open(&g_vcodec) != 0) {
            va_capture_close();
            voice_close(&g_voice);
            MessageBoxA(dlg, "Could not start the Opus codec.",
                        "p99lfg", MB_ICONINFORMATION);
            return;
        }
        if (va_play_open(g_settings.voice_output) != 0) {
            /* mic works, speakers failed: voice without hearing */
            group_log_append("--- voice: could not open speakers (check "
                             "Voice options) ---\r\n");
        }
        g_voice_on = 1;
        char myip[64];
        voice_local_ip(myip, sizeof(myip));
        if (voice_presence_publish_ip(&g_sync, g_group_name,
                                      g_sync.pubkey, g_voice.port,
                                      myip[0] ? myip : NULL) == 0) {
            char note[128];
            snprintf(note, sizeof(note),
                     "--- voice ready (UDP %d) - waiting for members ---\r\n",
                     g_voice.port);
            group_log_append(note);
        } else {
            group_log_append("--- voice: presence publish failed (relays down; will not retry - toggle voice again) ---\r\n");
        }
    } else {
        va_capture_close();
        va_play_close();
        vcodec_close(&g_vcodec);
        voice_close(&g_voice);
        g_voice_on = 0;
        g_mic_level = 0;
        group_log_append("--- voice off ---\r\n");
    }
    group_refresh();
    PostMessageA(g_hwnd, WM_MICUPDATE, 0, 0);
}
static LRESULT CALLBACK group_wnd_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CTLCOLORSTATIC) {
        /* mic indicator: red while transmitting, normal otherwise */
        if ((HWND)lp == g_voice_indicator) {
            HDC dc = (HDC)wp;
            SetBkMode(dc, OPAQUE);
            if (g_mic_live) {
                SetBkColor(dc, RGB(200, 40, 40));
                SetTextColor(dc, RGB(255, 255, 255));
            } else {
                SetBkColor(dc, RGB(230, 230, 230));
                SetTextColor(dc, RGB(60, 60, 60));
            }
            static HBRUSH live_br, idle_br;
            if (!live_br) live_br = CreateSolidBrush(RGB(200, 40, 40));
            if (!idle_br) idle_br = CreateSolidBrush(RGB(230, 230, 230));
            return (LRESULT)(g_mic_live ? live_br : idle_br);
        }
    }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_GROUP_SEND)
        { on_group_send(dlg); return 0; }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_GROUP_JOIN)
        { on_group_join(dlg); return 0; }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_GROUP_CREATE)
        { on_group_create(dlg); return 0; }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_GROUP_VOICEOPT)
        { on_voice_options(dlg); return 0; }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_GROUP_VOICE)
        { on_group_voice_toggle(dlg); return 0; }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_GROUP_LEAVE)
        { on_group_leave(dlg); return 0; }
    if (msg == WM_CONTEXTMENU && (HWND)wp == g_group_roster) {
        /* leader right-clicks the roster: toggle invite rights on a
         * member, or promote someone from the waitlist */
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT client_pt = pt;
        ScreenToClient(g_group_roster, &client_pt);
        int idx = -1;
        int widx = -1;
        if (g_group_is_leader && (g_group_nmembers > 0 || g_group_nwait > 0)) {
            int line = HIWORD(SendMessageA(g_group_roster, EM_CHARFROMPOS, 0,
                                           (LPARAM)&client_pt));
            char buf[128] = {0};
            *(WORD *)buf = sizeof(buf) - 1;
            SendMessageA(g_group_roster, EM_GETLINE, line, (LPARAM)buf);
            /* skip header lines: 'Group (n/6):' / 'Waiting (n/2):' */
            char *nm = buf;
            while (*nm == ' ')
                nm++;
            if (strncmp(nm, "Group", 5) != 0 && strncmp(nm, "Waiting", 7) != 0) {
                char *sp = strchr(nm, ' ');
                if (sp)
                    *sp = '\0';
                for (int i = 0; i < g_group_nmembers; i++)
                    if (strcmp(g_group_members[i].name, nm) == 0 &&
                        strcmp(g_group_members[i].pubkey, g_sync.pubkey) != 0) {
                        idx = i;
                        break;
                    }
                for (int i = 0; i < g_group_nwait; i++)
                    if (strcmp(g_group_wait[i].name, nm) == 0) {
                        widx = i;
                        break;
                    }
            }
        }
        if (widx >= 0 && g_group_nmembers < GROUP_MEMBERS_MAX) {
            /* waitlisted player: promote to member */
            HMENU menu = CreatePopupMenu();
            char label[96];
            snprintf(label, sizeof(label), "Promote %s to member",
                     g_group_wait[widx].name);
            AppendMenuA(menu, MF_STRING, ID_ROSTER_PROMOTE, label);
            int cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                          pt.x, pt.y, 0, dlg, NULL);
            DestroyMenu(menu);
            if (cmd == ID_ROSTER_PROMOTE) {
                g_group_members[g_group_nmembers++] = g_group_wait[widx];
                for (int i = widx; i < g_group_nwait - 1; i++)
                    g_group_wait[i] = g_group_wait[i + 1];
                g_group_nwait--;
                group_state_publish(&g_sync, g_group_name, g_group_members,
                                    g_group_nmembers, g_group_wait, g_group_nwait);
                char note[160];
                snprintf(note, sizeof(note), "--- %s promoted from waitlist ---\r\n",
                         g_group_members[g_group_nmembers - 1].name);
                group_log_append(note);
                group_roster_refresh();
                group_refresh();
            }
            return 0;
        }
        if (idx >= 0) {
            HMENU menu = CreatePopupMenu();
            char label[96];
            snprintf(label, sizeof(label), "%s invites",
                     g_group_members[idx].officer ? "Disallow" : "Allow");
            AppendMenuA(menu, MF_STRING, g_group_members[idx].officer
                        ? ID_ROSTER_INV_DISALLOW : ID_ROSTER_INV_ALLOW,
                        label);
            int cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                          pt.x, pt.y, 0, dlg, NULL);
            DestroyMenu(menu);
            if (cmd == ID_ROSTER_INV_ALLOW || cmd == ID_ROSTER_INV_DISALLOW) {
                g_group_members[idx].officer =
                    (cmd == ID_ROSTER_INV_ALLOW) ? 1 : 0;
                group_state_publish(&g_sync, g_group_name, g_group_members,
                                    g_group_nmembers, g_group_wait, g_group_nwait);
                char note[160];
                snprintf(note, sizeof(note), "--- %s: %s invites ---\r\n",
                         g_group_members[idx].name,
                         g_group_members[idx].officer ? "can now" : "no longer can");
                group_log_append(note);
                group_roster_refresh();
                group_refresh();
            }
        }
        return 0;
    }
    if (msg == WM_USER + 4)
        { group_refresh(); return 0; }
    if (msg == WM_GROUPREFRESH)
        { group_refresh(); group_roster_refresh(); return 0; }
    if (msg == WM_DESTROY) {
        if (g_group_wnd == dlg)
            g_group_wnd = NULL;
        return 0;
    }
    return DefWindowProcA(dlg, msg, wp, lp);
}

static void on_group_window(void)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = group_wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "p99lfg_group";
    RegisterClassA(&wc);
    RECT pr;
    GetWindowRect(g_hwnd, &pr);
    if (g_group_wnd) { /* already open: just bring it to the front */
        ShowWindow(g_group_wnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_group_wnd);
        return;
    }
    HWND dlg = CreateWindowExA(0, "p99lfg_group",
                               g_group_name[0] ? "Group chat" : "Group chat (join a channel)",
                               WS_OVERLAPPEDWINDOW,
                               pr.left + 100, pr.top + 80, 640, 460,
                               g_hwnd, NULL, NULL, NULL);
    CreateWindowA("STATIC", "Channel:", WS_CHILD | WS_VISIBLE,
                 10, 12, 50, 18, dlg, NULL, NULL, NULL);
    g_group_channel = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                 65, 10, 140, 22, dlg, (HMENU)ID_GROUP_CHANNEL, NULL, NULL);
    CreateWindowA("BUTTON", "Join", WS_CHILD | WS_VISIBLE,
                 210, 9, 60, 24, dlg, (HMENU)ID_GROUP_JOIN, NULL, NULL);
    g_group_log = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                 WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                 10, 40, 400, 260, dlg, (HMENU)ID_GROUP_LOG, NULL, NULL);
    g_group_input = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                 10, 310, 320, 24, dlg, (HMENU)ID_GROUP_INPUT, NULL, NULL);
    g_group_input_proc = (WNDPROC)SetWindowLongPtrA(g_group_input, GWLP_WNDPROC,
                                                    (LONG_PTR)group_input_subclassProc);
    SendMessageA(g_group_input, EM_SETLIMITTEXT, 500, 0);
    CreateWindowA("BUTTON", "Send", WS_CHILD | WS_VISIBLE,
                 335, 308, 75, 26, dlg, (HMENU)ID_GROUP_SEND, NULL, NULL);
    /* group roster + join/leave */
    g_group_roster = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                 WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
                 420, 40, 200, 220, dlg, (HMENU)ID_GROUP_ROSTER, NULL, NULL);
    CreateWindowA("BUTTON", "Create group", WS_CHILD | WS_VISIBLE,
                 420, 270, 95, 26, dlg, (HMENU)ID_GROUP_CREATE, NULL, NULL);
    CreateWindowA("BUTTON", "Voice (setup)", WS_CHILD | WS_VISIBLE,
                 420, 300, 95, 26, dlg, (HMENU)ID_GROUP_VOICE, NULL, NULL);
    CreateWindowA("BUTTON", "Voice options", WS_CHILD | WS_VISIBLE,
                 420, 330, 95, 26, dlg, (HMENU)ID_GROUP_VOICEOPT, NULL, NULL);
    g_voice_indicator = CreateWindowA("STATIC", "MIC: off", WS_CHILD | WS_VISIBLE,
                                      420, 362, 200, 18, dlg, NULL, NULL, NULL);
    CreateWindowA("BUTTON", "Leave", WS_CHILD | WS_VISIBLE,
                 525, 270, 95, 26, dlg, (HMENU)ID_GROUP_LEAVE, NULL, NULL);
    HFONT hf = (HFONT)SendMessageA(g_hwnd, WM_GETFONT, 0, 0);
    HWND c = GetWindow(dlg, GW_CHILD);
    while (c) {
        SendMessageA(c, WM_SETFONT, (WPARAM)hf, TRUE);
        c = GetWindow(c, GW_HWNDNEXT);
    }
    g_group_wnd = dlg;
    if (g_group_name[0])
        SetWindowTextA(g_group_channel, g_group_name);
    ShowWindow(dlg, SW_SHOW);
    group_refresh();
    group_roster_refresh();
}

/* ---- QR identity ---- */
/* ---- QR identity ---- */
static uint8_t g_qr[qrcodegen_BUFFER_LEN_MAX];
static int g_qr_size;
static int g_qr_ready;
static HWND g_qr_wnd;

static LRESULT CALLBACK qr_wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        if (g_qr_ready) {
            int scale = 6;
            int px = g_qr_size * scale;
            int ox = (rc.right - px) / 2;
            int oy = 10;
            if (ox < 0) ox = 0;
            for (int y = 0; y < g_qr_size; y++) {
                for (int x = 0; x < g_qr_size; x++) {
                    if (qrcodegen_getModule(g_qr, x, y)) {
                        RECT cell = { ox + x * scale, oy + y * scale,
                                      ox + (x + 1) * scale, oy + (y + 1) * scale };
                        FillRect(dc, &cell, (HBRUSH)GetStockObject(BLACK_BRUSH));
                    }
                }
            }
        }
        EndPaint(w, &ps);
        return 0;
    }
    case WM_DESTROY:
        g_qr_wnd = NULL;
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static void on_show_qr(void)
{
    if (!g_sync.privkey[0]) {
        MessageBoxA(g_hwnd, "No key available.", "p99lfg", MB_ICONINFORMATION);
        return;
    }
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(g_sync.privkey, tmp, g_qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true)) {
        MessageBoxA(g_hwnd, "Could not encode key as QR.", "p99lfg", MB_ICONINFORMATION);
        return;
    }
    g_qr_size = qrcodegen_getSize(g_qr);
    g_qr_ready = 1;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = qr_wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = "p99lfg_qr";
    RegisterClassA(&wc);

    if (g_qr_wnd) {
        InvalidateRect(g_qr_wnd, NULL, TRUE);
        SetForegroundWindow(g_qr_wnd);
        return;
    }
    int px = g_qr_size * 6;
    g_qr_wnd = CreateWindowExA(WS_EX_TOPMOST, "p99lfg_qr",
                               "p99lfg key - scan with the iOS app",
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, px + 40, px + 70,
                               g_hwnd, NULL, NULL, NULL);
    ShowWindow(g_qr_wnd, SW_SHOW);
    UpdateWindow(g_qr_wnd);
}

static BOOL list_pick_row(POINT pt)
{
    LRESULT idx = SendMessageA(g_list, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
    if (HIWORD(idx) != 0)
        return FALSE;
    SendMessageA(g_list, LB_SETCURSEL, LOWORD(idx), 0);
    return TRUE;
}

static void on_clear_list(void)
{
    /* wipe non-favourite entries (manual + log-scan + expired remotes);
     * remote players come back on their next event */
    lfg_remove_manual_all(g_state);
    /* also purge log-derived entries by clearing the in-memory list */
    for (int i = lfg_count(g_state) - 1; i >= 0; i--) {
        const LfgEntry *e = lfg_get(g_state, i);
        if (g_favs && fav_has(g_favs, e->name))
            continue;
        char namebuf[64];
        snprintf(namebuf, sizeof(namebuf), "%s", e->name);
        lfg_remove(g_state, namebuf);
    }
    populate_list();
}

/* ---- history window: known players not seen in >= 1 hour ---- */
#define HISTORY_STALE_SECS 3600
static HWND g_hist_wnd, g_hist_list;
static char g_knownpath[LFG_PATH_MAX];

static void known_save(void)
{
    if (!g_knownpath[0])
        return;
    FILE *f = fopen(g_knownpath, "w");
    if (!f)
        return;
    for (int i = 0; i < g_npresence; i++) {
        if (!g_presence[i].pubkey[0])
            continue;
        fprintf(f, "%s|%s|%llu|%s|%d|%s\n",
                 g_presence[i].name, g_presence[i].pubkey,
                 (unsigned long long)g_presence[i].last_seen,
                 g_presence[i].cls, g_presence[i].level,
                 g_presence[i].user);
    }
    fclose(f);
}

static void known_load(void)
{
    if (!g_knownpath[0])
        return;
    FILE *f = fopen(g_knownpath, "r");
    if (!f)
        return;
    char line[600];
    while (fgets(line, sizeof(line), f) && g_npresence < PRESENCE_MAX) {
        char *nl = strpbrk(line, "\r\n");
        if (nl)
            *nl = '\0';
        PresenceEntry *p = &g_presence[g_npresence];
        char lvl[16] = "0";
        if (sscanf(line, "%63[^|]|%64[^|]|%llu|%31[^|]|%15[^|]|%63[^\n]",
                   p->name, p->pubkey,
                   (unsigned long long *)&p->last_seen,
                   p->cls, lvl, p->user) >= 3) {
            p->level = atoi(lvl);
            g_npresence++;
        }
    }
    fclose(f);
}

static void history_populate(void)
{
    if (!g_hist_list)
        return;
    SendMessageA(g_hist_list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_npresence; i++) {
        if (!g_presence[i].pubkey[0] || g_presence[i].last_seen == 0)
            continue;
        uint64_t age = (uint64_t)time(NULL) - g_presence[i].last_seen;
        if (age < HISTORY_STALE_SECS)
            continue;
        char row[256];
        char when[32];
        if (age < 86400)
            snprintf(when, sizeof(when), "%lluh", (unsigned long long)(age / 3600));
        else
            snprintf(when, sizeof(when), "%llud", (unsigned long long)(age / 86400));
        snprintf(row, sizeof(row), "%s  [%s]  seen %s ago",
                 g_presence[i].name,
                 g_presence[i].cls[0] ? g_presence[i].cls : "?",
                 when);
        SendMessageA(g_hist_list, LB_ADDSTRING, 0, (LPARAM)row);
    }
}

static LRESULT CALLBACK hist_wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp);

static void on_history_window(void)
{
    if (g_hist_wnd) {
        history_populate();
        ShowWindow(g_hist_wnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_hist_wnd);
        return;
    }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = hist_wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "p99lfg_hist";
    RegisterClassA(&wc);
    RECT pr;
    GetWindowRect(g_hwnd, &pr);
    g_hist_wnd = CreateWindowExA(0, "p99lfg_hist",
                                 "History - players offline 1h+",
                                 WS_OVERLAPPEDWINDOW,
                                 pr.right - 380, pr.top + 60, 360, 400,
                                 g_hwnd, NULL, NULL, NULL);
    g_hist_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                 10, 10, 320, 340, g_hist_wnd, (HMENU)252, NULL, NULL);
    HFONT hf = (HFONT)SendMessageA(g_hwnd, WM_GETFONT, 0, 0);
    SendMessageA(g_hist_list, WM_SETFONT, (WPARAM)hf, TRUE);
    ShowWindow(g_hist_wnd, SW_SHOW);
    history_populate();
}

static LRESULT CALLBACK hist_wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CONTEXTMENU && (HWND)wp == g_hist_list) {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        LRESULT idx = SendMessageA(g_hist_list, LB_GETCURSEL, 0, 0);
        if (idx == LB_ERR) {
            /* pick row under cursor */
            POINT client_pt = pt;
            ScreenToClient(g_hist_list, &client_pt);
            LRESULT pick = SendMessageA(g_hist_list, LB_ITEMFROMPOINT, 0,
                                        MAKELPARAM(client_pt.x, client_pt.y));
            if (HIWORD(pick) != 0)
                return 0;
            idx = LOWORD(pick);
            SendMessageA(g_hist_list, LB_SETCURSEL, (WPARAM)idx, 0);
        }
        char buf[256] = {0};
        SendMessageA(g_hist_list, LB_GETTEXT, (WPARAM)idx, (LPARAM)buf);
        HMENU menu = CreatePopupMenu();
        AppendMenuA(menu, MF_STRING, ID_BTN_FAV, "Toggle Favourite");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, w, NULL);
        DestroyMenu(menu);
        /* remember which history row for the command handler */
        static char hist_sel_name[64];
        char *name = buf;
        char *sp = strchr(name, ' ');
        if (sp) {
            size_t n = (size_t)(sp - name);
            if (n < sizeof(hist_sel_name)) {
                memcpy(hist_sel_name, name, n);
                hist_sel_name[n] = '\0';
                SetWindowLongPtrA(w, GWLP_USERDATA, (LONG_PTR)hist_sel_name);
            }
        }
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_BTN_FAV) {
        const char *name = (const char *)GetWindowLongPtrA(w, GWLP_USERDATA);
        if (name && name[0])
            fav_toggle(g_favs, name);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static LRESULT CALLBACK name_wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        HWND ed = (HWND)GetWindowLongPtrA(w, GWLP_USERDATA);
        char nm[64];
        GetWindowTextA(ed, nm, sizeof(nm));
        snprintf(g_settings.displayname, sizeof(g_settings.displayname), "%s", nm);
        settings_save(&g_settings, g_inipath);
        snprintf(g_sync.user_name, sizeof(g_sync.user_name), "%s", nm);
        profile_announce(&g_sync, nm);
        DestroyWindow(w);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static void on_set_name(void)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = name_wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "p99lfg_name";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, "p99lfg_name", "Enter your display name",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             300, 300, 320, 130, g_hwnd, NULL, NULL, NULL);
    CreateWindowA("STATIC", "Name (shown to other players):",
                  WS_CHILD | WS_VISIBLE, 10, 12, 280, 18, w, NULL, NULL, NULL);
    HWND ed = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_settings.displayname,
                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                  10, 34, 290, 22, w, (HMENU)250, NULL, NULL);
    CreateWindowA("BUTTON", "Save",
                  WS_CHILD | WS_VISIBLE, 120, 64, 80, 24,
                  w, (HMENU)IDOK, NULL, NULL);
    HFONT hf = (HFONT)SendMessageA(g_hwnd, WM_GETFONT, 0, 0);
    HWND c = GetWindow(w, GW_CHILD);
    while (c) {
        SendMessageA(c, WM_SETFONT, (WPARAM)hf, TRUE);
        c = GetWindow(c, GW_HWNDNEXT);
    }
    SetWindowLongPtrA(w, GWLP_USERDATA, (LONG_PTR)ed);
    ShowWindow(w, SW_SHOW);
    SetFocus(ed);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        create_controls(hwnd, (HFONT)cs->lpCreateParams);
        g_timer = SetTimer(hwnd, 1, 2000, NULL);
        refresh_dir_label(hwnd);
        populate_chars();
        do_poll();
        return 0;
    }
    case WM_TIMER:
        do_poll();
        return 0;
    case WM_REMOTE:
        populate_list();
        return 0;
    case WM_DMARRIVED:
        chat_refresh();
        return 0;
    case WM_GROUPREFRESH:
        /* relay traffic updated the group state/log (sync thread posts
         * this): repaint the group window if open, and the roster.
         * group_refresh/group_roster_refresh no-op on NULL handles. */
        group_refresh();
        group_roster_refresh();
        return 0;
    case WM_MICUPDATE: {
        /* audio thread posted a new mic level: paint the indicator.
         * off    = capture not running or PTT not held
         * [####  ] = transmitting, bar length = loudness
         * LIVE turns the label red (see WM_CTLCOLORSTATIC) so a human
         * can see the mic state from across the room. */
        if (g_voice_indicator) {
            char lbl[64];
            int live;
            if (!g_voice_on) {
                snprintf(lbl, sizeof(lbl), "MIC: off");
                live = 0;
            } else if (!g_settings.voice_ptt) {
                snprintf(lbl, sizeof(lbl), "MIC: open  [%-*.*s]",
                          g_mic_level / 5, g_mic_level / 5,
                          "####################");
                live = 1;
            } else if (!g_ptt_down) {
                snprintf(lbl, sizeof(lbl), "MIC: push %s",
                         g_settings.voice_ptt_mouse ? "mouse4"
                         : "your key");
                live = 0;
            } else {
                snprintf(lbl, sizeof(lbl), "MIC: LIVE  [%-*.*s]",
                          g_mic_level / 5, g_mic_level / 5,
                          "####################");
                live = 1;
            }
            SetWindowTextA(g_voice_indicator, lbl);
            if (live != g_mic_live) {
                g_mic_live = live;
                InvalidateRect(g_voice_indicator, NULL, TRUE);
            }
        }
        return 0;
    }
    case WM_INVITEPOPUP: {
        /* sync thread staged a popup (invite or new-DM notice); show it
         * here on the UI thread so relays never stall on a MessageBox.
         * The invite flag is sticky - a DM burst in the same poll batch
         * must never overwrite a pending invite. */
        if (g_pending_invite) {
            g_pending_invite = 0;
            char invmsg[160];
            snprintf(invmsg, sizeof(invmsg),
                     "%s invited you to a group.\nJoin now?",
                     g_pending_invite_by[0] ? g_pending_invite_by : "Someone");
            if (MessageBoxA(hwnd, invmsg, "p99lfg group invite",
                            MB_ICONQUESTION | MB_YESNO) == IDYES) {
                /* accept: open the group window, send the join control so
                 * the host adds us and republishes state, and register
                 * self locally so the roster is not empty while the
                 * consolidated state arrives. */
                on_group_window();
                if (g_group_channel)
                    SetWindowTextA(g_group_channel, g_pending_invite_channel);
                {
                    char myname[64] = "Player";
                    if (g_chars && g_chars->count > 0)
                        snprintf(myname, sizeof(myname), "%s",
                                 g_chars->chars[0].name);
                    group_lock();
                    int already = 0;
                    for (int i = 0; i < g_group_nmembers; i++)
                        if (strcmp(g_group_members[i].pubkey, g_sync.pubkey) == 0)
                            already = 1;
                    if (!already && g_group_nmembers < GROUP_MEMBERS_MAX) {
                        snprintf(g_group_members[g_group_nmembers].name,
                                 sizeof(g_group_members[0].name), "%s", myname);
                        snprintf(g_group_members[g_group_nmembers].pubkey,
                                 sizeof(g_group_members[0].pubkey), "%s",
                                 g_sync.pubkey);
                        g_group_nmembers++;
                    }
                    group_unlock();
                    group_control_send(&g_sync, g_pending_invite_channel,
                                       myname, g_sync.pubkey, 0);
                    group_log_append("--- joined group ---\r\n");
                }
                group_roster_refresh();
                group_refresh();
            } else {
                /* decline: leave the channel state as it was pre-invite */
                g_group_name[0] = '\0';
                g_group_nmembers = 0;
                g_group_nwait = 0;
                g_group_is_leader = 0;
            }
        } else if (g_pending_popup[0]) {
            MessageBoxA(hwnd, g_pending_popup, "p99lfg", MB_ICONINFORMATION);
            g_pending_popup[0] = '\0';
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        if ((HWND)wp != g_list)
            break;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt.x == -1 && pt.y == -1) {
            /* menu key: anchor on the current selection */
            RECT rc;
            GetWindowRect(g_list, &rc);
            pt.x = rc.left + 40;
            pt.y = rc.top + 20;
            LRESULT cur = SendMessageA(g_list, LB_GETCURSEL, 0, 0);
            if (cur == LB_ERR)
                break;
        } else {
            POINT client_pt = pt;
            ScreenToClient(g_list, &client_pt);
            if (!list_pick_row(client_pt))
                break;
        }
        HMENU menu = CreatePopupMenu();
        AppendMenuA(menu, MF_STRING, ID_BTN_CHAT, "Chat...");
        AppendMenuA(menu, MF_STRING, ID_BTN_FAV, "Toggle Favourite");
        AppendMenuA(menu, MF_STRING, ID_BTN_RATE, "Rate...");
        if (group_can_invite()) {
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(menu, MF_STRING, ID_INV_MEMBER, "Invite to my group");
            AppendMenuA(menu, MF_STRING, ID_INV_WAIT, "Invite to waitlist");
        }
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, MF_STRING, ID_BTN_REMOVE, "Remove from list");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(menu);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_REFRESH: do_poll(); return 0;
        case ID_BTN_DIR: on_pick_dir(hwnd); return 0;
        case ID_BTN_ADD: on_add(); return 0;
        case ID_BTN_REMOVE: on_remove(); return 0;
        case ID_INV_MEMBER:
        case ID_INV_WAIT: {
            LRESULT isel = SendMessageA(g_list, LB_GETCURSEL, 0, 0);
            if (isel != LB_ERR) {
                char ibuf[256] = {0};
                SendMessageA(g_list, LB_GETTEXT, (WPARAM)isel, (LPARAM)ibuf);
                char *iname = ibuf;
                if (strncmp(iname, "* ", 2) == 0)
                    iname += 2;
                while (*iname == '[') {
                    char *close = strchr(iname, ']');
                    if (!close) { iname = NULL; break; }
                    iname = close + 1;
                    if (*iname == ' ')
                        iname++;
                }
                if (iname) {
                    char *sp = strchr(iname, ' ');
                    if (sp)
                        *sp = '\0';
                    on_invite(iname, LOWORD(wp) == ID_INV_WAIT);
                }
            }
            return 0;
        }
        case ID_BTN_FAV: on_fav_toggle(); return 0;
        case ID_BTN_RATE: on_rate(); return 0;
        case ID_BTN_CHAT: on_chat(); return 0;
        case ID_GROUP_TAB: on_group_window(); return 0;
        case ID_BTN_SETNAME: on_set_name(); return 0;
        case ID_BTN_CLEAR: on_clear_list(); return 0;
        case ID_BTN_HISTORY: on_history_window(); return 0;
        case ID_BTN_QR: on_show_qr(); return 0;
        case ID_BTN_CHAR_ADD: on_char_add(); return 0;
        case ID_BTN_CHAR_DEL: on_char_del(); return 0;
        case ID_BTN_CHAR_LFG: on_char_lfg_toggle(); return 0;
        }
        break;
    case WM_DESTROY:
        g_sync_thread_run = 0;
        if (g_sync_thread)
            WaitForSingleObject(g_sync_thread, 6000);
        {
            extern void known_save(void);
            known_save();
        }
        KillTimer(hwnd, g_timer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int looks_like_p99_dir(const char *dir)
{
    char probe[LFG_PATH_MAX];
    snprintf(probe, sizeof(probe), "%s\\license.txt", dir);
    HANDLE h = CreateFileA(probe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    char buf[4096];
    DWORD readn = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &readn, NULL);
    CloseHandle(h);
    buf[readn] = '\0';
    return strstr(buf, "Project 1999") != NULL;
}

static void guess_default_dir(char *out, size_t outsz)
{
    char eqdir[LFG_PATH_MAX];
    const char *cands[] = {
        "C:\\Program Files (x86)\\Sony\\EverQuest",
        "C:\\Program Files\\Sony\\EverQuest",
        "C:\\Program Files (x86)\\EverQuest",
        "C:\\EverQuest",
    };
    const char *pf86 = getenv("LOCALAPPDATA");
    char vs_cands[4][LFG_PATH_MAX] = {{0}};
    int nvs = 0;
    if (pf86) {
        for (int i = 0; i < 4; i++)
            snprintf(vs_cands[nvs++], LFG_PATH_MAX,
                     "%s\\VirtualStore\\%s", pf86, cands[i] + 3);
    }
    for (int i = 0; i < 4; i++) {
        if (looks_like_p99_dir(cands[i])) {
            snprintf(out, outsz, "%s", cands[i]);
            return;
        }
    }
    for (int i = 0; i < nvs; i++) {
        if (looks_like_p99_dir(vs_cands[i])) {
            snprintf(out, outsz, "%s", vs_cands[i]);
            return;
        }
    }
    /* fall back: first dir containing eqlog_*.txt */
    for (int i = 0; i < 4; i++) {
        char probe[LFG_PATH_MAX];
        snprintf(probe, sizeof(probe), "%s\\eqlog_*", cands[i]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileExA(probe, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            snprintf(out, outsz, "%s", cands[i]);
            return;
        }
    }
    for (int i = 0; i < nvs; i++) {
        char probe[LFG_PATH_MAX];
        snprintf(probe, sizeof(probe), "%s\\eqlog_*", vs_cands[i]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileExA(probe, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            snprintf(out, outsz, "%s", vs_cands[i]);
            return;
        }
    }
    snprintf(out, outsz, "%s", "C:\\Program Files (x86)\\Sony\\EverQuest");
}


int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    (void)hPrev; (void)cmd;
    log_open();
    log_printf("BUILD 2026-10-13-polish  (if this line is missing you are on an old exe)\n");
    {
        char dbgpath[LFG_PATH_MAX];
        GetModuleFileNameA(NULL, dbgpath, sizeof(dbgpath));
        char *dsl = strrchr(dbgpath, '\\');
        if (dsl)
            strcpy(dsl + 1, "p99lfg.debug");
        g_debug = GetFileAttributesA(dbgpath) != INVALID_FILE_ATTRIBUTES;
    }
    log_printf("p99lfg starting (debug=%d)\n", g_debug);
    SetUnhandledExceptionFilter(crash_handler);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        log_printf("WSAStartup FAILED (err=%d)\n", WSAGetLastError());
    lfg_set_debug(dbg_line);
    {
        extern void (*tls_debug_hook)(const char *);
        extern void (*ws_debug_hook)(const char *);
        tls_debug_hook = dbg_line;
        ws_debug_hook = dbg_line;
    }
    InitCommonControls();
    g_state = lfg_new();
    voice_init(&g_voice); /* sock must start at -1: 0 is a VALID socket
                            handle on Windows and left the voice toggle
                            thinking it was already open without binding
                            ("presence publish failed" on every click) */
    char dir[LFG_PATH_MAX];
    {
        char inipath[LFG_PATH_MAX];
        GetModuleFileNameA(NULL, inipath, sizeof(inipath));
        char *isl = strrchr(inipath, '\\');
        if (isl)
            strcpy(isl + 1, "p99lfg.ini");
        snprintf(g_inipath, sizeof(g_inipath), "%s", inipath);
        settings_load(&g_settings, inipath);
        char knownpath[LFG_PATH_MAX];
        GetModuleFileNameA(NULL, knownpath, sizeof(knownpath));
        char *ksl = strrchr(knownpath, '\\');
        if (ksl)
            strcpy(ksl + 1, "p99lfg.known");
        snprintf(g_knownpath, sizeof(g_knownpath), "%s", knownpath);
    }
    {
        extern void known_load(void);
        known_load();
    }
    guess_default_dir(dir, sizeof(dir));
    if (g_settings.logdir[0])
        snprintf(dir, sizeof(dir), "%s", g_settings.logdir);
    log_printf("log dir guess: %s\n", dir);
    lfg_set_dir(g_state, dir);

    /* Nostr identity lives next to the exe: p99lfg.key */
    char keypath[LFG_PATH_MAX];
    GetModuleFileNameA(NULL, keypath, sizeof(keypath));
    char *slash = strrchr(keypath, '\\');
    if (slash)
        strcpy(slash + 1, "p99lfg.key");
    else
        strcat(keypath, ".key");
    log_printf("key path: %s\n", keypath);
    sync_init(&g_sync, keypath, g_settings.server[0] ? g_settings.server : "green",
              on_remote_event, NULL);
    snprintf(g_sync.user_name, sizeof(g_sync.user_name), "%s",
             g_settings.displayname);
    profile_announce(&g_sync, g_settings.displayname);
    log_printf("sync init done, pubkey=%s\n", g_sync.pubkey);
    sync_set_dm_callback(&g_sync, on_dm, NULL);
    sync_set_generic_callback(&g_sync, on_generic, NULL);
    for (int i = 0; i < (int)(sizeof(DEFAULT_RELAYS)/sizeof(DEFAULT_RELAYS[0])); i++)
        sync_add_relay(&g_sync, DEFAULT_RELAYS[i]);
    /* guild relay (guild's own website-hosted Nostr endpoint, if
     * configured in p99lfg.ini): same protocol, one more hop - guild
     * channels then pass through the guild site like any other. */
    if (g_settings.guild_relay[0])
        sync_add_relay(&g_sync, g_settings.guild_relay);
    /* guild channels (p99lfg.ini): auto-adopt the first one so the
     * group window opens straight into the guild's chat - same flow as
     * the manual semi-public Join box, but hands-free. Further guild
     * channels can be joined manually by typing the tag. */
    if (g_settings.nguild_channels > 0) {
        snprintf(g_group_name, sizeof(g_group_name), "%s",
                 g_settings.guild_channels[0]);
        g_group_is_leader = 0;
        g_group_nmembers = 0;
        g_group_nwait = 0;
        char note[128];
        snprintf(note, sizeof(note), "--- guild channel #%s ---\r\n",
                 g_settings.guild_channels[0]);
        group_log_append(note);
    }

    /* favourites + ratings stores next to the exe */
    char favpath[LFG_PATH_MAX];
    GetModuleFileNameA(NULL, favpath, sizeof(favpath));
    char *fsl = strrchr(favpath, '\\');
    if (fsl)
        strcpy(fsl + 1, "p99lfg.favs");
    char charpath[LFG_PATH_MAX];
    GetModuleFileNameA(NULL, charpath, sizeof(charpath));
    char *csl = strrchr(charpath, '\\');
    if (csl)
        strcpy(csl + 1, "p99lfg.chars");
    g_chars = chars_load(charpath);

    g_favs = fav_load(favpath);
    g_ratings = rating_store_new();
    strcpy(g_keypath, keypath);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "p99lfg";
    RegisterClassA(&wc);

    HFONT hf = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT hfb = CreateFontA(-15, 0, 0, 0, FW_BOLD, 0, 0, 0,
                           ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    /* section headers use the bold face; pass both via a global */
    g_font_bold = hfb;

    g_hwnd = CreateWindowExA(0, "p99lfg", "p99lfg  -  Project 1999 Group Finder",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 900, 640,
                             NULL, NULL, hInst, hf);
    if (!g_hwnd) {
        log_printf("window creation FAILED (err=%lu)\n", (unsigned long)GetLastError());
        if (g_log)
            fclose(g_log);
        return 1;
    }
    log_printf("window created\n");
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    log_printf("window shown, starting sync thread\n");

    g_sync_thread_run = 1;
    g_sync_thread = CreateThread(NULL, 0, sync_thread, NULL, 0, NULL);
    if (!g_sync_thread)
        log_printf("sync thread FAILED to start (err=%lu)\n", (unsigned long)GetLastError());

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    lfg_free(g_state);
    return (int)msg.wParam;
}
