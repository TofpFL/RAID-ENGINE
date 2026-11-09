#!/usr/bin/env bash
# Builds the complete source package: the game's own code plus the exact
# sources of every library it uses (raylib 5.5, Lua 5.4.7, miniz 3.0.2,
# pl_mpeg), in third_party/, so it builds anywhere without internet.
#
#   tools/make_source_package.sh [out.zip]
#
# The libraries are cloned from GitHub, or copied from local checkouts given in
# GH_RAYLIB_DIR, GH_LUA_DIR, GH_MINIZ_DIR and GH_PL_MPEG_DIR.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/build/GodsHand-complete-source.zip}"
case "$out" in /*) ;; *) out="$PWD/$out" ;; esac
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
stage="$tmp/GodsHand-source"
mkdir -p "$stage/third_party"

# The game: every tracked file of the current commit.
git -C "$root" archive --format=tar HEAD | tar -x -C "$stage"

# name, local dir, repository, tag or commit
get() {
  local dest="$stage/third_party/$1"
  if [ -n "$2" ]; then
    cp -R "$2" "$dest"
  else
    git init -q "$dest"
    git -C "$dest" fetch -q --depth 1 "$3" "$4"
    git -C "$dest" checkout -q FETCH_HEAD
  fi
  rm -rf "$dest/.git"
}
get raylib "${GH_RAYLIB_DIR:-}" https://github.com/raysan5/raylib.git 5.5
get lua "${GH_LUA_DIR:-}" https://github.com/lua/lua.git v5.4.7
get miniz "${GH_MINIZ_DIR:-}" https://github.com/richgel999/miniz.git 3.0.2
get pl_mpeg "${GH_PL_MPEG_DIR:-}" https://github.com/phoboslab/pl_mpeg.git c871f2be022ece7ef4f64230b4fb8e1fb9eb6023
# raylib's examples and IDE projects are not needed to build the game.
rm -rf "$stage/third_party/raylib/"{examples,projects,logo,parser}

cat > "$stage/third_party/README.txt" <<'EOF'
Libraries used by God's Hand, exactly as released by their authors.
CMakeLists.txt uses these folders automatically, so nothing is downloaded.

  raylib   5.5        window, input, audio, drawing      zlib license   https://www.raylib.com
  lua      5.4.7      mod scripts                        MIT license    https://www.lua.org
  miniz    3.0.2      installing mods from .zip          MIT license    https://github.com/richgel999/miniz
  pl_mpeg  c871f2b    cutscene videos (MPEG-1)           MIT license    https://github.com/phoboslab/pl_mpeg

raylib's examples/, projects/, logo/ and parser/ folders were left out.
EOF

cat > "$stage/BUILDING.txt" <<'EOF'
God's Hand of Principality and Demise - complete source
=======================================================

Everything needed is in this folder: the game (src/, tests/, assets/) and the
libraries (third_party/). No internet connection is needed to build.

Windows, Linux, macOS
---------------------
Install CMake 3.20+ and a C++20 compiler (Visual Studio 2022, GCC 11+ or Clang 14+), then:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

The game is build/godshand (Windows with Visual Studio: build\Release\godshand.exe),
with assets/, songs/ and mods/ next to it. build/gh_tests runs the test suite.

Linux also needs: libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev
libxi-dev libgl1-mesa-dev libasound2-dev

Android
-------
Install the Android SDK with platforms;android-34, build-tools;34.0.0 and
ndk;27.2.12479018, a JDK 17+, CMake, Ninja and zip, then:

    ANDROID_SDK=/path/to/android-sdk android/build_apk.sh

The APK is build/GodsHand-android.apk (arm64-v8a, armeabi-v7a and x86_64).
EOF

mkdir -p "$(dirname "$out")"
rm -f "$out"
(cd "$tmp" && zip -q -r -X "$out" GodsHand-source)
echo "Built $out ($(du -h "$out" | cut -f1))"
