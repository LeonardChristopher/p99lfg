#include "channels.h"
#include "nostr.h"
#include "ws_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHANNEL_KIND 9  /* regular event: every message persists. 30000-39999 are REPLACEABLE (same pubkey+kind+d deletes the previous event on the relay) - using 30083 here silently deleted older messages and let chat replace pending join/leave control events. */
#define CHANNEL_PREFIX "p99lfg-g-"
#define CHANNEL_CTL_KIND 9  /* join/leave control: ephemeral regular event. Must NOT share a replaceable kind+dtag with chat - a chat message would delete a pending join request on the relay. */

static void json_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 7 < outsz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

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

int channel_send_ex(SyncState *s, const char *channel, const char *from_char,
                     const char *text, char *id_out, size_t idsz,
                     char *raw_out, size_t rawsz)
{
    if (id_out && idsz)
        id_out[0] = '\0';
    if (raw_out && rawsz)
        raw_out[0] = '\0';
    if (!channel[0] || !text[0])
        return -1;

    /* build the content: from-character rides inside like chat DMs */
    char inner[CHANNEL_MSG_MAX + 64];
    if (from_char && from_char[0])
        snprintf(inner, sizeof(inner), "{\"from\":\"%s\",\"msg\":\"%s\"}",
                 from_char, text);
    else
        snprintf(inner, sizeof(inner), "{\"msg\":\"%s\"}", text);

    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", s->pubkey);
    ev.created_at = (uint64_t)time(NULL);
    ev.kind = CHANNEL_KIND;
    /* channel goes in the d tag: p99lfg-g-<name>; t tag stays p99lfg */
    snprintf(ev.dtag, sizeof(ev.dtag), "%s%s", CHANNEL_PREFIX, channel);
    snprintf(ev.content, sizeof(ev.content), "%s", inner);
    if (nostr_sign_event(&ev, s->privkey) != 0)
        return -1;

    char msg[4200];
    nostr_event_message(&ev, msg, sizeof(msg));
    if (id_out && idsz)
        snprintf(id_out, idsz, "%s", ev.id);
    if (raw_out && rawsz)
        snprintf(raw_out, rawsz, "%s", msg);
    return sync_send_event(s, msg);
}

int channel_send(SyncState *s, const char *channel, const char *from_char,
                 const char *text)
{
    return channel_send_ex(s, channel, from_char, text, NULL, 0, NULL, 0);
}

int channel_parse(const char *evobj_json, char *channel_out, size_t chsz,
                  char *sender_out, size_t sesz, char *text_out, size_t txsz,
                  uint64_t *created_at_out)
{
    /* kind must be 42 */
    const char *kp = strstr(evobj_json, "\"kind\":");
    if (!kp)
        return 0;
    long kind = strtol(kp + 7, NULL, 10);
    if (kind != CHANNEL_KIND)
        return 0;

    /* d tag = channel; sits in tags array as ["d","p99lfg-g-<name>"] */
    const char *tp = strstr(evobj_json, "[\"d\",\"" CHANNEL_PREFIX);
    if (!tp)
        return 0;
    tp += 6 + strlen(CHANNEL_PREFIX);
    size_t ci = 0;
    while (*tp && *tp != '"' && ci < chsz - 1)
        channel_out[ci++] = *tp++;
    channel_out[ci] = '\0';
    if (ci == 0)
        return 0;

    if (json_get_string(evobj_json, "pubkey", sender_out, sesz) < 0)
        return 0;
    if (json_get_string(evobj_json, "content", text_out, txsz) < 0)
        return 0;
    {
        char nums[24] = {0};
        const char *cp = strstr(evobj_json, "\"created_at\":");
        if (cp) {
            cp += 14;
            size_t ni = 0;
            while (*cp >= '0' && *cp <= '9' && ni < sizeof(nums) - 1)
                nums[ni++] = *cp++;
            *created_at_out = strtoull(nums, NULL, 10);
        }
    }
    return 1;
}

/* ---- 6-member groups with 2-slot waitlist ---- */

static void group_members_to_csv(GroupMember *m, int n, char *out, size_t outsz)
{
    out[0] = '\0';
    size_t o = 0;
    for (int i = 0; i < n && o < outsz; i++)
        o += (size_t)snprintf(out + o, outsz - o, "%s%s:%s:%d",
                              i ? "," : "", m[i].name, m[i].pubkey,
                              m[i].officer);
}

static int group_csv_to_members(const char *csv, GroupMember *m, int max)
{
    int n = 0;
    const char *p = csv;
    while (p && *p && n < max) {
        const char *c = strchr(p, ',');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        const char *colon = memchr(p, ':', len);
        if (colon && colon > p) {
            size_t namelen = (size_t)(colon - p);
            if (namelen >= sizeof(m[n].name))
                namelen = sizeof(m[n].name) - 1;
            memcpy(m[n].name, p, namelen);
            m[n].name[namelen] = '\0';
            const char *rest = colon + 1;
            size_t restlen = len - namelen - 1;
            const char *colon2 = memchr(rest, ':', restlen);
            size_t pklen = colon2 ? (size_t)(colon2 - rest) : restlen;
            if (pklen >= NOSTR_PUBKEY_HEX)
                pklen = NOSTR_PUBKEY_HEX - 1;
            memcpy(m[n].pubkey, rest, pklen);
            m[n].pubkey[pklen] = '\0';
            m[n].officer = (colon2 && restlen - pklen - 1 >= 1 &&
                            rest[pklen + 1] == '1') ? 1 : 0;
            n++;
        }
        p = c ? c + 1 : NULL;
    }
    return n;
}

int group_state_publish(SyncState *s, const char *channel,
                        GroupMember *members, int nm,
                        GroupMember *wait, int nw)
{
    char mcsv[GROUP_MEMBERS_MAX * 200];
    char wcsv[GROUP_WAIT_MAX * 200];
    group_members_to_csv(members, nm, mcsv, sizeof(mcsv));
    group_members_to_csv(wait, nw, wcsv, sizeof(wcsv));

    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", s->pubkey);
    ev.created_at = (uint64_t)time(NULL);
    ev.kind = GROUP_STATE_KIND;
    snprintf(ev.dtag, sizeof(ev.dtag), "p99lfg-g-%s", channel);
    snprintf(ev.content, sizeof(ev.content),
             "{\"members\":\"%s\",\"wait\":\"%s\",\"v\":1}",
             mcsv, wcsv);
    if (nostr_sign_event(&ev, s->privkey) != 0)
        return -1;
    char msg[4200];
    nostr_event_message(&ev, msg, sizeof(msg));
    return sync_send_event(s, msg);
}

int group_state_parse(const char *content,
                      GroupMember *members, int *nm,
                      GroupMember *wait, int *nw)
{
    char mcsv[GROUP_MEMBERS_MAX * 200] = "";
    char wcsv[GROUP_WAIT_MAX * 200] = "";
    if (json_get_string(content, "members", mcsv, sizeof(mcsv)) < 0)
        return 0;
    json_get_string(content, "wait", wcsv, sizeof(wcsv));
    *nm = group_csv_to_members(mcsv, members, GROUP_MEMBERS_MAX);
    *nw = group_csv_to_members(wcsv, wait, GROUP_WAIT_MAX);
    return 1;
}

int group_control_send(SyncState *s, const char *channel,
                       const char *name, const char *pubkey, int leave)
{
    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", s->pubkey);
    ev.created_at = (uint64_t)time(NULL);
    ev.kind = CHANNEL_CTL_KIND;
    snprintf(ev.dtag, sizeof(ev.dtag), "p99lfg-g-%s", channel);
    snprintf(ev.content, sizeof(ev.content),
             leave ? "{\"leave\":\"%s:%s\"}" : "{\"join\":\"%s:%s\"}",
             name, pubkey);
    if (nostr_sign_event(&ev, s->privkey) != 0)
        return -1;
    char msg[4200];
    nostr_event_message(&ev, msg, sizeof(msg));
    return sync_send_event(s, msg);
}

/* ---- profile announce (kind 30082): display name for the key ----
 * Published at startup and on name change so other users can resolve
 * display names without waiting for an LFG event. */
int profile_announce(SyncState *s, const char *displayname)
{
    if (!displayname || !displayname[0])
        return 0;
    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", s->pubkey);
    ev.created_at = (uint64_t)time(NULL);
    ev.kind = 30082;
    snprintf(ev.dtag, sizeof(ev.dtag), "profile");
    snprintf(ev.content, sizeof(ev.content), "{\"user\":\"%s\"}", displayname);
    if (nostr_sign_event(&ev, s->privkey) != 0)
        return -1;
    char msg[4200];
    nostr_event_message(&ev, msg, sizeof(msg));
    return sync_send_event(s, msg);
}

/* ---- Chat delivery audit / reconciliation ("receipts") ----
 * See channels.h for the design rationale. */

void recon_note_seen(ReconState *r, const char *event_id)
{
    if (!event_id || !event_id[0])
        return;
    if (recon_has_seen(r, event_id))
        return;
    snprintf(r->seen[r->seen_n % RECON_SEEN_MAX], NOSTR_EVENT_ID_HEX, "%s",
             event_id);
    r->seen_n++;
}

int recon_has_seen(ReconState *r, const char *event_id)
{
    if (!event_id || !event_id[0])
        return 0;
    int lim = r->seen_n < RECON_SEEN_MAX ? r->seen_n : RECON_SEEN_MAX;
    for (int i = 0; i < lim; i++)
        if (strcmp(r->seen[i], event_id) == 0)
            return 1;
    return 0;
}

void recon_note_sent(ReconState *r, const char *event_id, const char *raw_msg)
{
    if (!event_id || !event_id[0])
        return;
    int slot = -1;
    for (int i = 0; i < r->spool_n && i < RECON_SPOOL_MAX; i++)
        if (strcmp(r->spool_ids[i], event_id) == 0)
            return; /* already spooled */
    if (r->spool_n < RECON_SPOOL_MAX)
        slot = r->spool_n++;
    else
        slot = 0; /* ring full: evict oldest (index 0 after compaction) */
    snprintf(r->spool_ids[slot], NOSTR_EVENT_ID_HEX, "%s", event_id);
    snprintf(r->spool[slot], RECON_SPOOL_MSG, "%s", raw_msg);
}

int recon_send_receipt(SyncState *s, ReconState *r, const char *channel)
{
    if (!channel[0])
        return -1;
    uint64_t now = (uint64_t)time(NULL);
    if (r->last_receipt_at && now - r->last_receipt_at < 15)
        return -1; /* throttled */
    /* build the id list; newest-first order is fine - it's a set */
    char ids[RECON_SEEN_MAX * (NOSTR_EVENT_ID_HEX + 1) + 8];
    size_t o = 0;
    int lim = r->seen_n < RECON_SEEN_MAX ? r->seen_n : RECON_SEEN_MAX;
    for (int i = lim - 1; i >= 0 && o + sizeof(ids) - o > NOSTR_EVENT_ID_HEX + 2; i--) {
        if (o)
            ids[o++] = ',';
        snprintf(ids + o, NOSTR_EVENT_ID_HEX + 1, "%s", r->seen[i]);
        o += strlen(r->seen[i]);
    }
    ids[o] = '\0';
    char content[RECON_SEEN_MAX * (NOSTR_EVENT_ID_HEX + 1) + 64];
    snprintf(content, sizeof(content), "{\"rcpt\":\"%s\",\"v\":1}", ids);
    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", s->pubkey);
    ev.created_at = now;
    ev.kind = CHANNEL_CTL_KIND;
    snprintf(ev.dtag, sizeof(ev.dtag), "%s%s", CHANNEL_PREFIX, channel);
    snprintf(ev.content, sizeof(ev.content), "%s", content);
    if (nostr_sign_event(&ev, s->privkey) != 0)
        return -1;
    char msg[RECON_SPOOL_MSG];
    nostr_event_message(&ev, msg, sizeof(msg));
    int rc = sync_send_event(s, msg);
    if (rc == 0)
        r->last_receipt_at = now;
    return rc;
}

int recon_on_message(ReconState *r, const char *content)
{
    if (!content)
        return -1;
    const char *rp = strstr(content, "\"rcpt\"");
    if (!rp)
        return 0; /* not a receipt */
    char ids[RECON_SEEN_MAX * (NOSTR_EVENT_ID_HEX + 1) + 8];
    if (json_get_string(content, "rcpt", ids, sizeof(ids)) < 0)
        return -1;
    /* remove acknowledged ids from the spool */
    int w = 0;
    for (int i = 0; i < r->spool_n; i++) {
        char pat[NOSTR_EVENT_ID_HEX + 3];
        snprintf(pat, sizeof(pat), ",%s,", r->spool_ids[i]);
        char *p = strstr(ids, r->spool_ids[i]);
        (void)p; (void)pat;
        /* membership test: the id must appear as a whole element */
        int acked = 0;
        {
            /* walk the comma-separated list once */
            const char *cur = ids;
            while (cur && *cur) {
                const char *comma = strchr(cur, ',');
                size_t len = comma ? (size_t)(comma - cur) : strlen(cur);
                if (len == strlen(r->spool_ids[i]) &&
                    strncmp(cur, r->spool_ids[i], len) == 0) {
                    acked = 1;
                    break;
                }
                cur = comma ? comma + 1 : NULL;
            }
        }
        if (!acked) {
            if (w != i) {
                snprintf(r->spool[w], RECON_SPOOL_MSG, "%s", r->spool[i]);
                snprintf(r->spool_ids[w], NOSTR_EVENT_ID_HEX, "%s",
                         r->spool_ids[i]);
            }
            w++;
        }
    }
    r->spool_n = w;
    return 1;
}

int recon_rebroadcast_missing(SyncState *s, ReconState *r,
                              const char *peer_receipt_ids)
{
    if (!peer_receipt_ids)
        return 0;
    int sent = 0;
    for (int i = 0; i < r->spool_n; i++) {
        int acked = 0;
        const char *cur = peer_receipt_ids;
        while (cur && *cur) {
            const char *comma = strchr(cur, ',');
            size_t len = comma ? (size_t)(comma - cur) : strlen(cur);
            if (len == strlen(r->spool_ids[i]) &&
                strncmp(cur, r->spool_ids[i], len) == 0) {
                acked = 1;
                break;
            }
            cur = comma ? comma + 1 : NULL;
        }
        if (!acked) {
            if (sync_send_event(s, r->spool[i]) == 0)
                sent++;
        }
    }
    return sent;
}
