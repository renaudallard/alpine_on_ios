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

/// Swift bridge to the C emulator library.
class EmulatorBridge: ObservableObject {
    @Published var pid: Int = -1
    @Published var isRunning: Bool = false

    private var termFD: Int32 = -1
    private var readThread: Thread?

    // MARK: - Initialization

    /// Initialize the emulator with the given rootfs path.
    /// Returns 0 on success, -1 on failure.
    func initializeEmulator(rootfsPath: String) -> Int32 {
        return rootfsPath.withCString { path in
            emu_init(path)
        }
    }

    /// Start the emulator event loop on a background thread.
    func startEmulator() {
        isRunning = true
        Thread.detachNewThread {
            emu_run()
            DispatchQueue.main.async {
                self.isRunning = false
            }
        }
    }

    // MARK: - Process management

    /// Spawn /bin/sh as the initial shell process.
    /// Returns pid on success, -1 on failure.
    @discardableResult
    func spawnShell() -> Int {
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
            pid = Int(result)
            termFD = fd
        }
        return Int(result)
    }

    /// Spawn X server with a window manager for graphical mode.
    func spawnGraphical() {
        let path = "/bin/sh"
        let argv = ["/bin/sh", "-c",
            "mkdir -p /tmp/xdg; export XDG_RUNTIME_DIR=/tmp/xdg; " +
            "X :0 -config /etc/X11/xorg.conf & sleep 1; " +
            "export DISPLAY=:0; " +
            "if command -v openbox >/dev/null; then openbox & " +
            "elif command -v twm >/dev/null; then twm & fi; " +
            "xterm || sh"]
        let envp = [
            "HOME=/root",
            "TERM=xterm-256color",
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            "USER=root",
            "SHELL=/bin/sh",
            "DISPLAY=:0",
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
            pid = Int(result)
            termFD = fd
        }
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

    /// Start reading from the terminal fd on a background thread.
    /// Calls the callback with received data on each read.
    func startReading(callback: @escaping (Data) -> Void) {
        guard termFD >= 0 else { return }

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

    // MARK: - Terminal control

    /// Set the terminal window size.
    func setWindowSize(rows: Int, cols: Int) {
        guard pid >= 0 else { return }
        emu_set_winsize(Int32(pid), UInt16(rows), UInt16(cols))
    }

    /// Send a signal to the emulated process.
    func sendSignal(sig: Int32) {
        guard pid >= 0 else { return }
        emu_kill(Int32(pid), sig)
    }

    // MARK: - Cleanup

    deinit {
        if termFD >= 0 {
            close(termFD)
        }
        emu_shutdown()
    }

    // MARK: - C String Helpers

    /// Convert an array of Swift strings to a NULL-terminated C string array,
    /// calling the closure with the result.
    private func withCStrings<R>(
        _ strings: [String],
        body: (UnsafeMutablePointer<UnsafePointer<CChar>?>) -> R
    ) -> R {
        /* Allocate array of C string pointers + NULL terminator. */
        var cStrings = strings.map { strdup($0) }
        cStrings.append(nil)

        defer {
            for ptr in cStrings {
                free(UnsafeMutablePointer(mutating: ptr))
            }
        }

        let count = cStrings.count
        let cArray = UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>.allocate(capacity: count)
        for i in 0..<count {
            cArray[i] = cStrings[i]
        }
        defer { cArray.deallocate() }

        return cArray.withMemoryRebound(
            to: UnsafePointer<CChar>?.self,
            capacity: count
        ) { ptr in
            body(ptr)
        }
    }
}
