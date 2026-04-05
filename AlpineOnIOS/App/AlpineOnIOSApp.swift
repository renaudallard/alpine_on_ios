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

        /* Create writable overlay for config/data. */
        bridge.state = .extracting
        createOverlay(bundleRootfs: bundleRootfs, overlay: overlay)

        /* Create busybox symlinks in the overlay. */
        if !fm.fileExists(atPath: overlay + "/bin/ls") {
            createBusyboxSymlinks(rootfs: overlay)
        }

        /* Use bundle as rootfs (signed, for AOT exec).
         * Overlay provides writable config/data. */
        bridge.startAll(rootfsPath: bundleRootfs,
                        overlayPath: overlay)
    }

    private func bundleRootfsPath() -> String {
        #if os(iOS)
        return Bundle.main.bundlePath + "/alpine"
        #elseif os(macOS)
        return Bundle.main.resourcePath! + "/alpine"
        #endif
    }

    private func overlayPath() -> String {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        return docs.appendingPathComponent("alpine").path
    }

    /// Create writable overlay with config files and directories.
    private func createOverlay(bundleRootfs: String, overlay: String) {
        let fm = FileManager.default

        /* Create essential writable directories. */
        for dir in ["/etc", "/etc/apk", "/root", "/tmp", "/var",
                    "/home", "/run", "/bin", "/sbin",
                    "/usr/bin", "/usr/sbin"] {
            try? fm.createDirectory(atPath: overlay + dir,
                withIntermediateDirectories: true, attributes: nil)
        }

        /* Copy mutable config files from bundle if not already in overlay. */
        let configFiles = ["/etc/resolv.conf", "/etc/apk/repositories",
                           "/etc/passwd", "/etc/group", "/etc/shadow",
                           "/root/.profile", "/root/.xinitrc",
                           "/root/start-firefox.sh"]
        for file in configFiles {
            let dest = overlay + file
            let src = bundleRootfs + file
            if !fm.fileExists(atPath: dest) && fm.fileExists(atPath: src) {
                try? fm.copyItem(atPath: src, toPath: dest)
            }
        }

        createEssentialConfig(overlay: overlay)
    }

    /// Create resolv.conf and APK repos if not present.
    private func createEssentialConfig(overlay: String) {
        let fm = FileManager.default
        let etcDir = overlay + "/etc"

        /* Ensure resolv.conf exists. */
        let resolvConf = etcDir + "/resolv.conf"
        if !fm.fileExists(atPath: resolvConf) {
            let dns = "nameserver 9.9.9.9\nnameserver 149.112.112.112\n"
            fm.createFile(atPath: resolvConf,
                contents: dns.data(using: .utf8), attributes: nil)
        }

        /* Configure APK repositories. */
        let reposDir = etcDir + "/apk"
        try? fm.createDirectory(atPath: reposDir,
            withIntermediateDirectories: true, attributes: nil)
        let reposFile = reposDir + "/repositories"
        if !fm.fileExists(atPath: reposFile) {
            let repos = [
                "https://raw.githubusercontent.com/renaudallard/alpine_on_ios/aot-repo/aarch64",
                "http://dl-cdn.alpinelinux.org/alpine/v3.21/main",
                "http://dl-cdn.alpinelinux.org/alpine/v3.21/community",
            ].joined(separator: "\n") + "\n"
            fm.createFile(atPath: reposFile,
                contents: repos.data(using: .utf8), attributes: nil)
        }
    }

    /// Create busybox applet symlinks and essential config in the rootfs.
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
