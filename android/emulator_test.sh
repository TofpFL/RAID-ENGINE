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

wait_log() {  # pattern, seconds: the game logs each screen it opens
  local end=$((SECONDS + $2))
  while [ $SECONDS -lt $end ]; do
    adb logcat -d -s GodsHand:I | grep -q "$1" && return 0
    sleep 1
  done
  fail "the game did not log '$1'"
}

adb install -r "$apk" || fail "install"
# The first launch creates the game's folder (Android does not let adb create it).
adb logcat -c
adb shell am start -W -n "$pkg/.GameActivity" || fail "start"
wait_log "screen .*TitleState" 30
# A song made of a plain audio file (AutoChart) checks that audio files load.
python3 - "$out/tone.wav" <<'PY'
import math, struct, sys, wave
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100)
    w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * i / 44100))) for i in range(44100 * 4)))
PY
# Android 11+ does not let adb write into another app's Android/data folder:
# then the check is skipped (fonts already go through the same file reading).
pushed=0
if adb push "$out/tone.wav" "/sdcard/Android/data/$pkg/files/songs/Test Tone.wav"; then
  pushed=1
  adb shell am force-stop "$pkg"; sleep 1
  adb logcat -c
  adb shell am start -W -n "$pkg/.GameActivity" || fail "restart"
else
  echo "note: cannot copy files into the game's folder here, skipping the audio file check"
fi
sleep 3
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

wait_log "screen .*TitleState" 30
shot 1-title
tap $(( W / 2 )) $(( H / 2 ))           # TAP TO BEGIN
wait_log "screen .*MainMenuState" 15; sleep 1
shot 2-menu
tap $down_x $down_y; sleep 1            # STORY MODE -> FREEPLAY
tap $ok_x $ok_y
wait_log "screen .*FreeplayState" 15; sleep 2
shot 3-freeplay
if [ "$pushed" = 1 ]; then
  tap $cx $(( cy - s - g ))             # UP: the last song, Test Tone (its preview decodes the file)
  wait_log "decoded .*Test Tone.wav" 30
  shot 3b-preview
  tap $down_x $down_y; sleep 1          # back to the first song
fi
tap $ok_x $ok_y                         # play it
wait_log "song builtin:" 60; sleep 4
alive "song"; shot 4-song
# Each adb tap takes a second or two: pause early, before the misses add up.
tap $pause_x $pause_y
wait_log "paused at" 10; sleep 1
shot 5-pause
tap $ok_x $ok_y                         # Resume
wait_log "resumed" 10
for i in 0 1 2 3; do                    # taps on the lanes
  tap $(( W * (2 * i + 1) / 8 )) $(( H * 3 / 4 ))
done
shot 6-lanes
adb shell input keyevent KEYCODE_HOME; sleep 4
adb shell am start -n "$pkg/.GameActivity"
wait_log "back from the background" 20; sleep 2
[ "$(adb logcat -d -s GodsHand:I | grep -c 'paused at')" -ge 2 ] || fail "the song did not pause after coming back"
adb logcat -d -s GodsHand:I | grep "paused at" | tail -n1 | grep -qE ": [1-9][0-9]* presses" || fail "the taps on the lanes did not press them"
alive "resume"; shot 7-resumed

adb logcat -d > "$out/logcat.txt"
if grep -qE "FATAL EXCEPTION|Fatal signal|SIGSEGV|SIGABRT" "$out/logcat.txt"; then fail "crash in the log"; fi
if grep -q "Failed to open file" "$out/logcat.txt"; then fail "a file could not be opened"; fi
grep -E " (GodsHand|raylib) *:" "$out/logcat.txt" | tail -40
echo "Android smoke test passed"
