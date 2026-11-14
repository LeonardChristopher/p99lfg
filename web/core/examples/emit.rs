fn main() {
    let sk = "0000000000000000000000000000000000000000000000000000000000000003";
    let peer = "5cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc";
    let key = p99lfg_core::nip04::derive_shared_key(sk, peer).unwrap();
    let blob = p99lfg_core::nip04::encrypt_content(&key, "{\"from\":\"Soandso\",\"msg\":\"hello p99\"}").unwrap();
    println!("{}", blob);
}
