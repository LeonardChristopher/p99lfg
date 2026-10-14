/* Group voice transport groundwork - see voice.h for the design notes. */
#include "voice.h"
#include "chat.h"
#include "aes.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
typedef int socklen_t;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define CLOSESOCK close
#define INVALID_SOCKET (-1)
#endif

void voice_init(VoiceState *v)
{
    memset(v, 0, sizeof(*v));
    v->sock = -1;
}

int voice_open(VoiceState *v)
{
    if (v->sock >= 0)
        return 0;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET)
        return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0; /* ephemeral; we advertise the real port */
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        CLOSESOCK(s);
        return -1;
    }
    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    if (getsockname(s, (struct sockaddr *)&bound, &blen) != 0) {
        CLOSESOCK(s);
        return -1;
    }
    v->sock = (int)s;
    v->port = ntohs(bound.sin_port);
#ifdef _WIN32
    /* MSG_DONTWAIT doesn't exist on Windows: make the socket
     * non-blocking once, voice_poll drains with recvfrom EAGAIN. */
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#endif
    return 0;
}

void voice_close(VoiceState *v)
{
    if (v->sock >= 0) {
        CLOSESOCK(v->sock);
        v->sock = -1;
    }
    v->npeers = 0;
    v->port = 0;
}

/* packet layout on the wire:
 * magic(4) seq(4) stamp(4) payload_len(2) payload(N, AES-CBC blocks,
 * IV prepended inside the encrypted blob as chat does: iv(16)+ct) */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static VoicePeer *peer_find_or_add(VoiceState *v, const char *pubkey,
                                   const char *host, int port)
{
    VoicePeer *p = NULL;
    for (int i = 0; i < v->npeers; i++)
        if (strcmp(v->peers[i].pubkey, pubkey) == 0) {
            p = &v->peers[i];
            break;
        }
    if (!p) {
        if (v->npeers >= VOICE_PEERS_MAX)
            return NULL;
        p = &v->peers[v->npeers++];
        snprintf(p->pubkey, sizeof(p->pubkey), "%s", pubkey);
        p->last_seq = 0;
    }
    snprintf(p->host, sizeof(p->host), "%s", host ? host : "");
    p->port = port;
    p->active = 1;
    return p;
}

/* encrypt payload with the pairwise secret; blob = iv(16) + ct */
static int encrypt_frame(const uint8_t key[32], const uint8_t *in,
                         size_t len, uint8_t *blob, size_t blobsz)
{
    if (len > VOICE_FRAME_MAX)
        return -1;
    /* CBC needs PKCS-ish padding to a 16-byte block */
    size_t padded = (len + 16) / 16 * 16;
    if (16 + padded > blobsz)
        return -1;
    uint8_t iv[16];
#ifdef _WIN32
    {
        HCRYPTPROV h = 0;
        if (CryptAcquireContextA(&h, NULL, NULL, PROV_RSA_FULL,
                                CRYPT_VERIFYCONTEXT)) {
            CryptGenRandom(h, 16, iv);
            CryptReleaseContext(h, 0);
        } else {
            for (int i = 0; i < 16; i++)
                iv[i] = (uint8_t)(rand() ^ (unsigned)(i * 7 + 3));
        }
    }
#else
    {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            (void)read(fd, iv, 16);
            close(fd);
        } else {
            for (int i = 0; i < 16; i++)
                iv[i] = (uint8_t)(rand() ^ (unsigned)(i * 7 + 3));
        }
    }
#endif
    uint8_t buf[VOICE_FRAME_MAX + 16];
    memset(buf, padded - len, padded); /* pad value, simplified PKCS#7-ish */
    memcpy(buf, in, len);
    aes256_cbc_encrypt(key, iv, buf, padded, buf);
    memcpy(blob, iv, 16);
    memcpy(blob + 16, buf, padded);
    return (int)(16 + padded);
}

static int decrypt_frame(const uint8_t key[32], const uint8_t *blob,
                         size_t blobsz, uint8_t *out, size_t outsz)
{
    if (blobsz < 32 || (blobsz - 16) % 16 != 0)
        return -1;
    size_t padded = blobsz - 16;
    if (padded > outsz)
        return -1;
    uint8_t buf[VOICE_FRAME_MAX + 16];
    aes256_cbc_decrypt(key, blob, blob + 16, padded, buf);
    uint8_t pad = buf[padded - 1];
    if (pad == 0 || pad > 16 || pad > padded)
        return -1;
    size_t len = padded - pad;
    if (len > outsz)
        return -1;
    memcpy(out, buf, len);
    return (int)len;
}

int voice_send_frame(VoiceState *v, const char *peer_pubkey,
                     const char *my_privkey,
                     const uint8_t *frame, size_t len, uint32_t stamp_ms)
{
    if (v->sock < 0 || len == 0 || len > VOICE_FRAME_MAX)
        return -1;
    VoicePeer *p = NULL;
    for (int i = 0; i < v->npeers; i++)
        if (strcmp(v->peers[i].pubkey, peer_pubkey) == 0)
            p = &v->peers[i];
    if (!p || !p->host[0] || p->port <= 0)
        return -1;
    uint8_t key[32];
    if (derive_shared_key(my_privkey, peer_pubkey, key) != 0)
        return -1;
    uint8_t blob[VOICE_FRAME_MAX + 32];
    int blen = encrypt_frame(key, frame, len, blob, sizeof(blob));
    if (blen <= 0)
        return -1;
    static uint32_t seq_counter;
    uint32_t seq = ++seq_counter;
    uint8_t pkt[VOICE_FRAME_MAX + 64];
    put_u32(pkt, VOICE_MAGIC);
    put_u32(pkt + 4, seq);
    put_u32(pkt + 8, stamp_ms);
    put_u32(pkt + 12, (uint32_t)blen);
    memcpy(pkt + 16, blob, (size_t)blen);
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((unsigned short)p->port);
    if (inet_pton(AF_INET, p->host, &dest.sin_addr) != 1)
        return -1;
    ssize_t sent = sendto(v->sock, (const char *)pkt, 16 + (size_t)blen, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    return sent > 0 ? 0 : -1;
}

int voice_poll(VoiceState *v, const char *my_privkey,
               VoiceFrameCb cb, void *ud)
{
    if (v->sock < 0 || !cb)
        return 0;
    int delivered = 0;
    for (;;) {
        uint8_t pkt[VOICE_FRAME_MAX + 64];
        struct sockaddr_in from_;
        socklen_t flen = sizeof(from_);
        ssize_t n = recvfrom(v->sock, (char *)pkt, sizeof(pkt),
#ifdef _WIN32
                             0, /* socket is non-blocking already */
#else
                             MSG_DONTWAIT,
#endif
                             (struct sockaddr *)&from_, &flen);
        if (n < 16)
            break;
        if (get_u32(pkt) != VOICE_MAGIC)
            continue;
        uint32_t seq = get_u32(pkt + 4);
        uint32_t stamp = get_u32(pkt + 8);
        uint32_t blen = get_u32(pkt + 12);
        if ((size_t)16 + blen > (size_t)n || blen < 32)
            continue;
        /* find the peer by UDP address (we don't sign packets in v1;
         * identity is bound by the ECDH key: only a holder of the
         * peer's private key can produce a decryptable frame, and the
         * shared secret is unique per pair) */
        char ip[INET_ADDRSTRLEN] = "";
        inet_ntop(AF_INET, &from_.sin_addr, ip, sizeof(ip));
        int port = ntohs(from_.sin_port);
        VoicePeer *match = NULL;
        for (int i = 0; i < v->npeers; i++)
            if (strcmp(v->peers[i].host, ip) == 0 && v->peers[i].port == port)
                match = &v->peers[i];
        if (!match)
            continue;
        if (seq <= match->last_seq && match->last_seq - seq < 1000000)
            continue; /* dupe/replay within window */
        match->last_seq = seq;
        uint8_t key[32];
        if (derive_shared_key(my_privkey, match->pubkey, key) != 0)
            continue;
        uint8_t frame[VOICE_FRAME_MAX];
        int fl = decrypt_frame(key, pkt + 16, blen, frame, sizeof(frame));
        if (fl <= 0)
            continue; /* wrong key: not really this peer */
        cb(match->pubkey, frame, (size_t)fl, stamp, ud);
        delivered++;
    }
    return delivered;
}

int voice_presence_publish(SyncState *s, const char *channel,
                           const char *my_pubkey, int udp_port)
{
    if (!channel[0] || udp_port <= 0)
        return -1;
    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", s->pubkey);
    ev.created_at = (uint64_t)time(NULL);
    ev.kind = 9; /* rides the channel as a control message */
    snprintf(ev.dtag, sizeof(ev.dtag), "p99lfg-g-%s", channel);
    snprintf(ev.content, sizeof(ev.content),
             "{\"voice\":\"%s:%d\"}", my_pubkey, udp_port);
    if (nostr_sign_event(&ev, s->privkey) != 0)
        return -1;
    char msg[4200];
    nostr_event_message(&ev, msg, sizeof(msg));
    return sync_send_event(s, msg);
}

int voice_presence_parse(const char *content, char *pubkey_out,
                         size_t psz, char *host_out, size_t hsz,
                         int *port_out)
{
    const char *v = strstr(content, "\"voice\":\"");
    if (!v)
        return 0;
    v += 9;
    const char *colon = strchr(v, ':');
    if (!colon)
        return 0;
    /* pubkey:port */
    if ((size_t)(colon - v) >= psz)
        return 0;
    memcpy(pubkey_out, v, (size_t)(colon - v));
    pubkey_out[colon - v] = '\0';
    *port_out = atoi(colon + 1);
    host_out[0] = '\0'; /* host is learned from actual UDP traffic */
    return 1;
}
