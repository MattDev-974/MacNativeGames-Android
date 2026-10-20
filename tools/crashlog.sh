#!/usr/bin/env bash
# crashlog.sh - reproduce the launch and capture why the app stops.
#
#   ./tools/crashlog.sh            # to the terminal
#   ./tools/crashlog.sh out.txt    # to a file
#
# Needs the phone connected with `adb devices` showing it.
set -u

PKG=org.macnativegames.totk
ACT=android.app.NativeActivity
OUT=${1:-}

if ! adb get-state >/dev/null 2>&1; then
    echo "no device: check 'adb devices' and the USB debugging prompt" >&2
    exit 1
fi

TMP=$(mktemp)
adb shell am force-stop "$PKG" >/dev/null 2>&1
sleep 1
adb logcat -c

adb logcat -v threadtime >"$TMP" 2>&1 &
LOGPID=$!
sleep 1

adb shell am start -n "$PKG/$ACT" >/dev/null 2>&1
sleep 8

kill "$LOGPID" 2>/dev/null
wait "$LOGPID" 2>/dev/null

FILTERED=$(grep -E \
    'mnge-|threaded_app|AndroidRuntime|DEBUG   |libtotk|Fatal signal|signal [0-9]+|backtrace|UnsatisfiedLink|NativeActivity|ActivityManager: (Start|Process|Force)|vulkan' \
    "$TMP")

if [ -n "$OUT" ]; then
    printf '%s\n' "$FILTERED" >"$OUT"
    echo "written to $OUT ($(printf '%s\n' "$FILTERED" | wc -l) lines)"
else
    printf '%s\n' "$FILTERED"
fi

rm -f "$TMP"

echo
echo "--- tombstone (if any) ---"
adb shell ls -t /data/tombstones/ 2>/dev/null | head -1 | while read -r t; do
    adb shell "cat /data/tombstones/$t 2>/dev/null" | head -40
done
