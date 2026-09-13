# alienfx-tray

RGB lighting control for Alienware laptops on Linux, as a KDE Plasma system-tray
application.

There is no other GUI for this hardware. OpenRGB does not support the internal keyboard
controller on this generation, the older AKBL / `alienfx` / `alienfx-gtk` projects speak a
protocol these machines dropped, and the one tool that does drive both controllers is
command-line only. This fills that gap, so that lighting is never a reason to reboot into
Windows.

Developed and tested on an Alienware m15 R7 AMD running Fedora with KDE Plasma 6 on
Wayland.

## What it does

- A tray icon that fits Plasma 6 on Wayland. Left-click opens the control panel,
  right-click gives schemes and an on/off toggle.
- Independent control of the two lighting domains: the keyboard, and the external lights
  (rear ring, lid logo, interior logo).
- Real colour pickers with live preview while you drag.
- Named schemes, saved to disk and switchable from the tray.
- Applied at login and after resume, through the mechanisms already on the machine.
- A device that is missing or busy is reported in the UI. Nothing blocks on USB.

## Building

Needs CMake 3.21+, a C++23 compiler, and Qt 6 (Core, Gui, Widgets, Network, Svg).
libusb, hidapi, loguru and nlohmann/json are fetched at configure time at pinned
revisions.

```sh
git clone --recurse-submodules https://github.com/barr3tttt/alienfx-tray.git
cd alienfx-tray
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix ~/.local
```

On Fedora the build dependencies are `qt6-qtbase-devel qt6-qtsvg-devel cmake ninja-build
gcc-c++`. On Debian and Ubuntu they are `qt6-base-dev qt6-svg-dev cmake ninja-build g++`.

## Device access

Both controllers need to be reachable without root. On a distribution using systemd-logind
seats, a `uaccess` tag is the cleanest way:

```
# /etc/udev/rules.d/60-alienfx.rules
SUBSYSTEM=="usb", ATTR{idVendor}=="0d62", ATTR{idProduct}=="cabc", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="187c", ATTR{idProduct}=="0550", TAG+="uaccess"
```

Then `sudo udevadm control --reload && sudo udevadm trigger`.

## Command line

```
alienfx-tray                    run the tray icon
alienfx-tray --apply            apply the saved scheme and exit
alienfx-tray --off              black out every light and exit
alienfx-tray --probe-external   light each external zone a different colour
alienfx-tray --probe-external=N light one external zone on its own
```

`--apply` never opens a display, which is what makes it usable from a resume hook.

`--probe-external` exists because the external light IDs carry no self-description. The
only way to learn which ID drives which fixture is to light one and look, so the probe
lights them in distinct colours and you write down what you see.

## Applying at login and after resume

This ships no startup mechanism of its own. If you already have a user unit and a
system-sleep hook calling `~/.local/bin/alienfx-apply` - the usual arrangement for this
hardware - the Schemes tab has a button that makes that one script delegate here, backing
up whatever was there first. The unit and the hook are not touched.

Note that a *user* systemd unit cannot handle resume: the user manager has no
`suspend.target`, so such a unit silently never fires. The hook has to be at system level.

If you have no such setup, the button creates the script and you point a unit at it:

```ini
# ~/.config/systemd/user/alienfx.service
[Unit]
Description=Apply AlienFX lighting scheme
After=graphical-session.target
PartOf=graphical-session.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=%h/.local/bin/alienfx-apply

[Install]
WantedBy=graphical-session.target
```

```sh
# /usr/lib/systemd/system-sleep/alienfx
#!/bin/sh
[ "$1" = "post" ] || exit 0
# $HOME must stay single-quoted: the hook runs as root, so a double-quoted one would
# expand to /root before su ever sees it.
su - "$SLEEP_USER" -c '$HOME/.local/bin/alienfx-apply' >/dev/null 2>&1 &
exit 0
```

## Configuration

`~/.config/alienfx-tray/config.json`, written atomically and safe to hand-edit. An
unparseable file falls back to the built-in defaults rather than leaving you in the dark.

Nothing is ever written to the controllers' own memory, so a reboot always restores the
firmware default. That is deliberate: it means no scheme you try can strand the machine.

## Hardware notes

`docs/hardware-notes.md` documents what these controllers actually do - the two APIs, the
command that bricks the keyboard until reboot, the colour channel order, the measured
latencies, and the kernel driver that hidapi forgets to hand back. Most of it is not
written down anywhere else.

## Licence

MIT. See `LICENSE`.
