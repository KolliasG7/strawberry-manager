# Strawberry Kernel Manager

Strawberry Kernel Manager is a native C11 + GTK4 desktop application for Strawberry PS4 Linux systems. It provides a lightweight control panel for hardware features exposed by Strawberry kernel builds, including fan threshold control, front LED control, GPU SCLK control, HDMI reprobe, and live system status.

Current version: `0.2.0`

## Screenshots

![Strawberry Kernel Manager - Screenshot 1](https://github.com/user-attachments/assets/5becc19a-a1f8-43c2-af83-40bb5b4794b2)

![Strawberry Kernel Manager - Screenshot 2](https://github.com/user-attachments/assets/3eb56bb1-5d20-47e3-bfaf-879cc7aea9ee)

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Supported Hardware and Kernel Interfaces](#supported-hardware-and-kernel-interfaces)
- [Requirements](#requirements)
- [Build Dependencies](#build-dependencies)
- [Build and Run](#build-and-run)
- [Permissions](#permissions)
- [Runtime Interfaces](#runtime-interfaces)
- [Troubleshooting](#troubleshooting)
- [Project Layout](#project-layout)
- [Development Notes](#development-notes)
- [License](#license)

## Overview

This project aims to give Strawberry PS4 Linux users a simple desktop UI for common runtime controls without dragging in a large framework stack. The application is written in C with GTK4, uses GLib/GIO for async work and utility APIs, and keeps hardware I/O off the main UI thread so the window stays responsive while reading or writing sysfs nodes.

The application is also deliberately defensive:

- Missing kernel nodes disable only the affected section instead of crashing the entire app.
- Polling keeps displayed values fresh without manual refreshes.
- Writes surface permission-aware error messages instead of generic failures.
- Unsupported GPU hardware stays visible but read-only where force operations would be unsafe or meaningless.

## Features

### System Overview

- Displays kernel release from `uname`
- Detects PS4 GPU variant when known
- Shows human-readable uptime

### Settings

- Adds dedicated Dashboard / Settings tab layout
- Supports OLED black mode for lower-glow panels
- Lets you tune dashboard refresh interval in `ms`
- Lets you tune fan auto-apply debounce in `ms`

### Fan Control

- Detects `ps4_fan` dynamically through hwmon name lookup
- Displays current temperature and RPM
- Writes fan threshold through `temp1_crit`
- Enforces threshold range `20` to `85` degrees C
- Uses `79` degrees C as reset/default value
- Debounces slider writes with user-configurable timing

### Front LED Control

- Discovers available LED effects from exposed Strawberry sysfs nodes
- Supports static effect selection plus `off`
- Supports thermal mode when kernel exposes LED mode controls
- Supports thermal interval control when kernel exposes interval node
- Restores captured runtime defaults through reset

### GPU SCLK Control

- Reads vendor and device IDs from DRM sysfs
- Detects exposed SCLK levels from `pp_dpm_sclk`
- Allows manual or auto performance mode switching
- Restricts forced SCLK writes to supported PS4 GPU variants
- Confirms level writes after a `500 ms` verification poll

### HDMI Reprobe

- Resolves preferred connector `card0-HDMI-A-1`
- Falls back to first DRM connector that exposes a status node
- Displays current connector name and status
- Triggers reprobe by writing `detect` to connector status
- Tracks last reprobe timestamp in the UI

### Remote Control — Braska

The experimental HTTP control page has been removed. Remote control is handled by [Braska](https://github.com/rmuxnet/Braska), a Flutter mobile app that connects to the Strawberry Kernel Manager API over LAN.

- Headless mode exposes the same REST API, WebSocket telemetry, and PTY terminal that Braska uses
- No browser UI is served; `GET /` returns a JSON health blob
- Auth is opt-in via `remote_password` in `settings.ini`

### UX and Reliability

- Worker-thread sysfs I/O keeps UI responsive
- Automatic polling uses user-configurable interval
- Section-level availability handling
- Clear success and failure notifications for operations

## Supported Hardware and Kernel Interfaces

This application is intended for Strawberry PS4 Linux systems. It is not a generic Linux hardware control panel.

Feature availability depends entirely on what your current kernel build exposes:

- Fan section requires Strawberry `ps4_fan` hwmon nodes
- LED section requires Strawberry PS4 LED nodes
- GPU force controls require exposed AMDGPU SCLK interfaces
- HDMI reprobe requires DRM connector status node access

GPU force support is currently gated to:

- Vendor ID `0x1002`
- Device ID `0x9920` (`Liverpool`)
- Device ID `0x9923` (`Gladius`)

If a node is missing, that section stays disabled and the rest of the app continues to work.

## Requirements

- Linux desktop session with GTK4 available
- Strawberry PS4 kernel build exposing relevant sysfs and procfs interfaces
- C compiler with C11 support
- Meson
- Ninja
- `pkg-config`

## Build Dependencies

### Debian / Ubuntu

```bash
sudo apt update
sudo apt install build-essential pkg-config meson ninja-build libgtk-4-dev
```

### Fedora

```bash
sudo dnf install gcc pkgconf-pkg-config meson ninja-build gtk4-devel
```

### Arch Linux

```bash
sudo pacman -S base-devel pkgconf meson ninja gtk4
```

## Build and Run

### Build with Meson

```bash
meson setup builddir
meson compile -C builddir
./builddir/strawberry-kernel-manager
```

### Headless Remote Mode

```bash
./builddir/strawberry-kernel-manager --headless --port 8080
```

Exposes the Braska REST API, WebSocket telemetry, and PTY terminal on the specified port. Connect with the [Braska](https://github.com/rmuxnet/Braska) app. No browser UI is served.

### Optional Install

```bash
meson install -C builddir
```

### Direct Compile

If you want quick local build without Meson:

```bash
cc -Iinclude src/*.c -o strawberry-kernel-manager $(pkg-config --cflags --libs gtk4 gio-2.0 glib-2.0) -lm
./strawberry-kernel-manager
```

## Permissions

Most useful actions in this app require write access to sysfs nodes. Repository includes udev rule intended to grant `video` group access to common Strawberry PS4 control paths.

Recommended setup:

```bash
sudo cp udev/99-strawberry-km.rules /etc/udev/rules.d/99-strawberry-km.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo usermod -aG video "$USER"
```

After changing group membership, log out and back in.

Notes:

- Included rule is good starting point, but some Strawberry kernel builds may expose slightly different node names.
- If writes still fail, verify actual node paths on your system.
- If you are testing before finalizing udev permissions, run application with elevated privileges.

## Runtime Interfaces

These are primary interfaces used by application.

### System

- `/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`
- `/proc/uptime`
- `uname(2)` for kernel release

### Fan

- `/sys/class/hwmon/hwmonX/name`
- `/sys/class/hwmon/hwmonX/temp1_input`
- `/sys/class/hwmon/hwmonX/fan1_input`
- `/sys/class/hwmon/hwmonX/temp1_crit`

### Front LED

- `/sys/class/leds/ps4:*:status/brightness`
- `/sys/bus/platform/devices/ps4-led/mode`
- `/sys/bus/platform/devices/ps4-led/thermal_interval_ms`

### GPU

- `/sys/class/drm/card0/device/vendor`
- `/sys/class/drm/card0/device/device`
- `/sys/class/drm/card0/device/power_dpm_force_performance_level`
- `/sys/class/drm/card0/device/pp_dpm_sclk`

### Display

- `/sys/class/drm/card0-HDMI-A-1/status` as preferred connector
- `/sys/class/drm/card*-*/status` as fallback connector scan

## Troubleshooting

### A section is disabled or says feature is not exposed

Your current kernel build probably does not expose required node for that feature. This is expected behavior. App disables only affected section and keeps remaining controls available.

### Writes fail with permission errors

Check three things:

1. Udev rules are installed and reloaded
2. Your user is in `video` group
3. Kernel exposed paths match what your system actually provides

If needed, test with elevated privileges to confirm problem is permissions rather than missing interfaces.

### Fan control is visible but values look incomplete

Fan section expects temperature, RPM, and threshold nodes. If only some nodes exist, app will display what it can and leave unavailable values blank.

### GPU controls are read-only

This usually means one of these:

- Required GPU sysfs nodes are missing
- Hardware is not recognized as supported forcing target
- Kernel rejected manual control path

Only Liverpool and Gladius PS4 variants are treated as supported SCLK force targets.

### HDMI status does not update after reconnecting display

Use `Reprobe` button. Strawberry kernels may not poll hotplug state automatically, so manual reprobe is expected workflow.

### Theme does not load when running from unusual directory

Application looks for CSS in:

- `data/theme.css`
- `./data/theme.css`
- `../data/theme.css`

If you launch binary from custom location, make sure stylesheet is reachable through one of those paths or install project through Meson.

## Project Layout

- `src/main.c` - application bootstrap and CSS loading
- `src/ui.c` - top-level window creation and section assembly
- `src/ui-common.c` - shared GTK widget helpers and notices
- `src/ui-update.c` - snapshot-to-widget update logic
- `src/ui-actions.c` - async task dispatch and signal handlers
- `src/ui-settings.c` - settings persistence hooks and runtime toggles
- `src/service.c` - shared service lifetime and snapshot orchestration
- `src/service-system.c` - system summary and HDMI service logic
- `src/service-fan.c` - fan read and write operations
- `src/service-led.c` - LED discovery, defaults, and apply logic
- `src/service-gpu.c` - GPU detection, SCLK parsing, and force logic
- `src/settings.c` - local settings load and save helpers
- `src/remote.c` - Braska API server: REST endpoints, WebSocket telemetry, PTY terminal
- `src/sysfs.c` - low-level sysfs and procfs helpers
- `include/skm-service.h` - public service data model and API
- `include/skm-sysfs.h` - low-level sysfs helper API
- `include/skm-ui.h` - public UI entry point
- `data/theme.css` - application styling
- `udev/99-strawberry-km.rules` - example udev permissions rule

## Development Notes

- Language: C11
- UI toolkit: GTK4
- Utility/runtime libraries: GLib and GIO
- Build system: Meson + Ninja
- Poll interval: configurable, defaults to `2` seconds
- Fan threshold debounce: configurable, defaults to `500 ms`
- GPU/HDMI confirmation waits: `500 ms`

Project is structured so UI and hardware service logic stay separate. Public interfaces remain in `include/`, while internal module boundaries are split into smaller source files under `src/`.

## License

No license file is currently included in this repository.
