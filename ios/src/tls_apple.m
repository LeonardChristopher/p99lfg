/* Apple TLS hooks for ws_client using the system Security framework
 * (SecureTransport) over the raw socket fd — no OpenSSL shipping, matching
 * the SChannel approach on Windows. Compiled as Objective-C so we can use
 * SecTrustEvaluate for certificate validation. */
#include "tls_hooks.h"

#if defined(__APPLE__) && !defined(_WIN32)

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <sys/socket.h>
#import <unistd.h>

struct TlsCtx {
    int fd;
    SSLContextRef ssl;
    /* Decrypted record buffer */
    unsigned char decbuf[16384];
    size_t declen;
    size_t dekpos;
};

static int tls_connect_impl(const char *host, void *sockp, TlsCtx **out);
static int tls_send_impl(TlsCtx *t, const void *data, unsigned int len);
static int tls_recv_impl(TlsCtx *t, void *data, unsigned int len);
static void tls_close_impl(TlsCtx *t);

int (*tls_connect_hook)(const char *, void *, TlsCtx **) = tls_connect_impl;
int (*tls_send_hook)(TlsCtx *, const void *, unsigned int) = tls_send_impl;
int (*tls_recv_hook)(TlsCtx *, void *, unsigned int) = tls_recv_impl;
void (*tls_close_hook)(TlsCtx *) = tls_close_impl;

static OSStatus tcp_write(TlsCtx *t, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(t->fd, p, len);
        if (n <= 0)
            return errSSLClosedAbort;
        p += n;
        len -= (size_t)n;
    }
    return noErr;
}

/* TLS read callback: pull bytes off the raw socket. */
static OSStatus tls_socket_read(SSLConnectionRef conn, void *buf, size_t *len)
{
    TlsCtx *t = (TlsCtx *)conn;
    ssize_t n = read(t->fd, buf, *len);
    if (n > 0) {
        *len = (size_t)n;
        return noErr;
    }
    if (n == 0)
        return errSSLClosedGraceful;
    return errSSLClosedAbort;
}

/* TLS write callback: push bytes onto the raw socket. */
static OSStatus tls_socket_write(SSLConnectionRef conn, const void *buf, size_t *len)
{
    TlsCtx *t = (TlsCtx *)conn;
    OSStatus st = tcp_write(t, buf, *len);
    if (st != noErr)
        return st;
    return noErr;
}

static int tls_connect_impl(const char *host, void *sockp, TlsCtx **out)
{
    *out = NULL;
    TlsCtx *t = calloc(1, sizeof(TlsCtx));
    if (!t)
        return -1;
    t->fd = (int)(intptr_t)sockp;

    t->ssl = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
    if (!t->ssl)
        goto fail;

    SSLSetIoFuncs(t->ssl, tls_socket_read, tls_socket_write);
    SSLSetProtocolVersionMin(t->ssl, kTLSProtocol12);
    SSLSetSessionOption(t->ssl, kSSLSessionOptionBreakOnServerAuth, true);

    /* Connection ref is the fd; hostname for SNI + default trust eval. */
    SSLSetConnection(t->ssl, (SSLConnectionRef)(uintptr_t)t->fd);
    SSLSetPeerDomainName(t->ssl, host, strlen(host));

    OSStatus st;
    for (;;) {
        st = SSLHandshake(t->ssl);
        if (st == errSSLWouldBlock)
            continue;
        if (st == errSSLPeerAuthCompleted) {
            /* Evaluate the server certificate against system trust. */
            SecTrustRef trust = NULL;
            OSStatus tst = SSLCopyPeerTrust(t->ssl, &trust);
            if (tst != noErr || !trust) {
                if (trust)
                    CFRelease(trust);
                goto fail;
            }
            SecTrustResultType tres;
            tst = SecTrustEvaluate(trust, &tres);
            CFRelease(trust);
            if (tst != noErr ||
                (tres != kSecTrustResultUnspecified && tres != kSecTrustResultProceed))
                goto fail;
            continue;
        }
        break;
    }
    if (st != noErr)
        goto fail;

    *out = t;
    return 0;
fail:
    if (t->ssl)
        CFRelease(t->ssl);
    free(t);
    return -1;
}

static int tls_send_impl(TlsCtx *t, const void *data, unsigned int len)
{
    size_t sent = 0;
    OSStatus st;
    while (sent < len) {
        st = SSLWrite(t->ssl, (const uint8_t *)data + sent, len - sent, &sent);
        if (st != noErr)
            return -1;
    }
    return 0;
}

static int tls_recv_impl(TlsCtx *t, void *data, unsigned int len)
{
    /* Serve buffered plaintext first. */
    if (t->dekpos < t->declen) {
        size_t avail = t->declen - t->dekpos;
        size_t n = avail < len ? avail : (size_t)len;
        memcpy(data, t->decbuf + t->dekpos, n);
        t->dekpos += n;
        if (t->dekpos == t->declen) {
            t->dekpos = t->declen = 0;
        }
        return (int)n;
    }
    size_t got = 0;
    OSStatus st = SSLRead(t->ssl, data, len, &got);
    if (got > 0)
        return (int)got;
    if (st == errSSLClosedGraceful)
        return 0;
    return -1;
}

static void tls_close_impl(TlsCtx *t)
{
    if (t) {
        if (t->ssl) {
            SSLClose(t->ssl);
            CFRelease(t->ssl);
        }
        free(t);
    }
}

#endif /* __APPLE__ && !_WIN32 */
