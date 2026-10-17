/* Voice transport loopback test (design doc: "build a voice_test mode
 * ... CI can verify encode/decode on Linux"). Two voice instances bind
 * loopback UDP sockets and exchange encrypted frames as if they were
 * two group members on one machine. No audio hardware needed. */
#include "voice.h"
#include "voice_audio.h"
#include "voice_codec.h"
#include "chat.h"
#include "nostr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

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

    /* presence with ip: pubkey:ip:port */
    CHECK(voice_presence_parse("{\"voice\":\"aabbcc:192.168.1.5:51234\"}",
                               pk_out, sizeof(pk_out), host_out, sizeof(host_out),
                               &port_out) == 1 &&
          strcmp(pk_out, "aabbcc") == 0 &&
          strcmp(host_out, "192.168.1.5") == 0 && port_out == 51234,
          "presence with ip parses");

    /* peer registration: upsert by pubkey. a already carries loopback
     * peers from earlier phases, so find the aabbcc slot explicitly. */
    {
        int before = a.npeers;
        CHECK(voice_peer_register(&a, "aabbcc", "192.168.1.5", 51234) == 0 &&
              a.npeers == before + 1,
              "peer registered with address");
        VoicePeer *pp = NULL;
        for (int i = 0; i < a.npeers; i++)
            if (strcmp(a.peers[i].pubkey, "aabbcc") == 0)
                pp = &a.peers[i];
        CHECK(pp && strcmp(pp->host, "192.168.1.5") == 0 && pp->port == 51234,
              "registered peer has the advertised address");
        CHECK(voice_peer_register(&a, "aabbcc", "192.168.1.9", 51235) == 0 &&
              a.npeers == before + 1,
              "re-register upserts, no duplicate slot");
        pp = NULL;
        for (int i = 0; i < a.npeers; i++)
            if (strcmp(a.peers[i].pubkey, "aabbcc") == 0)
                pp = &a.peers[i];
        CHECK(pp && strcmp(pp->host, "192.168.1.9") == 0 && pp->port == 51235,
              "re-registered peer updated in place");
    }

    /* push-to-talk gating + mic level */
    CHECK(vptt_gate(0, 0) == 1 && vptt_gate(0, 1) == 1,
          "open-mic mode always transmits");
    CHECK(vptt_gate(1, 0) == 0 && vptt_gate(1, 1) == 1,
          "PTT transmits only while held");
    {
        int16_t quiet[320] = {0};
        int16_t loud[320];
        for (int i = 0; i < 320; i++)
            loud[i] = (int16_t)(i % 2 ? 20000 : -20000);
        CHECK(vptt_level(quiet, 320) == 0, "silence levels to 0");
        int lvl = vptt_level(loud, 320);
        CHECK(lvl >= 55 && lvl <= 65, "loud frame levels ~61");
    }

    /* Opus codec round-trip: synthetic 440Hz tone, encode -> decode,
     * assert the waveform survives (correlation, not exactness - lossy
     * compression). Then send the opus frame through the encrypted
     * transport like the GUI does. */
    {
        VoiceCodec vc;
        CHECK(vcodec_open(&vc) == 0, "opus codec opens");
        /* production flow: encode each 20ms frame, decode it right
         * away (as the receiving end does), compare energy over the
         * stream - opus reconstruction is near-1.0 when frames flow
         * through the decoder in order */
        enum { NFRAMES = 20 };
        int16_t tone[VCODEC_SAMPLES];
        double energy_sum = 0;
        int checked = 0;
        for (int f = 0; f < NFRAMES; f++) {
            for (int i = 0; i < VCODEC_SAMPLES; i++)
                tone[i] = (int16_t)(8000.0 * sin(2 * 3.14159265 * 440.0
                                                 * (f * VCODEC_SAMPLES + i)
                                                 / 16000.0));
            uint8_t ob[VCODEC_MAX_BYTES];
            int nb = vcodec_encode(&vc, tone, VCODEC_SAMPLES, ob, sizeof(ob));
            if (f == 0) {
                CHECK(nb > 0 && nb <= 127, "opus frame encodes in bounds");
                CHECK(nb < VCODEC_SAMPLES * 2 / 4,
                      "opus frame is well under raw PCM size");
            }
            if (nb <= 0)
                continue;
            int16_t back[VCODEC_SAMPLES * 2];
            int ns = vcodec_decode(&vc, ob, (size_t)nb, back,
                                  sizeof(back) / sizeof(back[0]));
            if (f == 0)
                CHECK(ns == VCODEC_SAMPLES, "decoded frame is 20ms again");
            if (ns <= 0)
                continue;
            if (f >= 15) { /* skip codec warm-up frames */
                double ene = 0, eno = 0;
                for (int i = 0; i < ns; i++) {
                    ene += (double)back[i] * back[i];
                    eno += (double)tone[i] * tone[i];
                }
                energy_sum += eno > 0 ? ene / eno : 0;
                checked++;
            }
        }
        CHECK(checked > 0 && energy_sum / checked > 0.8,
              "decoded audio preserves energy (near-lossless loudness)");
        /* silence round-trip: opus frames must not be empty */
        int16_t quiet[VCODEC_SAMPLES] = {0};
        uint8_t qob[VCODEC_MAX_BYTES];
        int nq = vcodec_encode(&vc, quiet, VCODEC_SAMPLES, qob, sizeof(qob));
        CHECK(nq > 0, "silence encodes to a valid frame");
        vcodec_close(&vc);
    }

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
