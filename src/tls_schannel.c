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

static int tls_connect_impl(const char *host, void *sockp, TlsCtx **out);
static int tls_send_impl(TlsCtx *t, const void *data, unsigned int len);
static int tls_recv_impl(TlsCtx *t, void *data, unsigned int len);
static void tls_close_impl(TlsCtx *t);

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

static int tls_connect_impl(const char *host, void *sockp, TlsCtx **out)
{
    TlsCtx *t = calloc(1, sizeof(TlsCtx));
    if (!t)
        return -1;
    t->sock = (SOCKET)sockp;

    SCHANNEL_CRED cred;
    memset(&cred, 0, sizeof(cred));
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION |
                   SCH_CRED_NO_SERVERNAME_CHECK;
    cred.grbitEnabledProtocols = 0; /* system defaults */

    SECURITY_STATUS ss = AcquireCredentialsHandleA(NULL, (SEC_CHAR *)"Microsoft Unified Security Protocol Provider",
        SECPKG_CRED_OUTBOUND, NULL, &cred, NULL, NULL, &t->hcred, NULL);
    if (ss != SEC_E_OK) {
        free(t);
        return -1;
    }

    SecBufferDesc obd, ibd;
    SecBuffer obuf[2], ibuf[2];
    DWORD ctxreq = ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT |
                    ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR |
                    ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    DWORD ctxattr = 0;
    int first = 1;
    unsigned char inbuf[16384];
    size_t inlen = 0;

    for (;;) {
        memset(&obd, 0, sizeof(obd));
        obd.cBuffers = 2;
        obd.pBuffers = obuf;
        obuf[0].BufferType = SECBUFFER_TOKEN;
        obuf[0].cbBuffer = 0;
        obuf[0].pvBuffer = NULL;
        obuf[1].BufferType = SECBUFFER_EMPTY;

        memset(&ibd, 0, sizeof(ibd));
        if (!first) {
            ibd.cBuffers = 2;
            ibd.pBuffers = ibuf;
            ibuf[0].BufferType = SECBUFFER_TOKEN;
            ibuf[0].cbBuffer = (unsigned long)inlen;
            ibuf[0].pvBuffer = inbuf;
            ibuf[1].BufferType = SECBUFFER_EMPTY;
        }

        ss = InitializeSecurityContextA(&t->hcred,
            first ? NULL : &t->hctx,
            (SEC_CHAR *)host,
            ctxreq,
            0,
            0,
            first ? NULL : &ibd,
            0,
            &t->hctx,
            &obd,
            &ctxattr,
            NULL);

        if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) {
            if (obuf[0].cbBuffer > 0)
                if (tcp_send_all(t, obuf[0].pvBuffer, obuf[0].cbBuffer) != 0)
                    goto fail;
            if (ss == SEC_E_OK)
                break;
            first = 0;
            /* read more handshake data */
            inlen = 0;
            for (;;) {
                int n = recv(t->sock, inbuf + inlen, (int)(sizeof(inbuf) - inlen), 0);
                if (n <= 0)
                    goto fail;
                inlen += (size_t)n;
                /* try to process what we have */
                memset(&ibd, 0, sizeof(ibd));
                ibd.cBuffers = 2;
                ibd.pBuffers = ibuf;
                ibuf[0].BufferType = SECBUFFER_TOKEN;
                ibuf[0].cbBuffer = (unsigned long)inlen;
                ibuf[0].pvBuffer = inbuf;
                ibuf[1].BufferType = SECBUFFER_EMPTY;
                DWORD attr2 = 0;
                SecBufferDesc obd2;
                SecBuffer obuf2[2];
                memset(&obd2, 0, sizeof(obd2));
                obd2.cBuffers = 2;
                obd2.pBuffers = obuf2;
                obuf2[0].BufferType = SECBUFFER_TOKEN;
                obuf2[1].BufferType = SECBUFFER_EMPTY;
                ss = InitializeSecurityContextA(&t->hcred, &t->hctx, (SEC_CHAR *)host,
                    ctxreq, 0, 0, &ibd, 0, &t->hctx, &obd2, &attr2, NULL);
                if (ss == SEC_E_OK) {
                    if (obuf2[0].cbBuffer > 0)
                        tcp_send_all(t, obuf2[0].pvBuffer, obuf2[0].cbBuffer);
                    goto done;
                }
                if (ss == SEC_I_CONTINUE_NEEDED) {
                    if (obuf2[0].cbBuffer > 0)
                        tcp_send_all(t, obuf2[0].pvBuffer, obuf2[0].cbBuffer);
                    if (ibuf[1].BufferType == SECBUFFER_EXTRA)
                        inlen = ibuf[1].cbBuffer;
                    continue;
                }
                if (ss == SEC_E_INCOMPLETE_MESSAGE)
                    continue;
                goto fail;
            }
        } else if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            int n = recv(t->sock, inbuf, (int)sizeof(inbuf), 0);
            if (n <= 0)
                goto fail;
            (void)n;
            continue;
        } else {
            goto fail;
        }
    }
done:
    QueryContextAttributesA(&t->hctx, SECPKG_ATTR_STREAM_SIZES, &t->sizes);
    t->encbuf = malloc(t->sizes.cbHeader + 16384 + t->sizes.cbTrailer + 64);
    if (!t->encbuf)
        goto fail;
    t->established = 1;
    t->dead = 0;
    *out = t;
    return 0;
fail:
    FreeCredentialsHandle(&t->hcred);
    free(t->encbuf);
    free(t);
    return -1;
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
            if (bufs[i].BufferType == SECBUFFER_DATA) {
                DWORD c = bufs[i].cbBuffer;
                if (c > len) {
                    /* buffer more than requested: stash the rest */
                    memcpy(t->decbuf, bufs[i].pvBuffer, c);
                    t->declen = c;
                    t->dekpos = 0;
                    continue;
                }
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
        return 0;
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
