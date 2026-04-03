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
    @State private var statusMessage = "Starting..."

    var body: some Scene {
        WindowGroup {
            ContentView(statusMessage: $statusMessage)
                .environmentObject(settings)
                .environmentObject(bridge)
                .onAppear {
                    if !isSetup {
                        isSetup = true
                        DispatchQueue.global(qos: .userInitiated).async {
                            setupEmulator()
                        }
                    }
                }
        }
    }

    private func setupEmulator() {
        updateStatus("Locating rootfs...")
        let rootfsDir = rootfsPath()
        let fm = FileManager.default

        /* Extract rootfs if /bin directory is missing */
        if !fm.fileExists(atPath: rootfsDir + "/bin") {
            updateStatus("Extracting rootfs...")
            extractRootfs()
        }

        /* Verify the rootfs has essential files */
        let binSh = rootfsDir + "/bin/busybox"
        if !fm.fileExists(atPath: binSh) {
            /* Check what we actually have */
            var diag = "Rootfs at: \(rootfsDir)\n"
            if fm.fileExists(atPath: rootfsDir) {
                let contents = (try? fm.contentsOfDirectory(atPath: rootfsDir)) ?? []
                diag += "Contents: \(contents.joined(separator: ", "))\n"
            } else {
                diag += "Directory does not exist\n"
            }
            /* Check bundle */
            diag += "Bundle path: \(Bundle.main.bundlePath)\n"
            let bundleContents = (try? fm.contentsOfDirectory(
                atPath: Bundle.main.bundlePath)) ?? []
            diag += "Bundle root: \(bundleContents.joined(separator: ", "))\n"
            updateStatus("ERROR: /bin/busybox not found\n\(diag)")
            return
        }

        updateStatus("Initializing emulator...")
        bridge.initialize(rootfsPath: rootfsDir)

        updateStatus("Spawning shell...")
        bridge.spawnShell()

        if bridge.pid < 0 {
            updateStatus("ERROR: Failed to spawn shell (pid=\(bridge.pid))")
            return
        }

        updateStatus("")  /* Clear status, shell is running */
        bridge.startEmulator()
    }

    private func updateStatus(_ msg: String) {
        DispatchQueue.main.async {
            statusMessage = msg
        }
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

        if fm.fileExists(atPath: rootfsDir.path) {
            return
        }

        /* Try multiple locations for the bundled rootfs */
        let candidates = [
            Bundle.main.path(forResource: "alpine", ofType: nil),
            Bundle.main.bundlePath + "/alpine",
            Bundle.main.resourcePath.map { $0 + "/alpine" },
            Bundle.main.bundlePath + "/rootfs/alpine",
        ].compactMap { $0 }

        for candidate in candidates {
            if fm.fileExists(atPath: candidate) {
                do {
                    try fm.copyItem(atPath: candidate, toPath: rootfsDir.path)
                    updateStatus("Rootfs extracted from: \(candidate)")
                    return
                } catch {
                    updateStatus("Copy failed from \(candidate): \(error)")
                }
            }
        }

        /* No bundled rootfs found */
        try? fm.createDirectory(at: rootfsDir, withIntermediateDirectories: true)
        updateStatus("No bundled rootfs found. Searched: \(candidates)")
    }
}
