#ifndef P99LFG_TLS_H
#define P99LFG_TLS_H

/* TLS transport hooks used by ws_client. On Windows these are implemented
 * with SChannel (built into the OS, no OpenSSL shipping). On other platforms
 * they stay NULL and only ws:// is available. */

typedef struct TlsCtx TlsCtx;

extern int (*tls_connect_hook)(const char *host, void *sock, TlsCtx **out);
extern int (*tls_send_hook)(TlsCtx *t, const void *data, unsigned int len);
extern int (*tls_recv_hook)(TlsCtx *t, void *data, unsigned int len);
extern void (*tls_close_hook)(TlsCtx *t);

#endif
