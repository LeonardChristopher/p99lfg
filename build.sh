#!/bin/sh
# Build and run all tests on Linux/POSIX. Requires cmake + gcc.
set -e
cd "$(dirname "$0")"

if [ ! -f vendor/secp256k1/build/lib/libsecp256k1.a ]; then
    cmake -B vendor/secp256k1/build -S vendor/secp256k1 \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DSECP256K1_ENABLE_BENCHMARK=OFF -DSECP256K1_ENABLE_TESTS=OFF \
        -DSECP256K1_ENABLE_MODULE_SCHNORRSIG=ON -DSECP256K1_ENABLE_MODULE_ECDH=ON \
        -DSECP256K1_BUILD_EXAMPLES=OFF -DSECP256K1_BUILD_TESTS=OFF
    cmake --build vendor/secp256k1/build -j"$(nproc)"
fi

mkdir -p build
CC=${CC:-gcc}
FLAGS="-O2 -Isrc -Ivendor/secp256k1/include -Ivendor/secp256k1/build/include -Ivendor/qrcodegen"
LIBS="vendor/secp256k1/build/lib/libsecp256k1.a"

$CC $FLAGS tests/test_all.c src/sha256.c src/nostr.c src/core.c src/sync.c src/ws_client.c src/aes.c src/chat.c src/fav.c src/rating.c vendor/qrcodegen/qrcodegen.c $LIBS -o build/test_all
$CC $FLAGS tests/test_ws.c src/ws_client.c src/sha256.c src/nostr.c $LIBS -o build/test_ws
$CC $FLAGS tests/cli_main.c src/core.c src/sha256.c src/nostr.c $LIBS -o build/cli

./build/test_all
./build/test_ws
./build/cli tests/fixtures
