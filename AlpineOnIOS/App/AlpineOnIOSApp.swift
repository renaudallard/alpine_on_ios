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
        let rootfs = rootfsPath()

        /* First launch: copy rootfs from bundle to Documents
         * (Documents is writable, bundle is read-only).
         * The bundle has no symlinks - busybox --install creates them. */
        if !fm.fileExists(atPath: rootfs + "/bin/busybox") {
            bridge.state = .extracting
            extractRootfs()
        }

        if !fm.fileExists(atPath: rootfs + "/bin/busybox") {
            var diag = "Rootfs: \(rootfs)\n"
            diag += "Bundle: \(Bundle.main.bundlePath)\n"
            let items = (try? fm.contentsOfDirectory(
                atPath: Bundle.main.bundlePath)) ?? []
            diag += "Items: \(items.joined(separator: ", "))"
            bridge.state = .error(diag)
            return
        }

        /* Start emulator. It will run busybox --install before
         * spawning the shell if /bin/sh doesn't exist yet. */
        bridge.startAll(rootfsPath: rootfs)
    }

    private func rootfsPath() -> String {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        return docs.appendingPathComponent("alpine").path
    }

    private func extractRootfs() {
        let fm = FileManager.default
        let dest = rootfsPath()

        /* Remove stale/incomplete rootfs */
        if fm.fileExists(atPath: dest) &&
           !fm.fileExists(atPath: dest + "/bin/busybox") {
            try? fm.removeItem(atPath: dest)
        }

        guard !fm.fileExists(atPath: dest) else { return }

        /* Copy from bundle (no symlinks, just real files) */
        let src = Bundle.main.bundlePath + "/alpine"
        guard fm.fileExists(atPath: src) else { return }

        do {
            try fm.copyItem(atPath: src, toPath: dest)
        } catch {
            bridge.state = .error("Copy failed: \(error.localizedDescription)")
        }
    }
}
