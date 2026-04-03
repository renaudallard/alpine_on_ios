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

/// ANSI color palette for the terminal.
struct ANSIColors {
    /// Default foreground: light gray.
    static let defaultFG = Color(red: 0.8, green: 0.8, blue: 0.8)

    /// Default background: black.
    static let defaultBG = Color.black

    /// Standard 16 ANSI colors (dark theme).
    static let standard: [Color] = [
        /* 0  Black   */ Color(red: 0.0,  green: 0.0,  blue: 0.0),
        /* 1  Red     */ Color(red: 0.8,  green: 0.0,  blue: 0.0),
        /* 2  Green   */ Color(red: 0.0,  green: 0.8,  blue: 0.0),
        /* 3  Yellow  */ Color(red: 0.8,  green: 0.8,  blue: 0.0),
        /* 4  Blue    */ Color(red: 0.0,  green: 0.0,  blue: 0.8),
        /* 5  Magenta */ Color(red: 0.8,  green: 0.0,  blue: 0.8),
        /* 6  Cyan    */ Color(red: 0.0,  green: 0.8,  blue: 0.8),
        /* 7  White   */ Color(red: 0.75, green: 0.75, blue: 0.75),
        /* 8  Bright Black   */ Color(red: 0.5,  green: 0.5,  blue: 0.5),
        /* 9  Bright Red     */ Color(red: 1.0,  green: 0.0,  blue: 0.0),
        /* 10 Bright Green   */ Color(red: 0.0,  green: 1.0,  blue: 0.0),
        /* 11 Bright Yellow  */ Color(red: 1.0,  green: 1.0,  blue: 0.0),
        /* 12 Bright Blue    */ Color(red: 0.33, green: 0.33, blue: 1.0),
        /* 13 Bright Magenta */ Color(red: 1.0,  green: 0.0,  blue: 1.0),
        /* 14 Bright Cyan    */ Color(red: 0.0,  green: 1.0,  blue: 1.0),
        /* 15 Bright White   */ Color(red: 1.0,  green: 1.0,  blue: 1.0),
    ]

    /// Full 256-color palette. Built lazily on first access.
    static let palette256: [Color] = {
        var colors = [Color]()
        colors.reserveCapacity(256)

        /* 0-15: standard colors */
        colors.append(contentsOf: standard)

        /* 16-231: 6x6x6 color cube */
        for r in 0..<6 {
            for g in 0..<6 {
                for b in 0..<6 {
                    let rv = r == 0 ? 0.0 : (55.0 + Double(r) * 40.0) / 255.0
                    let gv = g == 0 ? 0.0 : (55.0 + Double(g) * 40.0) / 255.0
                    let bv = b == 0 ? 0.0 : (55.0 + Double(b) * 40.0) / 255.0
                    colors.append(Color(red: rv, green: gv, blue: bv))
                }
            }
        }

        /* 232-255: grayscale ramp */
        for i in 0..<24 {
            let v = (8.0 + Double(i) * 10.0) / 255.0
            colors.append(Color(red: v, green: v, blue: v))
        }

        return colors
    }()

    /// Return Color for an ANSI 256-color index.
    static func color(forIndex idx: Int) -> Color {
        guard idx >= 0, idx < 256 else { return defaultFG }
        return palette256[idx]
    }

    /// Return Color for an RGB triple.
    static func color(r: Int, g: Int, b: Int) -> Color {
        Color(red: Double(r) / 255.0,
              green: Double(g) / 255.0,
              blue: Double(b) / 255.0)
    }
}
