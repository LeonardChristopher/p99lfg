# p99lfg web / browser-extension shells

`core/` is the shared core: a Rust crate compiled to WASM (`p99lfg_core.wasm`)
with a thin `extern "C"` string ABI (`src/ffi.rs`) that the JS shells call.
It is a transliteration of the C core in `../src/` and is wire-compatible with
the Windows exe and the iOS app:

- same identity format: 64-char hex secp256k1 secret key (the `p99lfg.key` file)
- NIP-04 DMs exactly as `src/chat.c`: ECDH shared key = raw x-coord, AES-256-CBC
  with PKCS#7, content `<base64 ct>?iv=<base64 iv>`
- BIP340 schnorr signatures over the sha256 event id, aux = 32 zero bytes,
  id serialization byte-identical to `nostr_serialize_for_id` (src/nostr.c)
- kinds 30078 (LFG), 4 (DM), 9 (group chat), 30079/30080 (ratings/votes),
  30081/30082 (state); REQ filter `#t p99lfg` with optional `since`
- rating consensus rule (hide at >= 2 disagreeing *active* raters)

Cross-implementation test vectors (generated from the actual C code, linking
`src/nostr.c` + `src/chat.c` + vendored libsecp256k1) live in
`core/tests/cross_vectors.rs`: event id/sig/message, REQ strings, NIP-04 shared
key and encrypted blob, plus BIP340 vector 0 and the FIPS-197 AES-256 vector.

Build + test:

    ./build-core.sh        # cargo test + wasm32 release build

Planned shells (see docs/SESSION-HANDOFF.md section 9):

1. `p99lfg.html` - single-file webpage (esbuild bundle of the JS glue + the wasm)
2. browser extension (manifest, popup, service worker) reusing the same core
3. app lock (passphrase + TOTP QR for password managers) - parked near the end
