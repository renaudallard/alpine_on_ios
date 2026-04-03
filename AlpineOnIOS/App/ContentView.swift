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
    @Binding var statusMessage: String
    @State private var showSettings = false

    var body: some View {
        TabView {
            NavigationView {
                ZStack {
                    TerminalView()
                        .environmentObject(bridge)
                        .environmentObject(settings)

                    /* Show status/error overlay when emulator isn't ready */
                    if !statusMessage.isEmpty {
                        VStack {
                            Spacer()
                            Text(statusMessage)
                                .font(.system(.body, design: .monospaced))
                                .foregroundColor(.green)
                                .multilineTextAlignment(.leading)
                                .padding()
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .background(Color.black.opacity(0.85))
                                .cornerRadius(8)
                                .padding()
                            Spacer()
                        }
                    }
                }
                .navigationTitle("Alpine Terminal")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .navigationBarTrailing) {
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
            .navigationViewStyle(.stack)
            .tabItem {
                Label("Terminal", systemImage: "terminal")
            }

            DisplayContainerView()
                .tabItem {
                    Label("Display", systemImage: "display")
                }
        }
    }
}
