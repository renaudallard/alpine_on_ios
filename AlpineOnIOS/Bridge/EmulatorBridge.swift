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

enum EmulatorState: Equatable {
    case idle
    case extracting
    case initializing
    case spawning
    case running
    case error(String)
}

/// Swift bridge to the C emulator library.
class EmulatorBridge: ObservableObject {
    @Published var state: EmulatorState = .idle
    @Published var pid: Int = -1

    private(set) var termFD: Int32 = -1
    private var readThread: Thread?
    private var readCallback: ((Data) -> Void)?

    // MARK: - Full startup sequence

    /// Run the complete startup on a background thread with status updates.
    func startAll(rootfsPath: String) {
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            /* Step 1: Initialize emulator */
            updateState(.initializing)
            let rc = rootfsPath.withCString { emu_init($0) }
            if rc != 0 {
                updateState(.error("emu_init failed (\(rc))"))
                return
            }

            /* Step 2: Spawn shell */
            updateState(.spawning)
            let spawnResult = doSpawnShell()
            if spawnResult < 0 {
                updateState(.error("emu_spawn failed (\(spawnResult))"))
                return
            }

            /* Step 3: Start reader thread (if callback already registered) */
            updateState(.running)
            if let cb = readCallback {
                startReaderThread(callback: cb)
            }

            /* Step 4: Run emulator loop */
            emu_run()
            updateState(.idle)
        }
    }

    private func updateState(_ newState: EmulatorState) {
        DispatchQueue.main.async {
            self.state = newState
        }
        /* Small delay to let UI update */
        usleep(50000)
    }

    // MARK: - Process management

    private func doSpawnShell() -> Int {
        let path = "/bin/sh"
        let argv = ["/bin/sh", "-l"]
        let envp = [
            "HOME=/root",
            "TERM=xterm-256color",
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            "USER=root",
            "SHELL=/bin/sh",
            "PS1=alpine:\\w\\$ ",
        ]

        var fd: Int32 = -1
        let result = withCStrings(argv) { cArgv in
            withCStrings(envp) { cEnvp in
                path.withCString { cPath in
                    emu_spawn(cPath, cArgv, cEnvp, &fd)
                }
            }
        }

        if result >= 0 {
            DispatchQueue.main.async {
                self.pid = Int(result)
            }
            termFD = fd
        }
        return Int(result)
    }

    // MARK: - I/O

    /// Write data to the terminal file descriptor.
    func write(data: Data) {
        guard termFD >= 0 else { return }
        data.withUnsafeBytes { buf in
            if let ptr = buf.baseAddress {
                _ = Darwin.write(termFD, ptr, buf.count)
            }
        }
    }

    /// Register a read callback. If the emulator is already running,
    /// starts the reader immediately. Otherwise, it will start when
    /// the emulator reaches the running state.
    func startReading(callback: @escaping (Data) -> Void) {
        readCallback = callback
        if state == .running && termFD >= 0 {
            startReaderThread(callback: callback)
        }
    }

    private func startReaderThread(callback: @escaping (Data) -> Void) {
        guard termFD >= 0, readThread == nil else { return }

        let fd = termFD
        let thread = Thread {
            let bufSize = 4096
            let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: bufSize)
            defer { buf.deallocate() }

            while true {
                let n = read(fd, buf, bufSize)
                if n <= 0 { break }
                let data = Data(bytes: buf, count: n)
                callback(data)
            }
        }
        thread.name = "TerminalReader"
        thread.qualityOfService = .userInteractive
        thread.start()
        readThread = thread
    }

    // MARK: - Control

    func setWindowSize(rows: Int, cols: Int) {
        if pid > 0 {
            emu_set_winsize(Int32(pid), UInt16(rows), UInt16(cols))
        }
    }

    func sendSignal(sig: Int32) {
        if pid > 0 {
            emu_kill(Int32(pid), sig)
        }
    }

    deinit {
        if termFD >= 0 {
            close(termFD)
        }
        emu_shutdown()
    }

    // MARK: - C String Helpers

    private func withCStrings<R>(
        _ strings: [String],
        body: (UnsafeMutablePointer<UnsafePointer<CChar>?>) -> R
    ) -> R {
        var cStrings = strings.map { strdup($0) }
        cStrings.append(nil)

        defer {
            for ptr in cStrings {
                free(UnsafeMutablePointer(mutating: ptr))
            }
        }

        let count = cStrings.count
        let cArray = UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>
            .allocate(capacity: count)
        for i in 0..<count {
            cArray[i] = cStrings[i]
        }
        defer { cArray.deallocate() }

        return cArray.withMemoryRebound(
            to: UnsafePointer<CChar>?.self, capacity: count
        ) { ptr in
            body(ptr)
        }
    }
}
