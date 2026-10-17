#ifndef P99LFG_VOICE_H
#define P99LFG_VOICE_H
/* Group voice transport groundwork (ROADMAP doc section 2, phase 1).
 *
 * Design decisions from the doc, honoured here:
 * - full mesh UDP, no relays, no floor control for 6-person groups
 * - audio frames encrypted with the same ECDH shared secret as chat
 *   (AES-256-CBC per packet; audio privacy == chat privacy)
 * - no audio-hardware dependency: encode/decode/transport are pure and
 *   loopback-testable in CI (sandbox and CI have no sound devices)
 *
 * The codec layer (Opus via libopus, WinMM capture) is deliberately
 * NOT here yet - this module owns the wire: sockets, framing, crypto,
 * and the voice-presence signaling event. Audio device + codec work
 * lands on top once this is proven by the loopback tests. */
#include "nostr.h"
#include "sync.h"

#define VOICE_MAGIC 0x50395647 /* "P9VG" */
#define VOICE_FRAME_MAX 512    /* 20ms of Opus at 16-24kbps fits easily */
#define VOICE_PORT_DEFAULT 0  /* bind ephemeral, advertise the real one */
#define VOICE_PEERS_MAX 6     /* one group */

typedef struct {
    uint32_t magic;      /* VOICE_MAGIC */
    uint32_t seq;        /* per-sender sequence, receiver drops dupes */
    uint32_t stamp_ms;   /* sender timestamp for jitter debug/quality */
    uint16_t payload_len;
    uint8_t  payload[VOICE_FRAME_MAX];
} VoicePacket;

typedef struct {
    char pubkey[NOSTR_PUBKEY_HEX]; /* who this peer is */
    char host[64];                 /* their advertised IP */
    int  port;                     /* their advertised UDP port */
    uint32_t last_seq;             /* dedup: highest seq seen */
    int  active;
} VoicePeer;

typedef struct {
    int sock;                     /* UDP socket, or -1 */
    int port;                     /* bound port (for advertising) */
    VoicePeer peers[VOICE_PEERS_MAX];
    int npeers;
} VoiceState;

void voice_init(VoiceState *v);
/* Open the UDP socket (bind ephemeral). Returns 0 on success. */
int voice_open(VoiceState *v);
void voice_close(VoiceState *v);

/* Encrypt+send one frame to a peer (dtls-free: AES-256-CBC under the
 * pairwise ECDH secret, same as chat). Returns 0 on success. */
int voice_send_frame(VoiceState *v, const char *peer_pubkey,
                     const char *my_privkey,
                     const uint8_t *frame, size_t len, uint32_t stamp_ms);

/* Poll the socket; deliver decrypted frames of any peer via cb.
 * Returns number of frames delivered. */
typedef void (*VoiceFrameCb)(const char *sender_pubkey,
                             const uint8_t *frame, size_t len,
                             uint32_t stamp_ms, void *ud);
int voice_poll(VoiceState *v, const char *my_privkey,
               VoiceFrameCb cb, void *ud);

/* Signaling: the "I'm in voice, my address" presence event content
 * that rides the existing group channel (kind 9 control message).
 * Returns 0 on success. */
int voice_presence_publish(SyncState *s, const char *channel,
                           const char *my_pubkey, int udp_port);

/* Presence with the sender's own address (pubkey:ip:port) - the peer
 * can send to us immediately, no traffic-learning chicken-and-egg. */
int voice_presence_publish_ip(SyncState *s, const char *channel,
                               const char *my_pubkey, int udp_port,
                               const char *my_ip);

/* Send a greeting/keepalive frame to a peer (NAT punch + address
 * learning). Returns 0 on success. */
int voice_ping(VoiceState *v, const char *peer_pubkey, const char *my_privkey);

/* Forget all peers (voice off / leaving a group / manual flush). */
void voice_flush(VoiceState *v);

/* Register/refresh a voice peer from a presence event. */
int voice_peer_register(VoiceState *v, const char *pubkey, const char *host,
                         int port);
/* Parse a voice-presence control message. Returns 1 and fills the
 * peer's advertised address on match. */
int voice_presence_parse(const char *content, char *pubkey_out,
                         size_t psz, char *host_out, size_t hsz,
                         int *port_out);

#endif
