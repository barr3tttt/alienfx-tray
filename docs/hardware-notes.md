# AlienFX hardware notes

Everything here was established by measurement on an Alienware m15 R7 AMD running Linux.
Very little of it appears anywhere else, which is why it is written down.

## Two controllers, not one

| Device | USB ID | API version | Report shape | Drives |
|---|---|---|---|---|
| Darfon | `0d62:cabc` | APIv5 | 64-byte feature reports, report ID `0xCC` | the RGB keyboard |
| Alienware AW-ELC | `187c:0550` | APIv4 | 33-byte reports, vendor page `0xFF00` | rear ring, lid logo, interior logo |

This is the fact that makes advice about this laptop contradict itself. A tool that finds
one controller and reports success has still left half the lights alone. It is also why
"OpenRGB doesn't work" and "OpenRGB works fine" are both true statements about this
machine - OpenRGB's `AlienwareController` can reach `187c:0550`, while its
`AlienwareKeyboardController` only covers the AW410K and AW510K external desktop keyboards,
not the internal Darfon.

The kernel's `alienware-wmi-wmax` driver exposes hwmon and platform-profile only. There are
no `/sys/class/leds` entries for any of this lighting.

## The command that leaves the keyboard dark

In `AlienFX_SDK.cpp`, `Functions::SetGlobalEffects`, case `API_V5`:

```cpp
if (!effType)
    PrepareAndSend(COMMV5_setEffect, {{2, {1, 0xfe}}});
```

An effect type of zero is not "no effect". It is a clear/disable, and it leaves the
keyboard dark in a state that no subsequent colour command can override - `setall`
included. The external controller carries on responding normally, so the machine looks
half-broken rather than obviously wrong. The only recoveries are sending a non-zero effect
type or rebooting.

This application makes zero unrepresentable rather than guarding against it:

- `KeyboardEffect` is a scoped enum with no zero enumerator.
- `kAllKeyboardEffects` is the complete enumerator table, `static_assert`ed non-zero.
- `toWireValue()` is a lookup in that table, not a cast, so its range is the table and
  contains no zero even for a value cast in from outside.
- `IKeyboardDevice::applyEffect` takes a `KeyboardEffect`. There is no `uint8_t` overload.
- Parsing an unrecognised effect name from config resolves to a listed enumerator.

`tests/test_effect_guard.cpp` walks all 256 values the underlying byte can hold and fails
if any of them produces zero.

Turning the lights off is therefore a **black colour with a real effect type**, never a
disable. Brightness is clamped to a minimum of 1 for the same reason: zero brightness is
the other route to a controller that looks dead.

## External lights are power-type lights

They do not respond to ordinary colour commands. `SetColor` across all 23 mapped light IDs
does nothing whatsoever. They need `SetPowerAction`, which walks control IDs `0x5b` through
`0x60` - AC sleep, AC power, charge, battery sleep, battery power, battery critical.

That is also where the AC/battery colour pair comes from. Each light stores two colours and
the controller switches between them itself; exposing that costs nothing because the
protocol requires both slots to be filled anyway.

## Colour channel order

`Afx_colorcode`'s positional constructor takes `(b, g, r)`:

```cpp
Afx_colorcode(uint8_t b, uint8_t g, uint8_t r);
```

This is the entire explanation for "I sent 255 0 0 and the keyboard went blue". The
underlying protocol is not reversed - the *constructor argument order* is. Always assign
`.r`, `.g` and `.b` by name.

## External light IDs

The AW-ELC maps 23 light IDs, `0` through `22`. They carry no self-description: nothing in
the protocol says which fixture an ID drives. The only way to find out is to light one and
look, which is what `alienfx-tray --probe-external` is for.

Observed on an m15 R7 AMD:

| ID | Fixture | Confidence |
|---|---|---|
| 0 | nothing observed | lit during a probe, nothing reported |
| 1 | rear ring | **provisional** - see below |
| 2 | interior logo | confirmed |
| 3 | lid logo | confirmed |
| 4 | nothing observed | lit during a probe, nothing reported |
| 5 | nothing observed | lit during a probe, nothing reported |
| 6-22 | nothing | accept commands, do nothing |

ID 1 is marked provisional deliberately. In the probe that identified 2 and 3, the rear
ring was reported as showing *both* the colour assigned to ID 1 and the colour assigned to
ID 3, while ID 3 was separately identified as the lid logo. That leaves three
possibilities, and guessing between them would be worse than admitting the gap:

1. the ring and the lid logo are driven by the same ID,
2. the ring is segmented across more than one ID,
3. the lid logo sits within the ring assembly and one fixture was described twice.

`alienfx-tray --probe-external=1` lights a single ID white with IDs 0-5 driven black, which
distinguishes them.

## Measured latencies

With `ALIENFX_TRAY_TIMING=1`:

| Operation | Time |
|---|---|
| keyboard open (find path, probe, claim) | ~40 ms |
| keyboard brightness | ~20 ms |
| keyboard colour | ~20 ms |
| external open | ~45 ms |
| external brightness | ~3 ms |
| external zone write, **first after open** | ~1600 ms |
| external zone write, subsequent | ~175 ms |

Two consequences shape the design:

- **Send only what changed.** Six zones at 175 ms is over a second, which would put a
  colour wheel drag visibly behind the cursor. Editing one zone costs one write.
- **Different idle timeouts per domain.** Releasing the external controller costs 1600 ms
  to reopen and gains nothing, so it is held for 20 s. The keyboard controller is released
  after 1.5 s, for the reason in the next section.

For comparison, driving the same six zones through six `alienfx-cli` invocations takes
about 11.6 s, because each process pays full USB enumeration. Linking the SDK rather than
shelling out is worth roughly a factor of four even before change tracking.

## Claiming a controller takes it from the kernel

The SDK builds hidapi against the libusb backend, so talking to either controller means
taking the USB interface away from `usbhid`. On this laptop the keyboard controller is a
real keyboard input device: with `usbhid` bound it appears in `/proc/bus/input/devices` as

```
I: Bus=0003 Vendor=0d62 Product=cabc Version=0110
N: Name="DELL Technologies Keyboard"
H: Handlers=sysrq kbd leds event3
```

While the interface is claimed, that device is gone.

**hidapi does not give it back.** Its `libusb_attach_kernel_driver` calls are inside
`#ifdef DETACH_KERNEL_DRIVER`, and its CMake never defines that macro. Closing the device
releases the interface but leaves it with no driver bound, and it stays that way - not for
a moment, indefinitely, until something reattaches it or the machine reboots. Anything
built on this stack has the same bug, including the reference CLI.

`UsbContext::reattachKernelDriver()` does it explicitly after every close, using
`libusb_attach_kernel_driver`. It needs no root: the `uaccess` udev tag already grants the
descriptor. `tests/e2e/kernel-driver-reattach.sh` drives the real binary and fails if any
interface is left unbound.

The application also never uses `Mappings::AlienFXEnumDevices()`, which runs
`hid_enumerate(0, 0)` and opens every path matching a known vendor ID before discarding the
duplicates - momentarily claiming the keyboard as a side effect of looking around. It
resolves the one path it wants and constructs `Functions` directly.

## An unbounded wait in the SDK

`Functions::WaitForReady()`, case `API_V4`:

```cpp
while (!IsDeviceReady()) usleep(20);
```

No iteration cap, no timeout. It is reachable from `SetPowerAction`, which is the hot path
for every external colour change, so a wedged controller spins a thread forever.

The external device checks `IsDeviceReady()` against a deadline of its own before calling
in, and reports the controller as busy rather than entering the path. That is a filter, not
a guarantee, and it is worth being precise about why: the unbounded spin is not at the
entry. `SetPowerAction`'s `API_V4` branch runs the full `0x5b..0x60` write loop, then
`PrepareAndSend(COMMV4_control)`, then `WaitForBusy()`, and only then `WaitForReady()`. A
controller that answers ready at the pre-check and latches `ALIENFX_V4_BUSY` during the
write therefore still wedges the calling thread, once per zone. The pre-check makes that
rarer. Nothing bounds it.

So the containment is structural instead. The device thread is the only thread that can be
wedged, and nothing that must finish is allowed to wait on it without a bound: `DeviceThread`
owns it, hands out no handle to it, and its only stop path is a bounded one that walks away
and says so if the thread does not come back. `LightingController`'s apply watchdog reports
every affected domain as `DeviceState::Unresponsive` through the ordinary per-domain status
path, so a wedge reaches the UI rather than leaving the lights silently frozen. That state
exists separately from `Error` because a refusal came back and a hang did not, and telling
someone a controller "refused the last command" when it is hanging sends them looking for a
udev rule instead of a stuck device. Escalation covers every state that has not already
admitted failure - `Ready`, `Unknown`, `Busy` and `Waiting` - because each of them leaves the
user expecting lighting that is not coming, `Waiting` most of all: it promises the frame will
be repainted "automatically until it lets go", which a wedged worker never delivers.
`Missing` and `Error` are left alone, having already said the domain is not working. The
kernel reattaches `usbhid` when the usbfs descriptor closes on process exit, so even the
walk-away recovers.

## Nothing persists unless asked

The SDK's `SaveLightsState` / `SaveLightsStateToStartup` write the current state into the
controller's own memory. This application never calls them. A reboot always restores the
firmware default, which is what makes the hardware safe to experiment with at all, and
login and resume repaint the saved scheme anyway - so persistence in firmware would buy
nothing and cost the safety net.
