/*
 * Copyright (c) 2026 Alpine on iOS contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

import SwiftUI

@main
struct AlpineOnIOSApp: App {
    @StateObject private var settings = AppSettings()
    @StateObject private var bridge = EmulatorBridge()
    @State private var isSetup = false

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(settings)
                .environmentObject(bridge)
                .onAppear { setup() }
        }
    }

    private func setup() {
        guard !isSetup else { return }
        isSetup = true

        let fm = FileManager.default
        let bundleRootfs = bundleRootfsPath()
        let overlay = overlayPath()

        /* Verify bundle rootfs exists. */
        if !fm.fileExists(atPath: bundleRootfs + "/bin/busybox") {
            var diag = "Rootfs not found in bundle\n"
            diag += "Bundle: \(Bundle.main.bundlePath)\n"
            let items = (try? fm.contentsOfDirectory(
                atPath: Bundle.main.bundlePath)) ?? []
            diag += "Items: \(items.joined(separator: ", "))"
            bridge.state = .error(diag)
            return
        }

        /* Create overlay directory. Writes go here automatically
         * via the VFS overlay (vfs_resolve_rw). */
        try? fm.createDirectory(atPath: overlay,
            withIntermediateDirectories: true, attributes: nil)

        /* Configure the C-side breadcrumb trail (used to diagnose
         * silent crashes where no .ips file is produced).  Read
         * the crumbs from the previous run before starting a new
         * one; if there are any, stash them so the Swift UI can
         * show them in the error state if needed. */
        let crumbPath = overlay + "/.breadcrumbs"
        emu_set_breadcrumb_path(crumbPath)
        if let cstr = emu_breadcrumbs_reset() {
            let prev = String(cString: cstr)
            if !prev.isEmpty {
                bridge.previousBreadcrumbs = prev
            }
        }

        /* Busybox symlinks in overlay so PATH finds them.
         * Safe and cheap to run every launch; createBusyboxSymlinks
         * skips entries whose target already points at the current
         * bundle's busybox and recreates any stale ones. */
        createBusyboxSymlinks(rootfs: overlay)

        /*
         * If the previous run wrote any breadcrumbs, delay the
         * actual emu startup by a few seconds so the crumbs panel
         * is guaranteed on screen long enough to read and
         * screenshot.  Subsequent launches of the emulator on
         * iOS can crash fast enough inside emu_init / emu_spawn
         * that the default auto-start flashes through the
         * initializing splash before the user can react.  First
         * launches (no prior crumbs) get no delay.
         */
        let br = bridge
        if !br.previousBreadcrumbs.isEmpty {
            DispatchQueue.main.asyncAfter(deadline: .now() + 5) {
                br.startAll(rootfsPath: bundleRootfs,
                            overlayPath: overlay)
            }
        } else {
            br.startAll(rootfsPath: bundleRootfs,
                        overlayPath: overlay)
        }
    }

    private func bundleRootfsPath() -> String {
        return Bundle.main.bundlePath + "/alpine"
    }

    private func overlayPath() -> String {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        return docs.appendingPathComponent("alpine").path
    }

    /// Create busybox applet symlinks in the overlay.
    ///
    /// Symlink targets are **guest-absolute paths** (`/bin/busybox`),
    /// NOT host paths.  From the iOS host's point of view these
    /// links are dead — `/bin/busybox` does not exist on iOS —
    /// but the emulator's VFS resolver re-interprets symlink
    /// targets as guest paths and walks them through the overlay
    /// + rootfs stack, ultimately finding the real file in the
    /// app bundle's alpine/bin/busybox.  A version of this
    /// function that used absolute host paths into the bundle
    /// broke the resolver because it then appended that host
    /// path to `rootfs` as though it were a guest path.
    ///
    /// Idempotent: compares the current link target against
    /// `/bin/busybox` and only recreates if different, so it is
    /// safe (and cheap) to run on every launch.
    private func createBusyboxSymlinks(rootfs: String) {
        let fm = FileManager.default

        /* Standard busybox applet list */
        let dirs = ["/bin", "/sbin", "/usr/bin", "/usr/sbin"]
        for dir in dirs {
            let full = rootfs + dir
            try? fm.createDirectory(atPath: full,
                withIntermediateDirectories: true, attributes: nil)
        }

        /* Common applets to create as symlinks to busybox */
        let applets = [
            "/bin": ["sh", "ash", "ls", "cat", "cp", "mv", "rm", "mkdir",
                "rmdir", "ln", "chmod", "chown", "chgrp", "touch", "echo",
                "grep", "egrep", "fgrep", "sed", "head", "tail", "wc",
                "sort", "uniq", "cut", "tr", "tee", "find", "xargs",
                "tar", "gzip", "gunzip", "zcat", "df", "du", "mount",
                "umount", "ps", "kill", "sleep", "date", "uname", "pwd",
                "hostname", "whoami", "id", "env", "printenv", "test",
                "true", "false", "yes", "seq", "expr", "basename",
                "dirname", "realpath", "readlink", "stat", "md5sum",
                "sha256sum", "dd", "sync", "dmesg", "more", "less",
                "vi", "ed", "diff", "patch", "wget", "nc", "ping",
                "traceroute", "nslookup", "ifconfig", "route", "ip",
                "arp", "netstat", "ss", "mount", "free", "uptime",
                "top", "watch", "hexdump", "od", "strings", "file",
                "mktemp", "base64", "rev", "which"],
            "/sbin": ["halt", "reboot", "poweroff", "init", "fdisk",
                "mkfs.ext2", "fsck", "blkid", "swapon", "swapoff",
                "ifconfig", "route", "iptables", "modprobe", "lsmod"],
            "/usr/bin": ["awk", "nohup", "install", "time", "xargs",
                "head", "tail", "tty", "clear", "reset"],
            "/usr/sbin": ["adduser", "deluser", "addgroup", "delgroup",
                "crond", "chpasswd"],
        ]

        /*
         * Guest-absolute target.  The resolver walks this through
         * overlay→rootfs and ends up at the real busybox inside
         * the app bundle, regardless of where the bundle UUID
         * moves between reinstalls.
         */
        let guestTarget = "/bin/busybox"

        for (dir, names) in applets {
            for name in names {
                let link = rootfs + dir + "/" + name
                /* Already correct? skip. */
                let current = try? fm.destinationOfSymbolicLink(
                    atPath: link)
                if current == guestTarget {
                    continue
                }
                /* Stale or non-existent — nuke and recreate. */
                try? fm.removeItem(atPath: link)
                try? fm.createSymbolicLink(atPath: link,
                    withDestinationPath: guestTarget)
            }
        }
    }
}
