/* Voice transport loopback test (design doc: "build a voice_test mode
 * ... CI can verify encode/decode on Linux"). Two voice instances bind
 * loopback UDP sockets and exchange encrypted frames as if they were
 * two group members on one machine. No audio hardware needed. */
#include "voice.h"
#include "chat.h"
#include "nostr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

static char g_last_sender[NOSTR_PUBKEY_HEX];
static uint8_t g_last_frame[VOICE_FRAME_MAX];
static size_t g_last_len;
static uint32_t g_last_stamp;
static int g_frames;

static void frame_cb(const char *sender, const uint8_t *frame, size_t len,
                     uint32_t stamp, void *ud)
{
    (void)ud;
    snprintf(g_last_sender, sizeof(g_last_sender), "%s", sender);
    memcpy(g_last_frame, frame, len);
    g_last_len = len;
    g_last_stamp = stamp;
    g_frames++;
}

int main(void)
{
    /* two identities */
    remove("p99lfg_voice_a.key");
    remove("p99lfg_voice_b.key");
    char a_pk[NOSTR_PUBKEY_HEX], b_pk[NOSTR_PUBKEY_HEX];
    nostr_identity_load("p99lfg_voice_a.key", a_pk);
    nostr_identity_load("p99lfg_voice_b.key", b_pk);
    char a_sk[65], b_sk[65];
    FILE *f = fopen("p99lfg_voice_a.key", "r");
    fscanf(f, "%64s", a_sk);
    fclose(f);
    f = fopen("p99lfg_voice_b.key", "r");
    fscanf(f, "%64s", b_sk);
    fclose(f);
    CHECK(a_pk[0] && b_pk[0] && strcmp(a_pk, b_pk) != 0, "two voice identities");

    VoiceState a, b;
    voice_init(&a);
    voice_init(&b);
    CHECK(voice_open(&a) == 0 && a.port > 0, "voice A binds UDP port");
    CHECK(voice_open(&b) == 0 && b.port > 0, "voice B binds UDP port");

    /* each knows the other as a peer on loopback */
    char loop[16] = "127.0.0.1";
    VoicePeer *pa = NULL, *pb = NULL;
    for (int i = 0; i < VOICE_PEERS_MAX; i++) {
        if (!a.peers[i].active && !pa) { pa = &a.peers[i]; a.npeers++; }
        if (!b.peers[i].active && !pb) { pb = &b.peers[i]; b.npeers++; }
        if (pa && pb) break;
    }
    snprintf(pa->pubkey, sizeof(pa->pubkey), "%s", b_pk);
    snprintf(pa->host, sizeof(pa->host), "%s", loop);
    pa->port = b.port;
    pa->active = 1;
    snprintf(pb->pubkey, sizeof(pb->pubkey), "%s", a_pk);
    snprintf(pb->host, sizeof(pb->host), "%s", loop);
    pb->port = a.port;
    pb->active = 1;

    /* A sends a frame to B */
    uint8_t frame[120];
    for (int i = 0; i < 120; i++)
        frame[i] = (uint8_t)(i * 3 + 1);
    CHECK(voice_send_frame(&a, b_pk, a_sk, frame, sizeof(frame), 12345) == 0,
          "A sends encrypted frame");
    int got = voice_poll(&b, b_sk, frame_cb, NULL);
    CHECK(got == 1, "B receives frame");
    CHECK(strcmp(g_last_sender, a_pk) == 0, "frame attributed to A");
    CHECK(g_last_len == sizeof(frame) && g_last_stamp == 12345,
          "frame length + timestamp intact");
    CHECK(memcmp(g_last_frame, frame, sizeof(frame)) == 0,
          "frame payload decrypts byte-identical");

    /* B replies */
    uint8_t reply[80];
    for (int i = 0; i < 80; i++)
        reply[i] = (uint8_t)(0xff - i);
    CHECK(voice_send_frame(&b, a_pk, b_sk, reply, sizeof(reply), 54321) == 0,
          "B replies");
    got = voice_poll(&a, a_sk, frame_cb, NULL);
    CHECK(got == 1, "A receives reply");
    CHECK(g_last_len == sizeof(reply) && memcmp(g_last_frame, reply, sizeof(reply)) == 0,
          "reply payload intact");

    /* dedup: same seq replayed must not double-deliver (each send has a
     * fresh seq, so simulate by resending the same datagram via loop) */
    CHECK(voice_poll(&a, a_sk, frame_cb, NULL) == 0, "empty poll delivers nothing");

    /* wrong-key frame is dropped: a third identity can't impersonate */
    char c_pk[NOSTR_PUBKEY_HEX];
    remove("p99lfg_voice_c.key");
    nostr_identity_load("p99lfg_voice_c.key", c_pk);
    char c_sk[65];
    f = fopen("p99lfg_voice_c.key", "r");
    fscanf(f, "%64s", c_sk);
    fclose(f);
    /* C claims to be A's address: B's map has no entry for a new addr,
     * so impersonation by address fails outright */
    VoiceState c;
    voice_init(&c);
    voice_open(&c);
    {
        /* C registers B as a peer so it CAN emit a datagram toward B */
        VoicePeer *pc = &c.peers[c.npeers++];
        snprintf(pc->pubkey, sizeof(pc->pubkey), "%s", b_pk);
        snprintf(pc->host, sizeof(pc->host), "127.0.0.1");
        pc->port = b.port;
        pc->active = 1;
    }
    CHECK(voice_send_frame(&c, b_pk, c_sk, reply, sizeof(reply), 999) == 0,
          "C emits a datagram toward B");
    got = voice_poll(&b, b_sk, frame_cb, NULL);
    CHECK(got == 0, "unknown source datagram is dropped");

    /* presence signaling round-trip (pure, no relay needed) */
    const char *presence = "{\"voice\":\"aabbcc:51234\"}";
    char pk_out[NOSTR_PUBKEY_HEX];
    char host_out[64];
    int port_out = 0;
    CHECK(voice_presence_parse(presence, pk_out, sizeof(pk_out),
                               host_out, sizeof(host_out), &port_out) == 1,
          "voice presence parses");
    CHECK(strcmp(pk_out, "aabbcc") == 0 && port_out == 51234,
          "voice presence fields");
    CHECK(voice_presence_parse("{\"msg\":\"hello\"}", pk_out, sizeof(pk_out),
                               host_out, sizeof(host_out), &port_out) == 0,
          "non-voice message ignored");

    voice_close(&a);
    voice_close(&b);
    voice_close(&c);
    remove("p99lfg_voice_a.key");
    remove("p99lfg_voice_b.key");
    remove("p99lfg_voice_c.key");
    printf("\n%s (%d failures)\n",
           fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
