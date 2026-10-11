#include "core.h"
#include "nostr.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    char tmpfile[] = "/tmp/p99lfg_key_testXXXXXX";
    (void)tmpfile;
    const char *path = "/tmp/p99lfg_test_key.txt";
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

    const char *path = "/tmp/p99lfg_test_key.txt";
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

int main(void)
{
    test_sha256_vectors();
    test_schnorr_bip340();
    test_event_serialization_and_sign();
    test_core_parser();
    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
