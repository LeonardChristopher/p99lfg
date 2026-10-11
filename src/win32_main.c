/* p99lfg - minimal Win32 LFG companion for Project 1999.
 * Single-file GUI. Build with mingw-w64 (see build-windows.sh or CI).
 */
#include "core.h"
#include "sync.h"
#include "chat.h"
#include "fav.h"
#include "rating.h"
#include "qrcodegen.h"

#include <windows.h>
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
static char g_keypath[LFG_PATH_MAX];

/* last DM per sender pubkey: simple single-conversation chat window */
#define CHAT_LOG_MAX 4096
static char g_chat_with[NOSTR_PUBKEY_HEX];
static char g_chat_name[64];
static char g_chat_log[CHAT_LOG_MAX];
static HWND g_chat_wnd, g_chat_edit, g_chat_input;

#define WM_DMARRIVED (WM_USER + 3)

/* ---- last-seen presence (option 1: freshness from event timestamps) ---- */
#define PRESENCE_MAX 512
#define PRESENCE_ONLINE_SECS 120
#define PRESENCE_RECENT_SECS (30 * 60)

typedef struct {
    char name[64];
    uint64_t last_seen;
} PresenceEntry;

static PresenceEntry g_presence[PRESENCE_MAX];
static int g_npresence;

static void presence_touch(const char *name, uint64_t when)
{
    if (!name[0])
        return;
    for (int i = 0; i < g_npresence; i++) {
        if (strcmp(g_presence[i].name, name) == 0) {
            if (when > g_presence[i].last_seen)
                g_presence[i].last_seen = when;
            return;
        }
    }
    if (g_npresence >= PRESENCE_MAX)
        return;
    snprintf(g_presence[g_npresence].name, 64, "%s", name);
    g_presence[g_npresence].last_seen = when;
    g_npresence++;
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
    presence_touch(entry->name, entry->created_at ? entry->created_at
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
    /* if no conversation open, start one with this sender */
    if (!g_chat_with[0]) {
        snprintf(g_chat_with, sizeof(g_chat_with), "%s", sender_pubkey);
        snprintf(g_chat_name, sizeof(g_chat_name), "%.12s", sender_pubkey);
        const LfgEntry *e = NULL;
        for (int i = 0; i < lfg_count(g_state); i++) {
            e = lfg_get(g_state, i);
            break;
        }
        (void)e;
    }
    if (strcmp(g_chat_with, sender_pubkey) != 0)
        return; /* DM from someone else while chatting: ignore for now */
    strncat(g_chat_log, "Them: ", sizeof(g_chat_log) - strlen(g_chat_log) - 1);
    strncat(g_chat_log, plain, sizeof(g_chat_log) - strlen(g_chat_log) - 1);
    strncat(g_chat_log, "\r\n", sizeof(g_chat_log) - strlen(g_chat_log) - 1);
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
        if (g_log) {
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
    }
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

static void create_controls(HWND hwnd, HFONT hf)
{
    g_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                             10, 10, 460, 260, hwnd, (HMENU)ID_LIST, NULL, NULL);
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
                             10, 368, 460, 20, hwnd, (HMENU)ID_ST_DIR, NULL, NULL);

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
    if (g_chat_edit)
        SetWindowTextA(g_chat_edit, g_chat_log);
}

static LRESULT CALLBACK chat_wnd_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND && LOWORD(wp) == ID_CHAT_SEND && lp) {
        if (((HWND)lp) == GetDlgItem(dlg, ID_CHAT_SEND)) {
            char m[512];
            GetWindowTextA(g_chat_input, m, sizeof(m));
            if (m[0] && g_chat_with[0]) {
                if (chat_send_message(&g_sync, g_chat_with, m) == 0) {
                    strncat(g_chat_log, "You: ", sizeof(g_chat_log) - strlen(g_chat_log) - 1);
                    strncat(g_chat_log, m, sizeof(g_chat_log) - strlen(g_chat_log) - 1);
                    strncat(g_chat_log, "\r\n", sizeof(g_chat_log) - strlen(g_chat_log) - 1);
                    SetWindowTextA(g_chat_input, "");
                    chat_refresh();
                } else {
                    MessageBoxA(dlg, "Send failed (no relay reachable).", "p99lfg",
                                MB_ICONINFORMATION);
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
                              pr.left + 80, pr.top + 60, 420, 340,
                              g_hwnd, NULL, NULL, NULL);
    g_chat_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                 WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                 10, 10, 380, 220, dlg, (HMENU)ID_CHAT_LOG, NULL, NULL);
    g_chat_input = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                 10, 240, 300, 24, dlg, (HMENU)ID_CHAT_INPUT, NULL, NULL);
    CreateWindowA("BUTTON", "Send", WS_CHILD | WS_VISIBLE,
                 315, 238, 75, 26, dlg, (HMENU)ID_CHAT_SEND, NULL, NULL);
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
static void on_show_qr(void)
{
    if (!g_sync.privkey[0]) {
        MessageBoxA(g_hwnd, "No key available.", "p99lfg", MB_ICONINFORMATION);
        return;
    }
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(g_sync.privkey, tmp, qr, qrcodegen_Ecc_MEDIUM,
                               qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                               qrcodegen_Mask_AUTO, true)) {
        MessageBoxA(g_hwnd, "Could not encode key as QR.", "p99lfg", MB_ICONINFORMATION);
        return;
    }
    int size = qrcodegen_getSize(qr);
    int scale = 6, px = size * scale;
    HDC sdc = GetDC(NULL);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP bm = CreateCompatibleBitmap(sdc, px, px);
    SelectObject(mdc, bm);
    for (int y = 0; y < px; y++)
        for (int x = 0; x < px; x++) {
            BOOL dark = qrcodegen_getModule(qr, x / scale, y / scale);
            SetPixel(mdc, x, y, dark ? RGB(0, 0, 0) : RGB(255, 255, 255));
        }
    BITMAP bmpinfo;
    GetObjectA(bm, sizeof(bmpinfo), &bmpinfo);
    /* render into a simple always-on-top popup via clipboard-free preview window */
    HWND w = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "STATIC", NULL,
                             WS_POPUP | WS_VISIBLE,
                             100, 100, px + 20, px + 40, NULL, NULL, NULL, NULL);
    /* draw bitmap on paint */
    SendMessageA(w, WM_SETTEXT, 0, (LPARAM)"p99lfg identity - scan with the iOS app");
    HDC wdc = GetDC(w);
    HDC bdc = CreateCompatibleDC(wdc);
    SelectObject(bdc, bm);
    BitBlt(wdc, 10, 10, px, px, bdc, 0, 0, SRCCOPY);
    ReleaseDC(w, wdc);
    DeleteDC(bdc);
    DeleteObject(bm);
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
    (void)bmpinfo;
    MessageBoxA(g_hwnd, "QR is displayed in the popup window.\nClose the message box to dismiss it.",
                "p99lfg key QR", MB_OK);
    DestroyWindow(w);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        create_controls(hwnd, (HFONT)cs->lpCreateParams);
        g_timer = SetTimer(hwnd, 1, 2000, NULL);
        refresh_dir_label(hwnd);
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
    lfg_set_debug(log_line);
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
                             CW_USEDEFAULT, CW_USEDEFAULT, 500, 445,
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
