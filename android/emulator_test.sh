#!/usr/bin/env bash
# Smoke test on a running emulator (CI): installs the APK, starts the game and
# plays through title -> menu -> Freeplay -> a song -> pause -> background ->
# resume with taps on the on-screen buttons, saving a screenshot of each step.
# Fails if the game dies or Android reports a crash.
#
#   android/emulator_test.sh GodsHand-android.apk [screenshot folder]
set -uo pipefail
apk="$1"
out="${2:-shots}"
pkg=com.godshand.game
mkdir -p "$out"

fail() {
  echo "FAILED: $*"
  adb logcat -d > "$out/logcat.txt" 2>/dev/null
  grep -E "GodsHand|raylib|DEBUG|libc|AndroidRuntime|FATAL" "$out/logcat.txt" | tail -120
  exit 1
}
shot() {  # name
  adb exec-out screencap -p > "$out/$1.png" || fail "screencap"
  echo "screenshot $1"
}
alive() {
  [ -n "$(adb shell pidof "$pkg" | tr -d '\r')" ] || fail "the game is not running ($1)"
}
tap() { adb shell input tap "$1" "$2"; }

adb install -r "$apk" || fail "install"
# A song made of a plain audio file (AutoChart) checks that audio files load.
python3 - "$out/tone.wav" <<'PY'
import math, struct, sys, wave
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100)
    w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * i / 44100))) for i in range(44100 * 4)))
PY
songs="/sdcard/Android/data/$pkg/files/songs"
adb shell mkdir -p "$songs" && adb push "$out/tone.wav" "$songs/Test Tone.wav" || fail "push a song"
adb logcat -c
adb shell am start -W -n "$pkg/.GameActivity" || fail "start"
sleep 5
alive "start"

# Screen size in the game's (landscape) orientation.
read -r W H < <(adb shell wm size | tr -d '\r' | sed -n 's/.*: \([0-9]*\)x\([0-9]*\).*/\1 \2/p' | tail -n1)
if [ "${W:-0}" -lt "${H:-0}" ]; then t=$W; W=$H; H=$t; fi
# Button positions, as laid out by src/touch_ui.cpp.
u=$(( (H < W ? H : W) * 1000 / 720 ))   # unit x1000
m=$(( 24 * u / 1000 )); s=$(( 92 * u / 1000 )); g=$(( 10 * u / 1000 ))
cx=$(( m + s * 3 / 2 + g )); cy=$(( H - m - s * 3 / 2 - g ))
down_x=$cx; down_y=$(( cy + s + g ))
ok=$(( 124 * u / 1000 ))
ok_x=$(( W - m - ok / 2 )); ok_y=$(( H - m - 36 * u / 1000 - ok / 2 ))
pause_x=$(( m + 50 * u / 1000 )); pause_y=$(( m + 29 * u / 1000 ))
echo "screen ${W}x${H}: down ($down_x,$down_y) ok ($ok_x,$ok_y) pause ($pause_x,$pause_y)"

wait_log() {  # pattern, seconds: the game logs each screen it opens
  local end=$((SECONDS + $2))
  while [ $SECONDS -lt $end ]; do
    adb logcat -d -s GodsHand:I | grep -q "$1" && return 0
    sleep 1
  done
  fail "the game did not log '$1'"
}

wait_log "screen .*TitleState" 30
shot 1-title
tap $(( W / 2 )) $(( H / 2 ))           # TAP TO BEGIN
wait_log "screen .*MainMenuState" 15; sleep 1
shot 2-menu
tap $down_x $down_y; sleep 1            # STORY MODE -> FREEPLAY
tap $ok_x $ok_y
wait_log "screen .*FreeplayState" 15; sleep 2
shot 3-freeplay
up_x=$cx; up_y=$(( cy - s - g ))
tap $up_x $up_y                         # the last song: Test Tone (its preview decodes the file)
wait_log "decoded .*Test Tone.wav" 30
shot 3b-preview
tap $down_x $down_y; sleep 1            # back to the first song
tap $ok_x $ok_y                         # play it
wait_log "song builtin:" 60; sleep 5
alive "song"; shot 4-song
for i in 0 1 2 3 3 2 1 0 0 1 2 3; do    # taps on the lanes
  tap $(( W * (2 * i + 1) / 8 )) $(( H * 3 / 4 )); sleep 0.25
done
shot 5-lanes
tap $pause_x $pause_y
wait_log "paused at" 10; sleep 1
shot 6-pause
adb shell input keyevent KEYCODE_HOME; sleep 4
adb shell am start -n "$pkg/.GameActivity"
wait_log "back from the background" 20; sleep 2
alive "resume"; shot 7-resumed

adb logcat -d > "$out/logcat.txt"
if grep -qE "FATAL EXCEPTION|Fatal signal|SIGSEGV|SIGABRT" "$out/logcat.txt"; then fail "crash in the log"; fi
if grep -q "Failed to open file" "$out/logcat.txt"; then fail "a file could not be opened"; fi
grep -E " (GodsHand|raylib) *:" "$out/logcat.txt" | tail -40
echo "Android smoke test passed"
