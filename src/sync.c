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

/* Send a raw ["EVENT",...] message to all enabled relays, as-is (no
 * outbox). Returns 0 if at least one accepted it. */
static int sync_send_event_now(SyncState *s, const char *msg)
{
    int ok = 0;
    for (int i = 0; i < s->nrelays; i++) {
        if (!s->relays[i].enabled)
            continue;
        WsUrl u;
        if (ws_parse_url(s->relays[i].url, &u) != 0)
            continue;
        WsConn c;
        if (ws_connect(&c, &u) != 0) {
            ws_log_public("publish: connect to %s failed", s->relays[i].url);
            continue;
        }
        s->relays[i].connected = 1;
        if (ws_send_text(&c, msg) != 0) {
            ws_log_public("publish: send to %s failed", s->relays[i].url);
            ws_close(&c);
            continue;
        }
        /* wait for the relay's verdict: ["OK",<id>,true|false,...].
         * A rejected event (ok:false) must NOT count as published,
         * otherwise messages silently vanish on that relay. */
        char resp[512];
        int n = ws_recv_text(&c, resp, sizeof(resp));
        int relay_ok = 0;
        if (n <= 0) {
            ws_log_public("publish: no confirmation from %s (not counted)",
                           s->relays[i].url);
        } else {
            ws_log_public("publish: %s said %.60s", s->relays[i].url, resp);
            /* rejected only on explicit ok:false or a NOTICE; anything
             * else (OK true, untyped reply) counts as accepted */
            if (!strstr(resp, "false") && !strstr(resp, "NOTICE"))
                relay_ok = 1;
        }
        if (relay_ok)
            ok++;
        ws_close(&c);
    }
    return ok > 0 ? 0 : -1;
}

int sync_outbox_pending(SyncState *s)
{
    return s->outbox_n;
}

/* Send a raw ["EVENT",...] to all enabled relays. If none accepts (all
 * down, slow, or rejecting), spool the message in the outbox instead of
 * dropping it; sync_poll retries until a relay accepts. Retrying is
 * idempotent: relays ignore duplicate event ids. Returns 0 when the
 * message is either published or durably queued; -1 only when it could
 * not even be queued (oversized). */
int sync_send_event(SyncState *s, const char *msg)
{
    if (sync_send_event_now(s, msg) == 0)
        return 0;
    if (strlen(msg) >= SYNC_OUTBOX_MSG)
        return -1;
    snprintf(s->outbox[s->outbox_next], SYNC_OUTBOX_MSG, "%s", msg);
    s->outbox_next = (s->outbox_next + 1) % SYNC_OUTBOX_MAX;
    if (s->outbox_n < SYNC_OUTBOX_MAX)
        s->outbox_n++;
    ws_log_public("outbox: all relays failed; queued for retry (%d pending)",
                  s->outbox_n);
    return 0;
}

int sync_publish_char(SyncState *s, const char *name, const char *cls,
                      int level, const char *zone, int lfg)
{
    NostrEvent ev = {0};
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", s->pubkey);
    ev.created_at = (uint64_t)time(NULL);
    ev.kind = NOSTR_KIND_LFG;
    /* one replaceable event per character */
    snprintf(ev.dtag, sizeof(ev.dtag), "%s/%s", s->server, name);
    snprintf(ev.content, sizeof(ev.content),
        "{\"v\":2,\"name\":\"%s\",\"class\":\"%s\",\"level\":%d,"
        "\"zone\":\"%s\",\"lfg\":%s,\"user\":\"%s\"}",
        name, cls, level, zone ? zone : "", lfg ? "true" : "false",
        s->user_name[0] ? s->user_name : "");
    if (nostr_sign_event(&ev, s->privkey) != 0)
        return -1;
    char msg[2048];
    nostr_event_message(&ev, msg, sizeof(msg));
    return sync_send_event(s, msg);
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
    /* duplicate suppression: every poll re-downloads stored events.
     * Applies ONLY to append-style kinds (chat kind 9, DM kind 4,
     * ratings) where a re-send would double-append. State kinds
     * (30081 group roster, 30082 profile, 30078 LFG) are replaceable /
     * latest-wins and must be re-delivered every poll: a client may see
     * the state event BEFORE its invite DM activates the group, and
     * suppressing it would leave the roster empty forever. */
    {
        char kindstr_dedup[16] = {0};
        const char *kdp = strstr(evobj, "\"kind\":");
        int dedup_kind = 1;
        if (kdp) {
            const char *kd = kdp + 7;
            size_t ki = 0;
            while (*kd >= '0' && *kd <= '9' && ki < sizeof(kindstr_dedup) - 1)
                kindstr_dedup[ki++] = *kd++;
            if (strcmp(kindstr_dedup, "30081") == 0 ||
                strcmp(kindstr_dedup, "30082") == 0 ||
                strcmp(kindstr_dedup, "30078") == 0)
                dedup_kind = 0;
        }
        if (dedup_kind) {
            char evid[NOSTR_EVENT_ID_HEX] = {0};
            if (json_get_string(evobj, "id", evid, sizeof(evid)) >= 0 && evid[0]) {
                for (int i = 0; i < s->seen_n; i++)
                    if (strcmp(s->seen_ids[i], evid) == 0)
                        return 1; /* already processed */
                snprintf(s->seen_ids[s->seen_next], NOSTR_EVENT_ID_HEX, "%s", evid);
                s->seen_next = (s->seen_next + 1) % SYNC_SEEN_MAX;
                if (s->seen_n < SYNC_SEEN_MAX)
                    s->seen_n++;
            }
        }
    }
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
            /* kinds 30079 (rating) / 30080 (vote) / 9 (group channel chat +
             * join/leave control): generic callback */
            if ((strcmp(kindstr, "30079") == 0 || strcmp(kindstr, "30080") == 0 ||
                 strcmp(kindstr, "9") == 0 || strcmp(kindstr, "30081") == 0 ||
                 strcmp(kindstr, "30082") == 0)
                && s->on_generic) {
                char content[2200], pubkey[80], dtag[140];
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

    char content[2200];
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
    json_get_string(content, "class", e.cls, sizeof(e.cls));
    json_get_string(content, "user", e.user, sizeof(e.user));
    {
        char lvl[16] = "";
        const char *lp = strstr(content, "\"level\":");
        if (lp) {
            lp += 8;
            size_t li = 0;
            while (*lp >= '0' && *lp <= '9' && li < sizeof(lvl) - 1)
                lvl[li++] = *lp++;
        }
        e.level = atoi(lvl);
        if (e.detail[0]) {
            char lvl2[32];
            snprintf(lvl2, sizeof(lvl2), "%d %s", e.level, e.cls);
            /* keep detail as the display string: "60 Archmage" style */
        }
    }
    e.lfg = json_get_bool(content, "lfg", 1);
    {
        char nums[24] = {0};
        const char *cp = strstr(evobj, "\"created_at\":");
        if (cp) {
            cp += 14;
            size_t ni = 0;
            while (*cp >= '0' && *cp <= '9' && ni < sizeof(nums) - 1)
                nums[ni++] = *cp++;
            e.created_at = strtoull(nums, NULL, 10);
        }
    }
    if (s->on_event)
        s->on_event(&e, pubkey, s->userdata);
    return 1;
}

void sync_poll(SyncState *s)
{
    /* flush the outbox first: pending events get one retry attempt per
     * poll each; those a relay finally accepts leave the queue, the rest
     * stay for the next poll. Walk oldest-first so delivery order is
     * preserved when multiple events are pending. */
    if (s->outbox_n > 0) {
        int start = (s->outbox_next - s->outbox_n + SYNC_OUTBOX_MAX) % SYNC_OUTBOX_MAX;
        for (int k = 0; k < s->outbox_n; k++) {
            int idx = (start + k) % SYNC_OUTBOX_MAX;
            if (s->outbox[idx][0] == '\0')
                continue;
            if (sync_send_event_now(s, s->outbox[idx]) == 0) {
                ws_log_public("outbox: event accepted by a relay (%d left)",
                              s->outbox_n - 1);
                s->outbox[idx][0] = '\0';
                s->outbox_n--;
            }
        }
    }
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
        int got_events = 0;
        int quiet = 0; /* consecutive quiet cycles; relays answer the REQ
                         within a few seconds - a single quiet window is not
                         end-of-stream */
        for (int m = 0; m < 60; m++) {
            char buf[8192];
            int n = ws_recv_text(&c, buf, sizeof(buf));
            if (n == -2) {
                quiet++;
                if (quiet >= 3) /* ~15s with nothing: really done */
                    break;
                continue; /* keep waiting: the backlog may still come */
            }
            quiet = 0;
            if (n < 0) {
                ws_dbg_public("sync: recv error after %d reads (%d events)", m, got_events);
                break;
            }
            if (n == 0) {
                ws_dbg_public("sync: connection closed by relay after %d reads (%d events)",
                       m, got_events);
                break;
            }
            int r = sync_handle_message(s, buf);
            if (r == 1)
                got_events++;
        }
        if (got_events == 0)
            ws_dbg_public("sync: drained relay with ZERO events parsed");
        ws_close(&c);
    }
}
