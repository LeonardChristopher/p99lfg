/* Two-instance group-flow harness (the "automated harness" the user
 * asked for while unable to test manually).
 *
 * A persistent in-process fake relay runs on 127.0.0.1: it completes
 * the WS handshake, answers REQ by replaying every stored event that
 * matches the kinds list, replies OK to EVENTs, and stores them so the
 * next client's REQ sees them. Two independent SyncState clients
 * (two key files) then run the real group flow against it:
 *
 *   leader: create group (state publish) -> invite (kind-4 DM)
 *   invitee: poll -> receives invite DM -> decrypts GROUPINV
 *   invitee: joins channel -> sends join control -> chat messages
 *   leader: polls -> receives chat, adopts state
 *
 * Loopback only; nothing leaves the machine. Not built on Windows
 * (same guard as test_ws.c - the Windows CI runs test_all instead).
 */
#ifndef _WIN32
#include "nostr.h"
#include "sync.h"
#include "chat.h"
#include "channels.h"
#include "ws_client.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

/* ---- fake relay: stores events, replays on REQ ---- */
#define STORE_MAX 256
static char g_store[STORE_MAX][4200];
static int g_store_n;
static pthread_mutex_t g_store_mu = PTHREAD_MUTEX_INITIALIZER;

static void store_put(const char *evjson)
{
    pthread_mutex_lock(&g_store_mu);
    if (g_store_n < STORE_MAX)
        snprintf(g_store[g_store_n++], sizeof(g_store[0]), "%s", evjson);
    pthread_mutex_unlock(&g_store_mu);
}

/* extract a JSON field value from the event object */
static void json_field(const char *json, const char *key, char *out, size_t outsz)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    out[0] = '\0';
    if (!p)
        return;
    p += strlen(pat);
    while (*p == ' ')
        p++;
    if (*p == '"') {
        p++;
        size_t o = 0;
        while (*p && *p != '"' && o + 1 < outsz)
            out[o++] = *p++;
        out[o] = '\0';
    } else {
        size_t o = 0;
        while (*p && *p != ',' && *p != '}' && o + 1 < outsz)
            out[o++] = *p++;
        out[o] = '\0';
    }
}

static void serve_client(int cfd)
{
    char buf[8192];
    ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(cfd); return; }
    buf[n] = '\0';
    const char *resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    if (send(cfd, resp, strlen(resp), 0) <= 0) { close(cfd); return; }

    /* serve up to 40 frames, then close (sync_poll reconnects anyway) */
    for (int round = 0; round < 40; round++) {
        unsigned char hdr[2] = {0};
        ssize_t h = recv(cfd, hdr, 2, MSG_WAITALL);
        if (h != 2)
            break;
        if ((hdr[0] & 0x0f) != 0x01)
            break;
        size_t len = hdr[1] & 0x7f;
        if (len == 126) {
            unsigned char ext[2];
            if (recv(cfd, ext, 2, MSG_WAITALL) != 2)
                break;
            len = ((size_t)ext[0] << 8) | ext[1];
        } else if (len == 127) {
            break; /* our messages are never that big */
        }
        unsigned char mask[4] = {0};
        if (recv(cfd, mask, 4, MSG_WAITALL) != 4)
            break;
        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        size_t got = 0;
        while (got < len) {
            ssize_t r = recv(cfd, buf + got, len - got, 0);
            if (r <= 0)
                goto done;
            got += (size_t)r;
        }
        for (size_t i = 0; i < got; i++)
            buf[i] ^= mask[i & 3];
        buf[got] = '\0';

        char out[4300];
        size_t oo = 0;
        if (strncmp(buf, "[\"EVENT\",", 9) == 0) {
            /* store the raw event object: ["EVENT",{"id":...}] -> {...} */
            const char *obj = strchr(buf, '{');
            store_put(obj ? obj : buf);
            snprintf(out, sizeof(out), "[\"OK\",\"x\",\"true\"]");
        } else if (strncmp(buf, "[\"REQ\",", 7) == 0) {
            /* reply with every stored event as its OWN frame (the
             * client parses one message per frame), then EOSE, then
             * close so the drain loop ends promptly. */
            pthread_mutex_lock(&g_store_mu);
            static const int kinds[] = {30078, 4, 9, 30079, 30080, 30081, 30082};
            for (int i = 0; i < g_store_n; i++) {
                char kind[16];
                json_field(g_store[i], "kind", kind, sizeof(kind));
                int kmatch = 0;
                for (unsigned j = 0; j < sizeof(kinds) / sizeof(kinds[0]); j++)
                    if (atoi(kind) == kinds[j])
                        kmatch = 1;
                if (!kmatch)
                    continue;
                snprintf(out, sizeof(out), "[\"EVENT\",\"sub\",%s]", g_store[i]);
                size_t ol = strlen(out);
                unsigned char frame[4400];
                size_t hl = 0;
                frame[0] = 0x81;
                if (ol <= 125) {
                    frame[1] = (unsigned char)ol;
                    hl = 2;
                } else {
                    frame[1] = 126;
                    frame[2] = (unsigned char)(ol >> 8);
                    frame[3] = (unsigned char)(ol & 0xff);
                    hl = 4;
                }
                memcpy(frame + hl, out, ol);
                if (send(cfd, frame, hl + ol, 0) <= 0)
                    break;
            }
            pthread_mutex_unlock(&g_store_mu);
            const char *eose = "[\"EOSE\",\"sub\"]";
            unsigned char eframe[16];
            eframe[0] = 0x81;
            eframe[1] = (unsigned char)strlen(eose);
            memcpy(eframe + 2, eose, strlen(eose));
            (void)send(cfd, eframe, 2 + strlen(eose), 0);
            goto done;
        } else {
            snprintf(out, sizeof(out), "[\"OK\"]");
        }

        /* one frame per message, with proper extended length */
        {
            size_t ol = strlen(out);
            unsigned char frame[4400];
            size_t hl = 0;
            frame[0] = 0x81;
            if (ol <= 125) {
                frame[1] = (unsigned char)ol;
                hl = 2;
            } else {
                frame[1] = 126;
                frame[2] = (unsigned char)(ol >> 8);
                frame[3] = (unsigned char)(ol & 0xff);
                hl = 4;
            }
            memcpy(frame + hl, out, ol);
            if (send(cfd, frame, hl + ol, 0) <= 0)
                goto done;
        }
    }
done:
    close(cfd);
}

static void *relay_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0)
            break;
        /* serve each client on its own thread so both clients can
         * interact with the relay concurrently */
        pthread_t t;
        if (pthread_create(&t, NULL, (void *(*)(void *))serve_client,
                          (void *)(intptr_t)cfd) == 0)
            pthread_detach(t);
        else
            close(cfd);
    }
    return NULL;
}

/* ---- two-client scenario ---- */
static char g_dm_content[600];
static char g_dm_sender[80];
static int g_dm_arrived;
static int g_generic_kind;
static char g_generic_content[2200];
static char g_generic_dtag[140];
static int g_generic_arrived;
static int g_saw_join_ctl;
static int g_saw_chat_invitee;
static int g_saw_chat_leader;

static void dm_cb(const char *sender, const char *content, void *ud)
{
    (void)ud;
    snprintf(g_dm_sender, sizeof(g_dm_sender), "%s", sender);
    snprintf(g_dm_content, sizeof(g_dm_content), "%s", content);
    g_dm_arrived++;
}

static void generic_cb(int kind, const char *pubkey, const char *dtag,
                      const char *content, uint64_t created, void *ud)
{
    (void)pubkey; (void)created; (void)ud;
    g_generic_kind = kind;
    snprintf(g_generic_dtag, sizeof(g_generic_dtag), "%s", dtag);
    snprintf(g_generic_content, sizeof(g_generic_content), "%s", content);
    g_generic_arrived++;
    /* phase trackers: one poll delivers MANY events; record what we
     * saw so assertions don't depend on callback order */
    if (kind == 9) {
        if (strstr(content, "\"join\""))
            g_saw_join_ctl = 1;
        if (strstr(content, "hello group"))
            g_saw_chat_invitee = 1;
        if (strstr(content, "welcome aboard"))
            g_saw_chat_leader = 1;
    }
}

int main(void)
{
    /* two identities */
    remove("p99lfg_harness_leader.key");
    remove("p99lfg_harness_invitee.key");
    char leader_pk[NOSTR_PUBKEY_HEX], invitee_pk[NOSTR_PUBKEY_HEX];
    nostr_identity_load("p99lfg_harness_leader.key", leader_pk);
    nostr_identity_load("p99lfg_harness_invitee.key", invitee_pk);
    char leader_sk[65], invitee_sk[65];
    FILE *f = fopen("p99lfg_harness_leader.key", "r");
    fscanf(f, "%64s", leader_sk);
    fclose(f);
    f = fopen("p99lfg_harness_invitee.key", "r");
    fscanf(f, "%64s", invitee_sk);
    fclose(f);
    CHECK(leader_pk[0] && invitee_pk[0] && strcmp(leader_pk, invitee_pk) != 0,
          "two distinct identities");

    /* start relay */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(lfd, (struct sockaddr *)&addr, sizeof(addr));
    socklen_t alen = sizeof(addr);
    getsockname(lfd, (struct sockaddr *)&addr, &alen);
    int port = ntohs(addr.sin_port);
    listen(lfd, 8);
    pthread_t rt;
    pthread_create(&rt, NULL, relay_thread, (void *)(intptr_t)lfd);
    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d/", port);
    printf("  harness relay on %s\n", url);

    SyncState leader, invitee;
    sync_init(&leader, "p99lfg_harness_leader.key", "green", NULL, NULL);
    sync_add_relay(&leader, url);
    sync_init(&invitee, "p99lfg_harness_invitee.key", "green", NULL, NULL);
    sync_add_relay(&invitee, url);
    /* the capture callbacks are process-global; the scenario is
     * strictly sequential so re-point them per phase. */

    /* 1. leader creates a group: seeds self and publishes state */
    GroupMember members[GROUP_MEMBERS_MAX];
    GroupMember wait[GROUP_WAIT_MAX];
    int nm = 0, nw = 0;
    memset(members, 0, sizeof(members));
    snprintf(members[0].name, sizeof(members[0].name), "Leaderone");
    snprintf(members[0].pubkey, sizeof(members[0].pubkey), "%s", leader_pk);
    members[0].officer = 1;
    nm = 1;
    const char *channel = "grp harness1"; /* space sanitized by the app,
                                             keep clean here */
    channel = "grpharness1";
    CHECK(group_state_publish(&leader, channel, members, nm, wait, nw) == 0,
          "leader publishes group state");

    /* 2. leader sends invite DM (encrypted GROUPINV payload) */
    char payload[160];
    snprintf(payload, sizeof(payload), "GROUPINV:%s:member", channel);
    CHECK(chat_send_message_from(&leader, invitee_pk, "Leaderone", payload) == 0,
          "leader sends invite DM");

    /* 3. invitee polls: must receive BOTH events, DM decrypts */
    sync_set_dm_callback(&invitee, dm_cb, NULL);
    sync_set_generic_callback(&invitee, generic_cb, NULL);
    g_dm_arrived = 0;
    g_generic_arrived = 0;
    sync_poll(&invitee);
    CHECK(g_dm_arrived >= 1, "invitee received invite DM");
    char plain[1024];
    CHECK(chat_try_decrypt(invitee_sk, g_dm_sender, g_dm_content,
                          plain, sizeof(plain)) == 0, "invite DM decrypts");
    /* the payload may carry the {"from":..,"msg":..} envelope; the
     * receiver checks the inner msg for GROUPINV (as win32_main on_dm
     * does after the unwrap fix) */
    const char *inv = plain;
    {
        const char *m = strstr(plain, "\"msg\":\"");
        if (m)
            inv = m + 7;
    }
    CHECK(strncmp(inv, "GROUPINV:", 9) == 0, "invite payload is GROUPINV");
    CHECK(g_generic_arrived >= 1 && g_generic_kind == 30081,
          "invitee received group state event");
    {
        GroupMember rm[GROUP_MEMBERS_MAX], rw[GROUP_WAIT_MAX];
        int rnm, rnw;
        CHECK(group_state_parse(g_generic_content, rm, &rnm, rw, &rnw) == 1 &&
              rnm == 1 && strcmp(rm[0].name, "Leaderone") == 0 &&
              rm[0].officer == 1,
              "invitee parses roster with officer flag");
    }

    /* 4. invitee joins: control message + first chat */
    CHECK(group_control_send(&invitee, channel, "Inviteetwo", invitee_pk, 0) == 0,
          "invitee sends join control");
    CHECK(channel_send(&invitee, channel, "Inviteetwo", "hello group") == 0,
          "invitee sends group chat");

    /* 5. leader polls: receives join control + chat */
    sync_set_dm_callback(&leader, NULL, NULL);
    sync_set_generic_callback(&leader, generic_cb, NULL);
    g_saw_join_ctl = 0;
    g_saw_chat_invitee = 0;
    sync_poll(&leader);
    CHECK(g_saw_join_ctl, "leader received join control (kind 9)");
    CHECK(g_saw_chat_invitee, "leader received group chat (kind 9)");

    /* 6. leader chat reaches invitee */
    CHECK(channel_send(&leader, channel, "Leaderone", "welcome aboard") == 0,
          "leader sends group chat");
    sync_set_generic_callback(&leader, NULL, NULL);
    sync_set_generic_callback(&invitee, generic_cb, NULL);
    g_dm_arrived = 0;
    g_generic_arrived = 0;
    g_saw_chat_leader = 0;
    sync_poll(&invitee);
    CHECK(g_saw_chat_leader, "invitee received leader chat");

    /* 7. full 6-member roster round-trips through publish + poll */
    nm = 0;
    for (int i = 0; i < 6; i++) {
        snprintf(members[i].name, sizeof(members[i].name), "Member%02d", i);
        snprintf(members[i].pubkey, sizeof(members[i].pubkey), "%s", leader_pk);
        members[i].officer = i % 2;
        nm++;
    }
    CHECK(group_state_publish(&leader, channel, members, nm, wait, nw) == 0,
          "leader publishes full 6-member state");
    g_generic_arrived = 0;
    sync_poll(&invitee);
    GroupMember rm[GROUP_MEMBERS_MAX], rw[GROUP_WAIT_MAX];
    int rnm, rnw;
    CHECK(group_state_parse(g_generic_content, rm, &rnm, rw, &rnw) == 1 &&
          rnm == 6 && rm[5].officer == 1,
          "invitee adopts full 6-member roster untruncated");

    pthread_cancel(rt);
    close(lfd);
    remove("p99lfg_harness_leader.key");
    remove("p99lfg_harness_invitee.key");
    printf("\n%s (%d failures)\n",
           fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
#else
#include <stdio.h>
int main(void) { printf("skipped on Windows\n"); return 0; }
#endif
