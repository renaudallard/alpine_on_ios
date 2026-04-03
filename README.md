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
  instruction set matches the host architecture. A future JIT could
  take advantage of this.

## Architecture

```
+-----------------------+
|  SwiftUI Terminal UI  |   iOS app layer
+-----------+-----------+
            |
+-----------+-----------+
|   EmulatorBridge      |   Swift <-> C bridge
+-----------+-----------+
            |
+-----------+-----------+
|  AArch64 CPU          |   Instruction interpreter
|  interpreter          |   (fetch-decode-execute loop)
+-----------+-----------+
            |
+-----------+-----------+
|  Linux syscall        |   System call emulation
|  emulation            |   (file I/O, memory, process, signals, etc.)
+-----------+-----------+
            |
+-----------+-----------+
|  VFS layer            |   Virtual filesystem with /proc, /dev
+-----------+-----------+
```

## Project Structure

```
alpine_on_ios/
+-- .github/workflows/build.yml    CI workflow
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
|   +-- Bridge/                     Swift-to-C bridge
|       +-- EmulatorBridge.swift
|       +-- AlpineOnIOS-Bridging-Header.h
+-- emu/
|   +-- include/                    C headers
|   +-- src/                        C implementation
|   +-- tests/                      C unit tests
|   +-- Makefile
|   +-- CMakeLists.txt
+-- rootfs/
|   +-- download_rootfs.sh          Download Alpine minirootfs
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

### CI

GitHub Actions builds the app automatically on pushes to `main` and on
pull requests. The workflow runs on macOS 14 (Apple Silicon) and uploads
the built IPA as an artifact.

## Current Status

Early development. The CPU interpreter, syscall emulation, and terminal
emulator are functional. Known limitations:

- No JIT compilation; pure interpreter, so performance is limited
- SIMD/FP instruction coverage is partial
- Network syscall support is incomplete
- No GPU or graphics emulation
- Single-threaded process execution (clone/fork emulation is basic)

## License

ISC License. See individual source files for the full license text.

```
Copyright (c) 2026 Alpine on iOS contributors
```
