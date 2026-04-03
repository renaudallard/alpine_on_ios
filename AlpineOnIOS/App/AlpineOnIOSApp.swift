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
    @State private var statusMessage = ""

    var body: some Scene {
        WindowGroup {
            ContentView(statusMessage: $statusMessage)
                .environmentObject(settings)
                .environmentObject(bridge)
                .onAppear {
                    /*
                     * Run setup synchronously on the main thread so that
                     * by the time TerminalView.onAppear fires, the shell
                     * is already spawned and termFD is valid.
                     */
                    setupEmulator()
                }
        }
    }

    private func setupEmulator() {
        guard !isSetup else { return }
        isSetup = true

        let fm = FileManager.default
        let rootfsDir = rootfsPath()

        /* Extract rootfs if not present */
        if !fm.fileExists(atPath: rootfsDir + "/bin") {
            extractRootfs()
        }

        /* Verify rootfs */
        if !fm.fileExists(atPath: rootfsDir + "/bin") {
            var diag = "Rootfs path: \(rootfsDir)\n"
            if fm.fileExists(atPath: rootfsDir) {
                let items = (try? fm.contentsOfDirectory(atPath: rootfsDir)) ?? []
                diag += "Contents: \(items.prefix(20).joined(separator: ", "))\n"
            } else {
                diag += "Directory does not exist\n"
            }
            diag += "Bundle: \(Bundle.main.bundlePath)\n"
            let bundleItems = (try? fm.contentsOfDirectory(
                atPath: Bundle.main.bundlePath)) ?? []
            diag += "Bundle root: \(bundleItems.joined(separator: ", "))\n"
            statusMessage = "ERROR: rootfs not found\n\(diag)"
            return
        }

        /* Initialize emulator */
        let rc = bridge.initializeEmulator(rootfsPath: rootfsDir)
        if rc != 0 {
            statusMessage = "ERROR: emu_init failed (\(rc))"
            return
        }

        /* Spawn shell */
        let pid = bridge.spawnShell()
        if pid < 0 {
            statusMessage = "ERROR: shell spawn failed (\(pid))"
            return
        }

        /* Start emulator event loop on background thread */
        bridge.startEmulator()
        statusMessage = ""
    }

    private func rootfsPath() -> String {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        return docs.appendingPathComponent("alpine").path
    }

    private func extractRootfs() {
        let fm = FileManager.default
        let docs = fm.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let rootfsDir = docs.appendingPathComponent("alpine")

        if fm.fileExists(atPath: rootfsDir.path) { return }

        /* Search for bundled rootfs in multiple locations */
        let candidates: [String?] = [
            Bundle.main.path(forResource: "alpine", ofType: nil),
            Bundle.main.resourcePath.map { $0 + "/alpine" },
            Bundle.main.bundlePath + "/alpine",
        ]

        for case let path? in candidates {
            if fm.fileExists(atPath: path) {
                do {
                    try fm.copyItem(atPath: path, toPath: rootfsDir.path)
                    return
                } catch {
                    statusMessage = "Copy error: \(error.localizedDescription)"
                }
            }
        }

        /* No bundled rootfs */
        try? fm.createDirectory(at: rootfsDir, withIntermediateDirectories: true)
    }
}
