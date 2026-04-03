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

enum ColorTheme: String, CaseIterable, Identifiable {
    case dark = "Dark"
    case light = "Light"
    case solarized = "Solarized"

    var id: String { rawValue }
}

class AppSettings: ObservableObject {
    @AppStorage("fontSize") var fontSize: Double = 14.0
    @AppStorage("colorTheme") var colorThemeRaw: String = ColorTheme.dark.rawValue
    @AppStorage("hapticFeedback") var hapticFeedback: Bool = true

    var colorTheme: ColorTheme {
        get { ColorTheme(rawValue: colorThemeRaw) ?? .dark }
        set { colorThemeRaw = newValue.rawValue }
    }
}
