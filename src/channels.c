#include "channels.h"
#include "nostr.h"
#include "ws_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHANNEL_KIND 30083  /* own kind-space: nos.lol rejects kind 42 ("not acceptable at this point") */
#define CHANNEL_PREFIX "p99lfg-g-"

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

int channel_send(SyncState *s, const char *channel, const char *from_char,
                 const char *text)
{
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

    char msg[2048];
    nostr_event_message(&ev, msg, sizeof(msg));
    return sync_send_event(s, msg);
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
        o += (size_t)snprintf(out + o, outsz - o, "%s%s:%s",
                              i ? "," : "", m[i].name, m[i].pubkey);
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
            size_t pklen = len - namelen - 1;
            if (pklen >= NOSTR_PUBKEY_HEX)
                pklen = NOSTR_PUBKEY_HEX - 1;
            memcpy(m[n].pubkey, colon + 1, pklen);
            m[n].pubkey[pklen] = '\0';
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
    char mcsv[GROUP_MEMBERS_MAX * 100];
    char wcsv[GROUP_WAIT_MAX * 100];
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
    char msg[2048];
    nostr_event_message(&ev, msg, sizeof(msg));
    return sync_send_event(s, msg);
}

int group_state_parse(const char *content,
                      GroupMember *members, int *nm,
                      GroupMember *wait, int *nw)
{
    char mcsv[GROUP_MEMBERS_MAX * 100] = "";
    char wcsv[GROUP_WAIT_MAX * 100] = "";
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
    ev.kind = 30083;
    snprintf(ev.dtag, sizeof(ev.dtag), "p99lfg-g-%s", channel);
    snprintf(ev.content, sizeof(ev.content),
             leave ? "{\"leave\":\"%s:%s\"}" : "{\"join\":\"%s:%s\"}",
             name, pubkey);
    if (nostr_sign_event(&ev, s->privkey) != 0)
        return -1;
    char msg[2048];
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
    char msg[2048];
    nostr_event_message(&ev, msg, sizeof(msg));
    return sync_send_event(s, msg);
}
