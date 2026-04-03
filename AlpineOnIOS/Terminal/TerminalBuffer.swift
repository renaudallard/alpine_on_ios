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

/// Attributes for a single terminal cell.
struct CellAttributes: Equatable {
    var bold: Bool = false
    var underline: Bool = false
    var reverse: Bool = false
}

/// Color stored as an enum to support default, indexed, and RGB.
enum TerminalColor: Equatable {
    case `default`
    case indexed(Int)
    case rgb(UInt8, UInt8, UInt8)

    func resolve(isForeground: Bool) -> Color {
        switch self {
        case .default:
            return isForeground ? ANSIColors.defaultFG : ANSIColors.defaultBG
        case .indexed(let idx):
            return ANSIColors.color(forIndex: idx)
        case .rgb(let r, let g, let b):
            return ANSIColors.color(r: Int(r), g: Int(g), b: Int(b))
        }
    }
}

/// A single character cell in the terminal grid.
struct TerminalCell: Equatable {
    var character: Character = " "
    var fg: TerminalColor = .default
    var bg: TerminalColor = .default
    var attrs: CellAttributes = CellAttributes()
}

/// Terminal screen buffer with scrollback support.
class TerminalBuffer: ObservableObject {
    @Published var cols: Int
    @Published var rows: Int
    @Published var cursorRow: Int = 0
    @Published var cursorCol: Int = 0
    @Published var cursorVisible: Bool = true

    /// The visible screen grid.
    @Published var grid: [[TerminalCell]]

    /// Scrollback buffer (rows that scrolled off the top).
    var scrollback: [[TerminalCell]] = []
    let maxScrollback = 1000

    /// Scroll region (inclusive, zero-based).
    var scrollTop: Int = 0
    var scrollBottom: Int

    /// Saved cursor state.
    var savedCursorRow: Int = 0
    var savedCursorCol: Int = 0

    /// Current drawing attributes.
    var currentFG: TerminalColor = .default
    var currentBG: TerminalColor = .default
    var currentAttrs = CellAttributes()

    /// Tab stops (every 8 columns by default).
    var tabStops: [Bool]

    init(cols: Int = 80, rows: Int = 24) {
        self.cols = cols
        self.rows = rows
        self.scrollBottom = rows - 1
        self.grid = Self.emptyGrid(cols: cols, rows: rows)
        self.tabStops = Self.defaultTabStops(cols: cols)
    }

    // MARK: - Grid helpers

    static func emptyGrid(cols: Int, rows: Int) -> [[TerminalCell]] {
        Array(repeating: Array(repeating: TerminalCell(), count: cols),
              count: rows)
    }

    static func emptyRow(cols: Int) -> [TerminalCell] {
        Array(repeating: TerminalCell(), count: cols)
    }

    static func defaultTabStops(cols: Int) -> [Bool] {
        (0..<cols).map { $0 % 8 == 0 }
    }

    // MARK: - Character output

    /// Put a character at the current cursor position and advance.
    func putChar(_ ch: Character) {
        if cursorCol >= cols {
            /* Line wrap */
            cursorCol = 0
            lineFeed()
        }

        var cell = TerminalCell()
        cell.character = ch
        cell.fg = currentFG
        cell.bg = currentBG
        cell.attrs = currentAttrs
        grid[cursorRow][cursorCol] = cell
        cursorCol += 1
    }

    // MARK: - Cursor movement

    func moveCursorUp(_ n: Int) {
        cursorRow = max(scrollTop, cursorRow - max(n, 1))
    }

    func moveCursorDown(_ n: Int) {
        cursorRow = min(scrollBottom, cursorRow + max(n, 1))
    }

    func moveCursorForward(_ n: Int) {
        cursorCol = min(cols - 1, cursorCol + max(n, 1))
    }

    func moveCursorBack(_ n: Int) {
        cursorCol = max(0, cursorCol - max(n, 1))
    }

    func setCursorPosition(row: Int, col: Int) {
        cursorRow = min(max(row, 0), rows - 1)
        cursorCol = min(max(col, 0), cols - 1)
    }

    func saveCursor() {
        savedCursorRow = cursorRow
        savedCursorCol = cursorCol
    }

    func restoreCursor() {
        cursorRow = savedCursorRow
        cursorCol = savedCursorCol
    }

    // MARK: - Line operations

    func carriageReturn() {
        cursorCol = 0
    }

    func lineFeed() {
        if cursorRow == scrollBottom {
            scrollUp()
        } else if cursorRow < rows - 1 {
            cursorRow += 1
        }
    }

    func reverseLineFeed() {
        if cursorRow == scrollTop {
            scrollDown()
        } else if cursorRow > 0 {
            cursorRow -= 1
        }
    }

    func tab() {
        var target = cursorCol + 1
        while target < cols {
            if tabStops[target] {
                break
            }
            target += 1
        }
        cursorCol = min(target, cols - 1)
    }

    func backspace() {
        if cursorCol > 0 {
            cursorCol -= 1
        }
    }

    // MARK: - Scroll

    func scrollUp() {
        /* Move the top row of the scroll region to scrollback. */
        if scrollTop == 0 {
            scrollback.append(grid[0])
            if scrollback.count > maxScrollback {
                scrollback.removeFirst()
            }
        }

        for r in scrollTop..<scrollBottom {
            grid[r] = grid[r + 1]
        }
        grid[scrollBottom] = Self.emptyRow(cols: cols)
    }

    func scrollDown() {
        for r in stride(from: scrollBottom, to: scrollTop, by: -1) {
            grid[r] = grid[r - 1]
        }
        grid[scrollTop] = Self.emptyRow(cols: cols)
    }

    func setScrollRegion(top: Int, bottom: Int) {
        scrollTop = max(0, min(top, rows - 1))
        scrollBottom = max(scrollTop, min(bottom, rows - 1))
    }

    // MARK: - Erase

    /// Erase in display.  mode: 0 = below, 1 = above, 2 = all.
    func eraseDisplay(mode: Int) {
        switch mode {
        case 0:
            eraseLine(mode: 0)
            for r in (cursorRow + 1)..<rows {
                grid[r] = Self.emptyRow(cols: cols)
            }
        case 1:
            eraseLine(mode: 1)
            for r in 0..<cursorRow {
                grid[r] = Self.emptyRow(cols: cols)
            }
        case 2:
            for r in 0..<rows {
                grid[r] = Self.emptyRow(cols: cols)
            }
        default:
            break
        }
    }

    /// Erase in line.  mode: 0 = right, 1 = left, 2 = all.
    func eraseLine(mode: Int) {
        switch mode {
        case 0:
            for c in cursorCol..<cols {
                grid[cursorRow][c] = TerminalCell()
            }
        case 1:
            for c in 0...cursorCol {
                grid[cursorRow][c] = TerminalCell()
            }
        case 2:
            grid[cursorRow] = Self.emptyRow(cols: cols)
        default:
            break
        }
    }

    func deleteChars(_ n: Int) {
        let count = min(n, cols - cursorCol)
        grid[cursorRow].removeSubrange(cursorCol..<(cursorCol + count))
        grid[cursorRow].append(contentsOf:
            Array(repeating: TerminalCell(), count: count))
    }

    func eraseChars(_ n: Int) {
        for i in 0..<min(n, cols - cursorCol) {
            grid[cursorRow][cursorCol + i] = TerminalCell()
        }
    }

    func insertChars(_ n: Int) {
        let count = min(n, cols - cursorCol)
        let blanks = Array(repeating: TerminalCell(), count: count)
        grid[cursorRow].insert(contentsOf: blanks, at: cursorCol)
        grid[cursorRow] = Array(grid[cursorRow].prefix(cols))
    }

    func insertLines(_ n: Int) {
        let count = min(n, scrollBottom - cursorRow + 1)
        for _ in 0..<count {
            grid.remove(at: scrollBottom)
            grid.insert(Self.emptyRow(cols: cols), at: cursorRow)
        }
    }

    func deleteLines(_ n: Int) {
        let count = min(n, scrollBottom - cursorRow + 1)
        for _ in 0..<count {
            grid.remove(at: cursorRow)
            grid.insert(Self.emptyRow(cols: cols), at: scrollBottom)
        }
    }

    // MARK: - Attributes

    func resetAttributes() {
        currentFG = .default
        currentBG = .default
        currentAttrs = CellAttributes()
    }

    // MARK: - Resize

    func resize(newRows: Int, newCols: Int) {
        var newGrid = Self.emptyGrid(cols: newCols, rows: newRows)
        let copyRows = min(rows, newRows)
        let copyCols = min(cols, newCols)
        for r in 0..<copyRows {
            for c in 0..<copyCols {
                newGrid[r][c] = grid[r][c]
            }
        }
        grid = newGrid
        cols = newCols
        rows = newRows
        scrollTop = 0
        scrollBottom = newRows - 1
        cursorRow = min(cursorRow, newRows - 1)
        cursorCol = min(cursorCol, newCols - 1)
        tabStops = Self.defaultTabStops(cols: newCols)
    }

    /// Full terminal reset.
    func reset() {
        grid = Self.emptyGrid(cols: cols, rows: rows)
        scrollback.removeAll()
        cursorRow = 0
        cursorCol = 0
        scrollTop = 0
        scrollBottom = rows - 1
        resetAttributes()
        cursorVisible = true
        tabStops = Self.defaultTabStops(cols: cols)
    }
}
