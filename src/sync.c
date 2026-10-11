#include "sync.h"
#include "ws_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- tiny JSON field extractor: finds "key":"value" or "key":value ---- */
static int json_get_string(const char *json, const char *key, char *out, size_t outsz)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == ':')
        p++;
    if (*p != '"')
        return -1;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < outsz) {
        if (*p == '\\' && p[1])
            p++;
        out[o++] = *p++;
    }
    out[o] = '\0';
    return (int)o;
}

static int json_get_bool(const char *json, const char *key, int dflt)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return dflt;
    p += strlen(pat);
    while (*p == ' ' || *p == ':')
        p++;
    if (strncmp(p, "true", 4) == 0)
        return 1;
    if (strncmp(p, "false", 5) == 0)
        return 0;
    return dflt;
}

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

/* Reload the identity from the key file (e.g. after nostr_import_key).
 * Updates privkey/pubkey in place. Returns 0 on success. */
int sync_reload_key(SyncState *s, const char *keyfile_path)
{
    FILE *f = fopen(keyfile_path, "r");
    if (!f)
        return -1;
    char hex[NOSTR_PRIVKEY_HEX] = {0};
    int ok = fscanf(f, "%64s", hex) == 1;
    fclose(f);
    if (!ok)
        return -1;
    char pubkey[NOSTR_PUBKEY_HEX] = {0};
    if (nostr_identity_load(keyfile_path, pubkey) != 0)
        return -1;
    snprintf(s->privkey, sizeof(s->privkey), "%s", hex);
    snprintf(s->pubkey, sizeof(s->pubkey), "%s", pubkey);
    return 0;
}

void sync_set_dm_callback(SyncState *s, SyncOnDm cb, void *userdata)
{
    s->on_dm = cb;
    s->dm_userdata = userdata;
}

void sync_set_generic_callback(SyncState *s, SyncOnGeneric cb, void *userdata)
{
    s->on_generic = cb;
    s->generic_userdata = userdata;
}

int sync_relay_count(SyncState *s)
{
    return s->nrelays;
}

const char *sync_relay_url(SyncState *s, int i)
{
    if (i < 0 || i >= s->nrelays)
        return NULL;
    return s->relays[i].url;
}

int sync_relay_connected(SyncState *s, int i)
{
    if (i < 0 || i >= s->nrelays)
        return 0;
    return s->relays[i].connected;
}

void sync_set_server(SyncState *s, const char *server)
{
    snprintf(s->server, sizeof(s->server), "%s", server ? server : "green");
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
    return (size_t)snprintf(out, outsz,
        "{\"v\":1,\"name\":\"%s\",\"zone\":\"%s\",\"detail\":\"%s\",\"lfg\":%s}",
        name, zone, detail, lfg ? "true" : "false");
}

/* Send a raw ["EVENT",...] message to all enabled relays. Returns 0 if at
 * least one accepted it. */
int sync_send_event(SyncState *s, const char *msg)
{
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
        s->relays[i].connected = 1;
        if (ws_send_text(&c, msg) == 0)
            ok++;
        ws_close(&c);
    }
    return ok > 0 ? 0 : -1;
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
        s->relays[i].connected = 1;
        if (ws_send_text(&c, msg) == 0) {
            char resp[512];
            ws_recv_text(&c, resp, sizeof(resp));
            ok++;
        }
        ws_close(&c);
    }
    return ok > 0 ? 0 : -1;
}

int sync_handle_message(SyncState *s, const char *msg)
{
    if (!msg || msg[0] != '[')
        return -1;
    if (strncmp(msg, "[\"EVENT\"", 8) != 0)
        return 0;
    const char *evobj = strchr(msg + 8, '{');
    if (!evobj)
        return -1;

    /* kind-4 DM: hand the raw content to the DM callback for decryption. */
    {
        char kindstr[16] = {0};
        const char *kp = strstr(evobj, "\"kind\":");
        if (kp) {
            const char *d = kp + 7;
            size_t i = 0;
            while (*d >= '0' && *d <= '9' && i < sizeof(kindstr) - 1)
                kindstr[i++] = *d++;
            if (strcmp(kindstr, "4") == 0 && s->on_dm) {
                char content[1200];
                char sender[80];
                if (json_get_string(evobj, "content", content, sizeof(content)) >= 0 &&
                    json_get_string(evobj, "pubkey", sender, sizeof(sender)) >= 0) {
                    s->on_dm(sender, content, s->dm_userdata);
                    return 1;
                }
            }
            /* kinds 30079 (rating) / 30080 (vote): generic callback */
            if ((strcmp(kindstr, "30079") == 0 || strcmp(kindstr, "30080") == 0)
                && s->on_generic) {
                char content[600], pubkey[80], dtag[140];
                uint64_t created = 0;
                if (json_get_string(evobj, "content", content, sizeof(content)) >= 0 &&
                    json_get_string(evobj, "pubkey", pubkey, sizeof(pubkey)) >= 0) {
                    if (json_get_string(evobj, "d", dtag, sizeof(dtag)) < 0) {
                        /* d tag sits inside tags array; find "d" entry */
                        const char *tp = strstr(evobj, "[\"d\",\"");
                        if (tp) {
                            tp += 6;
                            size_t di = 0;
                            while (*tp && *tp != '"' && di < sizeof(dtag) - 1)
                                dtag[di++] = *tp++;
                            dtag[di] = '\0';
                        } else {
                            dtag[0] = '\0';
                        }
                    }
                    {
                        char nums[24] = {0};
                        const char *cp = strstr(evobj, "\"created_at\":");
                        if (cp) {
                            cp += 14;
                            size_t ni = 0;
                            while (*cp >= '0' && *cp <= '9' && ni < sizeof(nums) - 1)
                                nums[ni++] = *cp++;
                            created = strtoull(nums, NULL, 10);
                        }
                    }
                    s->on_generic(atoi(kindstr), pubkey, dtag, content, created,
                                  s->generic_userdata);
                    return 1;
                }
            }
        }
    }

    char content[600];
    if (json_get_string(evobj, "content", content, sizeof(content)) < 0)
        return -1;
    char pubkey[80];
    if (json_get_string(evobj, "pubkey", pubkey, sizeof(pubkey)) < 0)
        return -1;

    SyncRemoteEntry e;
    memset(&e, 0, sizeof(e));
    if (json_get_string(content, "name", e.name, sizeof(e.name)) < 0)
        return -1; /* not our format; ignore */
    json_get_string(content, "zone", e.zone, sizeof(e.zone));
    json_get_string(content, "detail", e.detail, sizeof(e.detail));
    e.lfg = json_get_bool(content, "lfg", 1);

    if (s->on_event)
        s->on_event(&e, pubkey, s->userdata);
    return 1;
}

void sync_poll(SyncState *s)
{
    for (int i = 0; i < s->nrelays; i++) {
        if (!s->relays[i].enabled)
            continue;
        /* fire-and-forget poll: connect, REQ, drain a few messages, close.
           (A persistent background thread version lives in the GUI app.) */
        WsUrl u;
        if (ws_parse_url(s->relays[i].url, &u) != 0)
            continue;
        WsConn c;
        if (ws_connect(&c, &u) != 0) {
            s->relays[i].connected = 0;
            continue;
        }
        s->relays[i].connected = 1;
        char req[512];
        nostr_req_message("p99lfg-poll", req, sizeof(req));
        ws_send_text(&c, req);
        for (int m = 0; m < 16; m++) {
            char buf[8192];
            int n = ws_recv_text(&c, buf, sizeof(buf));
            if (n <= 0)
                break;
            sync_handle_message(s, buf);
        }
        ws_close(&c);
    }
}
