/* p99lfg - minimal Win32 LFG companion for Project 1999.
 * Single-file GUI. Build with mingw-w64 (see build-windows.sh or CI).
 */
#include "core.h"
#include "sync.h"
#include "chat.h"
#include "fav.h"
#include "win32_qr.h"

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
#define ID_BTN_KEY_COPY 110
#define ID_BTN_KEY_PASTE 111
#define ID_BTN_KEY_QR 112
#define ID_BTN_FAV 113
#define ID_BTN_CHAT 114

static LfgState *g_state;
static HWND g_hwnd, g_list, g_ed_name, g_ed_zone, g_st_dir;
static UINT g_timer;
static SyncState g_sync;
static int g_sync_thread_run;
static HANDLE g_sync_thread;

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

static FavList *g_favs;
/* name -> pubkey of recently seen LFG players, for chat addressing */
#define NAME_PUB_MAX 256
static char g_np_names[NAME_PUB_MAX][LFG_NAME_MAX];
static char g_np_pubs[NAME_PUB_MAX][NOSTR_PUBKEY_HEX];
static int g_np_count;

static void name_pub_record(const char *name, const char *pubkey)
{
    for (int i = 0; i < g_np_count; i++) {
        if (strcmp(g_np_names[i], name) == 0) {
            snprintf(g_np_pubs[i], sizeof(g_np_pubs[i]), "%s", pubkey);
            return;
        }
    }
    if (g_np_count < NAME_PUB_MAX) {
        snprintf(g_np_names[g_np_count], LFG_NAME_MAX, "%s", name);
        snprintf(g_np_pubs[g_np_count], sizeof(g_np_pubs[0]), "%s", pubkey);
        g_np_count++;
    }
}

const char *name_pub_lookup(const char *name)
{
    for (int i = 0; i < g_np_count; i++)
        if (strcmp(g_np_names[i], name) == 0)
            return g_np_pubs[i];
    return NULL;
}

/* ---- chat window ---- */
static HWND g_chat_hwnd, g_chat_list, g_chat_edit_in, g_chat_edit_out, g_chat_peer_lbl;
static char g_chat_peer_name[LFG_NAME_MAX];
#define WM_DM_RECEIVED (WM_USER + 3)
#define ID_CHAT_LIST 200
#define ID_CHAT_SEND 201
#define ID_CHAT_IN 202
#define ID_CHAT_PEER 203

static void chat_add_line(const char *fmt, ...)
{
    if (!g_chat_list)
        return;
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SendMessageA(g_chat_list, LB_ADDSTRING, 0, (LPARAM)buf);
    int n = (int)SendMessageA(g_chat_list, LB_GETCOUNT, 0, 0);
    SendMessageA(g_chat_list, LB_SETCURSEL, n - 1, 0);
}

static void on_dm_received(const char *sender, const char *content, void *ud)
{
    (void)ud;
    char plain[600];
    if (chat_try_decrypt(g_sync.privkey, sender, content, plain, sizeof(plain)) != 0)
        return;
    const char *name = sender;
    for (int i = 0; i < g_np_count; i++)
        if (strcmp(g_np_pubs[i], sender) == 0)
            name = g_np_names[i];
    log_printf("DM from %s: %s\n", name, plain);
    if (!g_chat_hwnd)
        return;
    chat_add_line("[%s] %s", name, plain);
}

static LRESULT CALLBACK ChatWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        HFONT hf = (HFONT)cs->lpCreateParams;
        g_chat_peer_lbl = CreateWindowA("STATIC", "Peer:", WS_CHILD | WS_VISIBLE,
                                        8, 8, 470, 18, hwnd, (HMENU)ID_CHAT_PEER, NULL, NULL);
        g_chat_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                      8, 30, 470, 300, hwnd, (HMENU)ID_CHAT_LIST, NULL, NULL);
        g_chat_edit_in = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        8, 340, 380, 22, hwnd, (HMENU)ID_CHAT_IN, NULL, NULL);
        CreateWindowA("BUTTON", "Send",
                      WS_CHILD | WS_VISIBLE, 395, 340, 80, 24, hwnd, (HMENU)ID_CHAT_SEND, NULL, NULL);
        HWND ctrl = GetWindow(hwnd, GW_CHILD);
        while (ctrl) {
            SendMessageA(ctrl, WM_SETFONT, (WPARAM)hf, TRUE);
            ctrl = GetWindow(ctrl, GW_HWNDNEXT);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_CHAT_SEND) {
            char text[512];
            GetWindowTextA(g_chat_edit_in, text, sizeof(text));
            if (!text[0])
                return 0;
            const char *pub = name_pub_lookup(g_chat_peer_name);
            if (pub) {
                chat_add_line("[you] %s", text);
                if (chat_send_message(&g_sync, pub, text) != 0)
                    chat_add_line("(send failed - check relays)");
            } else {
                chat_add_line("(no pubkey known for %s yet)", g_chat_peer_name);
            }
            SetWindowTextA(g_chat_edit_in, "");
            return 0;
        }
        break;
    case WM_DESTROY:
        g_chat_hwnd = NULL;
        g_chat_list = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void open_chat(const char *name)
{
    snprintf(g_chat_peer_name, sizeof(g_chat_peer_name), "%s", name);
    char title[LFG_NAME_MAX + 16];
    snprintf(title, sizeof(title), "Chat - %s", name);
    if (g_chat_hwnd) {
        SetWindowTextA(g_chat_hwnd, title);
        SetWindowTextA(g_chat_peer_lbl, title);
        ShowWindow(g_chat_hwnd, SW_SHOW);
        return;
    }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = ChatWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "p99lfg_chat";
    RegisterClassA(&wc);
    HFONT hf = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, "Tahoma");
    g_chat_hwnd = CreateWindowExA(0, "p99lfg_chat", title,
                                  WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 510, 430,
                                  g_hwnd, NULL, wc.hInstance, hf);
    ShowWindow(g_chat_hwnd, SW_SHOW);
    UpdateWindow(g_chat_hwnd);
}

static void upsert_remote(const SyncRemoteEntry *e)
{
    if (e->lfg)
        lfg_add_manual(g_state, e->name, e->zone, e->detail);
    else
        lfg_remove(g_state, e->name);
}

static void on_remote_event(const SyncRemoteEntry *entry, const char *pubkey, void *ud)
{
    (void)ud;
    if (!g_hwnd)
        return;
    if (pubkey && entry->lfg)
        name_pub_record(entry->name, pubkey);
    upsert_remote(entry);
    PostMessageA(g_hwnd, WM_REMOTE, 0, 0);
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
    for (int i = 0; i < lfg_count(g_state); i++) {
        const LfgEntry *e = lfg_get(g_state, i);
        char row[256];
        snprintf(row, sizeof(row), "%s%s  [%s]  %s%s",
                 (g_favs && fav_has(g_favs, e->name)) ? "* " : "",
                 e->name,
                 e->detail[0] ? e->detail : "?",
                 e->zone[0] ? e->zone : "-",
                 e->source == LFG_SRC_MANUAL ? "  (manual)" : "");
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

static void get_key_path(char *out, size_t outsz)
{
    GetModuleFileNameA(NULL, out, (DWORD)outsz);
    char *slash = strrchr(out, '\\');
    if (slash)
        strcpy(slash + 1, "p99lfg.key");
    else
        strcat(out, ".key");
}

static void get_fav_path(char *out, size_t outsz)
{
    get_key_path(out, outsz);
    char *slash = strrchr(out, '\\');
    if (slash)
        strcpy(slash + 1, "p99lfg_favs.txt");
    else
        strcat(out, ".favs");
}

/* Copy the Nostr secret key to the clipboard so it can be entered on a
 * phone — sharing the key shares the Nostr identity across devices. */
static void on_key_copy(void)
{
    char keypath[LFG_PATH_MAX];
    get_key_path(keypath, sizeof(keypath));
    FILE *f = fopen(keypath, "r");
    if (!f)
        return;
    char hex[NOSTR_PRIVKEY_HEX] = {0};
    int ok = fscanf(f, "%64s", hex) == 1;
    fclose(f);
    if (!ok)
        return;
    if (!OpenClipboard(g_hwnd))
        return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, strlen(hex) + 1);
    if (h) {
        char *p = GlobalLock(h);
        if (p) {
            strcpy(p, hex);
            GlobalUnlock(h);
            SetClipboardData(CF_TEXT, h);
        }
    }
    CloseClipboard();
    MessageBoxA(g_hwnd, "Secret key copied.\n\nPaste it into the iOS/Android app's\nImport key field to share your identity.",
                "p99lfg", MB_OK | MB_ICONINFORMATION);
}

/* ---- QR popup window ---- */
static const char *QR_WND_CLASS = "p99lfg_qr";

static LRESULT CALLBACK QrWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        int total = qr_total_px(6);
        qr_draw(hdc, 10, 10, 6);
        SetBkMode(hdc, TRANSPARENT);
        static const char hint[] = "Scan with the p99lfg phone app to import your identity.";
        TextOutA(hdc, 10, 10 + total + 4, hint, (int)strlen(hint));
        TextOutA(hdc, 10, 10 + total + 22,
                 qr_payload(), (int)strlen(qr_payload()));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void on_key_qr(HWND hwnd)
{
    char keypath[LFG_PATH_MAX];
    get_key_path(keypath, sizeof(keypath));
    FILE *f = fopen(keypath, "r");
    if (!f)
        return;
    char hex[NOSTR_PRIVKEY_HEX] = {0};
    int ok = fscanf(f, "%64s", hex) == 1;
    fclose(f);
    if (!ok || qr_encode_payload(hex) != 0) {
        MessageBoxA(hwnd, "Could not encode the key as a QR code.",
                    "p99lfg", MB_OK | MB_ICONERROR);
        return;
    }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = QrWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = QR_WND_CLASS;
    RegisterClassA(&wc);
    int total = qr_total_px(6);
    RECT rw = { 0, 0, total + 30, total + 90 };
    AdjustWindowRect(&rw, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    HWND q = CreateWindowExA(0, QR_WND_CLASS, "Your p99lfg key",
                             WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rw.right - rw.left, rw.bottom - rw.top,
                             hwnd, NULL, wc.hInstance, NULL);
    ShowWindow(q, SW_SHOW);
    UpdateWindow(q);
}

static void on_key_paste(HWND hwnd)
{
    if (!OpenClipboard(hwnd))
        return;
    HANDLE h = GetClipboardData(CF_TEXT);
    char hex[NOSTR_PRIVKEY_HEX] = {0};
    if (h) {
        const char *p = GlobalLock(h);
        if (p) {
            snprintf(hex, sizeof(hex), "%s", p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    char keypath[LFG_PATH_MAX];
    get_key_path(keypath, sizeof(keypath));
    char pubkey[NOSTR_PUBKEY_HEX];
    if (hex[0] && nostr_import_key(keypath, hex, pubkey) == 0) {
        snprintf(g_sync.pubkey, sizeof(g_sync.pubkey), "%s", pubkey);
        snprintf(g_sync.privkey, sizeof(g_sync.privkey), "%s", hex);
        MessageBoxA(hwnd, "Identity imported. Listings now publish under this key.",
                    "p99lfg", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxA(hwnd, "Clipboard does not contain a valid key (64 hex chars).",
                    "p99lfg", MB_OK | MB_ICONERROR);
    }
}

static void on_favourite(void)
{
    LRESULT sel = SendMessageA(g_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || !g_favs)
        return;
    const LfgEntry *e = lfg_get(g_state, (int)sel);
    if (!e)
        return;
    fav_toggle(g_favs, e->name);
    populate_list();
}

static void on_chat_selected(void)
{
    LRESULT sel = SendMessageA(g_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR)
        return;
    const LfgEntry *e = lfg_get(g_state, (int)sel);
    if (!e)
        return;
    open_chat(e->name);
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
    g_st_dir = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE,
                             10, 345, 460, 20, hwnd, (HMENU)ID_ST_DIR, NULL, NULL);
    CreateWindowA("BUTTON", "Copy key",
                  WS_CHILD | WS_VISIBLE, 10, 370, 75, 24, hwnd, (HMENU)ID_BTN_KEY_COPY, NULL, NULL);
    CreateWindowA("BUTTON", "Paste key",
                  WS_CHILD | WS_VISIBLE, 90, 370, 75, 24, hwnd, (HMENU)ID_BTN_KEY_PASTE, NULL, NULL);
    CreateWindowA("BUTTON", "Show key QR",
                  WS_CHILD | WS_VISIBLE, 170, 370, 95, 24, hwnd, (HMENU)ID_BTN_KEY_QR, NULL, NULL);
    CreateWindowA("BUTTON", "Favourite",
                  WS_CHILD | WS_VISIBLE, 270, 370, 80, 24, hwnd, (HMENU)ID_BTN_FAV, NULL, NULL);
    CreateWindowA("BUTTON", "Chat",
                  WS_CHILD | WS_VISIBLE, 355, 370, 60, 24, hwnd, (HMENU)ID_BTN_CHAT, NULL, NULL);

    HWND ctrl = GetWindow(hwnd, GW_CHILD);
    while (ctrl) {
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)hf, TRUE);
        ctrl = GetWindow(ctrl, GW_HWNDNEXT);
    }
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
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_REFRESH: do_poll(); return 0;
        case ID_BTN_DIR: on_pick_dir(hwnd); return 0;
        case ID_BTN_ADD: on_add(); return 0;
        case ID_BTN_REMOVE: on_remove(); return 0;
        case ID_BTN_KEY_COPY: on_key_copy(); return 0;
        case ID_BTN_KEY_PASTE: on_key_paste(hwnd); return 0;
        case ID_BTN_KEY_QR: on_key_qr(hwnd); return 0;
        case ID_BTN_FAV: on_favourite(); return 0;
        case ID_BTN_CHAT: on_chat_selected(); return 0;
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
    get_key_path(keypath, sizeof(keypath));
    log_printf("key path: %s\n", keypath);
    sync_init(&g_sync, keypath, "green", on_remote_event, NULL);
    log_printf("sync init done, pubkey=%s\n", g_sync.pubkey);
    sync_set_dm_callback(&g_sync, on_dm_received, NULL);
    {
        char fav_path[LFG_PATH_MAX];
        get_fav_path(fav_path, sizeof(fav_path));
        g_favs = fav_load(fav_path);
    }
    for (int i = 0; i < (int)(sizeof(DEFAULT_RELAYS)/sizeof(DEFAULT_RELAYS[0])); i++)
        sync_add_relay(&g_sync, DEFAULT_RELAYS[i]);

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
                             CW_USEDEFAULT, CW_USEDEFAULT, 500, 420,
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
