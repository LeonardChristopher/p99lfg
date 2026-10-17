#ifndef P99LFG_SYNC_PERSIST_H
#define P99LFG_SYNC_PERSIST_H
#include "sync.h"

/* Persistent relay connections: one background thread per configured
 * relay. Each holds a REQ subscription open with a `since` watermark
 * and feeds arriving EVENTs straight into sync_handle_message - chat
 * delivery becomes real-time instead of waiting for the 5s poll.
 * Reconnects with exponential backoff (1s..60s). The periodic poll
 * stays as a safety net (outbox flush, receipts, slow full rescan). */

/* Start persistent streams for all enabled relays. Idempotent. */
void sync_persist_start(SyncState *s);

/* Stop all streams (blocks briefly for threads to notice the flag). */
void sync_persist_stop(void);

/* Diagnostics: how many streams are currently connected. */
int sync_persist_connected(void);
#endif
