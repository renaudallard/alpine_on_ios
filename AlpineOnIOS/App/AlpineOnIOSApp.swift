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

        /* Busybox symlinks in overlay so PATH finds them. */
        if !fm.fileExists(atPath: overlay + "/bin/ls") {
            createBusyboxSymlinks(rootfs: overlay)
        }

        bridge.startAll(rootfsPath: bundleRootfs,
                        overlayPath: overlay)
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

        for (dir, names) in applets {
            /* Compute relative path from this dir to /bin/busybox */
            let depth = dir.components(separatedBy: "/").count - 2
            let relPath = String(repeating: "../", count: depth) + "bin/busybox"

            for name in names {
                let link = rootfs + dir + "/" + name
                if !fm.fileExists(atPath: link) {
                    try? fm.createSymbolicLink(
                        atPath: link, withDestinationPath: relPath)
                }
            }
        }
    }
}
