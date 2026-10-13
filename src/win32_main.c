/* p99lfg - minimal Win32 LFG companion for Project 1999.
 * Single-file GUI. Build with mingw-w64 (see build-windows.sh or CI).
 */
#include "core.h"
#include "sync.h"
#include "chat.h"
#include "chars.h"
#include "fav.h"
#include "rating.h"
#include "qrcodegen.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
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
static SyncState g_sync;
static int g_sync_thread_run;
static HANDLE g_sync_thread;
static FavList *g_favs;
static RatingStore *g_ratings;
static CharList *g_chars;
static char g_keypath[LFG_PATH_MAX];
static HWND g_char_list;

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

/* ---- last-seen presence (option 1: freshness from event timestamps) ---- */
#define PRESENCE_MAX 512
#define PRESENCE_ONLINE_SECS 120
#define PRESENCE_RECENT_SECS (30 * 60)

typedef struct {
    char name[64];
    char pubkey[NOSTR_PUBKEY_HEX];
    uint64_t last_seen;
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

static FILE *g_log;

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
    /* find the sender's display name from the roster */
    char sender_name[64] = "";
    for (int i = 0; i < g_npresence; i++)
        if (strcmp(g_presence[i].pubkey, sender_pubkey) == 0) {
            snprintf(sender_name, sizeof(sender_name), "%s", g_presence[i].name);
            break;
        }
    if (!sender_name[0])
        snprintf(sender_name, sizeof(sender_name), "%.12s", sender_pubkey);

    /* always append to the sender's own history */
    ChatPeer *peer = chat_peer_get(sender_pubkey, sender_name);
    strncat(peer->log, "Them: ", sizeof(peer->log) - strlen(peer->log) - 1);
    strncat(peer->log, plain, sizeof(peer->log) - strlen(peer->log) - 1);
    strncat(peer->log, "\r\n", sizeof(peer->log) - strlen(peer->log) - 1);

    if (strcmp(g_chat_with, sender_pubkey) != 0) {
        /* DM from someone we're not currently viewing: notify */
        char note[256];
        snprintf(note, sizeof(note),
                 "New message from %s (open Chat to read it).", sender_name);
        if (g_hwnd)
            MessageBoxA(g_hwnd, note, "p99lfg", MB_ICONINFORMATION);
    }
    PostMessageA(g_hwnd, WM_DMARRIVED, 0, 0);
}

static void on_generic(int kind, const char *pubkey, const char *dtag,
                       const char *content, uint64_t created_at, void *ud)
{
    (void)ud;
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
    while (g_sync_thread_run) {
        sync_poll(&g_sync);
        if (g_log && (round % 12 == 0)) {
            log_printf("sync poll round %d: ", round);
            for (int i = 0; i < g_sync.nrelays; i++)
                log_printf("[%s connected=%d] ", g_sync.relays[i].url, g_sync.relays[i].connected);
            log_printf("\n");
        }
        round++;
        for (int i = 0; i < 50 && g_sync_thread_run; i++)
            Sleep(100); /* ~5s between polls */
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

static void do_poll(void)
{
    lfg_poll(g_state);
    populate_list();
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
    g_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                             10, 10, 330, 250, hwnd, (HMENU)ID_LIST, NULL, NULL);

    /* ---- my characters panel (right) ---- */
    CreateWindowA("STATIC", "My Characters", WS_CHILD | WS_VISIBLE,
                 350, 10, 130, 18, hwnd, NULL, NULL, NULL);
    g_char_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                             350, 30, 140, 160, hwnd, (HMENU)ID_CHAR_LIST, NULL, NULL);
    CreateWindowA("STATIC", "Name:", WS_CHILD | WS_VISIBLE,
                 350, 198, 34, 16, hwnd, NULL, NULL, NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                 386, 195, 104, 20, hwnd, (HMENU)ID_CHAR_NAME, NULL, NULL);
    CreateWindowA("STATIC", "Class:", WS_CHILD | WS_VISIBLE,
                 350, 222, 34, 16, hwnd, NULL, NULL, NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                 386, 219, 104, 20, hwnd, (HMENU)ID_CHAR_CLASS, NULL, NULL);
    CreateWindowA("STATIC", "Level:", WS_CHILD | WS_VISIBLE,
                 350, 246, 34, 16, hwnd, NULL, NULL, NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                 386, 243, 40, 20, hwnd, (HMENU)ID_CHAR_LEVEL, NULL, NULL);
    CreateWindowA("BUTTON", "Add", WS_CHILD | WS_VISIBLE,
                 430, 242, 60, 22, hwnd, (HMENU)ID_BTN_CHAR_ADD, NULL, NULL);
    CreateWindowA("BUTTON", "Toggle LFG", WS_CHILD | WS_VISIBLE,
                 350, 270, 68, 22, hwnd, (HMENU)ID_BTN_CHAR_LFG, NULL, NULL);
    CreateWindowA("BUTTON", "Delete", WS_CHILD | WS_VISIBLE,
                 422, 270, 68, 22, hwnd, (HMENU)ID_BTN_CHAR_DEL, NULL, NULL);
    CreateWindowA("BUTTON", "Refresh",
                  WS_CHILD | WS_VISIBLE, 10, 280, 70, 24, hwnd, (HMENU)ID_BTN_REFRESH, NULL, NULL);
    CreateWindowA("BUTTON", "Change log folder...",
                  WS_CHILD | WS_VISIBLE, 90, 280, 130, 24, hwnd, (HMENU)ID_BTN_DIR, NULL, NULL);
    CreateWindowA("STATIC", "Name:", WS_CHILD | WS_VISIBLE,
                  10, 315, 40, 20, hwnd, NULL, NULL, NULL);
    g_ed_name = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                55, 312, 110, 22, hwnd, (HMENU)ID_ED_NAME, NULL, NULL);
    CreateWindowA("STATIC", "Zone:", WS_CHILD | WS_VISIBLE,
                  175, 315, 34, 20, hwnd, NULL, NULL, NULL);
    g_ed_zone = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                213, 312, 90, 22, hwnd, (HMENU)ID_ED_ZONE, NULL, NULL);
    CreateWindowA("BUTTON", "Mark LFG",
                  WS_CHILD | WS_VISIBLE, 310, 310, 80, 24, hwnd, (HMENU)ID_BTN_ADD, NULL, NULL);
    CreateWindowA("BUTTON", "Remove",
                  WS_CHILD | WS_VISIBLE, 395, 310, 75, 24, hwnd, (HMENU)ID_BTN_REMOVE, NULL, NULL);
    CreateWindowA("BUTTON", "Fav",
                  WS_CHILD | WS_VISIBLE, 10, 340, 45, 22, hwnd, (HMENU)ID_BTN_FAV, NULL, NULL);
    CreateWindowA("BUTTON", "Rate",
                  WS_CHILD | WS_VISIBLE, 60, 340, 45, 22, hwnd, (HMENU)ID_BTN_RATE, NULL, NULL);
    CreateWindowA("BUTTON", "Chat",
                  WS_CHILD | WS_VISIBLE, 110, 340, 45, 22, hwnd, (HMENU)ID_BTN_CHAT, NULL, NULL);
    CreateWindowA("BUTTON", "Show key QR",
                  WS_CHILD | WS_VISIBLE, 160, 340, 90, 22, hwnd, (HMENU)ID_BTN_QR, NULL, NULL);
    g_st_dir = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE,
                             10, 368, 480, 20, hwnd, (HMENU)ID_ST_DIR, NULL, NULL);

    HWND ctrl = GetWindow(hwnd, GW_CHILD);
    while (ctrl) {
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)hf, TRUE);
        ctrl = GetWindow(ctrl, GW_HWNDNEXT);
    }
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
    g_chat_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                 WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                 10, 10, 380, 220, dlg, (HMENU)ID_CHAT_LOG, NULL, NULL);
    g_chat_input = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                 10, 240, 300, 24, dlg, (HMENU)ID_CHAT_INPUT, NULL, NULL);
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
        case ID_BTN_FAV: on_fav_toggle(); return 0;
        case ID_BTN_RATE: on_rate(); return 0;
        case ID_BTN_CHAT: on_chat(); return 0;
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
    log_printf("p99lfg starting\n");
    SetUnhandledExceptionFilter(crash_handler);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        log_printf("WSAStartup FAILED (err=%d)\n", WSAGetLastError());
    lfg_set_debug(log_line);
    {
        extern void (*tls_debug_hook)(const char *);
        extern void (*ws_debug_hook)(const char *);
        tls_debug_hook = log_line;
        ws_debug_hook = log_line;
    }
    InitCommonControls();
    g_state = lfg_new();
    char dir[LFG_PATH_MAX];
    guess_default_dir(dir, sizeof(dir));
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
    sync_init(&g_sync, keypath, "green", on_remote_event, NULL);
    log_printf("sync init done, pubkey=%s\n", g_sync.pubkey);
    sync_set_dm_callback(&g_sync, on_dm, NULL);
    sync_set_generic_callback(&g_sync, on_generic, NULL);
    for (int i = 0; i < (int)(sizeof(DEFAULT_RELAYS)/sizeof(DEFAULT_RELAYS[0])); i++)
        sync_add_relay(&g_sync, DEFAULT_RELAYS[i]);

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

    HFONT hf = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, "Tahoma");

    g_hwnd = CreateWindowExA(0, "p99lfg", "p99lfg - Project 1999 LFG",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 510, 445,
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
