#ifndef P99LFG_CHAT_H
#define P99LFG_CHAT_H

#include "sync.h"

/* NIP-04 style direct messages between LFG players over the same relays.
 * Content is encrypted with AES-256-CBC under an ECDH shared key. */

/* Encrypt, sign and publish a DM to peer_pubkey_hex. Returns 0 if at least
 * one relay accepted it. */
int chat_send_message(SyncState *s, const char *peer_pubkey_hex,
                      const char *message);

/* Try to decrypt a received kind-4 content blob. Returns 0 on success with
 * the plaintext in out; -1 if it isn't for us or is malformed. */
int chat_try_decrypt(const char *privkey_hex, const char *sender_pubkey_hex,
                     const char *content, char *out, size_t outsz);

/* Encrypt a message into the NIP-04 content blob without publishing
 * (test seam / offline use). Returns 0 on success. */
int chat_encrypt_content(const char *privkey_hex, const char *peer_pubkey_hex,
                         const char *message, char *out, size_t outsz);

#endif
