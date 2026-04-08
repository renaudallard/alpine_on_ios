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

                    /* Show loading hint until shell produces output. */
                    if !bridge.hasOutput {
                        VStack {
                            Spacer()
                            HStack {
                                ProgressView()
                                    .tint(.green)
                                Text("Loading shell [\(String(cString: emu_mode_info()))]")
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

            /*
             * If the previous run left breadcrumbs behind (silent
             * crash with no .ips), show them here so the user can
             * see where the last attempt died while the current
             * one spins up.
             */
            if !bridge.previousBreadcrumbs.isEmpty {
                ScrollView {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Previous run crumbs:")
                            .font(.system(.caption2, design: .monospaced).bold())
                            .foregroundColor(.orange)
                        Text(bridge.previousBreadcrumbs)
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundColor(.orange)
                            .textSelection(.enabled)
                    }
                    .padding(8)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 240)
                .background(Color.black.opacity(0.6))
                .cornerRadius(8)
                .padding(.horizontal)
            }
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
                    .textSelection(.enabled)
                if !bridge.previousBreadcrumbs.isEmpty {
                    Text("Previous run crumbs:")
                        .font(.system(.caption2, design: .monospaced).bold())
                        .foregroundColor(.orange)
                        .padding(.top, 8)
                    Text(bridge.previousBreadcrumbs)
                        .font(.system(.caption2, design: .monospaced))
                        .foregroundColor(.orange)
                        .textSelection(.enabled)
                }
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
