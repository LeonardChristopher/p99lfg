#!/bin/sh
# Build and test the web/extension shared core (Rust -> WASM).
# Requires: rustup with the wasm32-unknown-unknown target.
set -e
cd "$(dirname "$0")/core"
cargo test
cargo build --release --target wasm32-unknown-unknown
echo "wasm: $(ls -la target/wasm32-unknown-unknown/release/p99lfg_core.wasm)"
