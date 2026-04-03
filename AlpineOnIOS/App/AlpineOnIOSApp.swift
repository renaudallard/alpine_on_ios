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
                .onAppear {
                    setupEmulator()
                }
        }
    }

    private func setupEmulator() {
        guard !isSetup else { return }
        isSetup = true

        let rootfsDir = rootfsPath()

        if !FileManager.default.fileExists(atPath: rootfsDir + "/bin/sh") {
            extractRootfs()
        }

        bridge.initialize(rootfsPath: rootfsDir)
        bridge.spawnShell()
        bridge.startEmulator()
    }

    /// Return the path where the rootfs is stored inside the app sandbox.
    private func rootfsPath() -> String {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        return docs.appendingPathComponent("alpine").path
    }

    /// Copy the bundled rootfs directory to Documents on first launch.
    /// Foundation.Process (NSTask) is macOS-only; on iOS we copy the
    /// pre-extracted rootfs directory that is included as a bundle resource.
    private func extractRootfs() {
        let fm = FileManager.default
        let docs = fm.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let rootfsDir = docs.appendingPathComponent("alpine")

        /* Already extracted */
        if fm.fileExists(atPath: rootfsDir.path) { return }

        /* Find bundled rootfs directory */
        guard let bundledRootfs = Bundle.main.path(forResource: "alpine",
                                                   ofType: nil) else {
            /* No bundled rootfs found; create empty placeholder */
            try? fm.createDirectory(at: rootfsDir,
                                    withIntermediateDirectories: true)
            return
        }

        /* Copy bundled rootfs directory to Documents */
        do {
            try fm.copyItem(atPath: bundledRootfs, toPath: rootfsDir.path)
        } catch {
            /* Log error but do not crash */
        }
    }
}
