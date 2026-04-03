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

        /* Extract rootfs if needed (synchronous, fast after first run) */
        if !fm.fileExists(atPath: rootfs + "/bin") {
            bridge.state = .extracting
            extractRootfs()
        }

        /* Verify rootfs exists */
        if !fm.fileExists(atPath: rootfs + "/bin") {
            var diag = "Rootfs: \(rootfs)\n"
            if fm.fileExists(atPath: rootfs) {
                let items = (try? fm.contentsOfDirectory(atPath: rootfs)) ?? []
                diag += "Contents: \(items.prefix(15).joined(separator: ", "))\n"
            } else {
                diag += "Directory missing\n"
            }
            diag += "Bundle: \(Bundle.main.bundlePath)\n"
            let bItems = (try? fm.contentsOfDirectory(
                atPath: Bundle.main.bundlePath)) ?? []
            diag += "Bundle items: \(bItems.joined(separator: ", "))"
            bridge.state = .error(diag)
            return
        }

        /* Start emulator on background thread */
        bridge.startAll(rootfsPath: rootfs)
    }

    private func rootfsPath() -> String {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        return docs.appendingPathComponent("alpine").path
    }

    private func extractRootfs() {
        let fm = FileManager.default
        let docs = fm.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let dest = docs.appendingPathComponent("alpine")

        guard !fm.fileExists(atPath: dest.path) else { return }

        /* Try to find the bundled rootfs */
        let candidates: [String?] = [
            Bundle.main.path(forResource: "alpine", ofType: nil),
            Bundle.main.resourcePath.map { $0 + "/alpine" },
            Bundle.main.bundlePath + "/alpine",
        ]

        for case let src? in candidates where fm.fileExists(atPath: src) {
            do {
                try fm.copyItem(atPath: src, toPath: dest.path)
                return
            } catch {
                /* Try next candidate */
            }
        }

        /* No rootfs found - create empty directory so error shows */
        try? fm.createDirectory(at: dest, withIntermediateDirectories: true)
    }
}
