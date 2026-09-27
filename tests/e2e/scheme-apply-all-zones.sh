#!/usr/bin/env bash
# Hardware check for the editing model: every scheme in a scratch config of the shipped
# built-ins is activated through a real LightingController on the real controllers, twice,
# and every zone's commanded colour is listed and verified. This is the end-to-end proof
# for "selecting a scheme sets every zone, every time" - including re-selecting the scheme
# that is already active, which used to command nothing at all.
#
# The user's own lighting is restored at the end with a plain --apply of the real config.

set -uo pipefail
# shellcheck source=tests/e2e/lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

heading "scheme apply: every zone, every time"

require_binary "scheme apply all zones" || { summary "scheme-apply-all-zones"; exit $?; }
require_hardware "scheme apply all zones" || { summary "scheme-apply-all-zones"; exit $?; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DRIVER="$ROOT/build/scheme_apply_driver"
BIN="$(tray_bin)"

if [[ ! -x "$DRIVER" ]]; then
  skip "no driver at $DRIVER - build first"
  summary "scheme-apply-all-zones"
  exit $?
fi

# Lock check matching the pattern in kernel-driver-reattach.sh: the driver politely waits
# for the lock, but a process that holds it for good would stall the whole check.
LOCK_FILE="${XDG_RUNTIME_DIR:-/tmp}/alienfx-tray.lock"
LOCK_HELD_EXIT=99
if command -v flock >/dev/null 2>&1; then
  if ! flock -n -E "$LOCK_HELD_EXIT" "$LOCK_FILE" true 2>/dev/null; then
    skip "another alienfx-tray process holds $LOCK_FILE"
    summary "scheme-apply-all-zones"
    exit 0
  fi
fi

TEST_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_DIR"' EXIT

note "activating every built-in scheme twice on the real controllers"
output="$(XDG_CONFIG_HOME="$TEST_DIR" "$DRIVER" 2>&1)"
status=$?
[[ -n "$output" ]] && note "$output"

if [[ "$status" -ne 0 ]]; then
  fail "driver reported failures (status $status)"
else
  pass "every scheme set every zone on both activation and re-selection"
fi

# Put the user's lighting back the way the check found it.
note "restoring the saved scheme with --apply"
restore="$("$BIN" --apply 2>&1)"
restore_status=$?
[[ -n "$restore" ]] && note "$restore"
if [[ "$restore_status" -ne 0 ]]; then
  fail "restoring the saved scheme failed (status $restore_status)"
else
  pass "saved scheme restored"
fi

summary "scheme-apply-all-zones"
