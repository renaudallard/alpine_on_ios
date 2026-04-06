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
#if os(iOS)
import UIKit
#elseif os(macOS)
import AppKit
#endif

// MARK: - Terminal View

/// SwiftUI terminal emulator view.
struct TerminalView: View {
    @EnvironmentObject var bridge: EmulatorBridge
    @EnvironmentObject var settings: AppSettings

    @StateObject private var termBuffer = TerminalBuffer(cols: 80, rows: 24)
    @State private var lastCols = 80
    @State private var lastRows = 24
    /// TerminalParser is a class (reference type). @State is used here
    /// intentionally: the reference identity is stable across re-renders
    /// and the parser is initialized once in onAppear.
    @State private var parser: TerminalParser?
    @State private var ctrlPressed = false

    var body: some View {
        VStack(spacing: 0) {
            /* Terminal character grid */
            GeometryReader { geo in
                ZStack(alignment: .topLeading) {
                    Color.black

                    TerminalGridView(buffer: termBuffer, fontSize: settings.fontSize)

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
                .onChange(of: geo.size) { newSize in
                    updateTerminalSize(newSize)
                }
                .onAppear {
                    updateTerminalSize(geo.size)
                }
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

    private func updateTerminalSize(_ size: CGSize) {
        let charW = settings.fontSize * 0.6
        let charH = settings.fontSize * 1.2
        let cols = max(Int(size.width / charW), 20)
        let rows = max(Int(size.height / charH), 5)
        if cols != lastCols || rows != lastRows {
            lastCols = cols
            lastRows = rows
            termBuffer.resize(newRows: rows, newCols: cols)
            bridge.setWindowSize(rows: rows, cols: cols)
        }
    }

    private func handleKey(_ key: String) {
        guard !key.isEmpty else { return }

        #if os(iOS)
        if settings.hapticFeedback {
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
        }
        #endif

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
        let sbCount = buffer.scrollback.count
        let totalRows = sbCount + buffer.rows

        ScrollViewReader { proxy in
            ScrollView(.vertical, showsIndicators: true) {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(0..<totalRows, id: \.self) { row in
                        HStack(spacing: 0) {
                            ForEach(0..<buffer.cols, id: \.self) { col in
                                cellView(row: row, col: col,
                                    sbCount: sbCount)
                            }
                        }
                        .id(row)
                    }
                }
            }
            .onChange(of: buffer.cursorRow) { _ in
                proxy.scrollTo(sbCount + buffer.cursorRow,
                    anchor: .bottom)
            }
            .onChange(of: sbCount) { _ in
                proxy.scrollTo(sbCount + buffer.cursorRow,
                    anchor: .bottom)
            }
        }
    }

    private func getCell(row: Int, col: Int, sbCount: Int) -> (TerminalCell, Bool) {
        if row < sbCount {
            let sbRow = buffer.scrollback[row]
            let cell = col < sbRow.count ? sbRow[col] : TerminalCell()
            return (cell, false)
        } else {
            return (buffer.grid[row - sbCount][col], true)
        }
    }

    @ViewBuilder
    private func cellView(row: Int, col: Int, sbCount: Int) -> some View {
        let (cell, isGrid) = getCell(row: row, col: col, sbCount: sbCount)

        let isCursor = buffer.cursorVisible && isGrid
            && (row - sbCount) == buffer.cursorRow
            && col == buffer.cursorCol

        let fg = resolvedFG(cell: cell, isCursor: isCursor)
        let bg = resolvedBG(cell: cell, isCursor: isCursor)

        let charWidth = CGFloat(fontSize) * 0.6
        let text = Text(String(cell.character))
            .font(cell.attrs.bold ? font.bold() : font)
            .foregroundColor(fg)
            .background(bg)
            .frame(width: charWidth, alignment: .leading)

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

// MARK: - Hidden Keyboard Input

#if os(iOS)

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
            /* Keep a character so backspace has something to delete */
            DispatchQueue.main.async {
                textField.text = " "
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

// MARK: - Accessory Key Bar (iOS)

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

#elseif os(macOS)

/// An invisible NSView that captures all keyboard input on macOS.
struct KeyboardInputView: NSViewRepresentable {
    var onKeyPress: (String) -> Void
    @Binding var ctrlPressed: Bool

    func makeNSView(context: Context) -> KeyCaptureView {
        let view = KeyCaptureView()
        view.onKeyPress = onKeyPress

        NotificationCenter.default.addObserver(
            context.coordinator,
            selector: #selector(Coordinator.focusKeyboard(_:)),
            name: .terminalFocusKeyboard,
            object: nil)
        context.coordinator.view = view

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
            view.window?.makeFirstResponder(view)
        }
        return view
    }

    func updateNSView(_ nsView: KeyCaptureView, context: Context) {
        nsView.onKeyPress = onKeyPress
        context.coordinator.view = nsView
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    class Coordinator: NSObject {
        var view: KeyCaptureView?

        @objc func focusKeyboard(_ notification: Notification) {
            if let v = view {
                v.window?.makeFirstResponder(v)
            }
        }

        deinit {
            NotificationCenter.default.removeObserver(self)
        }
    }
}

/// NSView subclass that handles ALL keyboard input via keyDown.
class KeyCaptureView: NSView {
    var onKeyPress: ((String) -> Void)?

    override var acceptsFirstResponder: Bool { true }
    override var canBecomeKeyView: Bool { true }

    override func keyDown(with event: NSEvent) {
        let chars = event.charactersIgnoringModifiers ?? ""
        let flags = event.modifierFlags

        /* Handle special keys */
        switch event.keyCode {
        case 126: onKeyPress?("\u{1B}[A"); return  /* up */
        case 125: onKeyPress?("\u{1B}[B"); return  /* down */
        case 124: onKeyPress?("\u{1B}[C"); return  /* right */
        case 123: onKeyPress?("\u{1B}[D"); return  /* left */
        case 53:  onKeyPress?("\u{1B}"); return     /* escape */
        case 48:  onKeyPress?("\t"); return          /* tab */
        case 51:  onKeyPress?("\u{7F}"); return      /* backspace (DEL) */
        case 36:  onKeyPress?("\n"); return           /* return */
        default: break
        }

        /* Ctrl+key → control character */
        if flags.contains(.control), let ch = chars.uppercased().unicodeScalars.first,
           ch.value >= 0x40, ch.value <= 0x5F {
            let ctrl = String(UnicodeScalar(ch.value - 0x40)!)
            onKeyPress?(ctrl)
            return
        }

        /* Regular characters */
        if let characters = event.characters, !characters.isEmpty {
            onKeyPress?(characters)
        }
    }

    /* Suppress the beep for unhandled keys */
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        return false
    }
}

/// No accessory key bar on macOS; the physical keyboard suffices.
struct AccessoryKeyBar: View {
    @Binding var ctrlPressed: Bool
    var onKey: (String) -> Void

    var body: some View {
        EmptyView()
    }
}

#endif

// MARK: - Notification Names

extension Notification.Name {
    static let terminalFocusKeyboard = Notification.Name("terminalFocusKeyboard")
    static let terminalSpecialKey = Notification.Name("terminalSpecialKey")
}
