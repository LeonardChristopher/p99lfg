#!/bin/sh
# Cross-compile the Windows 64-bit exe from Linux using mingw-w64:
#   apt install mingw-w64   (or use CI, which does this automatically)
# Produces build/p99lfg.exe
set -e
cd "$(dirname "$0")"

if [ ! -f vendor/secp256k1/build-mingw/lib/libsecp256k1.a ]; then
    cmake -B vendor/secp256k1/build-mingw -S vendor/secp256k1 \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
        -DSECP256K1_ENABLE_BENCHMARK=OFF -DSECP256K1_ENABLE_TESTS=OFF \
        -DSECP256K1_ENABLE_MODULE_SCHNORRSIG=ON -DSECP256K1_ENABLE_MODULE_ECDH=ON \
        -DSECP256K1_BUILD_EXAMPLES=OFF -DSECP256K1_BUILD_TESTS=OFF
    cmake --build vendor/secp256k1/build-mingw -j"$(nproc)"
fi

mkdir -p build
x86_64-w64-mingw32-gcc -O2 -mconsole -mwindows \
    -Isrc -Ivendor/secp256k1/include -Ivendor/secp256k1/build-mingw/include \
    src/app.rc src/win32_main.c src/core.c src/sha256.c src/nostr.c src/sync.c src/ws_client.c src/tls_schannel.c src/chat.c src/aes.c src/fav.c src/rating.c src/chars.c src/channels.c src/settings.c vendor/qrcodegen/qrcodegen.c \
    vendor/secp256k1/build-mingw/lib/libsecp256k1.a \
    -lgdi32 -luser32 -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lws2_32 -lcrypt32 \
    -o build/p99lfg.exe
echo "built build/p99lfg.exe"
