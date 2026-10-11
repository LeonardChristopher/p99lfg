#ifndef P99LFG_SYNC_H
#define P99LFG_SYNC_H

#include "nostr.h"

#define SYNC_MAX_RELAYS 4

/* Synchronize LFG entries over Nostr relays.
 *
 * Publish: a kind 30078 parameterized replaceable event per server (d tag =
 * "green"/"blue"/"teal"/"red"), tagged #t p99lfg, containing a JSON array of
 * the local player's LFG characters. Because it's parameterized+replaceable,
 * re-publishing replaces the previous state (so clearing LFG = publish empty
 * array or delete).
 *
 * Subscribe: REQ filter kinds [30078], #t [p99lfg]. Entries older than
 * stale_minutes are dropped when rendering.
 */

typedef struct {
    char url[192];
    int enabled;
} SyncRelayCfg;

typedef void (*SyncOnEvent)(const char *content_json, void *userdata);

typedef struct {
    SyncRelayCfg relays[SYNC_MAX_RELAYS];
    int nrelays;
    char privkey[NOSTR_PRIVKEY_HEX];
    char pubkey[NOSTR_PUBKEY_HEX];
    char server[32]; /* d tag: green/blue/teal/red */
    SyncOnEvent on_event;
    void *userdata;
} SyncState;

void sync_init(SyncState *s, const char *keyfile_path, const char *server,
               SyncOnEvent cb, void *userdata);
void sync_add_relay(SyncState *s, const char *url);

/* Publish current LFG list (json array built by caller via sync_build_content). */
int sync_publish(SyncState *s, const char *content_json);

/* Build the content JSON for one character. */
size_t sync_build_content(char *out, size_t outsz, const char *name,
                          const char *zone, const char *detail, int lfg);

#endif
