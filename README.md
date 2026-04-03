# Alpine on iOS

Run Alpine Linux aarch64 on iOS through an AArch64 CPU interpreter with
Linux syscall emulation.

## Overview

Alpine on iOS is a usermode Linux emulator for iOS. It interprets AArch64
instructions and translates Linux system calls, allowing unmodified
Alpine Linux aarch64 binaries to run inside an iOS app. A built-in
terminal emulator provides an interactive shell.

### Differences from iSH

- **Architecture**: emulates AArch64 (64-bit ARM), not x86 (32-bit).
  This means Alpine aarch64 packages work natively.
- **Instruction set**: since iOS devices are ARM-based, the emulated
  instruction set matches the host architecture. The JIT engine runs
  guest code natively with BRK-based syscall interception for
  near-native speed.

## Architecture

```
+-------------------------------+
|  SwiftUI Terminal + Display   |   iOS app layer (tabs)
+------+----------------+------+
       |                |
+------+------+  +------+------+
| TerminalView|  | DisplayView |   Metal-backed framebuffer display
+------+------+  +------+------+
       |                |
+-----------+-----------+
|   EmulatorBridge      |   Swift <-> C bridge
+-----------+-----------+
            |
+-----------+-----------+
|  AArch64 Execution    |   JIT native execution (aarch64 hosts)
|  Engine               |   or interpreter fallback
+-----------+-----------+
            |
+-----------+-----------+
|  Linux syscall        |   System call emulation
|  emulation            |   (file I/O, memory, process, signals, etc.)
+-----------+-----------+
            |
+-----------+-----------+
|  VFS layer            |   Virtual filesystem with /proc, /dev,
|                       |   /dev/fb0, /dev/input/event0
+-----------+-----------+
```

### JIT Native Execution

On aarch64 hosts (iOS, macOS ARM, Linux ARM64), the emulator runs guest
code natively instead of interpreting each instruction. Since both host
and guest use the same AArch64 instruction set, guest code is mapped at
its virtual addresses using `mmap(MAP_FIXED)` and executed directly.

Syscalls and thread-pointer accesses are intercepted by patching the
binary at load time:

- `SVC #0` is replaced with `BRK #0x0001`
- `MSR TPIDR_EL0, Xn` is replaced with `BRK #(0x0100 | Rn)`
- `MRS Xn, TPIDR_EL0` is replaced with `BRK #(0x0200 | Rn)`

A `SIGTRAP` signal handler intercepts these BRK instructions, reads
the register state from the signal context, dispatches the syscall or
TPIDR operation, and resumes execution. This gives near-native speed
with no per-instruction overhead.

The JIT engine is enabled automatically on aarch64 and can be toggled
with `emu_set_jit_enabled()`. On non-aarch64 hosts, the interpreter
fallback is used.

## Project Structure

```
alpine_on_ios/
+-- .github/workflows/             CI, release, auto-tag workflows
+-- project.yml                    XcodeGen project spec
+-- Makefile                       Top-level build
+-- AlpineOnIOS.entitlements       App entitlements
+-- README.md
+-- AlpineOnIOS/
|   +-- App/                       SwiftUI app entry point, ContentView
|   |   +-- AlpineOnIOSApp.swift
|   |   +-- ContentView.swift
|   |   +-- Info.plist
|   |   +-- Assets.xcassets/
|   +-- Terminal/                   Terminal emulator UI
|   |   +-- TerminalView.swift
|   |   +-- TerminalBuffer.swift
|   |   +-- TerminalParser.swift
|   |   +-- ANSIColors.swift
|   +-- Settings/                   User preferences
|   |   +-- SettingsView.swift
|   |   +-- AppSettings.swift
|   +-- Display/                    Metal framebuffer display + input
|   |   +-- DisplayView.swift
|   |   +-- InputMapper.swift
|   +-- Bridge/                     Swift-to-C bridge
|       +-- EmulatorBridge.swift
|       +-- AlpineOnIOS-Bridging-Header.h
+-- emu/
|   +-- include/                    C headers (cpu.h, jit.h, memory.h, ...)
|   +-- src/                        C implementation + jit_entry.S (asm)
|   +-- tests/                      C unit tests
|   +-- Makefile
|   +-- CMakeLists.txt
+-- rootfs/
|   +-- download_rootfs.sh          Download Alpine minirootfs
|   +-- overlay/                    Files copied into rootfs (X11 config, .profile, etc.)
+-- scripts/
    +-- build_rootfs.sh             Package rootfs for app bundle
    +-- package_ipa.sh              Create .ipa from build output
```

## Building

### Prerequisites

- **iOS build**: macOS with Xcode 15+, [XcodeGen](https://github.com/yonaskolb/XcodeGen)
- **Linux testing**: GCC or Clang with C11 support, make, pthreads

### Download the rootfs

```sh
./rootfs/download_rootfs.sh
```

This downloads the Alpine Linux 3.21 aarch64 minirootfs.

### Build the emulator (Linux)

```sh
make          # build libemu.a
make test     # run unit tests
```

### Build the iOS app

```sh
xcodegen generate
xcodebuild build \
    -project AlpineOnIOS.xcodeproj \
    -scheme AlpineOnIOS \
    -sdk iphoneos \
    -configuration Release \
    CODE_SIGNING_ALLOWED=NO
```

### Package an IPA

```sh
./scripts/package_ipa.sh build/Build/Products/Release-iphoneos
```

### CI and Releases

Three GitHub Actions workflows handle builds and releases:

- **ci.yml**: runs on every push and pull request. Tests the emulator
  on Linux, builds the iOS app on macOS 26, and uploads the IPA as a
  CI artifact.
- **version-tag.yml**: watches `project.yml` for changes to
  `MARKETING_VERSION`. When a version bump is pushed to `main`, it
  automatically creates a `v{version}` git tag.
- **release.yml**: triggered by `v*` tags. Builds a release IPA and
  publishes it as a GitHub Release for download.

To cut a release, bump `MARKETING_VERSION` in `project.yml` and push
to `main`. The tag and release are created automatically.

## Installing on iPhone and iPad

### Download the IPA

Go to the
[Releases](https://github.com/renaudallard/alpine_on_ios/releases)
page and download the latest `AlpineOnIOS-vX.Y.Z.ipa` file onto your
computer.

### Option 1: AltStore (recommended, free)

AltStore re-signs apps with your personal Apple ID and installs them
over Wi-Fi. Free accounts allow up to 3 sideloaded apps at a time,
renewed every 7 days automatically while AltStore is running.

1. Install AltServer on your Mac or Windows PC from
   https://altstore.io/.
2. Connect your iPhone or iPad to the same Wi-Fi network as your
   computer.
3. Open AltServer and install AltStore onto your device (follow the
   on-screen instructions; you will need your Apple ID).
4. On your device, open **Settings > General > Device Management** (or
   **VPN & Device Management**) and trust the profile for your Apple
   ID.
5. Transfer the downloaded `.ipa` to your device (AirDrop, Files app,
   or any file-transfer method).
6. Open AltStore on your device, go to the **My Apps** tab, tap the
   **+** button in the top-left corner, and select the `.ipa` file.
7. AltStore will sign and install the app. It appears on your home
   screen.

AltStore refreshes the app signature automatically every 7 days as
long as AltServer is running on your computer and both devices are on
the same Wi-Fi network.

### Option 2: Sideloadly (free)

Sideloadly signs and installs IPAs directly from your computer over
USB or Wi-Fi.

1. Download Sideloadly from https://sideloadly.io/ (macOS or Windows).
2. Connect your device via USB (or enable Wi-Fi pairing in Finder /
   iTunes first).
3. Open Sideloadly, drag the `.ipa` file into the window.
4. Enter your Apple ID and click **Start**.
5. If prompted for a password or two-factor code, enter it.
6. On your device, go to **Settings > General > Device Management**
   and trust the profile for your Apple ID.
7. The app is now on your home screen.

Like AltStore, free accounts require re-signing every 7 days. Repeat
step 3-4 to refresh.

### Option 3: TrollStore (no re-signing needed, limited device support)

TrollStore permanently installs apps without expiration, but only
works on specific iOS/iPadOS versions. Check
https://ios.cfw.guide/installing-trollstore/ for compatibility.

1. Install TrollStore following the guide for your device and iOS
   version.
2. Transfer the `.ipa` to your device.
3. Open the file with TrollStore (share sheet > TrollStore) or use
   the TrollStore URL scheme.
4. The app is installed permanently with no re-signing required.

### Option 4: Apple Developer account (paid, $99/year)

A paid Apple Developer account lets you install apps that last up to
a year and removes the 3-app limit.

1. Enroll at https://developer.apple.com/programs/.
2. Use Xcode, AltStore, or Sideloadly with your developer Apple ID
   to sign the `.ipa`.
3. With Xcode: open the `.ipa` in Xcode (Window > Devices and
   Simulators), select your device, drag the app onto it.
4. Go to **Settings > General > Device Management** and trust the
   developer profile if needed.

### After installation

1. Open **Alpine Terminal** from your home screen.
2. On first launch the app extracts the Alpine Linux rootfs. This
   takes a few seconds.
3. You are dropped into an interactive `sh` shell running inside the
   emulated Alpine Linux environment.
4. Use `apk` to install packages:
   ```
   apk update
   apk add curl git python3 gcc musl-dev
   ```

### Troubleshooting

- **"Untrusted Developer" or "Untrusted App"**: go to Settings >
  General > Device Management (or VPN & Device Management), find the
  profile for your Apple ID, and tap Trust.
- **App crashes on launch**: make sure you are running iOS 15.0 or
  later.
- **AltStore says "Could not find AltServer"**: ensure AltServer is
  running on your computer and both devices are on the same Wi-Fi.
- **Sideloadly error -36607**: this is an Apple server error; wait a
  few minutes and retry.
- **App expires after 7 days**: this is a limitation of free Apple ID
  signing. Reconnect to AltStore/Sideloadly to refresh. TrollStore or
  a paid developer account avoids this.

## Current Status

Early development. The JIT native execution engine, CPU interpreter
fallback, syscall emulation, terminal emulator, and framebuffer
display are functional. Known limitations:

- SIMD/FP instruction coverage is partial (interpreter mode)
- Network syscall support is incomplete
- Single-threaded process execution (clone/fork emulation is basic)

The rootfs is bundled as a pre-extracted directory resource and copied
to the Documents directory on first launch using FileManager (no
Foundation.Process dependency). The terminal parser supports full
UTF-8 multi-byte sequences. Arrow and escape keys from hardware
keyboards are delivered to the emulator via notification observers.

### Framebuffer and Input

The emulator provides `/dev/fb0` (framebuffer) and
`/dev/input/event0` (evdev touch/keyboard) virtual devices. X servers
and other framebuffer-aware programs can open `/dev/fb0`, query
resolution via `ioctl(FBIOGET_VSCREENINFO)`, and mmap the pixel
buffer to render graphics. The default resolution is 1280x720, 32bpp
BGRA.

On the iOS side, the Display tab shows the framebuffer contents
through a Metal-backed `MTKView` running at 60 fps. Touch events are
mapped to Linux `input_event` structs and delivered to the guest via
`/dev/input/event0`.

## License

ISC License. See individual source files for the full license text.

```
Copyright (c) 2026 Alpine on iOS contributors
```
