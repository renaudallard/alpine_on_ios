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

struct SettingsView: View {
    @EnvironmentObject var settings: AppSettings
    @Environment(\.dismiss) var dismiss
    @State private var showResetAlert = false

    var body: some View {
        NavigationView {
            Form {
                Section(header: Text("Terminal")) {
                    HStack {
                        Text("Font Size")
                        Spacer()
                        Text("\(Int(settings.fontSize)) pt")
                            .foregroundColor(.secondary)
                    }
                    Slider(value: $settings.fontSize, in: 8...24, step: 1)
                }

                Section(header: Text("Display")) {
                    Picker("Resolution", selection: $settings.displayResolutionRaw) {
                        ForEach(DisplayResolution.allCases) { res in
                            Text(res.rawValue).tag(res.rawValue)
                        }
                    }
                    Text("Changes take effect on next X server start.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }

                Section(header: Text("Keyboard")) {
                    Toggle("Haptic Feedback", isOn: $settings.hapticFeedback)
                }

                Section(header: Text("Data")) {
                    Button(role: .destructive) {
                        showResetAlert = true
                    } label: {
                        Text("Reset Root Filesystem")
                    }
                    .alert("Reset Rootfs?", isPresented: $showResetAlert) {
                        Button("Cancel", role: .cancel) {}
                        Button("Reset", role: .destructive) {
                            resetRootfs()
                        }
                    } message: {
                        Text("This will delete the Alpine rootfs and "
                             + "re-extract it on next launch. All data "
                             + "inside the rootfs will be lost.")
                    }
                }

                Section(header: Text("About")) {
                    HStack {
                        Text("Version")
                        Spacer()
                        Text(Bundle.main.infoDictionary?[
                            "CFBundleShortVersionString"] as? String ?? "0.1.0")
                            .foregroundColor(.secondary)
                    }
                    Text("Alpine on iOS runs Alpine Linux aarch64 using "
                         + "an AArch64 CPU interpreter with Linux syscall "
                         + "emulation.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }

    private func resetRootfs() {
        let docs = FileManager.default.urls(for: .documentDirectory,
                                            in: .userDomainMask).first!
        let rootfs = docs.appendingPathComponent("alpine")
        try? FileManager.default.removeItem(at: rootfs)
    }
}
