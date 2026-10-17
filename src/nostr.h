#ifndef P99LFG_NOSTR_H
#define P99LFG_NOSTR_H

#include <stddef.h>
#include <stdint.h>

#define NOSTR_PUBKEY_HEX 65
#define NOSTR_PRIVKEY_HEX 65
#define NOSTR_EVENT_ID_HEX 65
#define NOSTR_SIG_HEX 129

/* Kind used for LFG broadcast events. */
#define NOSTR_KIND_LFG 30078 /* parameterized replaceable event */

/* Canonical tag for all p99lfg events. */
#define LFG_TAG "p99lfg"

typedef struct {
    char id[NOSTR_EVENT_ID_HEX];
    char pubkey[NOSTR_PUBKEY_HEX];
    uint64_t created_at;
    int kind;
    /* 2048: a full 6-member + 2-wait roster CSV is ~1092 bytes raw and
     * must never truncate mid-JSON (truncation corrupts the parsed
     * roster). Escape buffers scale with it (2x + slack). */
    char content[2048];
    char sig[NOSTR_SIG_HEX];
    char dtag[128];
    /* kind-4 DM only: recipient pubkey tag ("p"). Empty for LFG events. */
    char ptag[NOSTR_PUBKEY_HEX];
} NostrEvent;

/* Load or create identity key at path (hex secp256k1 secret key). Returns 0 on success. */
int nostr_identity_load(const char *path, char pubkey_out[NOSTR_PUBKEY_HEX]);

/* Import an existing hex secret key (e.g. from the Windows app) and persist
 * it at path. Returns 0 on success. */
int nostr_import_key(const char *path, const char *privkey_hex,
                     char pubkey_out[NOSTR_PUBKEY_HEX]);

/* Serialize the canonical NIP-01 payload used for the event id. */
void nostr_serialize_for_id(const NostrEvent *ev, char *out, size_t outsz);

/* Compute id and schnorr-sign. Fills ev->id and ev->sig. Returns 0 on success. */
int nostr_sign_event(NostrEvent *ev, const char *privkey_hex);

/* Build the relay message `["EVENT", <ev>]`. Returns length written. */
size_t nostr_event_message(const NostrEvent *ev, char *out, size_t outsz);

/* Build a REQ filter message for the p99lfg tag. */
size_t nostr_req_message(const char *subid, char *out, size_t outsz);

/* REQ with a since watermark (persistent-connection resume). */
size_t nostr_req_message_since(const char *subid, char *out, size_t outsz,
                               uint64_t since);

#endif
