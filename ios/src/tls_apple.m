/* Apple TLS hooks for ws_client using Network.framework (NWConnection),
 * the supported TLS API on iOS/macOS — no OpenSSL shipping, matching the
 * SChannel approach on Windows. The hooks are blocking; NWConnection is
 * async, so each operation is bridged with a semaphore. */
#include "tls_hooks.h"

#if defined(__APPLE__) && !defined(_WIN32)

#import <Foundation/Foundation.h>
#import <Network/Network.h>
#import <sys/socket.h>
#import <netinet/in.h>
#import <unistd.h>

struct TlsCtx {
    nw_connection_t conn;
    dispatch_queue_t queue;
    dispatch_semaphore_t sem;
    /* decrypted bytes buffered between receive callbacks */
    unsigned char buf[65536];
    size_t buflen;
    size_t bufpos;
    int eof;
};

static int tls_connect_impl(const char *host, void *sockp, TlsCtx **out);
static int tls_send_impl(TlsCtx *t, const void *data, unsigned int len);
static int tls_recv_impl(TlsCtx *t, void *data, unsigned int len);
static void tls_close_impl(TlsCtx *t);

int (*tls_connect_hook)(const char *, void *, TlsCtx **) = tls_connect_impl;
int (*tls_send_hook)(TlsCtx *, const void *, unsigned int) = tls_send_impl;
int (*tls_recv_hook)(TlsCtx *, void *, unsigned int) = tls_recv_impl;
void (*tls_close_hook)(TlsCtx *) = tls_close_impl;

/* Pull the peer port off the socket ws_client already connected, so we
 * mirror its destination exactly. */
static uint16_t peer_port(void *sockp)
{
    int fd = (int)(intptr_t)sockp;
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0)
        return 443;
    if (ss.ss_family == AF_INET)
        return ((struct sockaddr_in *)&ss)->sin_port;
    if (ss.ss_family == AF_INET6)
        return ((struct sockaddr_in6 *)&ss)->sin6_port;
    return htons(443);
}

static void on_state_change(nw_connection_t conn, nw_connection_state_t state, void *ctx)
{
    TlsCtx *t = (TlsCtx *)ctx;
    switch (state) {
    case nw_connection_state_ready:
    case nw_connection_state_failed:
    case nw_connection_state_cancelled:
        dispatch_semaphore_signal(t->sem);
        break;
    default:
        break;
    }
    (void)conn;
}

/* Start one receive; completion buffers the data and signals. */
static void pump_receive(TlsCtx *t)
{
    nw_connection_receive(t->conn, 1, sizeof(t->buf) - t->buflen,
                         ^(dispatch_data_t data, NWEndpoint_t endpoint,
                           bool is_complete, NSError *error) {
        (void)endpoint;
        if (error) {
            t->eof = 1;
        } else if (data && !is_complete) {
            const void *p = NULL;
            size_t n = 0;
            dispatch_data_t mapped = dispatch_data_create_map(data, &p, &n);
            if (mapped && p && t->buflen + n <= sizeof(t->buf)) {
                memcpy(t->buf + t->buflen, p, n);
                t->buflen += n;
            }
            if (mapped)
                nw_release(mapped);
        } else if (is_complete) {
            t->eof = 1;
        }
        dispatch_semaphore_signal(t->sem);
    });
}

static int tls_connect_impl(const char *host, void *sockp, TlsCtx **out)
{
    *out = NULL;
    TlsCtx *t = calloc(1, sizeof(TlsCtx));
    if (!t)
        return -1;
    t->sem = dispatch_semaphore_create(0);
    t->queue = dispatch_queue_create("p99lfg.tls", DISPATCH_QUEUE_SERIAL);

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)ntohs(peer_port(sockp)));
    nw_endpoint_t ep = nw_endpoint_create_host(host, portstr);
    if (!ep)
        goto fail;

    nw_parameters_t params = nw_parameters_create_secure_tcp(
        NW_PARAMETERS_DEFAULT_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
    if (!params) {
        nw_release(ep);
        goto fail;
    }
    /* enforce hostname validation (default policy already does) */
    t->conn = nw_connection_create(ep, params);
    nw_release(ep);
    nw_release(params);
    if (!t->conn)
        goto fail;

    nw_connection_set_state_changed_handler(t->conn, ^(nw_connection_state_t state, NSError *error) {
        on_state_change(t->conn, state, t);
    });
    nw_connection_set_queue(t->conn, t->queue);
    nw_connection_start(t->conn);

    long r = dispatch_semaphore_wait(t->sem, dispatch_time(DISPATCH_TIME_NOW, 15ULL * NSEC_PER_SEC));
    if (r != 0 || nw_connection_get_state(t->conn) != nw_connection_state_ready)
        goto fail;

    *out = t;
    return 0;
fail:
    if (t->conn) {
        nw_connection_cancel(t->conn);
        nw_release(t->conn);
    }
    if (t->queue)
        dispatch_release(t->queue);
    dispatch_release(t->sem);
    free(t);
    return -1;
}

static int tls_send_impl(TlsCtx *t, const void *data, unsigned int len)
{
    dispatch_data_t d = dispatch_data_create(data, len, t->queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if (!d)
        return -1;
    __block int ok = 0;
    nw_connection_send(t->conn, d, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true,
                       ^(NSError *error) {
        ok = error ? -1 : 0;
        dispatch_semaphore_signal(t->sem);
    });
    dispatch_release(d);
    long r = dispatch_semaphore_wait(t->sem, dispatch_time(DISPATCH_TIME_NOW, 15ULL * NSEC_PER_SEC));
    if (r != 0)
        return -1;
    return ok;
}

static int tls_recv_impl(TlsCtx *t, void *data, unsigned int len)
{
    /* serve buffered plaintext first */
    if (t->bufpos < t->buflen) {
        size_t avail = t->buflen - t->bufpos;
        size_t n = avail < len ? avail : (size_t)len;
        memcpy(data, t->buf + t->bufpos, n);
        t->bufpos += n;
        if (t->bufpos == t->buflen) {
            t->bufpos = t->buflen = 0;
        }
        return (int)n;
    }
    if (t->eof)
        return 0;
    pump_receive(t);
    long r = dispatch_semaphore_wait(t->sem, dispatch_time(DISPATCH_TIME_NOW, 15ULL * NSEC_PER_SEC));
    if (r != 0)
        return -1;
    if (t->bufpos < t->buflen) {
        size_t avail = t->buflen - t->bufpos;
        size_t n = avail < len ? avail : (size_t)len;
        memcpy(data, t->buf + t->bufpos, n);
        t->bufpos += n;
        if (t->bufpos == t->buflen) {
            t->bufpos = t->buflen = 0;
        }
        return (int)n;
    }
    return t->eof ? 0 : -1;
}

static void tls_close_impl(TlsCtx *t)
{
    if (t) {
        if (t->conn) {
            nw_connection_cancel(t->conn);
            nw_release(t->conn);
        }
        if (t->queue)
            dispatch_release(t->queue);
        dispatch_release(t->sem);
        free(t);
    }
}

#endif /* __APPLE__ && !_WIN32 */
