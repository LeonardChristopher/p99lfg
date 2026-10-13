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

/* Try to parse a received event as a channel message. Returns 1 and fills
 * the out params if it is one (kind 42 with our tag family), else 0. */
int channel_parse(const char *evobj_json, char *channel_out, size_t chsz,
                  char *sender_out, size_t sesz, char *text_out, size_t txsz,
                  uint64_t *created_at_out);

#endif

/* Publish the display-name profile event (kind 30082). */
int profile_announce(SyncState *s, const char *displayname);
