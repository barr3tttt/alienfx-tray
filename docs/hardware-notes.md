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
does nothing whatsoever. They need the APIv4 power path, which walks control IDs `0x5b`
through `0x60` - AC sleep, AC power, charge, battery sleep, battery power, battery critical.

That is also where the AC/battery colour pair comes from. Each light stores two colours and
the controller switches between them itself; exposing that costs nothing because the
protocol requires both slots to be filled anyway.

## The power blocks are shared staging, not per-light memory

Measured 2026-10-03, after the owner reported that selecting a scheme moved the keyboard
and the lid logo but left the rear ring and the interior logo stuck.

The six control IDs `0x5b`..`0x60` are not six slots per light. They are six global staging
blocks, and each block binds a light selection to a set of actions. The SDK's
`SetPowerAction(light)` fills all six blocks with that one light's colours, then sends the
"play" control report. Calling it once per light - the obvious way to apply a multi-zone
scheme - re-stages every block for each new light, and the closing finish-and-play then
applies only the last light written:

- The last zone written took the scheme's colour. With zones in ID order that is the lid
  logo (ID 3), which made the bug read as "only the lid logo follows schemes".
- Every other zone kept whatever it was already showing. After a reboot that is the
  firmware default: the rear ring sits light blue, the interior logo stays dark. A zone the
  user had set by hand kept that colour forever, which is why the same bug was also
  reported as "the ring is stuck on the colour I set it to manually".
- Single-zone writes were never affected, which is why the zone-identification probe and
  manual edits all worked and the scheme path alone looked broken.

The fix is to stage every zone into each block within one transaction: remove block, start
block, then per zone a colour-select report (`COMMV4_colorSel`, which exists precisely to
bind a block entry to a light ID) followed by that zone's colour-set reports, then
finish-and-save. After all six blocks: one "play", then the finish-and-play commit. A
one-zone batch is byte-identical to the single `SetPowerAction` call it replaces; the wire
shape only changes from two lights up.

The application builds this transaction itself over `PrepareAndSend` (see
`SdkExternalDevice::applyZones`) instead of calling `SetPowerAction` once per light.

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
| 0 | rear ring | confirmed |
| 1 | rear ring | confirmed |
| 2 | interior logo | confirmed |
| 3 | lid logo | confirmed |
| 4 | no visible light | confirmed |
| 5 | no visible light | confirmed |
| 6-22 | nothing | accept commands, do nothing |

The rear ring is driven by two light IDs, 0 and 1.  Before the colour-persistence
fix a mono-colour probe of ID 0 appeared to do nothing because the colour washed
out; once colours held, lighting either ID white produced a visible white ring.
The application presents them as two separate zones so each pair of quadrants can be
coloured independently, with the quadrant pairing named in the zone labels. The probe
addresses them with distinct colours for identification.

Observed 2026-10-03 with the four-colour probe: the two IDs are not halves of the ring.
Each drives a pair of diagonal quadrants - ID 0 the top-left and bottom-right, ID 1 the
other two - and since the v5 schema the default zone labels say exactly that, so nobody
expects a left/right split. Also noted the same day: a pure red (255,0,0) reads orange on
the ring's LEDs, while the logos render it red. The ring's channel response differs from
the other fixtures; no correction is applied anywhere in the application. A per-fixture
correction, if one is ever wanted, belongs in the device layer and behind a fresh probe
measurement - not guessed.

IDs 4 and 5 accept commands and their power-state writes roll through the SDK's
0x5b--0x60 loop, but they produce no visible light on this chassis, so the application does
not list them as zones. Configs written before schema version 3 are migrated once on load:
zones 4 and 5 are dropped (v1->v2), a rear ring carried as one entry is split into two
segments that start with its settings while segments already stored separately keep their
own (v2->v3), and the original file is kept as `config.json.bak`. Configs written before
schema version 4 additionally have the shipped contents of built-in-named schemes restored
(v3->v4): the editing model before v4 wrote every manual edit into the selected scheme with
no save gesture, so a built-in whose colours differ from the shipped ones was damaged by
that write-through rather than edited on purpose. The v4->v5 migration renames zones still
carrying an old default name to the current defaults, whose rear-ring labels name the
diagonal quadrant pairing; a zone the user renamed keeps its name.

## Measured latencies

With `ALIENFX_TRAY_TIMING=1`, measured 2026-10-03 on this laptop with the batched power
transaction (one cold `--apply`; earlier per-zone figures are from 2026-09-27):

| Operation | Time |
|---|---|
| keyboard open (find path, probe, claim) | ~110-135 ms |
| keyboard brightness | ~20 ms |
| keyboard colour | ~20 ms |
| keyboard close (incl. driver reattach) | ~100 ms |
| external open | ~45-52 ms |
| external brightness | ~2-3 ms |
| external zone transaction, all 4 zones in one batch | ~261 ms |
| external zone transaction, single zone | ~110-140 ms |
| external commit (remove + start + finish-and-play) | ~12-15 ms |

The single-zone figure is the delta path a colour-wheel drag uses: a one-zone batch puts
exactly the bytes on the wire that a lone `SetPowerAction` used to, so a drag frame still
costs about 110-140 ms. A full four-zone repaint takes one ~260 ms transaction plus a
commit instead of four ~110-140 ms writes plus a commit, so the batch is also the faster
shape, at about half the old cost.

The two design decisions the earlier measurements drove still stand:

- **Send only what changed.** Preview frames send the zone being edited, nothing else.
- **Different idle timeouts per domain.** Releasing the keyboard controller quickly is
  worth real money: it is the interface the user types on, and every claim steals it from
  usbhid. Releasing the external controller buys nothing - nothing else wants it, no input
  device is affected, and reopening still costs a round of enumeration - so it is held for
  20 s while the keyboard is given back after 1.5 s.

For comparison, driving six zones through six `alienfx-cli` invocations takes
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

No iteration cap, no timeout. It is reachable from `SetPowerAction` and from `Reset()`, the
SDK's pieces of the external write path.

The application no longer calls either. The power transaction and the finish-and-play
commit are hand-rolled over `PrepareAndSend` (which performs a single synchronous write and
returns), and every readiness wait on the path is `SdkExternalDevice::waitUntilReady()`,
bounded by a 250 ms deadline. A controller that does not settle is reported busy; no call
into the SDK can spin the device thread forever.

The containment for a device that hangs anyway still stands: the device thread is the only
thread that can be wedged, nothing that must finish is allowed to wait on it without a
bound (`DeviceThread` owns it, hands out no handle to it, and its only stop path is a
bounded one that walks away and says so if the thread does not come back), and
`LightingController`'s apply watchdog reports every affected domain as
`DeviceState::Unresponsive` through the ordinary per-domain status path, so a wedge reaches
the UI rather than leaving the lights silently frozen. That state exists separately from
`Error` because a refusal came back and a hang did not, and telling someone a controller
"refused the last command" when it is hanging sends them looking for a udev rule instead of
a stuck device. Escalation covers every state that has not already admitted failure -
`Ready`, `Unknown`, `Busy` and `Waiting` - because each of them leaves the user expecting
lighting that is not coming, `Waiting` most of all: it promises the frame will be repainted
"automatically until it lets go", which a wedged worker never delivers. `Missing` and
`Error` are left alone, having already said the domain is not working. The kernel
reattaches `usbhid` when the usbfs descriptor closes on process exit, so even the walk-away
recovers.

## Nothing persists unless asked

The SDK's `SaveLightsState` / `SaveLightsStateToStartup` write the current state into the
controller's own memory. This application never calls them. A reboot always restores the
firmware default, which is what makes the hardware safe to experiment with at all, and
login and resume repaint the saved scheme anyway - so persistence in firmware would buy
nothing and cost the safety net.
