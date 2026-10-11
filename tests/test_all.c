#include "core.h"
#include "nostr.h"
#include "sha256.h"
#include "sync.h"
#include "ws_client.h"
#include "aes.h"
#include "chat.h"
#include "fav.h"
#include "rating.h"
#include "qrcodegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define TMPDIR "C:/Users/Public"
#else
#define TMPDIR "/tmp"
#endif

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

static void test_sha256_vectors(void)
{
    const char *abc = "abc";
    uint8_t out[32];
    sha256(abc, 3, out);
    const uint8_t expect[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    CHECK(memcmp(out, expect, 32) == 0, "sha256('abc') vector");

    sha256("", 0, out);
    const uint8_t expect_empty[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
    };
    CHECK(memcmp(out, expect_empty, 32) == 0, "sha256('') vector");

    /* 56-byte boundary case */
    sha256("abcdbcdecdefdefgefghfghighijhijkjkl", 34, out);
    (void)out;
}

/* BIP340 test vector #1 (from BIP340 reference) */
static void test_schnorr_bip340(void)
{
    const char *sk = "0000000000000000000000000000000000000000000000000000000000000003";
    const char *expected_pub = "F9308A019258C31049344F85F89D5229B531C845836F99B08601F113BCE036F9";

    char tmpfile[] = TMPDIR "/p99lfg_key_testXXXXXX";
    (void)tmpfile;
    const char *path = "p99lfg_test_key.txt";
    FILE *f = fopen(path, "w");
    fputs(sk, f);
    fputc('\n', f);
    fclose(f);

    char pub[NOSTR_PUBKEY_HEX];
    int rc = nostr_identity_load(path, pub);
    CHECK(rc == 0, "identity load rc");
    for (char *p = pub; *p; p++)
        if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 32);
    CHECK(strcmp(pub, expected_pub) == 0, "bip340 pubkey derivation");
}

static void test_event_serialization_and_sign(void)
{
    NostrEvent ev = {0};
    snprintf(ev.pubkey, sizeof(ev.pubkey), "%s",
             "f9308a019258c31049344f85f89d5229b531c845836f99b08601f1138f03df5c");
    ev.created_at = 1730000000;
    ev.kind = NOSTR_KIND_LFG;
    snprintf(ev.dtag, sizeof(ev.dtag), "green");
    snprintf(ev.content, sizeof(ev.content),
             "{\"name\":\"Soandso\",\"zone\":\"sebilis\",\"class\":\"Mage\",\"level\":60,\"lfg\":true}");

    char serial[2048];
    nostr_serialize_for_id(&ev, serial, sizeof(serial));
    CHECK(strstr(serial, "[0,") == serial, "serialized starts with [0,");
    CHECK(strstr(serial, "\"t\",\"p99lfg\"") != NULL, "serialized contains t tag");

    uint8_t id32[32];
    sha256(serial, strlen(serial), id32);
    char hexid[65];
    for (int i = 0; i < 32; i++)
        sprintf(hexid + i*2, "%02x", id32[i]);
    printf("  event id: %s\n", hexid);

    const char *path = "p99lfg_test_key.txt";
    char key[65];
    FILE *f = fopen(path, "r");
    fscanf(f, "%64s", key);
    fclose(f);
    CHECK(nostr_sign_event(&ev, key) == 0, "sign event");
    CHECK(strlen(ev.id) == 64, "event id is 64 hex chars");
    CHECK(strlen(ev.sig) == 128, "sig is 128 hex chars");

    char msg[4096];
    size_t n = nostr_event_message(&ev, msg, sizeof(msg));
    CHECK(n > 200 && msg[0] == '[' && strstr(msg, "\"EVENT\"") != NULL, "event message shape");
    CHECK(strstr(msg, ev.id) != NULL, "event message contains id");

    char req[512];
    nostr_req_message("sub1", req, sizeof(req));
    CHECK(strstr(req, "\"REQ\"") != NULL && strstr(req, "p99lfg") != NULL, "req message shape");
}

static void test_core_parser(void)
{
    LfgLine out;
    CHECK(lfg_parse_line("You are now LFG.", &out) == LFG_LINE_SELF_ON, "self LFG on");
    CHECK(lfg_parse_line("You are no longer LFG.", &out) == LFG_LINE_SELF_OFF, "self LFG off");
    CHECK(lfg_parse_line("[60 Archmage] Soandso (Dark Elf) ZONE: sebilis <LFG>", &out) == LFG_LINE_WHO
          && strcmp(out.name, "Soandso") == 0
          && strcmp(out.zone, "sebilis") == 0
          && strcmp(out.detail, "60 Archmage") == 0, "who row parse");
    CHECK(lfg_parse_line("[45 Wizard] Zerker (Gnome) ZONE: commons", &out) == LFG_LINE_NONE,
          "who row without LFG is ignored");
    CHECK(lfg_parse_line("You say, 'hi'", &out) == LFG_LINE_NONE, "chat ignored");
}

static SyncRemoteEntry g_last_remote;
static char g_last_pubkey[80];
static int g_remote_calls;

static void remote_cb(const SyncRemoteEntry *e, const char *pk, void *ud)
{
    (void)ud;
    g_last_remote = *e;
    snprintf(g_last_pubkey, sizeof(g_last_pubkey), "%s", pk);
    g_remote_calls++;
}

static void json_escape_into(char *dst, size_t dstsz, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < dstsz; p++) {
        if (*p == '"' || *p == '\\') {
            dst[o++] = '\\';
            dst[o++] = *p;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

static void test_sync_messages(void)
{
    SyncState st;
    sync_init(&st, "p99lfg_test_key.txt", "green", remote_cb, NULL);
    CHECK(strcmp(st.server, "green") == 0, "sync init server");

    const char *okmsg =
        "[\"OK\",\"abc\",\"true\"]";
    CHECK(sync_handle_message(&st, okmsg) == 0, "OK message ignored");

    char evmsg[1024];
    char content[300], esc[600];
    sync_build_content(content, sizeof(content), "Soandso", "sebilis", "60 Archmage", 1);
    json_escape_into(esc, sizeof(esc), content);
    snprintf(evmsg, sizeof(evmsg),
        "[\"EVENT\",\"sub1\",{\"id\":\"x\",\"pubkey\":\"%s\",\"created_at\":1,\"kind\":30078,"
        "\"tags\":[],\"content\":\"%s\",\"sig\":\"y\"}]",
        "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20", esc);
    CHECK(sync_handle_message(&st, evmsg) == 1, "EVENT message parsed");
    CHECK(g_remote_calls == 1 && strcmp(g_last_remote.name, "Soandso") == 0 &&
          strcmp(g_last_remote.zone, "sebilis") == 0 && strcmp(g_last_remote.detail, "60 Archmage") == 0,
          "remote entry fields");

    /* lfg:false should carry through */
    char content2[300], esc2[600];
    sync_build_content(content2, sizeof(content2), "Otherone", "unrest", "20 Necro", 0);
    json_escape_into(esc2, sizeof(esc2), content2);
    char evmsg2[1024];
    snprintf(evmsg2, sizeof(evmsg2),
        "[\"EVENT\",\"sub1\",{\"id\":\"x\",\"pubkey\":\"pk\",\"created_at\":1,\"kind\":30078,"
        "\"tags\":[],\"content\":\"%s\",\"sig\":\"y\"}]", esc2);
    CHECK(sync_handle_message(&st, evmsg2) == 1 && g_last_remote.lfg == 0,
          "lfg=false parsed");

    /* non-lfg-format content is ignored gracefully */
    const char *junk =
        "[\"EVENT\",\"sub1\",{\"content\":\"hello world\")]";
    CHECK(sync_handle_message(&st, junk) == -1, "malformed event returns -1");

    WsUrl u;
    CHECK(ws_parse_url("wss://relay.example.com", &u) == 0 && u.use_tls, "wss parse");
}

/* Import the Windows key on a mobile device: same secret, same pubkey. */
static void test_key_import(void)
{
    const char *sk = "0000000000000000000000000000000000000000000000000000000000000003";
    char path[256]; snprintf(path, sizeof(path), "%s/p99lfg_test_import.key", TMPDIR);
    remove(path);

    char pub1[65] = {0}, pub2[65] = {0};
    CHECK(nostr_identity_load(path, pub1) == 0, "identity load creates key");
    char sk1[65] = {0};
    FILE *f = fopen(path, "r");
    fscanf(f, "%64s", sk1);
    fclose(f);

    /* importing a different key replaces the file and derives its pubkey */
    CHECK(nostr_import_key(path, sk, pub2) == 0, "import key");
    f = fopen(path, "r");
    char sk2[65] = {0};
    fscanf(f, "%64s", sk2);
    fclose(f);
    CHECK(strcmp(sk1, sk2) != 0, "imported key differs from generated");
    CHECK(strcmp(sk2, sk) == 0, "file holds imported secret");

    /* BIP340 vector #1 pubkey for secret 0x...03 */
    const char *expect_pub =
        "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9";
    CHECK(strcmp(pub2, expect_pub) == 0, "imported key derives correct pubkey");

    /* sync_reload_key picks the new identity up */
    SyncState s;
    sync_init(&s, path, "green", NULL, NULL);
    CHECK(sync_reload_key(&s, path) == 0, "sync reload key");
    CHECK(strcmp(s.pubkey, expect_pub) == 0, "sync state holds imported pubkey");

    CHECK(nostr_import_key(path, "not-hex", pub2) != 0, "import rejects bad hex");
    remove(path);
}

/* NIP-04 DM round trip: two identities, encrypt to peer, decrypt as peer. */
static void test_chat_roundtrip(void)
{
    char alice_keyfile[256]; snprintf(alice_keyfile, sizeof(alice_keyfile), "%s/p99lfg_test_alice.key", TMPDIR);
    char bob_keyfile[256]; snprintf(bob_keyfile, sizeof(bob_keyfile), "%s/p99lfg_test_bob.key", TMPDIR);
    remove(alice_keyfile);
    remove(bob_keyfile);

    char alice_pub[65] = {0}, bob_pub[65] = {0};
    CHECK(nostr_identity_load(alice_keyfile, alice_pub) == 0, "alice identity");
    CHECK(nostr_identity_load(bob_keyfile, bob_pub) == 0, "bob identity");
    char alice_sk[65] = {0}, bob_sk[65] = {0};
    FILE *f = fopen(alice_keyfile, "r"); fscanf(f, "%64s", alice_sk); fclose(f);
    f = fopen(bob_keyfile, "r"); fscanf(f, "%64s", bob_sk); fclose(f);

    /* alice encrypts to bob */
    char content[1200];
    const char *msg = "hey, group up in sebilis?";
    CHECK(chat_encrypt_content(alice_sk, bob_pub, msg, content, sizeof(content)) == 0,
          "chat encrypt");
    CHECK(strstr(content, "?iv=") != NULL, "chat content has iv");

    /* bob decrypts from alice */
    char plain[600];
    CHECK(chat_try_decrypt(bob_sk, alice_pub, content, plain, sizeof(plain)) == 0,
          "chat decrypt");
    CHECK(strcmp(plain, msg) == 0, "chat plaintext round trip");

    /* wrong key fails */
    char mallory_keyfile[256]; snprintf(mallory_keyfile, sizeof(mallory_keyfile), "%s/p99lfg_test_mallory.key", TMPDIR);
    remove(mallory_keyfile);
    char mallory_pub[65];
    nostr_identity_load(mallory_keyfile, mallory_pub);
    char mallory_sk[65] = {0};
    f = fopen(mallory_keyfile, "r"); fscanf(f, "%64s", mallory_sk); fclose(f);
    CHECK(chat_try_decrypt(mallory_sk, alice_pub, content, plain, sizeof(plain)) != 0,
          "chat decrypt fails for eavesdropper");

    remove(alice_keyfile); remove(bob_keyfile); remove(mallory_keyfile);
}



/* Ratings: record, dedupe, aggregate, and the consensus anti-brigade rule. */
static void test_ratings(void)
{
    RatingStore *rs = rating_store_new();

    /* three distinct raters rate the same target pubkey */
    const char *target =
        "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9";
    char rater[3][65];
    for (int i = 0; i < 3; i++) {
        char path[64];
        snprintf(path, sizeof(path), "%s/p99lfg_rate_r%d.key", TMPDIR, i);
        remove(path);
        nostr_identity_load(path, rater[i]);
        remove(path);
    }

    CHECK(rating_record(rs, rater[0], target,
          "{\"competent\":5,\"friendly\":5,\"listens\":5}", 1000) == 1, "rating record 1");
    CHECK(rating_record(rs, rater[1], target,
          "{\"competent\":4,\"friendly\":4,\"listens\":4}", 1001) == 1, "rating record 2");
    /* replaceable: same rater re-rates -> overwrite not duplicate */
    CHECK(rating_record(rs, rater[0], target,
          "{\"competent\":3,\"friendly\":3,\"listens\":3}", 2000) == 1, "rating re-rate");
    CHECK(rs->count == 2, "ratings deduped per rater/target");

    int c, f, l, n;
    CHECK(rating_summary(rs, target, &c, &f, &l, &n) == 0, "summary computed");
    CHECK(n == 2, "summary counts visible ratings");
    CHECK(c == 3 || c == 4, "summary average in range");

    /* make rater[2] active by having them rate a different target */
    const char *other =
        "eff69ef2b1bd93a66ed7ef4709410101b1d0cba4c853a3ca3c8ad332f4f8f3b2";
    CHECK(rating_record(rs, rater[2], other,
          "{\"competent\":4,\"friendly\":4,\"listens\":4}", 1500) == 1,
          "rating record 3 (activates rater 2)");

    /* consensus: two active raters disagree with rater[1]'s rating -> hidden */
    rating_store_finalize(rs);
    const Rating *r1 = NULL;
    for (int i = 0; i < rs->count; i++)
        if (strcmp(rs->ratings[i].rater, rater[1]) == 0 &&
            strcmp(rs->ratings[i].target, target) == 0)
            r1 = &rs->ratings[i];
    CHECK(r1 != NULL, "rater found");

    CHECK(vote_record(rs, rater[0], r1->id, 0, 3000) == 1, "disagree vote 1");
    CHECK(vote_record(rs, rater[2], r1->id, 0, 3001) == 1, "disagree vote 2");
    rating_store_finalize(rs);
    CHECK(rating_is_hidden(rs, r1) == 1, "brigaded rating hidden by consensus");

    /* hidden rating drops out of the summary */
    CHECK(rating_summary(rs, target, &c, &f, &l, &n) == 0, "summary after hiding");
    CHECK(n == 1, "hidden rating excluded from summary");

    /* fresh keys that never rated anyone cannot brigade */
    RatingStore *rs2 = rating_store_new();
    CHECK(rating_record(rs2, rater[0], target,
          "{\"competent\":5,\"friendly\":5,\"listens\":5}", 1000) == 1, "fresh store rating");
    rating_store_finalize(rs2);
    const Rating *fr = &rs2->ratings[0];
    char anon1[65] = "1111111111111111111111111111111111111111111111111111111111111111";
    char anon2[65] = "2222222222222222222222222222222222222222222222222222222222222222";
    vote_record(rs2, anon1, fr->id, 0, 3000);
    vote_record(rs2, anon2, fr->id, 0, 3001);
    rating_store_finalize(rs2);
    CHECK(rating_is_hidden(rs2, fr) == 0, "inactive keys cannot brigade");

    rating_store_free(rs);
    rating_store_free(rs2);
}

/* QR payload encodes a 64-hex key (Windows shows it, phones scan it). */
static void test_qr_key(void)
{
    /* via win32_qr.c is GDI-bound; test the vendored encoder directly */
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    const char *key = "0000000000000000000000000000000000000000000000000000000000000003";
    bool ok = qrcodegen_encodeText(key, temp, qrcode, qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    CHECK(ok, "qr encode 64-hex key");
    CHECK(qrcodegen_getSize(qrcode) > 21, "qr has modules");
}

/* favourites: toggle, persistence, reload */
static void test_favourites(void)
{
    char path[256]; snprintf(path, sizeof(path), "%s/p99lfg_test_favs.txt", TMPDIR);
    remove(path);

    FavList *f = fav_load(path);
    CHECK(f && fav_count(f) == 0, "fav starts empty");
    CHECK(fav_toggle(f, "Soandso") == 0, "fav add");
    CHECK(fav_has(f, "Soandso"), "fav has after add");
    CHECK(fav_toggle(f, "Other") == 0, "fav add second");
    CHECK(fav_count(f) == 2, "fav count 2");
    CHECK(fav_toggle(f, "Soandso") == 0, "fav remove");
    CHECK(!fav_has(f, "Soandso"), "fav removed");
    fav_free(f);

    /* persistence */
    f = fav_load(path);
    CHECK(f && fav_count(f) == 1, "fav persisted");
    CHECK(fav_has(f, "Other"), "fav persisted content");
    fav_free(f);
    remove(path);
}

int main(void)
{
    test_sha256_vectors();
    test_schnorr_bip340();
    test_event_serialization_and_sign();
    test_core_parser();
    test_sync_messages();
    test_key_import();
    test_favourites();
    test_qr_key();
    test_chat_roundtrip();
    test_ratings();
    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
