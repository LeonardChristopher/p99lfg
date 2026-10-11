#ifndef P99LFG_SYNC_H
#define P99LFG_SYNC_H

#include "nostr.h"

#define SYNC_MAX_RELAYS 4

/* Synchronize LFG entries over Nostr relays.
 *
 * Publish: a kind 30078 parameterized replaceable event per server (d tag =
 * "green"/"blue"/...), tagged #t p99lfg, whose content is a JSON object
 * {"v":1,"name":...,"zone":...,"detail":...,"lfg":bool} for the local
 * player's LFG characters. Re-publishing replaces the previous state.
 *
 * Subscribe: REQ filter kinds [30078], #t [p99lfg].
 */

/* Callback for non-LFG, non-DM event kinds (ratings, votes): kind, pubkey,
 * d tag, content, created_at. */
typedef void (*SyncOnGeneric)(int kind, const char *pubkey, const char *dtag,
                               const char *content, uint64_t created_at, void *userdata);
typedef struct {
    char url[192];
    int enabled;
    int connected;
} SyncRelayState;

typedef struct {
    char name[64];
    char zone[64];
    char detail[96];
    int lfg;
} SyncRemoteEntry;

/* Callback when a remote player's LFG event arrives. */
typedef void (*SyncOnEvent)(const SyncRemoteEntry *entry, const char *pubkey, void *userdata);

/* Callback when a kind-4 DM arrives: sender pubkey + NIP-04 content blob. */
typedef void (*SyncOnDm)(const char *sender_pubkey, const char *content, void *userdata);

typedef struct {
    SyncRelayState relays[SYNC_MAX_RELAYS];
    int nrelays;
    char privkey[NOSTR_PRIVKEY_HEX];
    char pubkey[NOSTR_PUBKEY_HEX];
    char server[32];
    SyncOnEvent on_event;
    SyncOnDm on_dm;
    SyncOnGeneric on_generic;
    void *userdata;
    void *dm_userdata;
    void *generic_userdata;
} SyncState;

void sync_init(SyncState *s, const char *keyfile_path, const char *server,
               SyncOnEvent cb, void *userdata);
void sync_add_relay(SyncState *s, const char *url);

/* Reload the identity (privkey+pubkey) from the key file, e.g. after
 * nostr_import_key replaced it. Returns 0 on success. */
int sync_reload_key(SyncState *s, const char *keyfile_path);

size_t sync_build_content(char *out, size_t outsz, const char *name,
                          const char *zone, const char *detail, int lfg);

int sync_publish(SyncState *s, const char *content_json);

/* One non-blocking-ish pump: for each relay, connect if needed, send REQ,
 * drain any arrived events into on_event. Call periodically (e.g. 5s). */
void sync_poll(SyncState *s);

/* Parse a relay message ("[\"EVENT\",sub,{...}]" or "[\"OK\",...]"). Returns 1
 * if an EVENT was parsed and cb fired, 0 if not, -1 on malformed input. */
int sync_handle_message(SyncState *s, const char *msg);

/* Send a raw ["EVENT",...] message to all enabled relays. Returns 0 if at
 * least one accepted it. */
int sync_send_event(SyncState *s, const char *msg);


/* Set the DM callback (may be NULL). */
#define sync_set_dm_callback(s, cb, ud) do { (s)->on_dm = (cb); (s)->dm_userdata = (ud); } while (0)
#define sync_set_generic_callback(s, cb, ud) do { (s)->on_generic = (cb); (s)->generic_userdata = (ud); } while (0)

#endif
