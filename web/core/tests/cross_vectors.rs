use p99lfg_core::{event, keys, lfg, nip04, rating};

const SK3: &str = "0000000000000000000000000000000000000000000000000000000000000003";
const PUB3: &str = "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9";
const SK7: &str = "0000000000000000000000000000000000000000000000000000000000000007";
const PEER_PUB: &str = "5cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc";

const LFG_CONTENT: &str = "{\"v\":2,\"name\":\"Soandso\",\"class\":\"Mage\",\"level\":60,\"zone\":\"sebilis\",\"lfg\":true,\"user\":\"Tester\"}";
const EVID: &str = "bb6441f313dc3ba953f8293a4b7222898e299c7931ef434f1155045622083030";
const EVSIG: &str = "893c51330f2579776090908682bd5015bdea06835fa21562710c30280c73db868aba25eebcf1d32e30b381bc4e96491683e0769d202d4da7846c57f6b8f01ea5";
const EVMSG: &str = "[\"EVENT\",{\"id\":\"bb6441f313dc3ba953f8293a4b7222898e299c7931ef434f1155045622083030\",\"pubkey\":\"f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9\",\"created_at\":1730000000,\"kind\":30078,\"tags\":[[\"d\",\"green/Soandso\"],[\"t\",\"p99lfg\"]],\"content\":\"{\\\"v\\\":2,\\\"name\\\":\\\"Soandso\\\",\\\"class\\\":\\\"Mage\\\",\\\"level\\\":60,\\\"zone\\\":\\\"sebilis\\\",\\\"lfg\\\":true,\\\"user\\\":\\\"Tester\\\"}\",\"sig\":\"893c51330f2579776090908682bd5015bdea06835fa21562710c30280c73db868aba25eebcf1d32e30b381bc4e96491683e0769d202d4da7846c57f6b8f01ea5\"}]";
const REQ: &str = "[\"REQ\",\"p99lfg-poll\",{\"kinds\":[30078,4,9,30079,30080,30081,30082],\"#t\":[\"p99lfg\"]}]";
const REQ_SINCE: &str = "[\"REQ\",\"p99lfg-poll\",{\"kinds\":[30078,4,9,30079,30080,30081,30082],\"#t\":[\"p99lfg\"],\"since\":1730000000}]";
const SHARED: &str = "352bbf4a4cdd12564f93fa332ce333301d9ad40271f8107181340aef25be59d5";
const BLOB: &str = "PQt/aHOmMJmpwN17E2dnkUHe+UPuKv+nybXJlOk1ubt2v0ST2GxUXpuOhbjM8sK5?iv=DRSzt9UtB8+uI3Ip7ENOVg==";
const DM_PLAIN: &str = "{\"from\":\"Soandso\",\"msg\":\"hello p99\"}";
const DMID: &str = "965e3f06ac6adacee64d24ae7e6f794d695ba1ff96ef7847ff6f3f6342e2914f";
const DMSIG: &str = "40d350b1bb3803044b0be9bea5649d3549ec116ef106bcef9361f81d2870b5476defaf0123cf6baacae166b1cb9eaddbfdfc858a0a410d2f67ed980c389c2401";
const DMMSG: &str = "[\"EVENT\",{\"id\":\"965e3f06ac6adacee64d24ae7e6f794d695ba1ff96ef7847ff6f3f6342e2914f\",\"pubkey\":\"f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9\",\"created_at\":1730000001,\"kind\":4,\"tags\":[[\"p\",\"5cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc\"],[\"t\",\"p99lfg\"]],\"content\":\"PQt/aHOmMJmpwN17E2dnkUHe+UPuKv+nybXJlOk1ubt2v0ST2GxUXpuOhbjM8sK5?iv=DRSzt9UtB8+uI3Ip7ENOVg==\",\"sig\":\"40d350b1bb3803044b0be9bea5649d3549ec116ef106bcef9361f81d2870b5476defaf0123cf6baacae166b1cb9eaddbfdfc858a0a410d2f67ed980c389c2401\"}]";
const BIP340_V0_SIG: &str = "e907831f80848d1069a5371b402410364bdf1c5f8307b0084c55f1ce2dca821525f66a4a85ea8b71e482a74f382d2ce5ebeee8fdb2172f477df4900d310536c0";

#[test]
fn pubkey_derivation_matches_c() {
    let sk = keys::secret_from_hex(SK3).unwrap();
    assert_eq!(keys::pubkey_xonly_hex(&sk), PUB3);
    let sk7 = keys::secret_from_hex(SK7).unwrap();
    assert_eq!(keys::pubkey_xonly_hex(&sk7), PEER_PUB);
}

#[test]
fn bip340_vector0() {
    let sk = keys::secret_from_hex(SK3).unwrap();
    let sig = keys::sign_schnorr_aux0(&sk, &[0u8; 32]).unwrap();
    assert_eq!(hex::encode(sig), BIP340_V0_SIG);
}

#[test]
fn signed_lfg_event_matches_c_byte_for_byte() {
    let ev = event::build_signed_event(SK3, event::KIND_LFG, "green/Soandso", "", LFG_CONTENT, 1730000000).unwrap();
    assert_eq!(ev.id, EVID);
    assert_eq!(ev.sig, EVSIG);
    assert_eq!(event::event_message(&ev), EVMSG);
}

#[test]
fn req_messages_match_c() {
    assert_eq!(event::req_message("p99lfg-poll"), REQ);
    assert_eq!(event::req_message_since("p99lfg-poll", 1730000000), REQ_SINCE);
}

#[test]
fn nip04_shared_key_matches_c() {
    let key = nip04::derive_shared_key(SK3, PEER_PUB).unwrap();
    assert_eq!(hex::encode(key), SHARED);
}

#[test]
fn nip04_decrypts_c_encrypted_blob() {
    let key = nip04::derive_shared_key(SK3, PEER_PUB).unwrap();
    let pt = nip04::decrypt_content(&key, BLOB).unwrap();
    assert_eq!(pt, DM_PLAIN);
    let (from, msg) = nip04::parse_dm_payload(&pt);
    assert_eq!(from, "Soandso");
    assert_eq!(msg, "hello p99");
}

#[test]
fn signed_dm_event_matches_c_byte_for_byte() {
    let ev = event::build_signed_event(SK3, 4, "", PEER_PUB, BLOB, 1730000001).unwrap();
    assert_eq!(ev.id, DMID);
    assert_eq!(ev.sig, DMSIG);
    assert_eq!(event::event_message(&ev), DMMSG);
}

#[test]
fn aes256_fips197_vector() {
    use aes::Aes256;
    use cipher::{BlockEncrypt, KeyInit};
    let key = hex::decode("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f").unwrap();
    let pt = hex::decode("00112233445566778899aabbccddeeff").unwrap();
    let cipher = Aes256::new_from_slice(&key).unwrap();
    let mut block = cipher::generic_array::GenericArray::clone_from_slice(&pt);
    cipher.encrypt_block(&mut block);
    assert_eq!(hex::encode(&block), "8ea2b7ca516745bfeafc49904b496089");
}

#[test]
fn nip04_round_trip_and_wrong_key() {
    let key = nip04::derive_shared_key(SK3, PEER_PUB).unwrap();
    let blob = nip04::encrypt_content(&key, "hello").unwrap();
    assert!(blob.contains("?iv="));
    assert_eq!(nip04::decrypt_content(&key, &blob).unwrap(), "hello");
    let wrong = [0x42u8; 32];
    assert!(nip04::decrypt_content(&wrong, &blob).is_err());
    let sym = nip04::derive_shared_key(SK7, PUB3).unwrap();
    assert_eq!(sym, key);
    assert!(nip04::decrypt_content(&key, "not-base64?iv=AAAA").is_err());
}

#[test]
fn parse_event_message_round_trip() {
    let ev = event::build_signed_event(SK3, event::KIND_LFG, "green/Soandso", "", LFG_CONTENT, 1730000000).unwrap();
    let parsed = event::parse_event_message(&event::event_message(&ev)).unwrap();
    assert_eq!(parsed.id, ev.id);
    assert_eq!(parsed.kind, 30078);
    assert_eq!(parsed.dtag, "green/Soandso");
    assert_eq!(parsed.content, LFG_CONTENT);
    assert!(event::is_state_kind(30078));
    assert!(!event::is_state_kind(9));
}

#[test]
fn lfg_content_build_and_parse() {
    let c = lfg::build_lfg_content("Soandso", "Mage", 60, "sebilis", true, "Tester");
    assert_eq!(c, LFG_CONTENT);
    let e = lfg::parse_lfg(&c).unwrap();
    assert_eq!(e.name, "Soandso");
    assert_eq!(e.cls, "Mage");
    assert_eq!(e.level, 60);
    assert_eq!(e.zone, "sebilis");
    assert!(e.lfg);
    assert_eq!(e.user, "Tester");
    let v1 = lfg::build_lfg_content_v1("Soandso", "sebilis", "lfg grp", true);
    let e1 = lfg::parse_lfg(&v1).unwrap();
    assert_eq!(e1.detail, "lfg grp");
    assert!(e1.lfg);
    assert!(lfg::parse_lfg("{\"foo\":1}").is_none());
    assert_eq!(lfg::lfg_dtag("green", "Soandso"), "green/Soandso");
}

fn hexpk(n: u8) -> String {
    let mut s = format!("{:064x}", n as u64);
    s.truncate(64);
    while s.len() < 64 {
        s.insert(0, '0');
    }
    s
}

#[test]
fn rating_store_logic() {
    let alice = hexpk(0xaa);
    let bob = hexpk(0xbb);
    let carol = hexpk(0xcc);
    let target = hexpk(0xdd);
    let mut st = rating::RatingStore::new();

    let content = rating::build_rating_content(5, 4, 3);
    assert!(st.record(&alice, &target, &content, 1000));
    assert!(st.record(&bob, &target, &rating::build_rating_content(1, 1, 1), 1001));
    assert!(!st.record(&alice, &target, &content, 999));
    assert!(st.record(&alice, &target, &rating::build_rating_content(2, 2, 2), 1002));
    assert_eq!(st.ratings.len(), 2);

    let alice_id = st.find_id(&alice, &target).unwrap();
    assert_eq!(alice_id.len(), 64);

    assert!(st.vote(&bob, &alice_id, false, 1003));
    st.finalize();
    assert!(!st.is_hidden(&st.ratings[0].clone()) || st.ratings.iter().any(|r| r.rater == alice && !st.is_hidden(r)));
    let alice_rating = st.ratings.iter().find(|r| r.rater == alice).unwrap().clone();
    assert!(!st.is_hidden(&alice_rating));

    assert!(st.vote(&carol, &alice_id, false, 1004));
    st.finalize();
    let alice_rating = st.ratings.iter().find(|r| r.rater == alice).unwrap().clone();
    assert!(!st.is_hidden(&alice_rating));

    st.record(&carol, &hexpk(0xee), &rating::build_rating_content(3, 3, 3), 1005);
    st.finalize();
    let alice_rating = st.ratings.iter().find(|r| r.rater == alice).unwrap().clone();
    assert!(st.is_hidden(&alice_rating));

    let s = st.summary(&target).unwrap();
    assert_eq!(s.3, 1);
    assert_eq!(s.0, 1);
}
