/* Embedded Nostr relay node. See relay_node.h for the design notes.
 *
 * Implementation: a single accept-loop thread + one thread per
 * client. Plain ws:// (no TLS - the audience is a LAN group where
 * traffic never leaves the home network; the same reason game servers
 * ship plain TCP). Only #t p99lfg-tagged events are stored or
 * forwarded - the node is NOT an open Nostr proxy. */
#include "relay_node.h"
#include "ws_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET node_socket_t;
#define NODE_INVALID INVALID_SOCKET
#define CLOSESOCK(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int node_socket_t;
#define NODE_INVALID -1
#define CLOSESOCK(s) close(s)
#endif

/* portable thread + lock primitives */
#ifdef _WIN32
static CRITICAL_SECTION g_mu;
static int g_mu_init;
static void node_lock(void)
{
    if (!g_mu_init) { InitializeCriticalSection(&g_mu); g_mu_init = 1; }
    EnterCriticalSection(&g_mu);
}
static void node_unlock(void) { LeaveCriticalSection(&g_mu); }
typedef HANDLE node_thread_t;
typedef DWORD WINAPI node_fn_t(LPVOID);
static int node_thread_create(node_thread_t *t, DWORD (WINAPI *fn)(LPVOID), void *arg)
{ *t = CreateThread(NULL, 0, fn, arg, 0, NULL); return *t ? 0 : -1; }
#else
#include <pthread.h>
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static void node_lock(void)   { pthread_mutex_lock(&g_mu); }
static void node_unlock(void) { pthread_mutex_unlock(&g_mu); }
typedef pthread_t node_thread_t;
static int node_thread_create(pthread_t *t, void *(*fn)(void *), void *arg)
{ return pthread_create(t, NULL, fn, arg); }
#endif

/* event store: ring of raw event JSON objects */
static char g_store[RELAY_NODE_STORE_MAX][4200];
static uint64_t g_store_at[RELAY_NODE_STORE_MAX];
static int g_store_n, g_store_next;

static node_socket_t g_clients[RELAY_NODE_CLIENTS_MAX];
static int g_nclients;

static node_socket_t g_listener = NODE_INVALID;
static int g_port;
static volatile int g_running;
static node_thread_t g_accept_thread;

int relay_node_port(void) { return g_port; }
int relay_node_clients(void) { return g_nclients; }
int relay_node_events(void) { return g_store_n; }

static void b64(const unsigned char *in, int len, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                            "abcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < len) v |= (unsigned)in[i+1] << 8;
        if (i + 2 < len) v |= in[i+2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? t[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? t[v & 63] : '=';
    }
    out[o] = '\0';
}

static int read_client_frame(node_socket_t fd, char *out, size_t outsz)
{
    unsigned char hdr[2];
    int n = recv(fd, (char *)hdr, 2, 0);
    if (n != 2) return -1;
    int opcode = hdr[0] & 0x0f;
    size_t len = hdr[1] & 0x7f;
    if (len == 126) {
        unsigned char ext[2];
        if (recv(fd, (char *)ext, 2, 0) != 2) return -1;
        len = ((size_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        return -1;
    }
    unsigned char mask[4];
    if (recv(fd, (char *)mask, 4, 0) != 4) return -1;
    if (len >= outsz) len = outsz - 1;
    size_t got = 0;
    while (got < len) {
        int r = recv(fd, out + got, (int)(len - got), 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    for (size_t i = 0; i < got; i++)
        out[i] ^= mask[i & 3];
    out[got] = '\0';
    if (opcode == 0x8) return -1;
    return (int)got;
}

static int send_server_frame(node_socket_t fd, const char *msg)
{
    size_t len = strlen(msg);
    unsigned char frame[8];
    size_t hl;
    frame[0] = 0x81;
    if (len <= 125) {
        frame[1] = (unsigned char)len;
        hl = 2;
    } else {
        frame[1] = 126;
        frame[2] = (unsigned char)(len >> 8);
        frame[3] = (unsigned char)(len & 0xff);
        hl = 4;
    }
    if (send(fd, (const char *)frame, (int)hl, 0) <= 0)
        return -1;
    return send(fd, msg, (int)len, 0) <= 0 ? -1 : 0;
}

static int is_p99lfg(const char *evobj)
{
    return strstr(evobj, "\"p99lfg\"") != NULL;
}

static void fan_out(const char *msg, node_socket_t skip)
{
    node_lock();
    for (int i = 0; i < g_nclients; i++) {
        if (g_clients[i] == skip)
            continue;
        if (send_server_frame(g_clients[i], msg) != 0) {
            CLOSESOCK(g_clients[i]);
            for (int j = i; j < g_nclients - 1; j++)
                g_clients[j] = g_clients[j + 1];
            g_nclients--;
            i--;
        }
    }
    node_unlock();
}

static void store_put(const char *evobj)
{
    uint64_t at = 0;
    const char *cp = strstr(evobj, "\"created_at\":");
    if (cp) {
        cp += 14;
        while (*cp >= '0' && *cp <= '9') {
            at = at * 10 + (uint64_t)(*cp - '0');
            cp++;
        }
    }
    node_lock();
    snprintf(g_store[g_store_next], 4200, "%s", evobj);
    g_store_at[g_store_next] = at;
    g_store_next = (g_store_next + 1) % RELAY_NODE_STORE_MAX;
    if (g_store_n < RELAY_NODE_STORE_MAX)
        g_store_n++;
    node_unlock();
}

#ifdef _WIN32
static DWORD WINAPI client_thread(LPVOID arg)
#else
static void *client_thread(void *arg)
#endif
{
    node_socket_t fd;
#ifdef _WIN32
    fd = (node_socket_t)(intptr_t)arg;
#else
    fd = (int)(intptr_t)arg;
#endif
    char buf[4200];
    for (;;) {
        int n = read_client_frame(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        if (strncmp(buf, "[\"EVENT\",", 9) == 0) {
            const char *obj = strchr(buf, '{');
            if (!obj) continue;
            char evid[128] = "";
            {
                const char *idp = strstr(obj, "\"id\":\"");
                if (idp) {
                    idp += 6;
                    const char *e = strchr(idp, '"');
                    if (e && (size_t)(e - idp) < sizeof(evid)) {
                        memcpy(evid, idp, (size_t)(e - idp));
                        evid[e - idp] = '\0';
                    }
                }
            }
            if (!is_p99lfg(obj)) {
                char rej[256];
                snprintf(rej, sizeof(rej), "[\"OK\",\"%s\",false,\"p99lfg events only\"]", evid);
                send_server_frame(fd, rej);
                continue;
            }
            store_put(obj);
            {
                char fwd[4300];
                snprintf(fwd, sizeof(fwd), "[\"EVENT\",\"node\",%s]", obj);
                fan_out(fwd, fd);
            }
            char ok[256];
            snprintf(ok, sizeof(ok), "[\"OK\",\"%s\",true]", evid);
            send_server_frame(fd, ok);
        } else if (strncmp(buf, "[\"REQ\",", 7) == 0) {
            uint64_t since = 0;
            {
                const char *sp = strstr(buf, "\"since\":");
                if (sp) {
                    sp += 8;
                    while (*sp >= '0' && *sp <= '9') {
                        since = since * 10 + (uint64_t)(*sp - '0');
                        sp++;
                    }
                }
            }
            node_lock();
            int start = (g_store_next - g_store_n + RELAY_NODE_STORE_MAX)
                        % RELAY_NODE_STORE_MAX;
            for (int k = 0; k < g_store_n; k++) {
                int idx = (start + k) % RELAY_NODE_STORE_MAX;
                if (since && g_store_at[idx] <= since)
                    continue;
                char ev[4400];
                snprintf(ev, sizeof(ev), "[\"EVENT\",\"node\",%s]", g_store[idx]);
                if (send_server_frame(fd, ev) != 0)
                    break;
            }
            node_unlock();
            send_server_frame(fd, "[\"EOSE\",\"node\"]");
        }
    }
    node_lock();
    for (int i = 0; i < g_nclients; i++) {
        if (g_clients[i] == fd) {
            for (int j = i; j < g_nclients - 1; j++)
                g_clients[j] = g_clients[j + 1];
            g_nclients--;
            break;
        }
    }
    node_unlock();
    CLOSESOCK(fd);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI accept_thread(LPVOID arg)
#else
static void *accept_thread(void *arg)
#endif
{
    (void)arg;
    while (g_running) {
        node_socket_t fd = accept(g_listener, NULL, NULL);
        if (fd == NODE_INVALID)
            break;
        /* WebSocket handshake: parse the client GET, compute the
         * accept key, reply 101 */
        char hs[2048];
        int n = recv(fd, hs, sizeof(hs) - 1, 0);
        if (n <= 0) {
            CLOSESOCK(fd);
            continue;
        }
        hs[n] = '\0';
        char key[64] = "";
        {
            const char *kp = strstr(hs, "Sec-WebSocket-Key:");
            if (!kp) kp = strstr(hs, "ec-WebSocket-Key:");
            if (kp) {
                kp += 18;
                while (*kp == ' ') kp++;
                const char *e = strstr(kp, "\r\n");
                if (e && (size_t)(e - kp) < sizeof(key)) {
                    memcpy(key, kp, (size_t)(e - kp));
                    key[e - kp] = '\0';
                }
            }
        }
        if (!key[0]) {
            CLOSESOCK(fd);
            continue;
        }
        char acc[64];
        {
            unsigned char sha[20];
            char cat[160];
            snprintf(cat, sizeof(cat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
            ws_sha1((const unsigned char *)cat, strlen(cat), sha);
            b64(sha, 20, acc);
        }
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
        if (send(fd, resp, (int)strlen(resp), 0) <= 0) {
            CLOSESOCK(fd);
            continue;
        }
        node_lock();
        if (g_nclients >= RELAY_NODE_CLIENTS_MAX) {
            node_unlock();
            CLOSESOCK(fd);
            continue;
        }
        g_clients[g_nclients++] = fd;
        node_unlock();
        node_thread_t t;
        if (node_thread_create(&t, client_thread, (void *)(intptr_t)fd) != 0) {
            node_lock();
            for (int i = 0; i < g_nclients; i++) {
                if (g_clients[i] == fd) {
                    for (int j = i; j < g_nclients - 1; j++)
                        g_clients[j] = g_clients[j + 1];
                    g_nclients--;
                    break;
                }
            }
            node_unlock();
            CLOSESOCK(fd);
            continue;
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int relay_node_start(int port)
{
    if (g_running)
        return 0;
    g_store_n = g_store_next = 0;
    g_nclients = 0;
    node_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == NODE_INVALID)
        return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s, 8) != 0) {
        CLOSESOCK(s);
        return -1;
    }
    struct sockaddr_in bound;
    int bl = (int)sizeof(bound);
    getsockname(s, (struct sockaddr *)&bound, &bl);
    g_port = ntohs(bound.sin_port);
    g_listener = s;
    g_running = 1;
    if (node_thread_create(&g_accept_thread, accept_thread, NULL) != 0) {
        g_running = 0;
        CLOSESOCK(g_listener);
        g_listener = NODE_INVALID;
        return -1;
    }
    return 0;
}

void relay_node_stop(void)
{
    g_running = 0;
    if (g_listener != NODE_INVALID) {
        CLOSESOCK(g_listener);
        g_listener = NODE_INVALID;
    }
    node_lock();
    for (int i = 0; i < g_nclients; i++)
        CLOSESOCK(g_clients[i]);
    g_nclients = 0;
    node_unlock();
}
