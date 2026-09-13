# alienfx-tray

High-level project memory for agents. Keep it current; append lessons on error.

## What this is
A Qt6 system-tray application that controls the RGB lighting on Alienware laptops from
Linux, built for KDE Plasma 6 on Wayland. It exists because there is no GUI for this
hardware anywhere: OpenRGB does not support the internal keyboard controller, the older
Python and GTK projects speak a protocol this generation dropped, and the one tool that
does drive both controllers is command-line only. The goal is that lighting is never a
reason to reboot into Windows.

## Repository map
- `src/core/` - the lighting model. No Qt, no USB, no SDK. Colours, effects, schemes,
  config, and the engine that turns a scheme into device calls. Fully testable anywhere.
- `src/device/` - the only code that touches USB or the AlienFX SDK. `AlienFX_SDK.h` is
  included in exactly one translation unit here, because it does `using namespace std;` at
  file scope.
- `src/app/` - Qt plumbing: the device worker thread, the controller the GUI talks to, the
  headless `--apply` modes, the login/resume integration, single-instance, signals.
- `src/ui/` - widgets: colour picker, control panel, tray icon.
- `tests/` - ctest suites, run with no hardware and no display.
- `tests/e2e/` - end-to-end scripts that drive the real binary against real hardware. They
  skip themselves when the hardware or a graphical session is missing.
- `third_party/alienfx-linux` - the SDK, a pinned submodule.

## Terminology
- **Domain** - one of the two independent lighting controllers. `Keyboard` or `External`.
  They share no command vocabulary and fail independently.
- **Zone** - one addressable light on the external controller, identified by its light ID.
- **Scheme** - a named set of colours and brightnesses covering both domains.

## Runtime conventions
- Build: `cmake -S . -B build -G Ninja && cmake --build build`
- Test: `ctest --test-dir build --output-on-failure`
- End-to-end (needs the laptop): `tests/e2e/run-all.sh`
- Run: `./build/alienfx-tray`, or `--apply` / `--off` / `--probe-external[=N]` headless
- Timing breakdown: `ALIENFX_TRAY_TIMING=1 ./build/alienfx-tray --apply`

## Hardware notes
Everything here was established by measurement on an Alienware m15 R7 AMD. Most of it is
written down nowhere else. `docs/hardware-notes.md` has the long form.

### There are two controllers, not one
| Device | USB ID | API | Drives |
|---|---|---|---|
| Darfon | `0d62:cabc` | AlienFX APIv5, 64-byte feature reports, report ID `0xCC` | the RGB keyboard |
| Alienware AW-ELC | `187c:0550` | AlienFX APIv4, 33-byte reports, vendor page `0xFF00` | rear ring, lid logo, interior logo |

This single fact explains why advice about this laptop contradicts itself. Anything that
finds one controller and calls it a day gets half the lights.

`alienware-wmi-wmax` exposes hwmon and platform-profile only. There are no
`/sys/class/leds` entries for any of this; do not go looking.

### Effect type 0 is a trap, and the guard is structural
`SetGlobalEffects` for `API_V5` does `if (!effType) PrepareAndSend(COMMV5_setEffect, {{2,
{1, 0xfe}}})`. That is a clear/disable which leaves the keyboard dark in a state no later
colour set can override. External lights keep working, so the machine looks half-broken.
Recovery is a non-zero effect type or a reboot.

`KeyboardEffect` is therefore a scoped enum with no zero enumerator, `toWireValue()` is a
table lookup rather than a cast so its range provably excludes zero, and the device
interface has no `uint8_t` overload. `tests/test_effect_guard.cpp` walks all 256 byte
values through the conversion. Do not add an `Off` enumerator; "off" is a black colour.

### External lights are power-type lights
They need `SetPowerAction`, not `SetColor`. A plain colour set across all 23 mapped IDs
does nothing at all. Each light carries two colours, AC and battery, which is where the
"different colour on battery" feature comes from - it is the hardware's own behaviour.

### Colour channel order
`Afx_colorcode`'s positional constructor is `(b, g, r)`, not `(r, g, b)`. That is the whole
explanation for "I sent 255 0 0 and got blue". Always assign the named fields.

### Measured latencies
- keyboard open ~40 ms, brightness ~20 ms, colour ~20 ms
- external open ~45 ms, brightness ~3 ms
- **first** external zone write after an open: ~1600 ms
- every external zone write after that: ~175 ms

Those numbers drive two design decisions: the engine sends only what changed, and the two
domains get different idle-release timeouts.

### Brightness is also the "on" command
On APIv4 the brightness command carries the list of light IDs and is what turns those
lights on, so it has to be resent whenever the set of zones changes. On APIv5 it is what
wakes the controller.

### Nothing persists without an explicit save
A reboot restores firmware defaults, which is what makes this hardware safe to experiment
with. This application never issues the SDK's save-to-device commands at all - login and
resume repaint anyway, so persistence would buy nothing and cost the safety net.

## Lessons (append-only)
- 2026-09-13: hidapi's libusb backend never re-attaches the kernel driver. Its
  `libusb_attach_kernel_driver` calls sit inside `#ifdef DETACH_KERNEL_DRIVER` and its
  CMake never defines that macro. Closing a device therefore leaves the USB interface with
  no driver bound, permanently, until something reattaches it or the machine reboots. On
  this laptop that removes a real keyboard input device. `UsbContext::reattachKernelDriver`
  does it explicitly after every close; `tests/e2e/kernel-driver-reattach.sh` proves it.
  Never assume a USB library hands the interface back.
- 2026-09-13: `QLocalServer::listen()` with `UserAccessOption` does not give an exclusive
  name. Qt puts the socket in a fresh private directory and repoints a symlink at it on
  every listen, so a second process "succeeds" and you get two instances claiming the same
  USB interfaces. Bind an explicit absolute path under `XDG_RUNTIME_DIR` instead - it is
  already mode 0700, so the access restriction is the same and a second bind actually fails.
- 2026-09-13: on listen failure, never remove the socket before checking whether something
  is alive on it. Removing unconditionally evicts a healthy instance.
- 2026-09-13: `AlienFX_SDK`'s `WaitForReady()` has no timeout on its APIv4 branch -
  `while (!IsDeviceReady()) usleep(20);`. It is reachable from `SetPowerAction`, which is
  the hot path. Never call into it without checking readiness first against a deadline of
  our own, and never let the GUI thread or process exit depend on it returning.
- 2026-09-13: `Mappings::AlienFXEnumDevices()` runs `hid_enumerate(0, 0)` and opens every
  path matching a known vendor ID before discarding duplicates. On this laptop that
  momentarily claims the keyboard. Resolve the one device path wanted and construct
  `Functions` directly instead.
- 2026-09-13: `pgrep -c` prints `0` and exits non-zero when nothing matches, so
  `pgrep -c ... || echo 0` yields `00`. Count lines instead.
- 2026-09-13: `cp` is aliased to interactive on this machine. A plain `cp` in a script can
  silently do nothing and leave you testing a stale binary. Use `command cp -f`.
