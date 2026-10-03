#!/usr/bin/env bash
# Reproduction: launching the app twice leaves two of them running.
#
# Two tray processes is not a cosmetic problem. Each one owns a device worker that claims the
# same two USB interfaces, and claiming the keyboard controller takes it away from usbhid.
# Two processes trading that claim back and forth is how a keyboard ends up missing.
#
# The original guard removed the socket whenever listen() failed, on the assumption that a
# taken name meant a stale socket. It also meant a healthy instance got evicted by any later
# launch. This drives the real binary twice and asserts exactly one process survives.

set -uo pipefail
# shellcheck source=tests/e2e/lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

heading "single instance"

require_binary "single instance" || { summary "single-instance"; exit $?; }
require_display "single instance" || { summary "single-instance"; exit $?; }

BIN="$(tray_bin)"
WORK="$(mktemp -d)"

export XDG_CONFIG_HOME="$WORK/config"
mkdir -p "$XDG_CONFIG_HOME"

# `pgrep -c` prints 0 and exits non-zero when nothing matches, so a `|| echo 0` fallback
# would append a second zero. Count the lines instead.
count_instances() { pgrep -f "^$BIN\$" 2>/dev/null | wc -l | tr -d ' '; }

# The guard has to see ANY running tray, not just one started from this build tree: the
# single-instance socket does not care which binary path claimed it, and a tray running
# from ~/.local/bin would make the launched "first instance" exit on contact.
if pgrep -x alienfx-tray >/dev/null 2>&1; then
  skip "single instance (an alienfx-tray is already running)"
  rm -rf "$WORK"
  summary "single-instance"
  exit $?
fi

# Set only after the skip check: the cleanup must kill the instance this script started,
# never a tray that was already running - the skip path used to terminate exactly the
# instance it had just detected, on its way out.
first_pid=""
trap '[[ -n "$first_pid" ]] && kill -TERM "$first_pid" >/dev/null 2>&1; rm -rf "$WORK"' EXIT

note "starting the first instance"
"$BIN" >"$WORK/first.log" 2>&1 &
first_pid=$!
sleep 4

if ! kill -0 "$first_pid" 2>/dev/null; then
  fail "the first instance exited immediately"
  note "$(cat "$WORK/first.log")"
  summary "single-instance"
  exit $?
fi
pass "the first instance is running"

# Bounded, because the failure mode is that the second launch takes the name for itself and
# then sits there being a second tray icon forever. A test for "it does not hang" must not
# hang while proving it.
note "starting a second instance - it should hand off and exit"
second_start=$(date +%s)
timeout 20 "$BIN" >"$WORK/second.log" 2>&1
second_status=$?
second_elapsed=$(( $(date +%s) - second_start ))

if [[ "$second_status" -eq 124 ]]; then
  fail "the second launch never exited - it started a rival instance instead of handing off"
elif [[ "$second_status" -eq 0 ]]; then
  pass "the second launch exited cleanly (status 0, ${second_elapsed}s)"
else
  fail "the second launch exited with status $second_status"
  note "$(cat "$WORK/second.log")"
fi

sleep 1
instances="$(count_instances)"
if [[ "$instances" == "1" ]]; then
  pass "exactly one instance is running"
else
  fail "$instances instances are running - two processes will fight over the USB interfaces"
fi

# This also exercises panel construction: the hand-off asks the surviving instance to open
# its control panel, so an instance that is still alive here has built and shown the whole
# widget tree without falling over.
if kill -0 "$first_pid" 2>/dev/null; then
  pass "the original instance is the one that survived, with its panel opened"
else
  fail "the original instance was evicted by the second launch"
fi

note "stopping the first instance"
kill -TERM "$first_pid" 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
  kill -0 "$first_pid" 2>/dev/null || break
  sleep 0.5
done

if kill -0 "$first_pid" 2>/dev/null; then
  fail "the instance did not exit on SIGTERM within 5s"
else
  pass "the instance shut down cleanly on SIGTERM"
fi

summary "single-instance"
