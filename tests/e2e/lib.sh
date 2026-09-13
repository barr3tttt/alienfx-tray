#!/usr/bin/env bash
# Shared helpers for the end-to-end scripts.
#
# These drive the real binary against real hardware and a real session. They are not part
# of ctest, because ctest has to pass on a build machine with neither. Run them with
# tests/e2e/run-all.sh on the target laptop.

set -uo pipefail

E2E_PASS=0
E2E_FAIL=0
E2E_SKIP=0

# USB identities of the two controllers.
KEYBOARD_VID_PID="0d62:cabc"
EXTERNAL_VID_PID="187c:0550"

pass() { printf '  PASS  %s\n' "$1"; E2E_PASS=$((E2E_PASS + 1)); }
fail() { printf '  FAIL  %s\n' "$1"; E2E_FAIL=$((E2E_FAIL + 1)); }
skip() { printf '  SKIP  %s\n' "$1"; E2E_SKIP=$((E2E_SKIP + 1)); }

note() { printf '        %s\n' "$1"; }

heading() { printf '\n== %s ==\n' "$1"; }

# Resolves the binary under test. Override with ALIENFX_TRAY_BIN.
tray_bin() {
  if [[ -n "${ALIENFX_TRAY_BIN:-}" ]]; then
    printf '%s' "$ALIENFX_TRAY_BIN"
    return
  fi
  local root
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  printf '%s' "$root/build/alienfx-tray"
}

require_binary() {
  local bin
  bin="$(tray_bin)"
  if [[ ! -x "$bin" ]]; then
    skip "$1 (no binary at $bin - build first, or set ALIENFX_TRAY_BIN)"
    return 1
  fi
  return 0
}

# Both controllers present on the USB bus?
have_hardware() {
  command -v lsusb >/dev/null 2>&1 || return 1
  lsusb 2>/dev/null | grep -qi "$KEYBOARD_VID_PID" || return 1
  lsusb 2>/dev/null | grep -qi "$EXTERNAL_VID_PID" || return 1
  return 0
}

require_hardware() {
  if ! have_hardware; then
    skip "$1 (needs an Alienware laptop with both controllers attached)"
    return 1
  fi
  return 0
}

require_display() {
  if [[ -z "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]]; then
    skip "$1 (needs a graphical session)"
    return 1
  fi
  return 0
}

# Every sysfs USB interface directory belonging to a given vid:pid.
usb_interfaces_for() {
  local want="$1" device vendor product
  for device in /sys/bus/usb/devices/*; do
    [[ -f "$device/idVendor" && -f "$device/idProduct" ]] || continue
    vendor="$(cat "$device/idVendor")"
    product="$(cat "$device/idProduct")"
    [[ "$vendor:$product" == "$want" ]] || continue
    for interface in "$device/$(basename "$device"):"*; do
      [[ -d "$interface" ]] && printf '%s\n' "$interface"
    done
  done
}

# The kernel driver bound to a sysfs interface, or "NONE".
driver_of() {
  local interface="$1"
  if [[ -e "$interface/driver" ]]; then
    basename "$(readlink -f "$interface/driver")"
  else
    printf 'NONE'
  fi
}

summary() {
  printf '\n%s: %d passed, %d failed, %d skipped\n' "$1" "$E2E_PASS" "$E2E_FAIL" "$E2E_SKIP"
  [[ "$E2E_FAIL" -eq 0 ]]
}
