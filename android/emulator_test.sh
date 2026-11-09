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

seen() { adb logcat -d -s GodsHand:I | grep -c "$1"; }
wait_quiet() {  # pattern, seconds, how many times it must appear
  local end=$((SECONDS + $2))
  while [ $SECONDS -lt $end ]; do
    [ "$(seen "$1")" -ge "${3:-1}" ] && return 0
    sleep 1
  done
  return 1
}
wait_log() {  # pattern, seconds: the game logs each screen it opens
  wait_quiet "$1" "$2" || fail "the game did not log '$1'"
}
# Tap, then wait for the game to log pattern. If it did not and the game is
# still on the same screen, the tap was lost: send it once more.
tap_wait() {  # x, y, pattern, seconds
  local before screens
  before=$(seen "$3"); screens=$(seen "screen ")
  tap "$1" "$2"
  wait_quiet "$3" "$4" $((before + 1)) && return 0
  [ "$(seen "screen ")" -eq "$screens" ] || fail "the game opened another screen instead of logging '$3'"
  echo "note: tapping again ($3)"
  tap "$1" "$2"
  wait_quiet "$3" "$4" $((before + 1)) || fail "the game did not log '$3'"
}

# Right after boot Android is still starting every app's boot receivers;
# taps sent then can get lost. Let it settle first.
adb shell cmd activity wait-for-broadcast-idle > /dev/null 2>&1 || true
sleep 10

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
# The game must draw for the landscape window, even when it was started upright.
wait_log "screen size ${W}x${H}" 15
shot 1-title
python3 - "$out/1-title.png" <<'PY' || fail "the screen is one flat colour (the game is not drawing for this window)"
import sys, zlib, struct
data = open(sys.argv[1], "rb").read()
pos, chunks = 8, []
while pos < len(data):
    n, kind = struct.unpack(">I4s", data[pos:pos + 8]); chunks.append((kind, data[pos + 8:pos + 8 + n])); pos += 12 + n
w, h, depth, ctype = struct.unpack(">IIBB", dict(chunks)[b"IHDR"][:10])
raw = zlib.decompress(b"".join(c for k, c in chunks if k == b"IDAT"))
# Distinct byte values across the image (filter bytes included): a flat screen has very few.
sys.exit(0 if len(set(raw[::97])) > 24 else 1)
PY
tap_wait $(( W / 2 )) $(( H / 2 )) "screen .*MainMenuState" 15; sleep 1   # TAP TO BEGIN
shot 2-menu
tap $down_x $down_y; sleep 2            # STORY MODE -> FREEPLAY
tap_wait $ok_x $ok_y "screen .*FreeplayState" 15; sleep 2
shot 3-freeplay
if [ "$pushed" = 1 ]; then
  tap $cx $(( cy - s - g ))             # UP: the last song, Test Tone (its preview decodes the file)
  wait_log "decoded .*Test Tone.wav" 30
  shot 3b-preview
  tap $down_x $down_y; sleep 1          # back to the first song
fi
tap $(( cx - s - g )) $cy; sleep 1       # LEFT: Easy, so missed notes take longer to add up
tap_wait $ok_x $ok_y "song builtin:" 60; sleep 4   # play it
alive "song"; shot 4-song
# Each adb tap takes a second or more, and missed notes drain health: keep
# the time spent playing short.
tap_wait $pause_x $pause_y "paused at" 10; sleep 1
shot 5-pause
tap_wait $ok_x $ok_y "resumed" 10      # Resume
adb shell input keyevent KEYCODE_HOME; sleep 3
adb shell am start -n "$pkg/.GameActivity"
wait_log "back from the background" 20; sleep 1
wait_quiet "paused at" 10 2 || fail "the song did not pause after coming back"
alive "resume"; shot 6-resumed
tap_wait $ok_x $ok_y "resumed" 10      # Resume again
for i in 0 1 3; do                      # taps on the lanes
  tap $(( W * (2 * i + 1) / 8 )) $(( H * 3 / 4 ))
done
tap_wait $pause_x $pause_y "paused at" 10
adb logcat -d -s GodsHand:I | grep "paused at" | tail -n1 | grep -qE ": [1-9][0-9]* presses" || fail "the taps on the lanes did not press them"
shot 7-lanes

adb logcat -d > "$out/logcat.txt"
# Only the game's own lines count: other apps on the emulator log their own errors.
if grep -A2 -E "FATAL EXCEPTION|Fatal signal" "$out/logcat.txt" | grep -q "godshand"; then fail "the game crashed"; fi
if grep -qE " raylib *: FILEIO: .*Failed to open file" "$out/logcat.txt"; then fail "the game could not open a file"; fi
grep -E " (GodsHand|raylib) *:" "$out/logcat.txt" | tail -40
echo "Android smoke test passed"
