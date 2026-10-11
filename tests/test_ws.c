/* Loopback integration test: minimal in-process WebSocket "relay" that speaks
 * just enough NIP-01 to validate the client: handshake, REQ, EVENT publish.
 * The test relay runs on 127.0.0.1 only. */
#ifndef _WIN32

#include "nostr.h"
#include "ws_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

/* minimal SHA-1 via ws_client's own copy is static; test expected accept via known vector instead */
static const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

int main(void)
{
    /* 1. url parsing */
    WsUrl u;
    CHECK(ws_parse_url("ws://127.0.0.1:8080/", &u) == 0 && u.port == 8080 && strcmp(u.host, "127.0.0.1") == 0, "parse ws url");
    CHECK(ws_parse_url("wss://relay.example.com", &u) == 0 && u.use_tls && u.port == 443 && strcmp(u.path, "/") == 0, "parse wss url");
    CHECK(ws_parse_url("http://bad/", &u) != 0, "reject http url");

    /* 2. full loopback: spin a fake relay, connect, send REQ, get echo EVENT */
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
    listen(lfd, 1);
    printf("  fake relay on 127.0.0.1:%d\n", port);

    /* fake relay thread: accept, complete WS handshake ignoring key, then
       read REQ and reply OK, then read EVENT and reply OK */
    int cfd = -1;
    pid_t pid = fork();
    if (pid == 0) {
        cfd = accept(lfd, NULL, NULL);
        char buf[4096];
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        buf[n] = '\0';
        (void)buf;
        const char *resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
        send(cfd, resp, strlen(resp), 0);
        /* read two masked client frames (REQ, EVENT), reply OK to each */
        for (int round = 0; round < 2; round++) {
            unsigned char hdr[2] = {0};
            ssize_t n = recv(cfd, hdr, 2, 0);
            if (n != 2) break;
            size_t len = hdr[1] & 0x7f;
            if (len == 126) {
                unsigned char ext[2];
                recv(cfd, ext, 2, 0);
                len = ((size_t)ext[0] << 8) | ext[1];
            }
            unsigned char mask[4] = {0};
            recv(cfd, mask, 4, 0);
            unsigned char payload[512] = {0};
            size_t got = 0;
            while (got < len && got < sizeof(payload))
                got += (size_t)recv(cfd, payload + got, len - got, 0);
            for (size_t i = 0; i < got; i++)
                payload[i] ^= mask[i & 3];
            const char *ok = "[\"OK\"]";
            unsigned char frame[64];
            frame[0] = 0x81;
            frame[1] = (unsigned char)strlen(ok);
            memcpy(frame + 2, ok, strlen(ok));
            send(cfd, frame, 2 + strlen(ok), 0);
        }
        close(cfd);
        close(lfd);
        _exit(0);
    }
    (void)pid;
    close(lfd);
    (void)WS_GUID;

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d/", port);
    WsUrl ru;
    ws_parse_url(url, &ru);
    WsConn conn;
    CHECK(ws_connect(&conn, &ru) == 0, "ws connect + handshake");

    char req[512];
    nostr_req_message("t1", req, sizeof(req));
    CHECK(ws_send_text(&conn, req) == 0, "send REQ");

    char reply[1024];
    int n = ws_recv_text(&conn, reply, sizeof(reply));
    CHECK(n > 0 && strcmp(reply, "[\"OK\"]") == 0, "received [\"OK\"] reply");

    /* publish a signed event */
    NostrEvent ev = {0};
    const char *keypath = "p99lfg_test_key.txt";
    char key[65], pub[NOSTR_PUBKEY_HEX];
    FILE *kf = fopen(keypath, "r");
    fscanf(kf, "%64s", key);
    fclose(kf);
    nostr_identity_load(keypath, pub);
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", pub);
    ev.created_at = 1730000000;
    ev.kind = NOSTR_KIND_LFG;
    snprintf(ev.dtag, sizeof(ev.dtag), "test");
    snprintf(ev.content, sizeof(ev.content), "{\"name\":\"Soandso\"}");
    CHECK(nostr_sign_event(&ev, key) == 0, "sign before publish");
    char msg[2048];
    nostr_event_message(&ev, msg, sizeof(msg));
    CHECK(ws_send_text(&conn, msg) == 0, "send EVENT");

    ws_close(&conn);
    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
#else
#include <stdio.h>
int main(void) { printf("skipped on Windows\n"); return 0; }
#endif
