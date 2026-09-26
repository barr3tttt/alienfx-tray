#!/usr/bin/env bash
# Reproduction: "I changed the external colour and it flickered then reverted to light blue."
#
# Applies a scheme with two distinct external zone colours and checks, through the
# ALIENFX_TRAY_TIMING breakdown, that the test scheme was the one loaded (exactly its two
# zones were written, not the built-in defaults) and that the external commit ran and
# succeeded. This test cannot see the lights, so it does not prove the colours persist:
# that needs a person looking at the laptop, and the script says what to look for.
#
# Before the fix in SdkExternalDevice::commit() the commit was a no-op because the SDK's
# inSet flag was false after SetPowerAction, so nothing ever sent a "finish and play"
# (COMMV4_control type 3) after the power-state writes.

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
mkdir -p "$TEST_DIR/alienfx-tray"
cat >"$TEST_DIR/alienfx-tray/config.json" <<'EOF'
{
  "activeScheme": "Persist test",
  "lightsOn": true,
  "removedBuiltins": [],
  "schemaVersion": 2,
  "schemes": [
    {
      "external": {
        "brightness": 100,
        "zones": [
          { "color": "#ff0000", "id": 2, "name": "Interior (test red)" },
          { "color": "#00ff00", "id": 3, "name": "Lid (test green)" }
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
output="$(XDG_CONFIG_HOME="$TEST_DIR" ALIENFX_TRAY_TIMING=1 "$BIN" --apply 2>&1)"
status=$?
[[ -n "$output" ]] && note "$output"

count_lines() { grep -c -- "$2" <<<"$1"; }

# Exactly the test scheme's zones, then one commit, and nothing reported as refused.
check_apply() {
  local label="$1" output="$2" zones
  zones="$(count_lines "$output" 'external/applyZone')"
  if [[ "$zones" -ne 2 ]]; then
    fail "$label: expected 2 external zone writes from the test scheme, saw $zones"
  elif [[ "$(count_lines "$output" 'external/commit')" -ne 1 ]]; then
    fail "$label: external commit did not run exactly once"
  elif [[ "$output" == *"refused"* ]]; then
    fail "$label: a controller refused a command"
  else
    pass "$label: both test zones written and the external commit succeeded"
  fi
}

# Both controllers must report ready.
if [[ "$status" -ne 0 ]]; then
  fail "--apply exited with status $status"
elif [[ "$output" != *"External lighting controller ready"* ]]; then
  fail "external controller did not report ready"
elif [[ "$output" != *"Keyboard controller ready"* ]]; then
  fail "keyboard controller did not report ready"
elif [[ "$output" == *"standing down"* ]]; then
  fail "--apply stood down for the device lock"
else
  pass "both controllers reported ready"
fi
check_apply "first apply" "$output"

# Apply the same scheme a second time. Each --apply is a fresh process with a fresh engine,
# so every zone is sent again; this checks the controller is still reachable and not
# wedged after a commit.
sleep 2
note "re-applying the same scheme"
output2="$(XDG_CONFIG_HOME="$TEST_DIR" ALIENFX_TRAY_TIMING=1 "$BIN" --apply 2>&1)"
[[ -n "$output2" ]] && note "$output2"

if [[ "$output2" == *"External lighting controller ready"* ]]; then
  pass "controller still reachable on second apply"
else
  fail "second apply did not succeed"
fi
check_apply "second apply" "$output2"

# A third apply with one zone's colour changed. A fresh process sends both zones again,
# so this checks the new colour is accepted alongside the unchanged one.
cat >"$TEST_DIR/alienfx-tray/config.json" <<'EOF'
{
  "activeScheme": "Persist test",
  "lightsOn": true,
  "removedBuiltins": [],
  "schemaVersion": 2,
  "schemes": [
    {
      "external": {
        "brightness": 100,
        "zones": [
          { "color": "#0000ff", "id": 2, "name": "Interior (test blue)" },
          { "color": "#00ff00", "id": 3, "name": "Lid (test green)" }
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
output3="$(XDG_CONFIG_HOME="$TEST_DIR" ALIENFX_TRAY_TIMING=1 "$BIN" --apply 2>&1)"
[[ -n "$output3" ]] && note "$output3"

if [[ "$output3" == *"External lighting controller ready"* ]]; then
  pass "changed scheme applied"
else
  fail "changed scheme did not apply"
fi
check_apply "changed scheme" "$output3"

note "not verified by this script: look at the laptop now. The interior logo (ID 2) should stay"
note "blue and the lid logo (ID 3) green, and neither should fall back to the firmware light blue"
note "after a few seconds."

summary "external-colour-persistence"