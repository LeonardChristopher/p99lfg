#include "nostr.h"
#include "sha256.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static void json_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 7 < outsz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c == '\t') {
            out[o++] = '\\'; out[o++] = 't';
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static void to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i*2] = hexd[in[i] >> 4];
        out[i*2+1] = hexd[in[i] & 0xf];
    }
    out[len*2] = '\0';
}

static int from_hex(const char *in, uint8_t *out, size_t outlen)
{
    size_t n = strlen(in);
    if (n != outlen * 2)
        return -1;
    for (size_t i = 0; i < outlen; i++) {
        unsigned v;
        if (sscanf(in + i*2, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static void random_bytes(uint8_t *out, size_t len)
{
#ifdef _WIN32
    HCRYPTPROV h = 0;
    if (CryptAcquireContextA(&h, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(h, (DWORD)len, out);
        CryptReleaseContext(h, 0);
        return;
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, out, len);
        (void)r;
        close(fd);
        return;
    }
#endif
    for (size_t i = 0; i < len; i++)
        out[i] = (uint8_t)rand();
}

int nostr_identity_load(const char *path, char pubkey_out[NOSTR_PUBKEY_HEX])
{
    uint8_t sk[32];
    char hex[NOSTR_PRIVKEY_HEX];
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%64s", hex) == 1 && strlen(hex) == 64 &&
            from_hex(hex, sk, 32) == 0) {
            fclose(f);
        } else {
            fclose(f);
            f = NULL;
        }
    }
    if (!f) {
        srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)path);
        do {
            random_bytes(sk, 32);
        } while (sk[0] == 0); /* invalid key guard; full validity checked below */
        to_hex(sk, 32, hex);
        FILE *wf = fopen(path, "w");
        if (wf) {
            fputs(hex, wf);
            fputc('\n', wf);
            fclose(wf);
        }
    }

    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx)
        return -1;
    secp256k1_keypair kp;
    if (!secp256k1_keypair_create(ctx, &kp, sk)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    secp256k1_xonly_pubkey xpub;
    if (!secp256k1_keypair_xonly_pub(ctx, &xpub, NULL, &kp)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    uint8_t pub[32];
    secp256k1_xonly_pubkey_serialize(ctx, pub, &xpub);
    to_hex(pub, 32, pubkey_out);
    secp256k1_context_destroy(ctx);
    return 0;
}

/* Import an existing identity: validate the hex secret key, derive the
 * pubkey, and persist it to the key file. Returns 0 on success. Used to
 * share one Nostr identity between the Windows app and mobile apps. */
int nostr_import_key(const char *path, const char *privkey_hex,
                     char pubkey_out[NOSTR_PUBKEY_HEX])
{
    uint8_t sk[32];
    if (!privkey_hex || from_hex(privkey_hex, sk, 32) != 0)
        return -1;
    char hex[NOSTR_PRIVKEY_HEX];
    to_hex(sk, 32, hex);
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx)
        return -1;
    secp256k1_keypair kp;
    if (!secp256k1_keypair_create(ctx, &kp, sk)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    secp256k1_xonly_pubkey xpub;
    if (!secp256k1_keypair_xonly_pub(ctx, &xpub, NULL, &kp)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    uint8_t pub[32];
    secp256k1_xonly_pubkey_serialize(ctx, pub, &xpub);
    to_hex(pub, 32, pubkey_out);
    secp256k1_context_destroy(ctx);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(hex, f);
    fputc('\n', f);
    fclose(f);
    return 0;
}

void nostr_serialize_for_id(const NostrEvent *ev, char *out, size_t outsz)
{
    char esc_content[1024];
    json_escape(ev->content, esc_content, sizeof(esc_content));
    if (ev->ptag[0]) {
        /* kind-4 DM: p tag for the recipient, t tag so our REQ filter matches */
        snprintf(out, outsz,
            "[0,\"%s\",%llu,%d,[[\"p\",\"%s\"],[\"t\",\"" LFG_TAG "\"]],\"%s\"]",
            ev->pubkey,
            (unsigned long long)ev->created_at,
            ev->kind,
            ev->ptag,
            esc_content);
    } else {
        snprintf(out, outsz,
            "[0,\"%s\",%llu,%d,[[\"d\",\"%s\"],[\"t\",\"" LFG_TAG "\"]],\"%s\"]",
            ev->pubkey,
            (unsigned long long)ev->created_at,
            ev->kind,
            ev->dtag,
            esc_content);
    }
}

int nostr_sign_event(NostrEvent *ev, const char *privkey_hex)
{
    uint8_t sk[32];
    if (from_hex(privkey_hex, sk, 32) != 0)
        return -1;

    char serial[2048];
    nostr_serialize_for_id(ev, serial, sizeof(serial));
    uint8_t id32[32];
    sha256(serial, strlen(serial), id32);
    to_hex(id32, 32, ev->id);

    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx)
        return -1;
    secp256k1_keypair kp;
    if (!secp256k1_keypair_create(ctx, &kp, sk)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    uint8_t aux[32] = {0};
    uint8_t sig[64];
    if (!secp256k1_schnorrsig_sign32(ctx, sig, id32, &kp, aux)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    to_hex(sig, 64, ev->sig);
    secp256k1_context_destroy(ctx);
    return 0;
}

size_t nostr_event_message(const NostrEvent *ev, char *out, size_t outsz)
{
    char esc_content[1024];
    json_escape(ev->content, esc_content, sizeof(esc_content));
    if (ev->ptag[0]) {
        return (size_t)snprintf(out, outsz,
            "[\"EVENT\",{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":%llu,"
            "\"kind\":%d,\"tags\":[[\"p\",\"%s\"],[\"t\",\"" LFG_TAG "\"]],"
            "\"content\":\"%s\",\"sig\":\"%s\"}]",
            ev->id, ev->pubkey, (unsigned long long)ev->created_at,
            ev->kind, ev->ptag, esc_content, ev->sig);
    }
    return (size_t)snprintf(out, outsz,
        "[\"EVENT\",{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":%llu,"
        "\"kind\":%d,\"tags\":[[\"d\",\"%s\"],[\"t\",\"" LFG_TAG "\"]],"
        "\"content\":\"%s\",\"sig\":\"%s\"}]",
        ev->id, ev->pubkey, (unsigned long long)ev->created_at,
        ev->kind, ev->dtag, esc_content, ev->sig);
}

size_t nostr_req_message(const char *subid, char *out, size_t outsz)
{
    /* LFG listings, kind-4 DMs, and rating/vote events (30079/30080), all
     * under the #t p99lfg tag. */
    return (size_t)snprintf(out, outsz,
        "[\"REQ\",\"%s\",{\"kinds\":[%d,4,30079,30080],\"#t\":[\"" LFG_TAG "\"]}]",
        subid, NOSTR_KIND_LFG);
}
