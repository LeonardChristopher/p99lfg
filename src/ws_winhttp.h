#ifndef P99LFG_WS_WINHTTP_H
#define P99LFG_WS_WINHTTP_H

/* Windows transport for wss:// relays using WinHTTP's built-in WebSocket
 * support (Win 8+). Microsoft's own TLS stack - no SSPI hand-coding, no
 * third-party dependencies. The ws_client uses this automatically for
 * wss:// URLs on Windows when built with -lwinhttp. */

#include "ws_client.h"

/* Returns 1 if the WinHTTP WebSocket transport is available (always on
 * Win 8+ when linked). */
int ws_winhttp_available(void);

/* WinHTTP-based implementations, swapped in for wss:// on Windows. */
int ws_winhttp_connect(WsConn *c, const WsUrl *url);
int ws_winhttp_send_text(WsConn *c, const char *msg);
int ws_winhttp_recv_text(WsConn *c, char *out, size_t outsz);
void ws_winhttp_close(WsConn *c);

#endif
