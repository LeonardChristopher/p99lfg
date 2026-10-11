/* NIP-04 style direct messages between LFG players, on top of the existing
 * Nostr relay plumbing. Content is "<base64 ciphertext>?iv=<base64 iv>",
 * encrypted with AES-256-CBC under an ECDH shared key (secp256k1). */
#include "chat.h"

#include "aes.h"
#include "nostr.h"
#include "sha256.h"
#include "ws_client.h"

#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <secp256k1_extrakeys.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#define CHAT_KIND 4 /* NIP-04 direct message */

/* ---- base64 ---- */
static void b64_encode(const uint8_t *in, size_t len, char *out)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    while (len >= 3) {
        uint32_t v = ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8) | in[2];
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = b64[(v >> 6) & 63];
        out[o++] = b64[v & 63];
        in += 3;
        len -= 3;
    }
    if (len == 1) {
        uint32_t v = (uint32_t)in[0] << 16;
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len == 2) {
        uint32_t v = ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8);
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = b64[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, uint8_t *out, size_t outsz)
{
    size_t len = strlen(in), o = 0;
    int acc = 0, bits = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '=') break;
        int v = b64_val(in[i]);
        if (v < 0)
            return 0;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= outsz)
                return 0;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

/* ---- hex ---- */
static int from_hex(const char *in, uint8_t *out, size_t outlen)
{
    if (strlen(in) != outlen * 2)
        return -1;
    for (size_t i = 0; i < outlen; i++) {
        unsigned v;
        if (sscanf(in + i * 2, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int random_bytes(uint8_t *out, size_t len)
{
#ifdef _WIN32
    for (size_t i = 0; i < len; i++)
        out[i] = (uint8_t)(rand() ^ (unsigned)(i * 31 + 7) ^ GetTickCount());
    return 0;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, out + got, len - got);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);
    return 0;
#endif
}

/* NIP-04: the AES key is the raw ECDH x-coordinate, so the hash function
 * just copies the x bytes out of the compressed pubkey the lib hands us. */
static int ecdh_copy_x(unsigned char *output, const unsigned char *x32,
                      const unsigned char *y32, void *data)
{
    (void)y32; (void)data;
    memcpy(output, x32, 32);
    return 1;
}

/* Derive the NIP-04 shared key: ECDH(privkey, peer xonly pubkey) x-coord. */
static int derive_shared_key(const char *privkey_hex, const char *peer_pubkey_hex,
                             uint8_t key[32])
{
    uint8_t sk[32], pk[32];
    if (from_hex(privkey_hex, sk, 32) != 0 || from_hex(peer_pubkey_hex, pk, 32) != 0)
        return -1;
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx)
        return -1;
    secp256k1_xonly_pubkey xpub;
    if (!secp256k1_xonly_pubkey_parse(ctx, &xpub, pk)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    /* lift xonly -> full pubkey with a zero tweak */
    secp256k1_pubkey pub;
    uint8_t zero[32] = {0};
    if (!secp256k1_xonly_pubkey_tweak_add(ctx, &pub, &xpub, zero)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    if (!secp256k1_ecdh(ctx, key, &pub, sk, ecdh_copy_x, NULL)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    secp256k1_context_destroy(ctx);
    return 0;
}

/* Encrypt into NIP-04 content format: "<b64 ct>?iv=<b64 iv>". */
static int encrypt_content(const uint8_t key[32], const char *plaintext,
                          char *out, size_t outsz)
{
    size_t ptlen = strlen(plaintext);
    size_t padded = ((ptlen / 16) + 1) * 16; /* always pad */
    uint8_t *buf = malloc(padded);
    if (!buf)
        return -1;
    memcpy(buf, plaintext, ptlen);
    /* PKCS#7 padding */
    uint8_t pad = (uint8_t)(padded - ptlen);
    memset(buf + ptlen, pad, pad);
    uint8_t iv[16];
    if (random_bytes(iv, 16) != 0) {
        free(buf);
        return -1;
    }
    aes256_cbc_encrypt(key, iv, buf, padded, buf);
    char ct[1024], ivb[32];
    b64_encode(buf, padded, ct);
    b64_encode(iv, 16, ivb);
    free(buf);
    int n = snprintf(out, outsz, "%s?iv=%s", ct, ivb);
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}

/* Decrypt NIP-04 content. Returns 0 on success, plaintext in out. */
static int decrypt_content(const uint8_t key[32], const char *content,
                           char *out, size_t outsz)
{
    const char *sep = strstr(content, "?iv=");
    if (!sep)
        return -1;
    size_t ctlen = (size_t)(sep - content);
    if (ctlen >= sizeof(char) * 1024)
        return -1;
    char ctb[1024];
    memcpy(ctb, content, ctlen);
    ctb[ctlen] = '\0';
    uint8_t ct[768], iv[16];
    size_t ctbytes = b64_decode(ctb, ct, sizeof(ct));
    size_t ivbytes = b64_decode(sep + 4, iv, sizeof(iv));
    if (ctbytes == 0 || ctbytes % 16 != 0 || ivbytes != 16 || ctbytes > outsz)
        return -1;
    aes256_cbc_decrypt(key, iv, ct, ctbytes, (uint8_t *)out);
    /* strip PKCS#7 */
    uint8_t pad = (uint8_t)out[ctbytes - 1];
    if (pad == 0 || pad > 16 || pad > ctbytes)
        return -1;
    for (size_t i = ctbytes - pad; i < ctbytes; i++)
        if ((uint8_t)out[i] != pad)
            return -1;
    out[ctbytes - pad] = '\0';
    return 0;
}

/* ---- public API ---- */

int chat_send_message(SyncState *s, const char *peer_pubkey_hex,
                      const char *message)
{
    uint8_t key[32];
    if (derive_shared_key(s->privkey, peer_pubkey_hex, key) != 0)
        return -1;
    char content[1200];
    if (encrypt_content(key, message, content, sizeof(content)) != 0)
        return -1;
    /* kind-4 event tagged with the recipient (p tag) */
    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s", s->pubkey);
    ev.created_at = (uint64_t)time(NULL);
    ev.kind = CHAT_KIND;
    snprintf(ev.content, sizeof(ev.content), "%s", content);
    snprintf(ev.ptag, sizeof(ev.ptag), "%s", peer_pubkey_hex);
    if (nostr_sign_event(&ev, s->privkey) != 0)
        return -1;
    char msg[2048];
    nostr_event_message(&ev, msg, sizeof(msg));
    int ok = 0;
    for (int i = 0; i < s->nrelays; i++) {
        if (!s->relays[i].enabled)
            continue;
        WsUrl u;
        if (ws_parse_url(s->relays[i].url, &u) != 0)
            continue;
        WsConn c;
        if (ws_connect(&c, &u) != 0)
            continue;
        s->relays[i].connected = 1;
        if (ws_send_text(&c, msg) == 0)
            ok++;
        ws_close(&c);
    }
    return ok > 0 ? 0 : -1;
}

int chat_try_decrypt(const char *privkey_hex, const char *sender_pubkey_hex,
                     const char *content, char *out, size_t outsz)
{
    uint8_t key[32];
    if (derive_shared_key(privkey_hex, sender_pubkey_hex, key) != 0)
        return -1;
    return decrypt_content(key, content, out, outsz);
}

/* Test seam / non-relay use: produce the encrypted content blob for a
 * message without publishing. Returns 0 on success. */
int chat_encrypt_content(const char *privkey_hex, const char *peer_pubkey_hex,
                         const char *message, char *out, size_t outsz)
{
    uint8_t key[32];
    if (derive_shared_key(privkey_hex, peer_pubkey_hex, key) != 0)
        return -1;
    return encrypt_content(key, message, out, outsz);
}
