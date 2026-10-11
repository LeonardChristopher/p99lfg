#include "sync.h"
#include "ws_client.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

void sync_init(SyncState *s, const char *keyfile_path, const char *server,
               SyncOnEvent cb, void *userdata)
{
    memset(s, 0, sizeof(*s));
    nostr_identity_load(keyfile_path, s->pubkey);
    FILE *f = fopen(keyfile_path, "r");
    if (f) {
        if (fscanf(f, "%64s", s->privkey) != 1)
            s->privkey[0] = '\0';
        fclose(f);
    }
    snprintf(s->server, sizeof(s->server), "%s", server ? server : "green");
    s->on_event = cb;
    s->userdata = userdata;
}

void sync_add_relay(SyncState *s, const char *url)
{
    if (s->nrelays >= SYNC_MAX_RELAYS)
        return;
    snprintf(s->relays[s->nrelays].url, sizeof(s->relays[0].url), "%s", url);
    s->relays[s->nrelays].enabled = 1;
    s->nrelays++;
}

size_t sync_build_content(char *out, size_t outsz, const char *name,
                          const char *zone, const char *detail, int lfg)
{
    /* minimal JSON escaping for our own fields (alnum + spaces in practice) */
    return (size_t)snprintf(out, outsz,
        "{\"v\":1,\"name\":\"%s\",\"zone\":\"%s\",\"detail\":\"%s\",\"lfg\":%s}",
        name, zone, detail, lfg ? "true" : "false");
}

int sync_publish(SyncState *s, const char *content_json)
{
    NostrEvent ev = {0};
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", s->pubkey);
    ev.created_at = (uint64_t)time(NULL);
    ev.kind = NOSTR_KIND_LFG;
    snprintf(ev.dtag, sizeof(ev.dtag), "%s", s->server);
    snprintf(ev.content, sizeof(ev.content), "%s", content_json);
    if (nostr_sign_event(&ev, s->privkey) != 0)
        return -1;

    char msg[2048];
    nostr_event_message(&ev, msg, sizeof(msg));

    int ok = 0;
    for (int i = 0; i < s->nrelays; i++) {
        if (!s->relays[i].enabled)
            continue;
        WsUrl u;
        if (ws_parse_url(s->relays[i].url, &u) != 0)
            continue;
        WsConn c;
        if (ws_connect(&c, &u) != 0)
            continue;
        if (ws_send_text(&c, msg) == 0)
            ok++;
        char resp[512];
        ws_recv_text(&c, resp, sizeof(resp)); /* consume OK/notice */
        ws_close(&c);
    }
    return ok > 0 ? 0 : -1;
}
