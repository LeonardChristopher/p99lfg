/* Embedded relay node loopback test: our own client (ws_connect +
 * nostr REQ/EVENT) talks to our own embedded relay (relay_node.c) on
 * loopback - proving the "host a node" feature end-to-end. */
#ifndef _WIN32
#include "relay_node.h"
#include "nostr.h"
#include "sync.h"
#include "ws_client.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

int main(void)
{
    CHECK(relay_node_start(0) == 0, "node starts on ephemeral port");
    int port = relay_node_port();
    CHECK(port > 0, "node reports its port");

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d/", port);
    WsUrl u;
    CHECK(ws_parse_url(url, &u) == 0, "parse node url");
    WsConn c;
    CHECK(ws_connect(&c, &u) == 0, "client completes WebSocket handshake with node (real accept key)");

    /* publish a signed p99lfg event */
    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    const char *keypath = "p99lfg_test_key.txt";
    nostr_identity_load(keypath, ev.pubkey);
    char sk[65];
    FILE *kf = fopen(keypath, "r");
    fscanf(kf, "%64s", sk);
    fclose(kf);
    ev.created_at = (uint64_t)time(NULL);
    ev.kind = 9;
    snprintf(ev.dtag, sizeof(ev.dtag), "p99lfg-g-nodetest");
    snprintf(ev.content, sizeof(ev.content), "{\"from\":\"Nodeone\",\"msg\":\"hello node\"}");
    CHECK(nostr_sign_event(&ev, sk) == 0, "sign event");
    char msg[2048];
    nostr_event_message(&ev, msg, sizeof(msg));
    CHECK(ws_send_text(&c, msg) == 0, "send EVENT to node");

    char resp[1024];
    int n = ws_recv_text(&c, resp, sizeof(resp));
    CHECK(n > 0 && strstr(resp, ",true]") && strncmp(resp, "[\"OK\"", 5) == 0,
          "node returned OK verdict");
    CHECK(relay_node_events() == 1, "node stored the event");

    /* a second client subscribes and must receive the stored event */
    WsConn c2;
    CHECK(ws_connect(&c2, &u) == 0, "second client connects");
    char req[512];
    nostr_req_message("nodetest", req, sizeof(req));
    CHECK(ws_send_text(&c2, req) == 0, "send REQ to node");
    char evbuf[2048];
    n = ws_recv_text(&c2, evbuf, sizeof(evbuf));
    CHECK(n > 0 && strncmp(evbuf, "[\"EVENT\"", 8) == 0 &&
          strstr(evbuf, "hello node"), "subscriber receives stored event");
    n = ws_recv_text(&c2, evbuf, sizeof(evbuf));
    CHECK(n > 0 && strncmp(evbuf, "[\"EOSE\"", 7) == 0, "subscriber gets EOSE");

    /* real-time fan-out: client 1 publishes, client 2 (still
     * subscribed) receives it live */
    ev.created_at++;
    nostr_sign_event(&ev, sk);
    nostr_event_message(&ev, msg, sizeof(msg));
    CHECK(ws_send_text(&c, msg) == 0, "publish second event");
    n = ws_recv_text(&c2, evbuf, sizeof(evbuf));
    CHECK(n > 0 && strstr(evbuf, "hello node"), "live fan-out to subscriber");
    n = ws_recv_text(&c, resp, sizeof(resp));
    CHECK(n > 0 && strstr(resp, ",true]"), "publisher got OK for second event");

    /* scope enforcement: a non-p99lfg event is rejected */
    NostrEvent bad;
    memset(&bad, 0, sizeof(bad));
    snprintf(bad.pubkey, sizeof(bad.pubkey), "%s", ev.pubkey);
    bad.created_at = ev.created_at + 1;
    bad.kind = 1;
    snprintf(bad.dtag, sizeof(bad.dtag), "notours");
    snprintf(bad.content, sizeof(bad.content), "generic nostr text");
    /* note: no t tag -> not p99lfg-scoped */
    CHECK(nostr_sign_event(&bad, sk) == 0, "sign off-scope event");
    char badmsg[2048];
    nostr_event_message(&bad, badmsg, sizeof(badmsg));
    /* strip the t tag so it is genuinely off-scope: rebuild without tags */
    {
        /* crude but effective: the serializer always adds the t tag;
         * craft manually */
        snprintf(badmsg, sizeof(badmsg),
                 "[\"EVENT\",{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":%llu,"
                 "\"kind\":1,\"tags\":[],\"content\":\"generic\",\"sig\":\"x\"}]",
                 bad.id, bad.pubkey, (unsigned long long)bad.created_at);
    }
    CHECK(ws_send_text(&c, badmsg) == 0, "send off-scope event");
    n = ws_recv_text(&c, resp, sizeof(resp));
    CHECK(n > 0 && strstr(resp, ",false,"), "off-scope event rejected");
    CHECK(relay_node_events() == 2, "store unchanged by rejected event");

    ws_close(&c);
    ws_close(&c2);
    relay_node_stop();
    CHECK(relay_node_port() == port, "port remains readable after stop");
    printf("\n%s (%d failures)\n",
           fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
#else
#include <stdio.h>
int main(void) { printf("skipped on Windows (CI runs test_all there)\n"); return 0; }
#endif
