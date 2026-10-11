#ifndef P99LFG_AES_H
#define P99LFG_AES_H

#include <stddef.h>
#include <stdint.h>

/* AES-256-CBC over full blocks only (len must be a multiple of 16; the
 * caller handles padding). in and out may not alias. */
void aes256_cbc_encrypt(const uint8_t key[32], const uint8_t iv[16],
                        const uint8_t *in, size_t len, uint8_t *out);
void aes256_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16],
                        const uint8_t *in, size_t len, uint8_t *out);

#endif
