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

uint32_t voice_now_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                      (uint64_t)ts.tv_nsec / 1000000u);
#endif
}

static void peer_reset(VoicePeer *p)
{
    p->rtt_ms = VOICE_RTT_UNKNOWN;
    p->last_heard_ms = voice_now_ms();
    p->ping_sent_ms = 0;
    p->ping_id = 0;
    p->relayed = 0;
}

void voice_init(VoiceState *v)
{
    memset(v, 0, sizeof(*v));
    v->sock = -1;
    v->relay_after_ms = VOICE_RELAY_AFTER_MS;
    for (int i = 0; i < VOICE_PEERS_MAX; i++)
        peer_reset(&v->peers[i]);
}

int voice_open(VoiceState *v)
{
    if (v->sock >= 0 && v->port > 0)
        return 0; /* genuinely open: bound and has a real port */
    if (v->sock >= 0 && v->port == 0) {
        /* zero/uninitialized state leaked a handle: close and rebind so
         * we always advertise a real port */
        CLOSESOCK(v->sock);
        v->sock = -1;
    }
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
        peer_reset(p);
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

static VoicePeer *peer_find(VoiceState *v, const char *pubkey)
{
    for (int i = 0; i < v->npeers; i++)
        if (strcmp(v->peers[i].pubkey, pubkey) == 0)
            return &v->peers[i];
    return NULL;
}

static int peer_needs_relay(VoiceState *v, VoicePeer *p)
{
    if (!p)
        return 1; /* no known address: only a relay can reach them */
    if (!p->host[0] || p->port <= 0)
        return 1;
    if (!v->relay_after_ms)
        return 0; /* relaying disabled */
    return voice_now_ms() - p->last_heard_ms > v->relay_after_ms;
}

const char *voice_relay_for(VoiceState *v, const char *dest_pubkey)
{
    VoicePeer *best = NULL;
    uint32_t now = voice_now_ms();
    for (int i = 0; i < v->npeers; i++) {
        VoicePeer *p = &v->peers[i];
        if (!p->active || strcmp(p->pubkey, dest_pubkey) == 0)
            continue;
        if (!p->host[0] || p->port <= 0)
            continue;
        if (v->relay_after_ms &&
            now - p->last_heard_ms > v->relay_after_ms)
            continue; /* stale peers can't relay either */
        if (!best || p->rtt_ms < best->rtt_ms ||
            (p->rtt_ms == best->rtt_ms && strcmp(p->pubkey, best->pubkey) < 0))
            best = p;
    }
    return best ? best->pubkey : NULL;
}

int voice_update_paths(VoiceState *v)
{
    int changed = 0;
    for (int i = 0; i < v->npeers; i++) {
        VoicePeer *p = &v->peers[i];
        if (!p->active)
            continue;
        int nr = peer_needs_relay(v, p) && voice_relay_for(v, p->pubkey);
        if (p->relayed != nr) {
            p->relayed = nr;
            changed++;
        }
    }
    return changed;
}

static int send_pkt(VoiceState *v, const char *host, int port,
                    const uint8_t *pkt, size_t pktlen)
{
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &dest.sin_addr) != 1)
        return -1;
    ssize_t sent = sendto(v->sock, (const char *)pkt, pktlen, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    return sent > 0 ? 0 : -1;
}

/* The payload is always encrypted with the DESTINATION pairwise key, so
 * the destination can decrypt even when a relay peer only forwards the
 * packet opaquely (relayed packets carry the dest pubkey in clear text
 * in the header for exactly that). */
static int send_frame(VoiceState *v, const char *peer_pubkey,
                      const char *my_privkey,
                      const uint8_t *frame, size_t len, uint32_t stamp_ms,
                      int force_direct)
{
    if (v->sock < 0 || len == 0 || len > VOICE_FRAME_MAX)
        return -1;
    VoicePeer *p = peer_find(v, peer_pubkey);
    uint8_t key[32];
    if (derive_shared_key(my_privkey, peer_pubkey, key) != 0)
        return -1;
    uint8_t blob[VOICE_FRAME_MAX + 32];
    int blen = encrypt_frame(key, frame, len, blob, sizeof(blob));
    if (blen <= 0)
        return -1;
    static uint32_t seq_counter;
    uint32_t seq = ++seq_counter;
    uint8_t pkt[VOICE_FRAME_MAX + 160];
    if (!force_direct && peer_needs_relay(v, p)) {
        const char *relay = voice_relay_for(v, peer_pubkey);
        VoicePeer *rp = relay ? peer_find(v, relay) : NULL;
        if (!rp)
            return -1;
        if (p)
            p->relayed = 1;
        put_u32(pkt, VOICE_MAGIC_RELAY);
        put_u32(pkt + 4, seq);
        put_u32(pkt + 8, stamp_ms);
        put_u32(pkt + 12, (uint32_t)blen);
        snprintf((char *)pkt + 16, 65, "%s", peer_pubkey);
        memcpy(pkt + 80, blob, (size_t)blen);
        return send_pkt(v, rp->host, rp->port, pkt, 80 + (size_t)blen);
    }
    if (!p || !p->host[0] || p->port <= 0)
        return -1;
    if (p)
        p->relayed = 0;
    put_u32(pkt, VOICE_MAGIC);
    put_u32(pkt + 4, seq);
    put_u32(pkt + 8, stamp_ms);
    put_u32(pkt + 12, (uint32_t)blen);
    memcpy(pkt + 16, blob, (size_t)blen);
    return send_pkt(v, p->host, p->port, pkt, 16 + (size_t)blen);
}

int voice_send_frame(VoiceState *v, const char *peer_pubkey,
                     const char *my_privkey,
                     const uint8_t *frame, size_t len, uint32_t stamp_ms)
{
    return send_frame(v, peer_pubkey, my_privkey, frame, len, stamp_ms, 0);
}

/* Greeting/keepalive frame: sentinel + ping id. Sent periodically and
 * when a peer appears - opens NAT mappings, lets the receiver learn our
 * real source address, and doubles as the RTT probe (the receiver
 * answers with a pong echoing the id; see voice_poll). */
#define VOICE_PING_PAYLOAD "\xff\xfe"

int voice_ping(VoiceState *v, const char *peer_pubkey, const char *my_privkey)
{
    static uint32_t ping_counter;
    uint32_t id = ++ping_counter;
    uint8_t payload[6] = { VOICE_PING0, VOICE_PING1, 0, 0, 0, 0 };
    put_u32(payload + 2, id);
    VoicePeer *p = peer_find(v, peer_pubkey);
    if (p) {
        p->ping_id = id;
        p->ping_sent_ms = voice_now_ms();
    }
    /* pings always go direct: they are the NAT punch / RTT probe */
    return send_frame(v, peer_pubkey, my_privkey, payload, sizeof(payload), 0, 1);
}

/* Forget all peers (voice off / leaving a group / manual flush). */
void voice_flush(VoiceState *v)
{
    v->npeers = 0;
}

int voice_peer_register(VoiceState *v, const char *pubkey, const char *host,
                         int port)
{
    if (!pubkey || !pubkey[0] || port <= 0)
        return -1;
    for (int i = 0; i < v->npeers && i < VOICE_PEERS_MAX; i++) {
        if (strcmp(v->peers[i].pubkey, pubkey) == 0) {
            if (host && host[0])
                snprintf(v->peers[i].host, sizeof(v->peers[i].host), "%s",
                         host);
            v->peers[i].port = port;
            v->peers[i].active = 1;
            v->peers[i].last_heard_ms = voice_now_ms();
            return 0;
        }
    }
    if (v->npeers >= VOICE_PEERS_MAX)
        return -1;
    VoicePeer *p = &v->peers[v->npeers++];
    snprintf(p->pubkey, sizeof(p->pubkey), "%s", pubkey);
    snprintf(p->host, sizeof(p->host), "%s", host ? host : "");
    p->port = port;
    p->active = 1;
    p->last_seq = 0;
    peer_reset(p);
    return 0;
}

int voice_poll(VoiceState *v, const char *my_privkey,
               VoiceFrameCb cb, void *ud)
{
    if (v->sock < 0 || !cb)
        return 0;
    char my_pub[NOSTR_PUBKEY_HEX] = "";
    nostr_pubkey_from_privkey(my_privkey, my_pub);
    int delivered = 0;
    for (;;) {
        uint8_t pkt[VOICE_FRAME_MAX + 160];
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
        uint32_t magic = get_u32(pkt);
        size_t hdr = 16;
        if (magic == VOICE_MAGIC_RELAY) {
            if ((size_t)n < 80)
                continue;
            char dest[NOSTR_PUBKEY_HEX];
            memcpy(dest, pkt + 16, 64);
            dest[64] = '\0';
            if (strcmp(dest, my_pub) != 0) {
                /* not for us: forward opaquely to the dest's known
                 * address (we never decrypt relayed traffic) */
                VoicePeer *dp = peer_find(v, dest);
                if (dp && dp->host[0] && dp->port > 0)
                    send_pkt(v, dp->host, dp->port, pkt, (size_t)n);
                continue;
            }
            hdr = 80; /* we are the dest: fall through and process */
        } else if (magic != VOICE_MAGIC) {
            continue;
        }
        uint32_t seq = get_u32(pkt + 4);
        uint32_t stamp = get_u32(pkt + 8);
        uint32_t blen = get_u32(pkt + 12);
        if (hdr + blen > (size_t)n || blen < 32)
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
        uint8_t key[32];
        uint8_t frame[VOICE_FRAME_MAX];
        int fl = -1;
        if (match) {
            /* fast path: address matches the advertised presence */
            if (seq <= match->last_seq && match->last_seq - seq < 1000000)
                continue; /* dupe/replay within window */
            if (derive_shared_key(my_privkey, match->pubkey, key) == 0)
                fl = decrypt_frame(key, pkt + hdr, blen, frame, sizeof(frame));
        }
        if (fl <= 0) {
            /* no (working) address match: attribute by CRYPTO instead.
             * The sender's advertised IP can differ from the datagram
             * source (multi-NIC, VPN, virtual adapters - and relayed
             * packets always arrive from the RELAY's address), so only
             * the true peer's frames decrypt under the pairwise secret. */
            for (int i = 0; i < v->npeers && fl <= 0; i++) {
                if (seq <= v->peers[i].last_seq &&
                    v->peers[i].last_seq - seq < 1000000)
                    continue;
                if (derive_shared_key(my_privkey, v->peers[i].pubkey, key) != 0)
                    continue;
                fl = decrypt_frame(key, pkt + hdr, blen, frame, sizeof(frame));
                if (fl > 0) {
                    match = &v->peers[i];
                    match->last_seq = seq;
                    /* learn/refresh the real source address for the fast
                     * path and for sending back - but only from DIRECT
                     * packets; a relayed packet's source is the relay */
                    if (magic == VOICE_MAGIC) {
                        snprintf(match->host, sizeof(match->host), "%s", ip);
                        match->port = port;
                    }
                }
            }
        }
        if (!match || fl <= 0)
            continue;
        if (match) {
            if (match->last_seq < seq)
                match->last_seq = seq;
            match->last_heard_ms = voice_now_ms();
        }
        if (fl >= 6 && frame[0] == VOICE_PING0 && frame[1] == VOICE_PING1) {
            /* answer the RTT probe (still delivered to cb below for
             * back-compat with the sentinel skip in the app) */
            uint8_t pong[6] = { VOICE_PING0, VOICE_PONG1, 0, 0, 0, 0 };
            put_u32(pong + 2, get_u32(frame + 2));
            send_frame(v, match->pubkey, my_privkey, pong, sizeof(pong), 0, 1);
        } else if (fl >= 6 && frame[0] == VOICE_PING0 && frame[1] == VOICE_PONG1) {
            uint32_t id = get_u32(frame + 2);
            if (id == match->ping_id && match->ping_sent_ms)
                match->rtt_ms = voice_now_ms() - match->ping_sent_ms;
            continue; /* pong is control traffic, never audio */
        }
        cb(match->pubkey, frame, (size_t)fl, stamp, ud);
        delivered++;
    }
    return delivered;
}

int voice_presence_publish(SyncState *s, const char *channel,
                           const char *my_pubkey, int udp_port)
{
    return voice_presence_publish_ip(s, channel, my_pubkey, udp_port, NULL);
}

int voice_presence_publish_ip(SyncState *s, const char *channel,
                               const char *my_pubkey, int udp_port,
                               const char *my_ip)
{
    if (!channel[0] || udp_port <= 0)
        return -1;
    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", s->pubkey);
    ev.created_at = (uint64_t)time(NULL);
    ev.kind = 9; /* rides the channel as a control message */
    snprintf(ev.dtag, sizeof(ev.dtag), "p99lfg-g-%s", channel);
    if (my_ip && my_ip[0])
        snprintf(ev.content, sizeof(ev.content),
                 "{\"voice\":\"%s:%s:%d\"}", my_pubkey, my_ip, udp_port);
    else
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
    /* pubkey:port  OR  pubkey:ip:port */
    const char *colon2 = strchr(colon + 1, ':');
    if ((size_t)(colon - v) >= psz)
        return 0;
    memcpy(pubkey_out, v, (size_t)(colon - v));
    pubkey_out[colon - v] = '\0';
    if (colon2) {
        /* pubkey:ip:port */
        size_t iplen = (size_t)(colon2 - (colon + 1));
        if (iplen >= hsz)
            iplen = hsz - 1;
        memcpy(host_out, colon + 1, iplen);
        host_out[iplen] = '\0';
        *port_out = atoi(colon2 + 1);
    } else {
        *port_out = atoi(colon + 1);
        host_out[0] = '\0'; /* legacy presence: host learned from traffic */
    }
    return 1;
}
