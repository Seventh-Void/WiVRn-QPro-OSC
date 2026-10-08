#!/usr/bin/env bash
# Rebuild the Quest Pro headset binaries (aarch64, Android API 28).
# Needs the Android NDK: set ANDROID_NDK_HOME (or ANDROID_NDK_ROOT / ANDROID_NDK).
set -euo pipefail
cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"

clang=""
for root in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK:-}" /opt/android-ndk \
            "$HOME/Android/Sdk/ndk/"* "$HOME/Android/Sdk/ndk-bundle" /opt/android-sdk/ndk/*; do
	[ -n "$root" ] && [ -x "$root/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ] \
		&& { clang="$root/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"; break; }
done
[ -n "$clang" ] || { echo "Android NDK not found; set ANDROID_NDK_HOME." >&2; exit 1; }

stream_port=27273
"$clang" --target=aarch64-linux-android28 -std=c11 -O3 -Wall -Wextra -fPIC -shared "-Wl,-z,max-page-size=16384" streamer.c -o libquestpro-camera-streamer-v8.so
"$clang" --target=aarch64-linux-android28 -std=c11 -O3 -Wall -Wextra -fPIE -pie "-DSTREAM_PORT=$stream_port" "-Wl,-z,max-page-size=16384" relay.c -o questpro-camera-relay-v8
"$clang" --target=aarch64-linux-android28 -std=c11 -O2 -Wall -Wextra -fPIE -pie "-Wl,-z,max-page-size=16384" injector.c -o questpro-camera-injector -ldl
echo "Headset binaries built with $clang"
