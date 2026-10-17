#include "sync_persist.h"
#include "ws_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION persist_mutex_t;
static void pm_init(persist_mutex_t *m) { InitializeCriticalSection(m); }
static void pm_lock(persist_mutex_t *m) { EnterCriticalSection(m); }
static void pm_unlock(persist_mutex_t *m) { LeaveCriticalSection(m); }
static void pm_destroy(persist_mutex_t *m) { DeleteCriticalSection(m); }
typedef HANDLE persist_thread_t;
static int pt_create(persist_thread_t *t, DWORD (WINAPI *fn)(LPVOID), void *arg)
{ *t = CreateThread(NULL, 0, fn, arg, 0, NULL); return *t ? 0 : -1; }
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t persist_mutex_t;
static void pm_init(persist_mutex_t *m) { pthread_mutex_init(m, NULL); }
static void pm_lock(persist_mutex_t *m) { pthread_mutex_lock(m); }
static void pm_unlock(persist_mutex_t *m) { pthread_mutex_unlock(m); }
static void pm_destroy(persist_mutex_t *m) { pthread_mutex_destroy(m); }
typedef pthread_t persist_thread_t;
static void msleep(int ms) { usleep(ms * 1000); }
#define WINAPI
#define LPVOID void*
static int pt_create(persist_thread_t *t, void *(*fn)(void *), void *arg)
{ return pthread_create(t, NULL, fn, arg); }
#endif

/* serialise sync_handle_message between stream threads and the poll */
static persist_mutex_t g_handle_mu;
static int g_handle_mu_init;

static int g_persist_run;
static persist_thread_t g_threads[SYNC_MAX_RELAYS];
static int g_thread_count;
static SyncState *g_sync; /* shared: all streams feed the same state */

int sync_persist_connected(void)
{
    int n = 0;
    for (int i = 0; i < g_sync->nrelays; i++)
        if (g_sync->relays[i].connected)
            n++;
    return n;
}

/* One relay's streaming loop: connect, REQ (since watermark), pump
 * EVENTs into sync_handle_message until the socket drops, then
 * backoff and reconnect. */
#ifdef _WIN32
static DWORD WINAPI relay_stream(LPVOID arg)
#else
static void *relay_stream(void *arg)
#endif
{
    int idx = (int)(intptr_t)arg;
    char subid[32];
    snprintf(subid, sizeof(subid), "p99lfg-live%d", idx);
    int backoff_ms = 1000;
    while (g_persist_run) {
        SyncState *s = g_sync;
        if (!s || idx >= s->nrelays)
            break;
        if (!s->relays[idx].enabled) {
#ifdef _WIN32
            Sleep(2000);
#else
            msleep(2000);
#endif
            continue;
        }
        WsUrl u;
        if (ws_parse_url(s->relays[idx].url, &u) != 0) {
#ifdef _WIN32
            Sleep(5000);
#else
            msleep(5000);
#endif
            continue;
        }
        WsConn c;
        if (ws_connect(&c, &u) != 0) {
            ws_dbg_public("persist: connect %s failed, retry in %dms",
                          s->relays[idx].url, backoff_ms);
#ifdef _WIN32
            Sleep(backoff_ms);
#else
            msleep(backoff_ms);
#endif
            backoff_ms = backoff_ms >= 60000 ? 60000 : backoff_ms * 2;
            continue;
        }
        s->relays[idx].connected = 1;
        backoff_ms = 1000;
        /* subscribe from the watermark (minus overlap: relays may
         * timestamp slightly differently, and dedup absorbs replays) */
        char req[512];
        uint64_t since = s->last_created_at;
        if (since > 2)
            since -= 2;
        nostr_req_message_since(subid, req, sizeof(req), since);
        if (ws_send_text(&c, req) != 0) {
            s->relays[idx].connected = 0;
            ws_close(&c);
            continue;
        }
        while (g_persist_run) {
            char buf[8192];
            int n = ws_recv_text(&c, buf, sizeof(buf));
            if (n == -2)
                continue; /* quiet cycle: keep waiting on this socket */
            if (n < 0)
                break; /* error: drop, reconnect below */
            if (n == 0)
                break; /* relay closed: reconnect */
            sync_deliver_lock();
            sync_handle_message(s, buf);
            sync_deliver_unlock();
        }
        s->relays[idx].connected = 0;
        ws_close(&c);
        if (!g_persist_run)
            break;
        /* dropped: brief pause then reconnect (streaming connections
         * can be closed by relays periodically - this is normal) */
#ifdef _WIN32
        Sleep(backoff_ms);
#else
        msleep(backoff_ms);
#endif
        backoff_ms = backoff_ms >= 60000 ? 60000 : backoff_ms * 2;
    }
    return 0;
}

void sync_persist_start(SyncState *s)
{
    if (g_persist_run)
        return;
    g_sync = s;
    if (!g_handle_mu_init) {
        pm_init(&g_handle_mu);
        g_handle_mu_init = 1;
    }
    g_persist_run = 1;
    g_thread_count = 0;
    for (int i = 0; i < s->nrelays && g_thread_count < SYNC_MAX_RELAYS; i++) {
        if (pt_create(&g_threads[g_thread_count],
                      relay_stream, (void *)(intptr_t)i) == 0)
            g_thread_count++;
    }
    ws_dbg_public("persist: %d streaming threads started", g_thread_count);
}

void sync_persist_stop(void)
{
    g_persist_run = 0;
}
