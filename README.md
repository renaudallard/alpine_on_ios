<p align="center">
  <img src="AlpineOnIOS/App/Assets.xcassets/AppIcon.appiconset/AppIcon.png" width="128" height="128" alt="Alpine on iOS" style="border-radius: 22%;">
</p>

<h1 align="center">Alpine on iOS</h1>

<p align="center">
  <a href="https://github.com/renaudallard/alpine_on_ios/releases/latest">
    <img src="https://img.shields.io/github/v/release/renaudallard/alpine_on_ios?label=version&style=flat-square" alt="Latest Release">
  </a>
  <a href="https://github.com/renaudallard/alpine_on_ios/actions/workflows/ci.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/renaudallard/alpine_on_ios/ci.yml?branch=main&label=build&style=flat-square" alt="Build Status">
  </a>
  <a href="https://github.com/renaudallard/alpine_on_ios/releases/latest">
    <img src="https://img.shields.io/github/downloads/renaudallard/alpine_on_ios/total?style=flat-square&label=downloads" alt="Downloads">
  </a>
  <img src="https://img.shields.io/badge/platform-iOS%2015%2B-blue?style=flat-square" alt="Platform">
  <img src="https://img.shields.io/badge/license-ISC-green?style=flat-square" alt="License">
</p>

<p align="center">
  Run a full Alpine Linux aarch64 distribution on iPhone and iPad.<br>
  Near-native speed via AOT precompilation, no MAP_JIT required.
</p>

---

## Features

- **Full Alpine Linux** with `apk` package manager and AOT package repository
- **Near-native execution** via AOT precompilation (no MAP_JIT entitlement needed)
- **Terminal emulator** with VT100/xterm-256color, ANSI colors, line editing, history
- **Graphical display** via `/dev/fb0` framebuffer and Metal rendering at 60fps
- **Touch input** mapped to Linux evdev mouse events
- **Thread support** with full `clone()` and `futex()`
- **100+ Linux syscalls** including epoll, eventfd, timerfd, copy_file_range
- **Comprehensive SIMD/NEON** support (3600+ lines: vector, FP, permute, table, shifts)
- **Networking** with DNS resolution (musl getaddrinfo) and HTTP (wget, curl)
- **Virtual /proc and /dev** with stat, framebuffer, and evdev input devices
- **Interactive shell** with Ctrl+C/Ctrl+Z signal support
- **X11 ready** with preconfigured xorg.conf for fbdev, MATE/openbox/Firefox

## How It Works

Since iOS devices use ARM64 and Alpine Linux provides aarch64 packages,
guest code runs **natively** on the host CPU. At build time, `SVC`
(syscall) instructions are replaced with `BRK` traps. At runtime, a
`SIGTRAP` handler intercepts the traps and dispatches to emulated Linux
syscalls. This gives near-native performance with no per-instruction
overhead and no MAP_JIT entitlement.

A full AArch64 instruction interpreter serves as fallback for forked
child processes and non-aarch64 hosts.

```
+-----------------------+
|  Terminal | Display   |   SwiftUI tabs
+-----+-----+-----+----+
      |           |
+-----+-----+----+-----+
| VT100     | Metal     |   Terminal parser / MTKView 60fps
+-----+-----+-----+----+
            |
      +-----+-----+
      |  Bridge   |         Swift <-> C
      +-----+-----+
            |
      +-----+-----+
      | AOT Native |         Pre-patched BRK traps + SIGTRAP handler
      | / Interp   |         Interpreter fallback for child processes
      +-----+-----+
            |
      +-----+-----+
      |  Syscall  |         100+ Linux syscalls emulated
      |  Layer    |
      +-----+-----+
            |
      +-----+-----+
      |  VFS      |         rootfs + overlay + /proc + /dev + /dev/fb0
      +-----------+
```

## Installing

Download the latest `.ipa` from
[**Releases**](https://github.com/renaudallard/alpine_on_ios/releases/latest)
and sideload it.

| Method | Cost | Re-sign | Notes |
|--------|------|---------|-------|
| [AltStore](https://altstore.io/) | Free | Every 7 days | Recommended. Wi-Fi install via AltServer |
| [Sideloadly](https://sideloadly.io/) | Free | Every 7 days | USB or Wi-Fi, drag and drop |
| [TrollStore](https://ios.cfw.guide/installing-trollstore/) | Free | Never | Permanent install, limited iOS versions |
| [Apple Developer](https://developer.apple.com/programs/) | $99/year | Yearly | No app limit, Xcode install |

After installing, trust the developer profile in
**Settings > General > Device Management**.

### First launch

1. Open **Alpine Terminal** from your home screen
2. The app sets up symlinks and configuration on first launch
3. You get an interactive shell with line editing and history

### Installing packages

The app is preconfigured with an AOT repository (pre-patched for native
speed) and Alpine's HTTP mirrors as fallback:

```
apk update --allow-untrusted
apk add curl git python3 vim
```

### Graphical mode (X11)

```
apk add --allow-untrusted xorg-server xf86-video-fbdev xterm openbox
startx
```

Switch to the **Display** tab to see the graphical output. Touch
the display for mouse input.

### Firefox

```sh
apk add --allow-untrusted firefox-esr font-noto openbox dbus
sh ~/start-firefox.sh
```

Switch to the **Display** tab to browse the web.

## Building from Source

### Prerequisites

- **iOS**: macOS + Xcode 15+ + [XcodeGen](https://github.com/yonaskolb/XcodeGen)
- **Linux testing**: GCC or Clang (C11), make, pthreads

### Quick start

```sh
./rootfs/download_rootfs.sh          # download Alpine 3.21 aarch64
cd emu && make && make test && cd ..  # build and run 42 unit tests

# iOS build
xcodegen generate
xcodebuild build -project AlpineOnIOS.xcodeproj \
    -scheme AlpineOnIOS -sdk iphoneos \
    -configuration Release CODE_SIGNING_ALLOWED=NO

# package .ipa
./scripts/package_ipa.sh build/Build/Products/Release-iphoneos
```

The Xcode post-build script copies the rootfs into the app bundle and
runs `scripts/patch_rootfs_aot.sh` to AOT-patch all ELF binaries.

### CI and releases

| Workflow | Trigger | Action |
|----------|---------|--------|
| `ci.yml` | Push / PR | Test on Linux, build iOS, upload artifact |
| `version-tag.yml` | `MARKETING_VERSION` change | Auto-create `v*` tag |
| `release.yml` | `v*` tag | Build .ipa, publish GitHub Release |
| `aot-repo.yml` | Daily / manual | Build AOT-patched APK repository |

Bump `MARKETING_VERSION` in `project.yml` and push to release.

## Project Structure

```
alpine_on_ios/
  AlpineOnIOS/
    App/            SwiftUI entry point, ContentView, rootfs setup
    Terminal/       VT100 terminal emulator (view, buffer, parser, colors)
    Display/        Metal framebuffer display + touch input mapping
    Settings/       Font size preferences
    Bridge/         Swift-to-C bridge, bridging header
  emu/
    include/        C headers (cpu, jit, memory, process, vfs, syscall, ...)
    src/            C implementation + jit_entry.S assembly
    tests/          Unit + integration tests (42 unit, 2 integration)
  rootfs/
    download_rootfs.sh   Download Alpine minirootfs
    overlay/             X11 config, .profile, start-firefox.sh
  scripts/
    aot_patch.c          AOT patcher: SVC->BRK in ELF binaries
    patch_rootfs_aot.sh  Patch all ELFs in a rootfs directory
    build_aot_repo.sh    Build AOT-patched APK repository
    build_rootfs.sh      Rootfs assembly
    package_ipa.sh       IPA packaging
  project.yml            XcodeGen spec
  .github/workflows/     CI, release, auto-tag, AOT repo
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| "Untrusted Developer" | Settings > General > Device Management > Trust |
| App crashes on launch | Requires iOS 15.0 or later |
| AltStore can't find server | Ensure AltServer is running, same Wi-Fi |
| App expires after 7 days | Re-sign with AltStore/Sideloadly, or use TrollStore |
| No keyboard input | Tap the terminal area to focus the keyboard |
| Blank terminal | Delete and reinstall the app to re-extract rootfs |
| apk signature errors | Use `--allow-untrusted` for the AOT repository |
| Slow package install | AOT repo packages run natively; fallback repos use interpreter |

## License

ISC License. See source files for the full text.

```
Copyright (c) 2026 Alpine on iOS contributors
```
