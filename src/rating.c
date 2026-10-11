/* Player ratings over Nostr:
 *   kind 30079 (replaceable, d = target pubkey)  - one rating per rater/target
 *   kind 30080 (replaceable, d = rating event id) - agree/disagree votes
 *
 * Consensus anti-brigade rule (client-side, since Nostr can't censor):
 * a rating is hidden when at least RATING_HIDE_DISAGREES distinct raters
 * (pubkeys that have themselves published a rating = "active") disagree with
 * it. Fresh keys can't brigade because they haven't rated anyone.
 */
#include "rating.h"

#include "nostr.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATING_KIND 30079
#define VOTE_KIND 30080

/* ---- tiny JSON helpers (same style as sync.c) ---- */

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

static int json_get_int(const char *json, const char *key, int dflt)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return dflt;
    p += strlen(pat);
    while (*p == ' ' || *p == ':')
        p++;
    return atoi(p);
}

static int hex_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int is_hex64(const char *s)
{
    if (strlen(s) != 64)
        return 0;
    for (int i = 0; i < 64; i++)
        if (hex_nib(s[i]) < 0)
            return 0;
    return 1;
}

/* ---- local store ---- */

RatingStore *rating_store_new(void)
{
    return calloc(1, sizeof(RatingStore));
}

void rating_store_free(RatingStore *s)
{
    free(s);
}

static Rating *find_rating(RatingStore *s, const char *rater, const char *target)
{
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->ratings[i].rater, rater) == 0 &&
            strcmp(s->ratings[i].target, target) == 0)
            return &s->ratings[i];
    return NULL;
}

static Rating *add_rating(RatingStore *s, const char *rater, const char *target)
{
    if (s->count >= RATING_MAX)
        return NULL;
    Rating *r = &s->ratings[s->count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->rater, sizeof(r->rater), "%s", rater);
    snprintf(r->target, sizeof(r->target), "%s", target);
    return r;
}

static Vote *find_vote(RatingStore *s, const char *voter, const char *rating_id)
{
    for (int i = 0; i < s->count; i++)
        for (int j = 0; j < s->ratings[i].nvotes; j++)
            if (strcmp(s->ratings[i].votes[j].voter, voter) == 0 &&
                strcmp(s->ratings[i].votes[j].rating_id, rating_id) == 0)
                return &s->ratings[i].votes[j];
    return NULL;
}

/* A pubkey is "active" once they have published at least one rating. */
static int rater_is_active(RatingStore *s, const char *pubkey)
{
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->ratings[i].rater, pubkey) == 0)
            return 1;
    return 0;
}

/* Record a parsed kind-30079 rating. Replaceable per rater+target, so a
 * later event with a newer created_at overwrites. Returns 1 if stored. */
int rating_record(RatingStore *s, const char *rater, const char *target,
                 const char *content, uint64_t created_at)
{
    if (!is_hex64(rater) || !is_hex64(target))
        return 0;
    Rating *r = find_rating(s, rater, target);
    if (r) {
        if (created_at < r->created_at)
            return 0; /* keep newest */
    } else {
        r = add_rating(s, rater, target);
        if (!r)
            return 0;
    }
    r->created_at = created_at;
    snprintf(r->content, sizeof(r->content), "%s", content);
    /* content: {"competent":1..5,"friendly":1..5,"listens":1..5} */
    r->competent = json_get_int(content, "competent", 0);
    r->friendly = json_get_int(content, "friendly", 0);
    r->listens = json_get_int(content, "listens", 0);
    /* recompute the event id so votes can attach */
    r->votes_dirty = 1;
    return 1;
}

/* Record a kind-30080 vote on a rating id. Returns 1 if stored. */
int vote_record(RatingStore *s, const char *voter, const char *rating_id,
                int agree, uint64_t created_at)
{
    if (!is_hex64(voter) || !is_hex64(rating_id))
        return 0;
    Vote *v = find_vote(s, voter, rating_id);
    if (v) {
        if (created_at < v->created_at)
            return 0;
        v->agree = agree;
        v->created_at = created_at;
        return 1;
    }
    /* attach to the rating this id belongs to */
    for (int i = 0; i < s->count; i++) {
        if (s->ratings[i].created_at == 0)
            continue;
        /* we match by recomputed rater+target+time event id later; simplest
         * robust match: votes reference the id they saw, so store all votes
         * on the matching rating if we can find it via content hash later.
         * For now keep votes in a flat pending list attached below. */
    }
    if (s->nvotes_pending >= RATING_MAX * 4)
        return 0;
    Vote *p = &s->votes_pending[s->nvotes_pending++];
    memset(p, 0, sizeof(*p));
    snprintf(p->voter, sizeof(p->voter), "%s", voter);
    snprintf(p->rating_id, sizeof(p->rating_id), "%s", rating_id);
    p->agree = agree;
    p->created_at = created_at;
    return 1;
}

/* Recompute each rating's event id (deterministic from its fields) and pull
 * matching pending votes onto it. */
void rating_store_finalize(RatingStore *s)
{
    for (int i = 0; i < s->count; i++) {
        Rating *r = &s->ratings[i];
        if (!r->votes_dirty)
            continue;
        r->votes_dirty = 0;
        r->nvotes = 0;
        /* recompute id: serialize like nostr_serialize_for_id does */
        NostrEvent ev;
        memset(&ev, 0, sizeof(ev));
        snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", r->rater);
        ev.created_at = r->created_at;
        ev.kind = RATING_KIND;
        snprintf(ev.dtag, sizeof(ev.dtag), "%s", r->target);
        snprintf(ev.content, sizeof(ev.content), "%s", r->content);
        char serial[2048];
        nostr_serialize_for_id(&ev, serial, sizeof(serial));
        uint8_t id32[32];
        sha256(serial, strlen(serial), id32);
        static const char hexd[] = "0123456789abcdef";
        for (int k = 0; k < 32; k++) {
            r->id[k * 2] = hexd[id32[k] >> 4];
            r->id[k * 2 + 1] = hexd[id32[k] & 15];
        }
        r->id[64] = '\0';
        /* attach pending votes matching this id */
        for (int j = 0; j < s->nvotes_pending && r->nvotes < RATING_VOTES_MAX; j++) {
            if (strcmp(s->votes_pending[j].rating_id, r->id) == 0)
                r->votes[r->nvotes++] = s->votes_pending[j];
        }
    }
    /* also re-attach when new pending votes arrive for known ids */
    for (int j = 0; j < s->nvotes_pending; j++) {
        for (int i = 0; i < s->count; i++) {
            Rating *r = &s->ratings[i];
            if (strcmp(r->id, s->votes_pending[j].rating_id) != 0)
                continue;
            int dup = 0;
            for (int k = 0; k < r->nvotes; k++)
                if (strcmp(r->votes[k].voter, s->votes_pending[j].voter) == 0)
                    dup = 1;
            if (!dup && r->nvotes < RATING_VOTES_MAX)
                r->votes[r->nvotes++] = s->votes_pending[j];
        }
    }
}

/* Publish a rating (kind 30079, replaceable d=target pubkey). */
int rating_publish(SyncState *s, const char *target_pubkey,
                   int competent, int friendly, int listens)
{
    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", s->pubkey);
    ev.created_at = (uint64_t)time(NULL);
    ev.kind = RATING_KIND;
    snprintf(ev.dtag, sizeof(ev.dtag), "%s", target_pubkey);
    snprintf(ev.content, sizeof(ev.content),
             "{\"competent\":%d,\"friendly\":%d,\"listens\":%d}",
             competent, friendly, listens);
    if (nostr_sign_event(&ev, s->privkey) != 0)
        return -1;
    char msg[2048];
    nostr_event_message(&ev, msg, sizeof(msg));
    return sync_send_event(s, msg);
}

/* Publish an agree/disagree vote on a rating (kind 30080, d=rating id). */
int vote_publish(SyncState *s, const char *rating_id, int agree)
{
    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", s->pubkey);
    ev.created_at = (uint64_t)time(NULL);
    ev.kind = VOTE_KIND;
    snprintf(ev.dtag, sizeof(ev.dtag), "%s", rating_id);
    snprintf(ev.content, sizeof(ev.content), "{\"agree\":%s}", agree ? "true" : "false");
    if (nostr_sign_event(&ev, s->privkey) != 0)
        return -1;
    char msg[2048];
    nostr_event_message(&ev, msg, sizeof(msg));
    return sync_send_event(s, msg);
}

/* Aggregate: averages over visible (non-hidden) ratings for a target. */
int rating_summary(RatingStore *s, const char *target,
                   int *competent, int *friendly, int *listens, int *n)
{
    int c = 0, f = 0, l = 0, cnt = 0;
    for (int i = 0; i < s->count; i++) {
        Rating *r = &s->ratings[i];
        if (strcmp(r->target, target) != 0)
            continue;
        if (rating_is_hidden(s, r))
            continue;
        c += r->competent;
        f += r->friendly;
        l += r->listens;
        cnt++;
    }
    if (cnt == 0)
        return -1;
    *competent = (c * 10 / cnt + 5) / 10;
    *friendly = (f * 10 / cnt + 5) / 10;
    *listens = (l * 10 / cnt + 5) / 10;
    *n = cnt;
    return 0;
}

/* Consensus rule: hidden when >= RATING_HIDE_DISAGREES distinct *active*
 * raters disagree. Everyone running this rule sees the same result. */
int rating_is_hidden(const RatingStore *s, const Rating *r)
{
    int active_disagrees = 0;
    for (int k = 0; k < r->nvotes; k++) {
        if (!r->votes[k].agree && rater_is_active((RatingStore *)s, r->votes[k].voter))
            active_disagrees++;
    }
    return active_disagrees >= RATING_HIDE_DISAGREES ? 1 : 0;
}

const char *rating_find_id(RatingStore *s, const char *rater, const char *target)
{
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->ratings[i].rater, rater) == 0 &&
            strcmp(s->ratings[i].target, target) == 0)
            return s->ratings[i].id;
    return NULL;
}

int rating_store_count(RatingStore *s)
{
    return s->count;
}
