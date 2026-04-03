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

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(settings)
                .environmentObject(bridge)
                .onAppear {
                    setupEmulator()
                }
        }
    }

    private func setupEmulator() {
        let rootfsDir = rootfsPath()

        if !FileManager.default.fileExists(atPath: rootfsDir + "/bin/sh") {
            extractRootfs(to: rootfsDir)
        }

        bridge.initialize(rootfsPath: rootfsDir)
        bridge.startEmulator()
        bridge.spawnShell()
    }

    /// Return the path where the rootfs is stored inside the app sandbox.
    private func rootfsPath() -> String {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        return docs.appendingPathComponent("alpine-rootfs").path
    }

    /// Extract the bundled rootfs tarball on first launch.
    private func extractRootfs(to dest: String) {
        guard let archive = Bundle.main.url(forResource: "alpine-rootfs",
                                            withExtension: "tar.gz")
                ?? Bundle.main.url(forResource: "alpine",
                                   withExtension: nil) else {
            /* Rootfs may have been placed directly in the bundle directory. */
            let bundled = Bundle.main.bundlePath + "/alpine"
            if FileManager.default.fileExists(atPath: bundled) {
                try? FileManager.default.copyItem(atPath: bundled, toPath: dest)
            }
            return
        }

        try? FileManager.default.createDirectory(atPath: dest,
                                                  withIntermediateDirectories: true)

        /* Use tar to extract (available on iOS) */
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
        proc.arguments = ["xzf", archive.path, "-C", dest]
        try? proc.run()
        proc.waitUntilExit()
    }
}
