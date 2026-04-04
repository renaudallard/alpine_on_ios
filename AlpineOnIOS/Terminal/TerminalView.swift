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
import UIKit

// MARK: - Terminal View

/// SwiftUI terminal emulator view.
struct TerminalView: View {
    @EnvironmentObject var bridge: EmulatorBridge
    @EnvironmentObject var settings: AppSettings

    @StateObject private var termBuffer = TerminalBuffer(cols: 80, rows: 24)
    /// TerminalParser is a class (reference type). @State is used here
    /// intentionally: the reference identity is stable across re-renders
    /// and the parser is initialized once in onAppear.
    @State private var parser: TerminalParser?
    @State private var ctrlPressed = false

    var body: some View {
        VStack(spacing: 0) {
            /* Terminal character grid */
            ZStack {
                TerminalGridView(buffer: termBuffer, fontSize: settings.fontSize)
                    .background(Color.black)

                /* Full-size transparent text field captures keyboard */
                KeyboardInputView(
                    onKeyPress: { handleKey($0) },
                    ctrlPressed: $ctrlPressed
                )
            }
            .onTapGesture {
                NotificationCenter.default.post(
                    name: .terminalFocusKeyboard, object: nil)
            }

            /* Extra key row */
            AccessoryKeyBar(
                ctrlPressed: $ctrlPressed,
                onKey: { handleKey($0) }
            )
        }
        .onAppear {
            let p = TerminalParser(buffer: termBuffer)
            p.responseHandler = { data in
                bridge.write(data: data)
            }
            parser = p
            startReading()
            bridge.setWindowSize(rows: 24, cols: 80)
        }
        .onReceive(NotificationCenter.default.publisher(for: .terminalSpecialKey)) { notification in
            if let seq = notification.object as? String {
                bridge.write(data: Data(seq.utf8))
            }
        }
    }

    private func startReading() {
        bridge.startReading { data in
            DispatchQueue.main.async {
                parser?.feed(data)
            }
        }
    }

    private func handleKey(_ key: String) {
        guard !key.isEmpty else { return }

        if settings.hapticFeedback {
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
        }

        var data: Data
        if ctrlPressed, key.count == 1 {
            /* Send control character: Ctrl-A = 0x01, Ctrl-Z = 0x1A */
            let ch = key.uppercased()
            if let scalar = ch.unicodeScalars.first,
               scalar.value >= 0x40, scalar.value <= 0x5F {
                let ctrl = UInt8(scalar.value - 0x40)
                data = Data([ctrl])
            } else {
                data = Data(key.utf8)
            }
            ctrlPressed = false
        } else {
            data = Data(key.utf8)
        }

        bridge.write(data: data)

        /* Local echo for typed characters (no tty driver).
         * Only echo printable chars and backspace, not newline
         * (the shell output handles that). */
        for byte in data {
            if byte == 0x08 {
                /* Backspace */
                parser?.feed(Data([0x08, 0x20, 0x08]))
            } else if byte >= 0x20 && byte < 0x7F {
                /* Printable ASCII */
                parser?.feed(Data([byte]))
            }
            /* Skip newline/CR - shell handles line output */
        }
    }
}

// MARK: - Terminal Grid Rendering

/// Renders the terminal buffer as a grid of styled characters.
struct TerminalGridView: View {
    @ObservedObject var buffer: TerminalBuffer
    let fontSize: Double

    var font: Font {
        .system(size: CGFloat(fontSize), design: .monospaced)
    }

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView(.vertical, showsIndicators: true) {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(0..<buffer.rows, id: \.self) { row in
                        HStack(spacing: 0) {
                            ForEach(0..<buffer.cols, id: \.self) { col in
                                cellView(row: row, col: col)
                            }
                        }
                        .id(row)
                    }
                }
            }
            .onChange(of: buffer.cursorRow) { newRow in
                proxy.scrollTo(newRow, anchor: .bottom)
            }
        }
    }

    @ViewBuilder
    private func cellView(row: Int, col: Int) -> some View {
        let cell = buffer.grid[row][col]
        let isCursor = buffer.cursorVisible
            && row == buffer.cursorRow
            && col == buffer.cursorCol

        let fg = resolvedFG(cell: cell, isCursor: isCursor)
        let bg = resolvedBG(cell: cell, isCursor: isCursor)

        let text = Text(String(cell.character))
            .font(cell.attrs.bold ? font.bold() : font)
            .foregroundColor(fg)
            .background(bg)
            .frame(maxWidth: .infinity)

        if cell.attrs.underline {
            text.overlay(
                Rectangle()
                    .frame(height: 1)
                    .foregroundColor(fg)
                    .offset(y: 6),
                alignment: .bottom
            )
        } else {
            text
        }
    }

    private func resolvedFG(cell: TerminalCell, isCursor: Bool) -> Color {
        if isCursor {
            return cell.attrs.reverse
                ? cell.fg.resolve(isForeground: true)
                : cell.bg.resolve(isForeground: false)
        }
        if cell.attrs.reverse {
            return cell.bg.resolve(isForeground: false)
        }
        return cell.fg.resolve(isForeground: true)
    }

    private func resolvedBG(cell: TerminalCell, isCursor: Bool) -> Color {
        if isCursor {
            return cell.attrs.reverse
                ? cell.bg.resolve(isForeground: false)
                : cell.fg.resolve(isForeground: true)
        }
        if cell.attrs.reverse {
            return cell.fg.resolve(isForeground: true)
        }
        return cell.bg.resolve(isForeground: false)
    }
}

// MARK: - Hidden Keyboard Input (UIViewRepresentable)

/// A hidden UITextField to capture keyboard input on iOS.
struct KeyboardInputView: UIViewRepresentable {
    var onKeyPress: (String) -> Void
    @Binding var ctrlPressed: Bool

    func makeUIView(context: Context) -> HiddenTextField {
        let tf = HiddenTextField()
        tf.delegate = context.coordinator
        tf.autocapitalizationType = .none
        tf.autocorrectionType = .no
        tf.spellCheckingType = .no
        tf.smartQuotesType = .no
        tf.smartDashesType = .no
        tf.keyboardType = .asciiCapable
        tf.returnKeyType = .default
        tf.tintColor = .clear
        tf.textColor = .clear
        tf.backgroundColor = .clear

        /* Store direct reference for focus management */
        context.coordinator.textField = tf

        NotificationCenter.default.addObserver(
            context.coordinator,
            selector: #selector(Coordinator.focusKeyboard),
            name: .terminalFocusKeyboard,
            object: nil)

        /* Become first responder after a short delay */
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            tf.becomeFirstResponder()
        }

        return tf
    }

    func updateUIView(_ uiView: HiddenTextField, context: Context) {
        /* Ensure reference stays current */
        context.coordinator.textField = uiView
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(onKeyPress: onKeyPress)
    }

    class Coordinator: NSObject, UITextFieldDelegate {
        var onKeyPress: (String) -> Void
        var textField: HiddenTextField?

        init(onKeyPress: @escaping (String) -> Void) {
            self.onKeyPress = onKeyPress
        }

        deinit {
            NotificationCenter.default.removeObserver(self)
        }

        @objc func focusKeyboard(_ notification: Notification) {
            textField?.becomeFirstResponder()
        }

        func textField(_ textField: UITextField,
                        shouldChangeCharactersIn range: NSRange,
                        replacementString string: String) -> Bool {
            if string.isEmpty {
                /* Backspace/delete: send BS (0x08) */
                onKeyPress("\u{08}")
            } else {
                onKeyPress(string)
            }
            /* Clear the field to keep it ready for next input */
            DispatchQueue.main.async {
                textField.text = " "  /* Keep a space so backspace works */
            }
            return false
        }

        /* Handle return key */
        func textFieldShouldReturn(_ textField: UITextField) -> Bool {
            onKeyPress("\n")
            return false
        }
    }
}

/// UITextField subclass that stays invisible but always accepts input.
class HiddenTextField: UITextField {
    override var canBecomeFirstResponder: Bool { true }
    override var canResignFirstResponder: Bool { true }

    override func caretRect(for position: UITextPosition) -> CGRect {
        /* Hide the caret */
        .zero
    }

    override func selectionRects(for range: UITextRange) -> [UITextSelectionRect] {
        []
    }

    override func canPerformAction(_ action: Selector, withSender sender: Any?) -> Bool {
        /* Disable context menu */
        false
    }

    override func pressesBegan(_ presses: Set<UIPress>,
                                with event: UIPressesEvent?) {
        /* Handle special keys (arrows, escape, etc.) */
        for press in presses {
            guard let key = press.key else { continue }
            switch key.keyCode {
            case .keyboardUpArrow:
                NotificationCenter.default.post(
                    name: .terminalSpecialKey,
                    object: "\u{1B}[A")
                return
            case .keyboardDownArrow:
                NotificationCenter.default.post(
                    name: .terminalSpecialKey,
                    object: "\u{1B}[B")
                return
            case .keyboardRightArrow:
                NotificationCenter.default.post(
                    name: .terminalSpecialKey,
                    object: "\u{1B}[C")
                return
            case .keyboardLeftArrow:
                NotificationCenter.default.post(
                    name: .terminalSpecialKey,
                    object: "\u{1B}[D")
                return
            case .keyboardEscape:
                NotificationCenter.default.post(
                    name: .terminalSpecialKey,
                    object: "\u{1B}")
                return
            default:
                break
            }
        }
        super.pressesBegan(presses, with: event)
    }
}

// MARK: - Accessory Key Bar

/// Row of extra keys above the iOS keyboard.
struct AccessoryKeyBar: View {
    @Binding var ctrlPressed: Bool
    var onKey: (String) -> Void

    private let keys: [(String, String)] = [
        ("Tab", "\t"),
        ("Esc", "\u{1B}"),
        ("|", "|"),
        ("-", "-"),
        ("/", "/"),
        ("~", "~"),
    ]

    private let arrowKeys: [(String, String)] = [
        ("\u{2190}", "\u{1B}[D"),   /* left */
        ("\u{2191}", "\u{1B}[A"),   /* up */
        ("\u{2193}", "\u{1B}[B"),   /* down */
        ("\u{2192}", "\u{1B}[C"),   /* right */
    ]

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 6) {
                /* Keyboard toggle */
                Button {
                    NotificationCenter.default.post(
                        name: .terminalFocusKeyboard, object: nil)
                } label: {
                    Image(systemName: "keyboard")
                        .font(.system(size: 16))
                        .padding(.horizontal, 8)
                        .padding(.vertical, 6)
                        .background(Color(.systemGray5))
                        .cornerRadius(6)
                }

                /* Ctrl toggle */
                Button {
                    ctrlPressed.toggle()
                } label: {
                    Text("Ctrl")
                        .font(.system(size: 14, weight: .medium,
                                      design: .monospaced))
                        .padding(.horizontal, 10)
                        .padding(.vertical, 6)
                        .background(ctrlPressed
                            ? Color.blue : Color(.systemGray5))
                        .foregroundColor(ctrlPressed ? .white : .primary)
                        .cornerRadius(6)
                }

                ForEach(keys, id: \.0) { label, value in
                    Button {
                        onKey(value)
                    } label: {
                        Text(label)
                            .font(.system(size: 14, weight: .medium,
                                          design: .monospaced))
                            .padding(.horizontal, 10)
                            .padding(.vertical, 6)
                            .background(Color(.systemGray5))
                            .cornerRadius(6)
                    }
                }

                Divider().frame(height: 20)

                ForEach(arrowKeys, id: \.0) { label, value in
                    Button {
                        onKey(value)
                    } label: {
                        Text(label)
                            .font(.system(size: 16, weight: .medium))
                            .padding(.horizontal, 10)
                            .padding(.vertical, 6)
                            .background(Color(.systemGray5))
                            .cornerRadius(6)
                    }
                }
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
        }
        .background(Color(.systemGray6))
    }
}

// MARK: - Notification Names

extension Notification.Name {
    static let terminalFocusKeyboard = Notification.Name("terminalFocusKeyboard")
    static let terminalSpecialKey = Notification.Name("terminalSpecialKey")
}
