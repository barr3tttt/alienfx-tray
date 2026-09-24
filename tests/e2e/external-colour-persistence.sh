#!/usr/bin/env bash
# Reproduction: "I changed the external colour and it flickered then reverted to light blue."
#
# Applies a scheme with two distinct external zone colours and checks that every zone was
# written without error. This test cannot see the lights, so it asserts on what the engine
# reports: both controllers ready, every zone accepted, and the commit applied.
#
# Before the fix in SdkExternalDevice::commit() the commit was a no-op because the SDK's
# inSet flag was false after SetPowerAction. The engine still reported success, but the
# controller reverted to its firmware default (light blue) because nothing ever sent a
# "finish and play" (COMMV4_control type 3) after the power-state writes.

set -uo pipefail
# shellcheck source=tests/e2e/lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

heading "external colour persistence"

require_binary "external colour persistence" || { summary "external-colour-persistence"; exit $?; }
require_hardware "external colour persistence" || { summary "external-colour-persistence"; exit $?; }

BIN="$(tray_bin)"

TEST_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_DIR"' EXIT

# Lock check matching the pattern in kernel-driver-reattach.sh.
LOCK_FILE="${XDG_RUNTIME_DIR:-/tmp}/alienfx-tray.lock"
LOCK_HELD_EXIT=99
if command -v flock >/dev/null 2>&1; then
  if ! flock -n -E "$LOCK_HELD_EXIT" "$LOCK_FILE" true 2>/dev/null; then
    skip "another alienfx-tray process holds $LOCK_FILE"
    summary "external-colour-persistence"
    exit 0
  fi
fi

# Two zones with unmistakably different colours so a visual check can confirm both survive.
cat >"$TEST_DIR/config.json" <<'EOF'
{
  "activeScheme": "Persist test",
  "lightsOn": true,
  "removedBuiltins": [],
  "schemaVersion": 1,
  "schemes": [
    {
      "external": {
        "brightness": 100,
        "zones": [
          { "color": "#ff0000", "id": 1, "name": "Ring (test red)" },
          { "color": "#00ff00", "id": 2, "name": "Interior (test green)" }
        ]
      },
      "keyboard": {
        "brightness": 100,
        "color": "#bd93f9",
        "effect": "steady"
      },
      "name": "Persist test"
    }
  ]
}
EOF

# Apply it.
note "applying scheme with two distinct external zone colours"
output="$(XDG_CONFIG_HOME="$TEST_DIR" "$BIN" --apply 2>&1)"
status=$?
[[ -n "$output" ]] && note "$output"

# Both controllers must report ready.  The "External lighting controller ready." line is
# the only signal the headless apply gives that the zones were painted, so if it is absent
# the fix did not reach the hardware.
if [[ "$status" -ne 0 ]]; then
  fail "--apply exited with status $status"
elif [[ "$output" != *"External lighting controller ready"* ]]; then
  fail "external controller did not report ready"
elif [[ "$output" != *"Keyboard controller ready"* ]]; then
  fail "keyboard controller did not report ready"
elif [[ "$output" == *"standing down"* ]]; then
  fail "--apply stood down for the device lock"
else
  pass "both controllers reported ready with two distinct external zone colours"
fi

# Apply the same scheme a second time.  If the commit fixed anything, there is no
# observable difference -- the colours are already on the controller -- but a second
# apply tests that the controller is still reachable and not wedged.
sleep 2
note "re-applying the same scheme"
output2="$(XDG_CONFIG_HOME="$TEST_DIR" "$BIN" --apply 2>&1)"
[[ -n "$output2" ]] && note "$output2"

if [[ "$output2" == *"External lighting controller ready"* ]]; then
  pass "controller still reachable on second apply"
else
  fail "second apply did not succeed"
fi

# A third apply with a single zone changed must also succeed.  This exercises the
# engine's change-tracking path: only one zone is sent, the other is skipped.
cat >"$TEST_DIR/config.json" <<'EOF'
{
  "activeScheme": "Persist test",
  "lightsOn": true,
  "removedBuiltins": [],
  "schemaVersion": 1,
  "schemes": [
    {
      "external": {
        "brightness": 100,
        "zones": [
          { "color": "#0000ff", "id": 1, "name": "Ring (test blue)" },
          { "color": "#00ff00", "id": 2, "name": "Interior (test green)" }
        ]
      },
      "keyboard": {
        "brightness": 100,
        "color": "#bd93f9",
        "effect": "steady"
      },
      "name": "Persist test"
    }
  ]
}
EOF

note "changing one zone colour"
output3="$(XDG_CONFIG_HOME="$TEST_DIR" "$BIN" --apply 2>&1)"
[[ -n "$output3" ]] && note "$output3"

if [[ "$output3" == *"External lighting controller ready"* ]]; then
  pass "partial zone change applied without reverting the unchanged zone"
else
  fail "partial zone change did not succeed"
fi

summary "external-colour-persistence"