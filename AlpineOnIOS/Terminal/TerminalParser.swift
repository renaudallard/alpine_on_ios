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

import Foundation

/// VT100/xterm escape sequence parser.
/// Drives a TerminalBuffer based on received byte output from the emulator.
class TerminalParser {
    private enum State {
        case normal
        case escape
        case csi
        case osc
    }

    private let buffer: TerminalBuffer
    private var state: State = .normal

    /// Accumulated CSI parameter bytes (digits and ';').
    private var csiParams: [UInt8] = []

    /// CSI private marker ('?' or '>').
    private var csiPrivate: UInt8 = 0

    /// OSC accumulated string.
    private var oscData: [UInt8] = []

    /// UTF-8 multi-byte accumulator.
    private var utf8Buffer: [UInt8] = []
    private var utf8Remaining: Int = 0

    /// Callback to write a response back to the emulator.
    var responseHandler: ((Data) -> Void)?

    init(buffer: TerminalBuffer) {
        self.buffer = buffer
    }

    // MARK: - Feed data

    /// Process a chunk of bytes from the emulator.
    func feed(_ data: Data) {
        for byte in data {
            processByte(byte)
        }
    }

    // MARK: - Byte processing

    private func processByte(_ byte: UInt8) {
        switch state {
        case .normal:
            processNormal(byte)
        case .escape:
            processEscape(byte)
        case .csi:
            processCSI(byte)
        case .osc:
            processOSC(byte)
        }
    }

    private func processNormal(_ byte: UInt8) {
        /* If we are collecting a multi-byte UTF-8 sequence, handle it first. */
        if utf8Remaining > 0 {
            if byte >= 0x80 && byte <= 0xBF {
                /* Valid continuation byte */
                utf8Buffer.append(byte)
                utf8Remaining -= 1
                if utf8Remaining == 0 {
                    emitUTF8()
                }
            } else {
                /* Invalid continuation: emit replacement for the broken
                 * sequence and re-process this byte from scratch. */
                utf8Buffer.removeAll()
                utf8Remaining = 0
                buffer.putChar("\u{FFFD}")
                processNormal(byte)
            }
            return
        }

        switch byte {
        case 0x1B: /* ESC */
            state = .escape
        case 0x0A: /* LF - also do CR (newline mode) */
            buffer.carriageReturn()
            buffer.lineFeed()
        case 0x0D: /* CR */
            buffer.carriageReturn()
        case 0x09: /* TAB */
            buffer.tab()
        case 0x08: /* BS */
            buffer.backspace()
        case 0x07: /* BEL */
            break /* ignore bell */
        case 0x00...0x06, 0x0B, 0x0C, 0x0E...0x1A, 0x1C...0x1F:
            break /* ignore other control chars */
        default:
            if byte < 0x80 {
                /* ASCII */
                buffer.putChar(Character(UnicodeScalar(byte)))
            } else if byte >= 0xC0 && byte < 0xE0 {
                /* 2-byte UTF-8 sequence */
                utf8Buffer = [byte]
                utf8Remaining = 1
            } else if byte >= 0xE0 && byte < 0xF0 {
                /* 3-byte UTF-8 sequence */
                utf8Buffer = [byte]
                utf8Remaining = 2
            } else if byte >= 0xF0 && byte < 0xF8 {
                /* 4-byte UTF-8 sequence */
                utf8Buffer = [byte]
                utf8Remaining = 3
            } else {
                /* Unexpected continuation byte (0x80-0xBF) without leader */
                buffer.putChar("\u{FFFD}")
            }
        }
    }

    /// Decode the accumulated UTF-8 buffer and emit the character.
    private func emitUTF8() {
        if let str = String(bytes: utf8Buffer, encoding: .utf8),
           let ch = str.first {
            buffer.putChar(ch)
        } else {
            buffer.putChar("\u{FFFD}")
        }
        utf8Buffer.removeAll()
    }

    private func processEscape(_ byte: UInt8) {
        state = .normal
        switch byte {
        case 0x5B: /* '[' -> CSI */
            state = .csi
            csiParams = []
            csiPrivate = 0
        case 0x5D: /* ']' -> OSC */
            state = .osc
            oscData = []
        case 0x44: /* 'D' - Index (scroll up) */
            buffer.lineFeed()
        case 0x4D: /* 'M' - Reverse Index (scroll down) */
            buffer.reverseLineFeed()
        case 0x45: /* 'E' - Next Line */
            buffer.carriageReturn()
            buffer.lineFeed()
        case 0x37: /* '7' - Save Cursor */
            buffer.saveCursor()
        case 0x38: /* '8' - Restore Cursor */
            buffer.restoreCursor()
        case 0x63: /* 'c' - Full Reset */
            buffer.reset()
        default:
            break /* ignore unknown escape */
        }
    }

    // MARK: - CSI

    private func processCSI(_ byte: UInt8) {
        switch byte {
        case 0x30...0x39, 0x3B: /* '0'-'9', ';' */
            csiParams.append(byte)
        case 0x3F, 0x3E: /* '?', '>' */
            csiPrivate = byte
        default:
            executeCSI(finalByte: byte)
            state = .normal
        }
    }

    /// Parse CSI params string into integer array.
    private func parseParams() -> [Int] {
        if csiParams.isEmpty { return [] }
        let str = String(bytes: csiParams, encoding: .ascii) ?? ""
        return str.split(separator: ";", omittingEmptySubsequences: false)
            .map { Int($0) ?? 0 }
    }

    private func param(_ params: [Int], _ index: Int, default def: Int) -> Int {
        if index < params.count, params[index] != 0 {
            return params[index]
        }
        return def
    }

    private func executeCSI(finalByte: UInt8) {
        let params = parseParams()

        switch finalByte {
        case 0x41: /* 'A' - CUU: Cursor Up */
            buffer.moveCursorUp(param(params, 0, default: 1))

        case 0x42: /* 'B' - CUD: Cursor Down */
            buffer.moveCursorDown(param(params, 0, default: 1))

        case 0x43: /* 'C' - CUF: Cursor Forward */
            buffer.moveCursorForward(param(params, 0, default: 1))

        case 0x44: /* 'D' - CUB: Cursor Back */
            buffer.moveCursorBack(param(params, 0, default: 1))

        case 0x48, 0x66: /* 'H', 'f' - CUP: Cursor Position */
            let row = param(params, 0, default: 1) - 1
            let col = param(params, 1, default: 1) - 1
            buffer.setCursorPosition(row: row, col: col)

        case 0x47: /* 'G' - CHA: Cursor Horizontal Absolute */
            let col = param(params, 0, default: 1) - 1
            buffer.cursorCol = min(max(col, 0), buffer.cols - 1)

        case 0x64: /* 'd' - VPA: Vertical Position Absolute */
            let row = param(params, 0, default: 1) - 1
            buffer.cursorRow = min(max(row, 0), buffer.rows - 1)

        case 0x4A: /* 'J' - ED: Erase in Display */
            buffer.eraseDisplay(mode: param(params, 0, default: 0))

        case 0x4B: /* 'K' - EL: Erase in Line */
            buffer.eraseLine(mode: param(params, 0, default: 0))

        case 0x6D: /* 'm' - SGR: Select Graphic Rendition */
            handleSGR(params.isEmpty ? [0] : params)

        case 0x72: /* 'r' - DECSTBM: Set Scrolling Region */
            let top = param(params, 0, default: 1) - 1
            let bottom = param(params, 1, default: buffer.rows) - 1
            buffer.setScrollRegion(top: top, bottom: bottom)
            buffer.setCursorPosition(row: 0, col: 0)

        case 0x68: /* 'h' - SM: Set Mode */
            if csiPrivate == 0x3F { /* DEC private */
                for p in params {
                    if p == 25 { buffer.cursorVisible = true }
                }
            }

        case 0x6C: /* 'l' - RM: Reset Mode */
            if csiPrivate == 0x3F {
                for p in params {
                    if p == 25 { buffer.cursorVisible = false }
                }
            }

        case 0x6E: /* 'n' - DSR: Device Status Report */
            if param(params, 0, default: 0) == 6 {
                /* Respond with cursor position */
                let resp = "\u{1B}[\(buffer.cursorRow + 1);\(buffer.cursorCol + 1)R"
                responseHandler?(Data(resp.utf8))
            }

        case 0x50: /* 'P' - DCH: Delete Characters */
            buffer.deleteChars(param(params, 0, default: 1))

        case 0x58: /* 'X' - ECH: Erase Characters */
            buffer.eraseChars(param(params, 0, default: 1))

        case 0x40: /* '@' - ICH: Insert Characters */
            buffer.insertChars(param(params, 0, default: 1))

        case 0x4C: /* 'L' - IL: Insert Lines */
            buffer.insertLines(param(params, 0, default: 1))

        case 0x4D: /* 'M' - DL: Delete Lines */
            buffer.deleteLines(param(params, 0, default: 1))

        case 0x53: /* 'S' - SU: Scroll Up */
            for _ in 0..<param(params, 0, default: 1) {
                buffer.scrollUp()
            }

        case 0x54: /* 'T' - SD: Scroll Down */
            for _ in 0..<param(params, 0, default: 1) {
                buffer.scrollDown()
            }

        case 0x63: /* 'c' - DA: Device Attributes */
            let resp = "\u{1B}[?1;2c"
            responseHandler?(Data(resp.utf8))

        default:
            break /* ignore unknown CSI sequence */
        }
    }

    // MARK: - SGR

    private func handleSGR(_ params: [Int]) {
        var i = 0
        while i < params.count {
            let p = params[i]
            switch p {
            case 0:
                buffer.resetAttributes()
            case 1:
                buffer.currentAttrs.bold = true
            case 4:
                buffer.currentAttrs.underline = true
            case 7:
                buffer.currentAttrs.reverse = true
            case 22:
                buffer.currentAttrs.bold = false
            case 24:
                buffer.currentAttrs.underline = false
            case 27:
                buffer.currentAttrs.reverse = false
            case 30...37:
                buffer.currentFG = .indexed(p - 30)
            case 38:
                i += 1
                if let color = parseExtendedColor(params, &i) {
                    buffer.currentFG = color
                }
                continue /* i already advanced */
            case 39:
                buffer.currentFG = .default
            case 40...47:
                buffer.currentBG = .indexed(p - 40)
            case 48:
                i += 1
                if let color = parseExtendedColor(params, &i) {
                    buffer.currentBG = color
                }
                continue
            case 49:
                buffer.currentBG = .default
            case 90...97:
                buffer.currentFG = .indexed(p - 90 + 8)
            case 100...107:
                buffer.currentBG = .indexed(p - 100 + 8)
            default:
                break
            }
            i += 1
        }
    }

    /// Parse 256-color (5;N) or truecolor (2;R;G;B) after 38 or 48.
    private func parseExtendedColor(_ params: [Int],
                                    _ i: inout Int) -> TerminalColor? {
        guard i < params.count else { return nil }
        let mode = params[i]
        i += 1
        switch mode {
        case 5: /* 256-color */
            guard i < params.count else { return nil }
            let idx = params[i]
            i += 1
            return .indexed(min(max(idx, 0), 255))
        case 2: /* Truecolor */
            guard i + 2 < params.count else { return nil }
            let r = UInt8(clamping: params[i])
            let g = UInt8(clamping: params[i + 1])
            let b = UInt8(clamping: params[i + 2])
            i += 3
            return .rgb(r, g, b)
        default:
            return nil
        }
    }

    // MARK: - OSC

    private func processOSC(_ byte: UInt8) {
        switch byte {
        case 0x07: /* BEL - terminates OSC */
            state = .normal
        case 0x1B: /* ESC - might be ST (\e\\) */
            /* Simplified: just terminate OSC on ESC */
            state = .normal
        default:
            oscData.append(byte)
        }
    }
}
