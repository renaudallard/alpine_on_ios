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

struct ContentView: View {
    @EnvironmentObject var settings: AppSettings
    @EnvironmentObject var bridge: EmulatorBridge
    @State private var showSettings = false

    var body: some View {
        TabView {
            terminalTab
            .tabItem {
                Label("Terminal", systemImage: "terminal")
            }

            DisplayContainerView()
                .tabItem {
                    Label("Display", systemImage: "display")
                }
        }
    }

    private var terminalTab: some View {
        ZStack {
            Color.black.edgesIgnoringSafeArea(.all)

            switch bridge.state {
            case .idle, .extracting, .initializing, .spawning:
                statusView
            case .running:
                ZStack {
                    TerminalView()
                        .environmentObject(bridge)
                        .environmentObject(settings)

                    /* Show loading hint until shell produces output.
                     * Interpreter mode is slow (~1 min to first prompt). */
                    if !bridge.hasOutput {
                        VStack {
                            Spacer()
                            HStack {
                                ProgressView()
                                    .tint(.green)
                                Text("Loading shell (interpreter mode)...")
                                    .font(.system(.caption, design: .monospaced))
                                    .foregroundColor(.green)
                            }
                            .padding(8)
                            .background(Color.black.opacity(0.8))
                            .cornerRadius(8)
                            .padding(.bottom, 40)
                        }
                    }
                }
            case .error(let msg):
                errorView(msg)
            }
        }
        .toolbar {
            ToolbarItem(placement: .automatic) {
                Button {
                    showSettings = true
                } label: {
                    Image(systemName: "gearshape")
                }
            }
        }
        .sheet(isPresented: $showSettings) {
            SettingsView()
                .environmentObject(settings)
        }
    }

    private var statusView: some View {
        VStack(spacing: 16) {
            ProgressView()
                .scaleEffect(1.5)
                .tint(.green)
            Text(statusText)
                .font(.system(.body, design: .monospaced))
                .foregroundColor(.green)
        }
    }

    private var statusText: String {
        switch bridge.state {
        case .idle: return "Starting..."
        case .extracting: return "Extracting rootfs..."
        case .initializing: return "Initializing emulator..."
        case .spawning: return "Spawning shell..."
        case .running: return ""
        case .error: return ""
        }
    }

    private func errorView(_ message: String) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text("Error")
                    .font(.title2.bold())
                    .foregroundColor(.red)
                Text(message)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundColor(.green)
                Button("Restart") {
                    exit(0)
                }
                .buttonStyle(.borderedProminent)
                .tint(.blue)
                .padding(.top, 8)
            }
            .padding()
        }
    }
}
