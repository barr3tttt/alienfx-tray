#!/usr/bin/env bash
# Reproduction: "I changed my lighting and now a keyboard is missing."
#
# The SDK's hidapi is built against the libusb backend. To talk to either controller it has
# to take the USB interface away from the kernel's usbhid driver. hidapi's code to give it
# back lives inside `#ifdef DETACH_KERNEL_DRIVER`, and its CMake never defines that macro -
# so the interface is released but left with no driver bound, and it stays that way until
# something reattaches it or the machine reboots.
#
# On an Alienware m15 R7 AMD the keyboard controller (0d62:cabc) is a real keyboard input
# device: with usbhid bound it appears in /proc/bus/input/devices as a kbd handler. Leave it
# unbound and that device is simply gone.
#
# This script drives the real binary the way login does - `alienfx-tray --apply` - and then
# asserts the kernel got both interfaces back. Before the fix in UsbContext, it fails.

set -uo pipefail
# shellcheck source=tests/e2e/lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

heading "kernel driver reattach"

require_binary "kernel driver reattach" || { summary "kernel-driver-reattach"; exit $?; }
require_hardware "kernel driver reattach" || { summary "kernel-driver-reattach"; exit $?; }

BIN="$(tray_bin)"

report_state() {
  local label="$1" vid_pid interface
  for vid_pid in "$KEYBOARD_VID_PID" "$EXTERNAL_VID_PID"; do
    while read -r interface; do
      [[ -n "$interface" ]] || continue
      note "$label $(basename "$interface") -> $(driver_of "$interface")"
    done < <(usb_interfaces_for "$vid_pid")
  done
}

# A run that stood down for the device lock never opens an interface at all, and --apply
# reports that as success on purpose. Every assertion below would then pass having tested
# nothing, so refuse to start rather than green-light a vacuous run.
#
# Only a genuine conflict may skip. flock also exits non-zero when it is absent or cannot
# open the lock path, and reporting either of those as "another process holds it" would be a
# false reason to not run the one guard this failure has. -E gives the conflict its own exit
# code so the two cannot be confused; anything else falls through to the run, where the
# stand-down check below is still watching.
LOCK_FILE="${XDG_RUNTIME_DIR:-/tmp}/alienfx-tray.lock"
LOCK_HELD_EXIT=99
lock_probe=0
if command -v flock >/dev/null 2>&1; then
  flock -n -E "$LOCK_HELD_EXIT" "$LOCK_FILE" true 2>/dev/null
  lock_probe=$?
else
  note "flock is not installed, so the device lock cannot be checked before the run"
fi

if [[ "$lock_probe" -eq "$LOCK_HELD_EXIT" ]]; then
  skip "another alienfx-tray process holds $LOCK_FILE - this run would stand down without \
claiming an interface, which proves nothing about the driver handback"
  summary "kernel-driver-reattach"
  exit $?
elif [[ "$lock_probe" -ne 0 ]]; then
  note "flock could not test $LOCK_FILE (exit $lock_probe) - running anyway"
fi

report_state "before:"

note "running: $BIN --apply"
apply_output="$("$BIN" --apply 2>&1)"
apply_status=$?
[[ -n "$apply_output" ]] && note "$apply_output"

report_state "after: "

# 1. Every interface on both controllers is back under a kernel driver.
unbound=0
for vid_pid in "$KEYBOARD_VID_PID" "$EXTERNAL_VID_PID"; do
  while read -r interface; do
    [[ -n "$interface" ]] || continue
    if [[ "$(driver_of "$interface")" == "NONE" ]]; then
      unbound=$((unbound + 1))
      note "still unbound: $interface"
    fi
  done < <(usb_interfaces_for "$vid_pid")
done

if [[ "$unbound" -eq 0 ]]; then
  pass "every USB interface was handed back to its kernel driver"
else
  fail "$unbound USB interface(s) left with no kernel driver - a keyboard is missing until reboot"
fi

# 2. The keyboard controller is visible as an input device again. This is the symptom the
#    user actually notices, so assert on it directly rather than on sysfs alone.
if [[ -r /proc/bus/input/devices ]]; then
  if grep -qi "Vendor=${KEYBOARD_VID_PID%%:*}" /proc/bus/input/devices; then
    pass "keyboard controller is present in /proc/bus/input/devices"
  else
    fail "keyboard controller has no input device - usbhid is not bound to it"
  fi
else
  skip "/proc/bus/input/devices is unreadable"
fi

# 3. The apply itself has to have worked. A run that reattaches cleanly but never lit
#    anything would pass the checks above while being useless - and a lock stand-down is
#    exactly that, reported as success, so the pre-flight check is backed up here in case
#    the lock was taken between that check and the run.
if [[ "$apply_output" == *"standing down"* ]]; then
  fail "--apply stood down for the device lock, so no interface was ever claimed"
elif [[ "$apply_status" -eq 0 ]]; then
  pass "--apply reported success"
else
  fail "--apply exited with status $apply_status"
fi

summary "kernel-driver-reattach"
