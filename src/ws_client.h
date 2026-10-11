#ifndef P99LFG_WS_CLIENT_H
#define P99LFG_WS_CLIENT_H

#include <stddef.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ws_socket_t;
#define WS_INVALID_SOCKET INVALID_SOCKET
#else
typedef int ws_socket_t;
#define WS_INVALID_SOCKET (-1)
#endif

typedef struct {
    char host[128];
    char path[160];
    int port;
    int use_tls;
} WsUrl;

typedef struct TlsCtx TlsCtx;

typedef struct {
    ws_socket_t sock;
    TlsCtx *tls;
    int connected;
    unsigned char buf[8192];
    size_t len;
} WsConn;

/* Parse "wss://host:port/path" or "ws://host:port/path". Returns 0 on success. */
int ws_parse_url(const char *url, WsUrl *out);

/* Connect and perform the HTTP upgrade handshake. Returns 0 on success. */
int ws_connect(WsConn *c, const WsUrl *url);

/* Send a text message. Returns 0 on success. */
int ws_send_text(WsConn *c, const char *msg);

/* Read the next complete text message (blocks). Returns length, 0 on disconnect, -1 on error. */
int ws_recv_text(WsConn *c, char *out, size_t outsz);

void ws_close(WsConn *c);

#endif
