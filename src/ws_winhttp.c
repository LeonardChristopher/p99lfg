/* WinHTTP WebSocket transport for wss:// relays on Windows.
 * Uses Microsoft's built-in TLS + WebSocket (Win 8+), avoiding the
 * SSPI/SChannel header ABI minefield under MinGW. */
#include "ws_winhttp.h"

#ifdef _WIN32

#define _WINSOCKAPI_
#include <windows.h>
#include <winhttp.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

/* The WsConn struct holds a socket; we repurpose it to store the
 * HINTERNET handles via a small heap wrapper. */
typedef struct {
    HINTERNET session;
    HINTERNET connect;
    HINTERNET request;
    HINTERNET ws;
} WinHttpWs;

static WinHttpWs *wh_of(WsConn *c)
{
    return (WinHttpWs *)(uintptr_t)c->tls;
}

static void ws_dbg_safe(const char *fmt, ...)
{
    extern void (*ws_debug_hook)(const char *);
    if (!ws_debug_hook)
        return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ws_debug_hook(buf);
}

int ws_winhttp_available(void)
{
    return 1;
}

int ws_winhttp_connect(WsConn *c, const WsUrl *url)
{
    WinHttpWs *w = calloc(1, sizeof(WinHttpWs));
    if (!w)
        return -1;

    wchar_t whost[256];
    MultiByteToWideChar(CP_UTF8, 0, url->host, -1, whost, 256);

    w->session = WinHttpOpen(L"p99lfg/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!w->session)
        goto fail;
    {
        DWORD recv_timeout = 2000, send_timeout = 5000, conn_timeout = 5000;
        WinHttpSetTimeouts(w->session, conn_timeout, conn_timeout,
                           send_timeout, recv_timeout);
    }

    w->connect = WinHttpConnect(w->session, whost,
                                (INTERNET_PORT)url->port, 0);
    if (!w->connect)
        goto fail;

    /* request with websocket upgrade flag */
    wchar_t wpath[256];
    MultiByteToWideChar(CP_UTF8, 0, url->path, -1, wpath, 256);

    w->request = WinHttpOpenRequest(w->connect, L"GET", wpath,
                                    NULL, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    WINHTTP_FLAG_SECURE);
    if (!w->request)
        goto fail;

    /* must be set with NULL, 0 - any value pointer is ERROR_INVALID_PARAMETER */
    if (!WinHttpSetOption(w->request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
                          NULL, 0))
        goto fail;

    if (!WinHttpSendRequest(w->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        goto fail;

    if (!WinHttpReceiveResponse(w->request, NULL))
        goto fail;

    /* confirm the server upgraded */
    DWORD status_code = 0, size = sizeof(status_code);
    WinHttpQueryHeaders(w->request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status_code != 101) {
        ws_dbg_safe("winhttp: server did not upgrade (status=%lu)",
                    (unsigned long)status_code);
        goto fail;
    }

    /* complete the websocket */
    w->ws = WinHttpWebSocketCompleteUpgrade(w->request, 0);
    if (!w->ws)
        goto fail;
    /* the request handle is no longer needed */
    WinHttpCloseHandle(w->request);
    w->request = NULL;

    c->tls = (TlsCtx *)(uintptr_t)w; /* reuse the pointer slot */
    c->connected = 1;
    return 0;

fail:
    ws_dbg_safe("winhttp: connect to %s:%d failed (gle=%lu)",
                url->host, url->port, (unsigned long)GetLastError());
    if (w->ws)
        WinHttpCloseHandle(w->ws);
    if (w->request)
        WinHttpCloseHandle(w->request);
    if (w->connect)
        WinHttpCloseHandle(w->connect);
    if (w->session)
        WinHttpCloseHandle(w->session);
    free(w);
    return -1;
}

int ws_winhttp_send_text(WsConn *c, const char *msg)
{
    WinHttpWs *w = wh_of(c);
    if (!w || !w->ws)
        return -1;
    DWORD r = WinHttpWebSocketSend(w->ws,
                                   WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                   (PVOID)msg, (DWORD)strlen(msg));
    return r == NO_ERROR ? 0 : -1;
}

int ws_winhttp_recv_text(WsConn *c, char *out, size_t outsz)
{
    WinHttpWs *w = wh_of(c);
    if (!w || !w->ws)
        return -1;
    for (int attempt = 0; attempt < 20; attempt++) {
        DWORD readn = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD r = WinHttpWebSocketReceive(w->ws, out, (DWORD)outsz - 1, &readn, &type);
        if (r == NO_ERROR) {
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                return 0;
            out[readn] = '\0';
            return (int)readn;
        }
        DWORD gle = GetLastError();
        if (gle == ERROR_WINHTTP_TIMEOUT) {
            Sleep(250); /* relay hasn't answered yet; keep waiting */
            continue;
        }
        ws_dbg_safe("winhttp: recv failed gle=%lu", (unsigned long)gle);
        return -1;
    }
    /* waited ~5s with nothing: treat as end of stream for this poll */
    return -2;
}

void ws_winhttp_close(WsConn *c)
{
    WinHttpWs *w = wh_of(c);
    if (!w)
        return;
    if (w->ws)
        WinHttpWebSocketClose(w->ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
    if (w->request)
        WinHttpCloseHandle(w->request);
    if (w->connect)
        WinHttpCloseHandle(w->connect);
    if (w->session)
        WinHttpCloseHandle(w->session);
    free(w);
    c->tls = NULL;
    c->connected = 0;
}

#endif /* _WIN32 */
