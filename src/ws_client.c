#include "ws_client.h"
#include "tls_hooks.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Weak defaults so the core links without a TLS backend; tls_schannel.c
 * overrides these on Windows. */
__attribute__((weak)) int (*tls_connect_hook)(const char *, void *, TlsCtx **) = NULL;
__attribute__((weak)) int (*tls_send_hook)(TlsCtx *, const void *, unsigned int) = NULL;
__attribute__((weak)) int (*tls_recv_hook)(TlsCtx *, void *, unsigned int) = NULL;
__attribute__((weak)) void (*tls_close_hook)(TlsCtx *) = NULL;

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static void sha1(const unsigned char *d, size_t len, unsigned char out[20]);

static void base64(const unsigned char *in, int len, char *out)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0;
    while (i + 2 < len) {
        int v = (in[i] << 16) | (in[i+1] << 8) | in[i+2];
        *out++ = b64[(v >> 18) & 63];
        *out++ = b64[(v >> 12) & 63];
        *out++ = b64[(v >> 6) & 63];
        *out++ = b64[v & 63];
        i += 3;
    }
    if (len - i == 1) {
        int v = in[i] << 16;
        *out++ = b64[(v >> 18) & 63];
        *out++ = b64[(v >> 12) & 63];
        *out++ = '=';
        *out++ = '=';
    } else if (len - i == 2) {
        int v = (in[i] << 16) | (in[i+1] << 8);
        *out++ = b64[(v >> 18) & 63];
        *out++ = b64[(v >> 12) & 63];
        *out++ = b64[(v >> 6) & 63];
        *out++ = '=';
    }
    *out = '\0';
}

int ws_parse_url(const char *url, WsUrl *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = url;
    if (strncmp(p, "wss://", 6) == 0) {
        out->use_tls = 1;
        out->port = 443;
        p += 6;
    } else if (strncmp(p, "ws://", 5) == 0) {
        out->use_tls = 0;
        out->port = 80;
        p += 5;
    } else {
        return -1;
    }
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    const char *hostend = slash ? slash : (p + strlen(p));
    if (colon && colon < hostend) {
        if (hostend - p >= (ptrdiff_t)sizeof(out->host))
            return -1;
        memcpy(out->host, p, (size_t)(colon - p));
        out->host[colon - p] = '\0';
        out->port = atoi(colon + 1);
    } else {
        size_t n = strlen(p);
        if (hostend - p < (ptrdiff_t)n)
            n = (size_t)(hostend - p);
        if (n >= sizeof(out->host))
            return -1;
        memcpy(out->host, p, n);
        out->host[n] = '\0';
    }
    snprintf(out->path, sizeof(out->path), "%s", slash ? slash : "/");
    return 0;
}

static int tcp_connect(const WsUrl *url)
{
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", url->port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(url->host, portstr, &hints, &res) != 0 || !res)
        return -1;
    ws_socket_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == WS_INVALID_SOCKET) {
        freeaddrinfo(res);
        return -1;
    }
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }
    freeaddrinfo(res);
#ifndef _WIN32
    /* Bounded I/O so a silent relay can't stall the poll loop forever. */
    struct timeval tv = {15, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const void *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const void *)&tv, sizeof(tv));
#endif
    return (int)s;
}

static int raw_send(WsConn *c, const void *data, size_t len)
{
    if (c->tls) {
        if (tls_send_hook)
            return tls_send_hook(c->tls, data, (unsigned int)len);
        return -1;
    }
    const char *p = data;
    while (len > 0) {
#ifdef _WIN32
        int n = send(c->sock, p, (int)len, 0);
#else
        ssize_t n = send(c->sock, p, len, 0);
#endif
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int raw_recv(WsConn *c, void *data, size_t len)
{
    if (c->tls) {
        if (tls_recv_hook)
            return tls_recv_hook(c->tls, data, (unsigned int)len);
        return -1;
    }
#ifdef _WIN32
    return recv(c->sock, data, (int)len, 0);
#else
    return (int)recv(c->sock, data, len, 0);
#endif
}

int ws_connect(WsConn *c, const WsUrl *url)
{
    memset(c, 0, sizeof(*c));
    c->sock = WS_INVALID_SOCKET;
    if (url->use_tls && !tls_connect_hook)
        return -1;
    int s = tcp_connect(url);
    if (s < 0)
        return -1;
    c->sock = (ws_socket_t)s;
    if (url->use_tls) {
        if (tls_connect_hook(url->host, (void *)(uintptr_t)c->sock, &c->tls) != 0) {
            ws_close(c);
            return -1;
        }
    }

    unsigned char rnd[16];
    for (int i = 0; i < 16; i++)
        rnd[i] = (unsigned char)(rand() ^ (i * 31 + 7));
    char key[32];
    base64(rnd, 16, key);

    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        url->path, url->host, url->port, key);
    if (raw_send(c, req, (size_t)n) != 0) {
        ws_close(c);
        return -1;
    }

    /* read response headers */
    char hdr[2048];
    size_t got = 0;
    while (got < sizeof(hdr) - 1) {
        int n2 = raw_recv(c, hdr + got, 1);
        if (n2 <= 0) {
            ws_close(c);
            return -1;
        }
        got += (size_t)n2;
        if (got >= 4 && memcmp(hdr + got - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    hdr[got] = '\0';
    if (!strstr(hdr, " 101 ") && !strstr(hdr, "101 ")) {
        ws_close(c);
        return -1;
    }
    c->connected = 1;
    return 0;
}

int ws_send_text(WsConn *c, const char *msg)
{
    size_t len = strlen(msg);
    unsigned char mask[4];
    for (int i = 0; i < 4; i++)
        mask[i] = (unsigned char)(rand() >> (i * 8));
    unsigned char hdr[14];
    size_t hlen = 0;
    hdr[hlen++] = 0x81; /* FIN + text */
    if (len < 126) {
        hdr[hlen++] = (unsigned char)(0x80 | len);
    } else if (len < 65536) {
        hdr[hlen++] = (unsigned char)(0x80 | 126);
        hdr[hlen++] = (unsigned char)(len >> 8);
        hdr[hlen++] = (unsigned char)(len & 0xff);
    } else {
        hdr[hlen++] = (unsigned char)(0x80 | 127);
        for (int i = 7; i >= 0; i--)
            hdr[hlen++] = (unsigned char)((uint64_t)len >> (8 * i));
    }
    memcpy(hdr + hlen, mask, 4);
    hlen += 4;
    if (raw_send(c, hdr, hlen) != 0)
        return -1;
    char masked[2048];
    if (len >= sizeof(masked))
        return -1;
    for (size_t i = 0; i < len; i++)
        masked[i] = msg[i] ^ mask[i & 3];
    return raw_send(c, masked, len);
}

int ws_recv_text(WsConn *c, char *out, size_t outsz)
{
    unsigned char hdr[2];
    size_t need = 2;
    size_t have = 0;
    while (have < need) {
        int n = raw_recv(c, hdr + have, need - have);
        if (n <= 0)
            return -1;
        have += (size_t)n;
    }
    int opcode = hdr[0] & 0x0f;
    int masked = (hdr[1] & 0x80) != 0;
    size_t len = hdr[1] & 0x7f;
    if (len == 126) {
        unsigned char ext[2];
        if (raw_recv(c, ext, 2) != 2)
            return -1;
        len = ((size_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8];
        if (raw_recv(c, ext, 8) != 8)
            return -1;
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | ext[i];
    }
    unsigned char mask[4] = {0};
    if (masked && raw_recv(c, mask, 4) != 4)
        return -1;
    if (len >= outsz)
        len = outsz - 1;
    size_t got = 0;
    while (got < len) {
        int n = raw_recv(c, out + got, len - got);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    if (masked)
        for (size_t i = 0; i < got; i++)
            out[i] ^= mask[i & 3];
    out[got] = '\0';
    if (opcode == 0x8)
        return 0; /* close */
    if (opcode == 0x9) { /* ping -> pong */
        unsigned char pong[2] = {0x8a, (unsigned char)got};
        raw_send(c, pong, 2);
        return ws_recv_text(c, out, outsz);
    }
    return (int)got;
}

void ws_close(WsConn *c)
{
    if (c->tls) {
        if (tls_close_hook)
            tls_close_hook(c->tls);
        c->tls = NULL;
    }
    if (c->sock != WS_INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(c->sock);
#else
        close(c->sock);
#endif
    }
    c->sock = WS_INVALID_SOCKET;
    c->connected = 0;
}

/* ---- SHA-1 (RFC 3174), used for the WebSocket accept header ---- */
static void sha1(const unsigned char *d, size_t len, unsigned char out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    unsigned char msg[128];
    size_t blocks = len / 64;
    size_t rem = len % 64;
    size_t total = blocks * 64 + ((rem < 56) ? 64 : 128);
    memset(msg, 0, sizeof(msg));
    for (size_t blk = 0; blk < total; blk += 64) {
        if (blk + 64 <= len) {
            memcpy(msg, d + blk, 64);
        } else {
            memset(msg, 0, 64);
            if (len > blk)
                memcpy(msg, d + blk, len - blk);
            msg[len - blk] = 0x80;
            uint64_t bits = (uint64_t)len * 8;
            for (int i = 0; i < 8; i++)
                msg[63 - i] = (unsigned char)(bits >> (8 * i));
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)msg[i*4] << 24) | ((uint32_t)msg[i*4+1] << 16) |
                   ((uint32_t)msg[i*4+2] << 8) | msg[i*4+3];
        for (int i = 16; i < 80; i++) {
            uint32_t v = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a=h[0],b=h[1],cc=h[2],dd=h[3],e=h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & cc) | ((~b) & dd); k = 0x5A827999; }
            else if (i < 40) { f = b ^ cc ^ dd; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b&cc)|(b&dd)|(cc&dd); k = 0x8F1BBCDC; }
            else { f = b ^ cc ^ dd; k = 0xCA62C1D6; }
            uint32_t t = ((a<<5)|(a>>27)) + f + e + k + w[i];
            e=dd; dd=cc; cc=(b<<30)|(b>>2); b=a; a=t;
        }
        h[0]+=a; h[1]+=b; h[2]+=cc; h[3]+=dd; h[4]+=e;
    }
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (unsigned char)(h[i] >> 24);
        out[i*4+1] = (unsigned char)(h[i] >> 16);
        out[i*4+2] = (unsigned char)(h[i] >> 8);
        out[i*4+3] = (unsigned char)(h[i]);
    }
}
