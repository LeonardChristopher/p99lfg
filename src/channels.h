#ifndef P99LFG_CHANNELS_H
#define P99LFG_CHANNELS_H

#include "sync.h"

/* Group/raid chat channels (Phase 1 of the voice roadmap):
 * a channel is an agreed Nostr tag - p99lfg-g-<name> for groups,
 * p99lfg-r-<name> for raids. Messages are kind-42 events carrying the
 * channel tag + #t p99lfg so our existing subscription ingests them.
 * Membership = whoever knows the tag. Owner invites via DM.
 */

#define CHANNEL_NAME_MAX 32
#define CHANNEL_MSG_MAX 512

typedef struct {
    char name[CHANNEL_NAME_MAX];   /* tag suffix, e.g. "sebgroup" */
    int is_raid;
} Channel;

/* ---- 6-member groups with a 2-slot waitlist ----
 * State is a replaceable kind-30081 event (d tag = p99lfg-g-<name>):
 *   {"members":"Name:pubkey,Name:pubkey", "wait":"Name:pubkey"}
 * The lowest-pubkey member is the host and consolidates join/leave
 * requests (kind-42 control messages: {"join":"Name:pubkey"} /
 * {"leave":"Name:pubkey"}). */
#define GROUP_MEMBERS_MAX 6
#define GROUP_WAIT_MAX 2
#define GROUP_STATE_KIND 30081

typedef struct {
    char name[64];
    char pubkey[NOSTR_PUBKEY_HEX];
    int officer; /* 1 = may invite others (leader grants) */
} GroupMember;

/* Publish the group state. Returns 0 if a relay accepted. */
int group_state_publish(SyncState *s, const char *channel,
                        GroupMember *members, int nm,
                        GroupMember *wait, int nw);

/* Parse a state event's content. Returns 1 on success. */
int group_state_parse(const char *content,
                      GroupMember *members, int *nm,
                      GroupMember *wait, int *nw);

/* Send a join or leave request (control message on the channel). */
int group_control_send(SyncState *s, const char *channel,
                       const char *name, const char *pubkey, int leave);

/* Publish a chat message to the channel. Returns 0 if a relay accepted. */
int channel_send(SyncState *s, const char *channel, const char *from_char,
                 const char *text);

/* Like channel_send, but also returns the signed event id and the raw
 * ["EVENT",...] wire message, so callers can spool them for the
 * delivery-audit reconciliation (receipts). id_out/raw_out may be NULL. */
int channel_send_ex(SyncState *s, const char *channel, const char *from_char,
                     const char *text, char *id_out, size_t idsz,
                     char *raw_out, size_t rawsz);

int channel_parse(const char *evobj_json, char *channel_out, size_t chsz,
                  char *sender_out, size_t sesz, char *text_out, size_t txsz,
                  uint64_t *created_at_out);

/* ---- Chat delivery audit / reconciliation ----
 *
 * Problem being solved: a chat message can be visible on the SENDER's
 * screen (local append) while no other member ever received it (no
 * relay accepted the send, or a relay lost it). No one notices.
 *
 * Mechanism ("receipts"):
 * - Every client records the event ids of channel-chat messages it has
 *   seen (received OR sent) in a per-channel ledger.
 * - Senders also keep a copy of each sent chat message (raw ["EVENT",...])
 *   in a rebroadcast spool.
 * - Periodically, each client publishes a RECEIPT control message on the
 *   channel: {"rcpt":"id,id,..."} - everything it has seen so far.
 * - On receiving a peer's receipt, a client compares it against its sent
 *   spool: any message the peer has NOT seen is re-broadcast verbatim
 *   (identical event -> relays and dedup make this a no-op for clients
 *   that already have it, a delivery for the one that does not).
 * This makes "sent but undelivered" a detectable, self-healing state
 * instead of a silent loss. */
#define RECON_SEEN_MAX 64      /* per-channel chat ids remembered */
#define RECON_SPOOL_MAX 16     /* sent-but-unacknowledged messages kept */
#define RECON_SPOOL_MSG 4200   /* raw ["EVENT",...] buffer size */

typedef struct {
    /* ids of chat messages this client has seen (received or sent) */
    char seen[RECON_SEEN_MAX][NOSTR_EVENT_ID_HEX];
    int seen_n;
    /* raw sent messages awaiting acknowledgement by all members */
    char spool[RECON_SPOOL_MAX][RECON_SPOOL_MSG];
    char spool_ids[RECON_SPOOL_MAX][NOSTR_EVENT_ID_HEX];
    int spool_n;
    uint64_t last_receipt_at; /* throttle: publish receipts at most 1/15s */
} ReconState;

/* Record a chat message id as seen (received or sent). Idempotent. */
void recon_note_seen(ReconState *r, const char *event_id);

/* Record a chat message this client SENT (raw wire message + its id).
 * Kept for rebroadcast until acknowledged. Ring: oldest evicted. */
void recon_note_sent(ReconState *r, const char *event_id, const char *raw_msg);

/* True if the id is already in the seen ledger. */
int recon_has_seen(ReconState *r, const char *event_id);

/* Publish a receipt (list of seen chat ids) on the channel. Throttled to
 * one per 15s. Returns 0 if sent/queued, -1 if throttled or error. */
int recon_send_receipt(SyncState *s, ReconState *r, const char *channel);

/* Feed an incoming channel control/content message to reconciliation.
 * If it is a peer's receipt ("rcpt"), removes acknowledged ids from the
 * spool; for chat ids, notes them seen. Returns 1 if a receipt was
 * processed, 0 if not a receipt, -1 on error. */
int recon_on_message(ReconState *r, const char *content);

/* Re-broadcast every spooled message the given peer receipt did NOT
 * acknowledge. Returns the number re-broadcast (0 = all acknowledged). */
int recon_rebroadcast_missing(SyncState *s, ReconState *r, const char *peer_receipt_ids);

#endif

/* Publish the display-name profile event (kind 30082). */
int profile_announce(SyncState *s, const char *displayname);
