#!/usr/bin/env bash
# Builds the Android APK: libgodshand.so for each ABI (NDK + CMake), the small
# Java activity, the resources and the game's files, then aligns and signs it.
#
#   ANDROID_SDK=~/android-sdk android/build_apk.sh [out.apk]
#
# Needs the Android SDK (platforms;android-34, build-tools;34.0.0,
# ndk;27.2.12479018), a JDK, CMake, Ninja and zip.
#
# Environment (all optional):
#   ABIS            ABIs to build (default: arm64-v8a armeabi-v7a x86_64)
#   CMAKE_ARGS      extra CMake arguments (e.g. FETCHCONTENT_SOURCE_DIR_*)
#   GH_KEYSTORE, GH_KEYSTORE_PASS, GH_KEY_ALIAS   signing key; by default the
#                   keystore in this folder, so every build can update the last one
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
sdk="${ANDROID_SDK:-${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/android-sdk}}}"
ndk="${ANDROID_NDK:-$sdk/ndk/27.2.12479018}"
bt="$sdk/build-tools/34.0.0"
jar="$sdk/platforms/android-34/android.jar"
out="${1:-$root/build/GodsHand-android.apk}"
abis="${ABIS:-arm64-v8a armeabi-v7a x86_64}"
keystore="${GH_KEYSTORE:-$here/godshand.keystore}"
storepass="${GH_KEYSTORE_PASS:-godshand}"
alias="${GH_KEY_ALIAS:-godshand}"
work="$root/build/apk"

for f in "$ndk/build/cmake/android.toolchain.cmake" "$bt/aapt2" "$bt/d8" "$bt/zipalign" "$bt/apksigner" "$jar"; do
  [ -e "$f" ] || { echo "missing $f (set ANDROID_SDK)" >&2; exit 1; }
done
strip="$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -n1)"

version="$(sed -n 's/^project(GodsHand VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
IFS=. read -r vmaj vmin vpat <<<"$version"
code=$((vmaj * 10000 + vmin * 100 + vpat))
echo "God's Hand $version (versionCode $code) for: $abis"

rm -rf "$work"
mkdir -p "$work/lib" "$work/classes" "$work/dex" "$work/assets/assets/fonts" "$work/assets/songs"

# ---- native code
for abi in $abis; do
  bdir="$root/build/android-$abi"
  # shellcheck disable=SC2086
  cmake -S "$root" -B "$bdir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release ${CMAKE_ARGS:-} >/dev/null
  cmake --build "$bdir" --target godshand -j"$(nproc 2>/dev/null || echo 4)"
  mkdir -p "$work/lib/$abi"
  "$strip" --strip-unneeded -o "$work/lib/$abi/libgodshand.so" "$bdir/libgodshand.so"
done

# ---- Java (fullscreen + mod zip import)
javac --release 8 -Xlint:-options -cp "$jar" -d "$work/classes" $(find "$here/java" -name '*.java')
"$bt/d8" --release --min-api 24 --lib "$jar" --output "$work/dex" $(find "$work/classes" -name '*.class')

# ---- files the game copies out of the APK on first launch
cp "$root"/assets/fonts/*.ttf "$root"/assets/fonts/*.txt "$work/assets/assets/fonts/"
cp "$root"/songs/* "$work/assets/songs/"

# ---- resources, manifest and packaging
"$bt/aapt2" compile --dir "$here/res" -o "$work/res.zip"
"$bt/aapt2" link -o "$work/unsigned.apk" -I "$jar" --manifest "$here/AndroidManifest.xml" \
  --min-sdk-version 24 --target-sdk-version 34 --version-code "$code" --version-name "$version" \
  -A "$work/assets" "$work/res.zip"
(cd "$work/dex" && zip -q -X "$work/unsigned.apk" classes.dex)
(cd "$work" && zip -q -X -r "$work/unsigned.apk" lib)
"$bt/zipalign" -f -p 4 "$work/unsigned.apk" "$work/aligned.apk"
mkdir -p "$(dirname "$out")"
"$bt/apksigner" sign --ks "$keystore" --ks-pass "pass:$storepass" --ks-key-alias "$alias" --out "$out" "$work/aligned.apk"
"$bt/apksigner" verify "$out"
echo "Built $out ($(du -h "$out" | cut -f1))"
