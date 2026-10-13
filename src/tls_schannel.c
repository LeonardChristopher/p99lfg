/* SChannel TLS for wss:// on Windows. Uses only built-in system libraries
 * (schannel.dll / crypt32 / secur32), keeping the exe dependency-free. */
#include "tls_hooks.h"

#ifdef _WIN32

#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ntdef.h>
#include <sspi.h>
#include <schannel.h>
#include <security.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct TlsCtx {
    SOCKET sock;
    CredHandle hcred;
    CtxtHandle hctx;
    SecPkgContext_StreamSizes sizes;
    unsigned char *encbuf;
    unsigned char decbuf[16384];
    size_t declen;
    size_t dekpos;
    int established;
    int dead;
};

static int tcp_send_all(TlsCtx *t, const void *data, size_t len);
static int tls_send_impl(TlsCtx *t, const void *data, unsigned int len);
static int tls_recv_impl(TlsCtx *t, void *data, unsigned int len);
static void tls_close_impl(TlsCtx *t);

static int tls_connect_impl(const char *host, void *sockp, TlsCtx **out)
{
    TlsCtx *t = calloc(1, sizeof(TlsCtx));
    if (!t)
        return -1;
    t->sock = (SOCKET)sockp;

    SCHANNEL_CRED cred;
    memset(&cred, 0, sizeof(cred));
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION;

    SECURITY_STATUS ss = AcquireCredentialsHandleA(NULL,
        (SEC_CHAR *)"Microsoft Unified Security Protocol Provider",
        SECPKG_CRED_OUTBOUND, NULL, &cred, NULL, NULL, &t->hcred, NULL);
    if (ss != SEC_E_OK) {
        free(t);
        return -1;
    }

    DWORD ctxreq = ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT |
                   ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR |
                   ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

    SecBuffer inbufs[4];
    SecBufferDesc indesc;
    unsigned char inmem[16384];
    size_t inlen = 0;
    int have_ctx = 0;
    int done = 0;

    while (!done) {
        SecBuffer outbufs[4];
        SecBufferDesc outdesc;
        DWORD attr = 0;

        memset(&indesc, 0, sizeof(indesc));
        indesc.ulVersion = SECBUFFER_VERSION;
        if (inlen > 0) {
            indesc.cBuffers = 4;
            indesc.pBuffers = inbufs;
            inbufs[0].BufferType = SECBUFFER_TOKEN;
            inbufs[0].cbBuffer = (unsigned long)inlen;
            inbufs[0].pvBuffer = inmem;
            inbufs[1].BufferType = SECBUFFER_EMPTY;
            inbufs[2].BufferType = SECBUFFER_EMPTY;
            inbufs[3].BufferType = SECBUFFER_EMPTY;
        }

        memset(outbufs, 0, sizeof(outbufs));
        outdesc.ulVersion = SECBUFFER_VERSION;
        outdesc.cBuffers = 4;
        outdesc.pBuffers = outbufs;
        outbufs[0].BufferType = SECBUFFER_TOKEN;
        outbufs[0].cbBuffer = 0;
        outbufs[0].pvBuffer = NULL;

        ss = InitializeSecurityContextA(&t->hcred,
            have_ctx ? &t->hctx : NULL,
            (SEC_CHAR *)host,
            ctxreq, 0, 0,
            inlen > 0 ? &indesc : NULL,
            0, &t->hctx, &outdesc, &attr, NULL);
        have_ctx = 1;

        if (ss == SEC_E_OK) {
            /* handshake complete; send any final flight */
            for (int i = 0; i < 4; i++)
                if (outbufs[i].BufferType == SECBUFFER_TOKEN && outbufs[i].cbBuffer > 0)
                    tcp_send_all(t, outbufs[i].pvBuffer, outbufs[i].cbBuffer);
            /* leftover decrypted data (rare during handshake) is in a
             * SECBUFFER_DATA buffer; stash it for the first recv */
            for (int i = 0; i < 4; i++) {
                if (outbufs[i].BufferType == SECBUFFER_DATA && outbufs[i].cbBuffer > 0 &&
                    outbufs[i].cbBuffer <= sizeof(t->decbuf)) {
                    memcpy(t->decbuf, outbufs[i].pvBuffer, outbufs[i].cbBuffer);
                    t->declen = outbufs[i].cbBuffer;
                    t->dekpos = 0;
                }
            }
            done = 1;
            break;
        }
        if (ss == SEC_I_CONTINUE_NEEDED) {
            for (int i = 0; i < 4; i++)
                if (outbufs[i].BufferType == SECBUFFER_TOKEN && outbufs[i].cbBuffer > 0)
                    tcp_send_all(t, outbufs[i].pvBuffer, outbufs[i].cbBuffer);
            /* consume input fully; EXTRA bytes must move to buffer front */
            if (inlen > 0) {
                for (int i = 0; i < 4; i++) {
                    if (inbufs[i].BufferType == SECBUFFER_EXTRA && inbufs[i].cbBuffer > 0) {
                        /* EXTRA pvBuffer may be NULL: bytes are at the tail
                         * of our own inmem buffer */
                        size_t extra = inbufs[i].cbBuffer;
                        memmove(inmem, inmem + inlen - extra, extra);
                        inlen = extra;
                        break;
                    }
                }
                /* if the input was fully consumed, reset; if the caller had
                 * EXTRA pvBuffer we already moved; either way, read more */
            }
            /* read more handshake bytes */
            for (;;) {
                int n = recv(t->sock, inmem + inlen, (int)(sizeof(inmem) - inlen), 0);
                if (n <= 0)
                    goto fail;
                inlen += (size_t)n;
                /* try to process: break to run InitializeSecurityContext again */
                break;
            }
            continue;
        }
        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            /* need more bytes; keep what we have and read more */
            int n = recv(t->sock, inmem + inlen, (int)(sizeof(inmem) - inlen), 0);
            if (n <= 0)
                goto fail;
            inlen += (size_t)n;
            continue;
        }
        /* any other status: fail */
        goto fail;
    }

    QueryContextAttributesA(&t->hctx, SECPKG_ATTR_STREAM_SIZES, &t->sizes);
    t->encbuf = malloc(t->sizes.cbHeader + 16384 + t->sizes.cbTrailer + 64);
    if (!t->encbuf)
        goto fail;
    t->established = 1;
    *out = t;
    return 0;
fail:
    if (have_ctx)
        DeleteSecurityContext(&t->hctx);
    FreeCredentialsHandle(&t->hcred);
    free(t->encbuf);
    free(t);
    return -1;
}

int (*tls_connect_hook)(const char *, void *, TlsCtx **) = tls_connect_impl;
int (*tls_send_hook)(TlsCtx *, const void *, unsigned int) = tls_send_impl;
int (*tls_recv_hook)(TlsCtx *, void *, unsigned int) = tls_recv_impl;
void (*tls_close_hook)(TlsCtx *) = tls_close_impl;

static int tcp_send_all(TlsCtx *t, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        int n = send(t->sock, p, (int)len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int tls_send_impl(TlsCtx *t, const void *data, unsigned int len)
{
    if (!t || !t->established || t->dead)
        return -1;
    const char *p = data;
    while (len > 0) {
        DWORD take = len < 12000 ? len : 12000;
        SecBuffer bufs[4];
        SecBufferDesc bd;
        bd.ulVersion = SECBUFFER_VERSION;
        bd.cBuffers = 4;
        bd.pBuffers = bufs;
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].cbBuffer = t->sizes.cbHeader;
        bufs[0].pvBuffer = t->encbuf;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].cbBuffer = take;
        bufs[1].pvBuffer = (char *)t->encbuf + t->sizes.cbHeader;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].cbBuffer = t->sizes.cbTrailer;
        bufs[2].pvBuffer = (char *)t->encbuf + t->sizes.cbHeader + take;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        memcpy(bufs[1].pvBuffer, p, take);
        SECURITY_STATUS ss = EncryptMessage(&t->hctx, 0, &bd, 0);
        if (ss != SEC_E_OK)
            return -1;
        if (tcp_send_all(t, t->encbuf, bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer) != 0)
            return -1;
        p += take;
        len -= take;
    }
    return 0;
}

static int tls_recv_impl(TlsCtx *t, void *data, unsigned int len)
{
    if (!t || !t->established || t->dead)
        return -1;
    /* serve buffered plaintext first */
    if (t->dekpos < t->declen) {
        size_t take = t->declen - t->dekpos;
        if (take > len) take = len;
        memcpy(data, t->decbuf + t->dekpos, take);
        t->dekpos += take;
        return (int)take;
    }
    /* read ciphertext and decrypt */
    unsigned char raw[16384];
    int n = recv(t->sock, raw, sizeof(raw), 0);
    if (n <= 0)
        return -1;
    SecBuffer bufs[4];
    SecBufferDesc bd;
    bd.ulVersion = SECBUFFER_VERSION;
    bd.cBuffers = 4;
    bd.pBuffers = bufs;
    bufs[0].BufferType = SECBUFFER_DATA;
    bufs[0].cbBuffer = (DWORD)n;
    bufs[0].pvBuffer = raw;
    bufs[1].BufferType = SECBUFFER_EMPTY;
    bufs[2].BufferType = SECBUFFER_EMPTY;
    bufs[3].BufferType = SECBUFFER_EMPTY;
    SECURITY_STATUS ss = DecryptMessage(&t->hctx, &bd, 0, NULL);
    if (ss == SEC_E_OK || ss == SEC_I_RENEGOTIATE || ss == SEC_I_CONTEXT_EXPIRED) {
        /* find the data buffer */
        DWORD got = 0;
        for (int i = 0; i < 4; i++) {
            if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer > 0) {
                if (got == 0 && bufs[i].cbBuffer > len) {
                    /* more plaintext than requested: stash it all */
                    memcpy(t->decbuf, bufs[i].pvBuffer, bufs[i].cbBuffer);
                    t->declen = bufs[i].cbBuffer;
                    t->dekpos = 0;
                    break;
                }
                DWORD c = bufs[i].cbBuffer;
                if (got + c > len)
                    c = len - got;
                memcpy((char *)data + got, bufs[i].pvBuffer, c);
                got += c;
            }
        }
        if (got > 0)
            return (int)got;
        if (t->dekpos < t->declen) {
            size_t take = t->declen - t->dekpos;
            if (take > len) take = len;
            memcpy(data, t->decbuf + t->dekpos, take);
            t->dekpos += take;
            return (int)take;
        }
        /* nothing yet: keep reading until some plaintext arrives */
        return tls_recv_impl(t, data, len);
    }
    if (ss == SEC_E_INCOMPLETE_MESSAGE)
        return 0; /* need more data; caller retries */
    t->dead = 1;
    return -1;
}

static void tls_close_impl(TlsCtx *t)
{
    if (!t)
        return;
    if (t->established) {
        DWORD shut = SCHANNEL_SHUTDOWN;
        SecBuffer bufs[1];
        SecBufferDesc bd;
        bd.ulVersion = SECBUFFER_VERSION;
        bd.cBuffers = 1;
        bd.pBuffers = bufs;
        bufs[0].BufferType = SECBUFFER_TOKEN;
        bufs[0].cbBuffer = sizeof(shut);
        bufs[0].pvBuffer = &shut;
        ApplyControlToken(&t->hctx, &bd);
        SecBufferDesc obd;
        SecBuffer obuf[2];
        memset(&obd, 0, sizeof(obd));
        obd.cBuffers = 2;
        obd.pBuffers = obuf;
        obuf[0].BufferType = SECBUFFER_TOKEN;
        DWORD attr = 0;
        InitializeSecurityContextA(&t->hcred, &t->hctx, NULL, 0, 0, 0, NULL, 0,
                                  &t->hctx, &obd, &attr, NULL);
        if (obuf[0].cbBuffer > 0)
            tcp_send_all(t, obuf[0].pvBuffer, obuf[0].cbBuffer);
    }
    DeleteSecurityContext(&t->hctx);
    FreeCredentialsHandle(&t->hcred);
    free(t->encbuf);
    free(t);
}

#else

/* Non-Windows: no TLS backend; ws:// only. Hooks stay NULL. */

#endif
