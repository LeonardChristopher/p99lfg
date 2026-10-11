#ifndef P99LFG_RATING_H
#define P99LFG_RATING_H

#include "nostr.h"
#include "sync.h"

#define RATING_MAX 512
#define RATING_VOTES_MAX 64

/* A rating is hidden once this many distinct active raters disagree. */
#define RATING_HIDE_DISAGREES 2

typedef struct {
    char voter[NOSTR_PUBKEY_HEX];
    char rating_id[NOSTR_EVENT_ID_HEX];
    int agree;
    uint64_t created_at;
} Vote;

typedef struct {
    char id[NOSTR_EVENT_ID_HEX];   /* recomputed event id */
    char rater[NOSTR_PUBKEY_HEX];
    char target[NOSTR_PUBKEY_HEX];
    uint64_t created_at;
    char content[512];
    int competent; /* 1..5, 0 = unset */
    int friendly;
    int listens;
    int votes_dirty;
    int nvotes;
    Vote votes[RATING_VOTES_MAX];
} Rating;

typedef struct {
    Rating ratings[RATING_MAX];
    int count;
    Vote votes_pending[RATING_MAX * 4];
    int nvotes_pending;
} RatingStore;

RatingStore *rating_store_new(void);
void rating_store_free(RatingStore *s);

/* Ingest parsed events. Return 1 if the store changed. */
int rating_record(RatingStore *s, const char *rater, const char *target,
                  const char *content, uint64_t created_at);
int vote_record(RatingStore *s, const char *voter, const char *rating_id,
                int agree, uint64_t created_at);

/* Recompute ids and attach pending votes. Call after a poll round. */
void rating_store_finalize(RatingStore *s);

/* Publish over relays. Return 0 if at least one relay accepted. */
int rating_publish(SyncState *s, const char *target_pubkey,
                   int competent, int friendly, int listens);
int vote_publish(SyncState *s, const char *rating_id, int agree);

/* Aggregate visible ratings for a target; -1 if none. */
int rating_summary(RatingStore *s, const char *target,
                   int *competent, int *friendly, int *listens, int *n);

/* Find the event id of rater->target's rating; NULL if not found. */
const char *rating_find_id(RatingStore *s, const char *rater, const char *target);

/* Number of stored ratings. */
int rating_store_count(RatingStore *s);

/* Consensus anti-brigade rule. */
int rating_is_hidden(const RatingStore *s, const Rating *r);

#endif
