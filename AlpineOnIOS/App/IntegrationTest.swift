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

/// Headless integration test triggered by --integration-test.
/// Initializes the emulator, spawns a shell, writes commands
/// to the terminal fd and verifies the output.  Exits with
/// code 0 on success, 1 on failure.
enum IntegrationTest {

    static func run(app: AlpineOnIOSApp) {
        let rootfs = Bundle.main.bundlePath + "/alpine"
        let docs = FileManager.default.urls(
            for: .documentDirectory, in: .userDomainMask).first!
        let overlay = docs.appendingPathComponent("alpine").path

        log("rootfs=\(rootfs)")
        log("overlay=\(overlay)")

        let fm = FileManager.default
        guard fm.fileExists(atPath: rootfs + "/bin/busybox") else {
            fail("rootfs not found at \(rootfs)/bin/busybox")
        }

        try? fm.createDirectory(atPath: overlay,
            withIntermediateDirectories: true, attributes: nil)
        app.createBusyboxSymlinks(rootfs: overlay)

        /* Initialize emulator */
        guard rootfs.withCString({ emu_init($0) }) == 0 else {
            fail("emu_init: \(String(cString: emu_last_error()))")
        }
        overlay.withCString { emu_set_overlay($0) }

        /* Spawn shell */
        var fd: Int32 = -1
        let pid = spawnShell(fd: &fd)
        guard pid >= 0 else {
            fail("emu_spawn: \(String(cString: emu_last_error()))")
        }
        log("spawned pid=\(pid) fd=\(fd)")

        /* Start emu_run on a background thread (polls pid 1) */
        DispatchQueue.global(qos: .background).async { emu_run() }

        /* Wait for shell to start and drain initial output */
        Thread.sleep(forTimeInterval: 3.0)
        let banner = drain(fd: fd, timeout: 2000)
        log("banner: \(escape(banner))")

        /* Test 1: echo */
        log("--- test: echo ---")
        send(fd: fd, command: "echo SELFTEST_OK\n")
        Thread.sleep(forTimeInterval: 2.0)
        let echoOut = drain(fd: fd, timeout: 2000)
        log("echo output: \(escape(echoOut))")
        guard echoOut.contains("SELFTEST_OK") else {
            fail("echo: expected SELFTEST_OK in output")
        }
        log("echo: PASS")

        /* Test 2: ls / (exercises fork+execve) */
        log("--- test: ls / ---")
        send(fd: fd, command: "ls /\n")
        Thread.sleep(forTimeInterval: 5.0)
        let lsOut = drain(fd: fd, timeout: 3000)
        log("ls output: \(escape(lsOut))")
        guard lsOut.contains("bin") else {
            fail("ls /: expected 'bin' in output")
        }
        log("ls /: PASS")

        log("ALL TESTS PASSED")
        exit(0)
    }

    // MARK: - Helpers

    private static func spawnShell(fd: inout Int32) -> Int32 {
        let path = "/bin/busybox"
        let argv = ["sh"]
        let envp = [
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:" +
                "/usr/bin:/sbin:/bin",
            "HOME=/root",
            "TERM=dumb",
            "USER=root",
            "SHELL=/bin/sh",
        ]

        let cArgv = UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>
            .allocate(capacity: argv.count + 1)
        for (i, s) in argv.enumerated() {
            cArgv[i] = strdup(s)
        }
        cArgv[argv.count] = nil

        let cEnvp = UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>
            .allocate(capacity: envp.count + 1)
        for (i, s) in envp.enumerated() {
            cEnvp[i] = strdup(s)
        }
        cEnvp[envp.count] = nil

        let result = path.withCString { cPath in
            cArgv.withMemoryRebound(
                to: UnsafePointer<CChar>?.self,
                capacity: argv.count + 1
            ) { avp in
                cEnvp.withMemoryRebound(
                    to: UnsafePointer<CChar>?.self,
                    capacity: envp.count + 1
                ) { evp in
                    emu_spawn(cPath, avp, evp, &fd)
                }
            }
        }

        for i in 0..<argv.count { free(cArgv[i]) }
        cArgv.deallocate()
        for i in 0..<envp.count { free(cEnvp[i]) }
        cEnvp.deallocate()

        return result
    }

    private static func send(fd: Int32, command: String) {
        command.utf8CString.withUnsafeBufferPointer { buf in
            _ = write(fd, buf.baseAddress!, command.utf8.count)
        }
    }

    private static func drain(fd: Int32, timeout: Int32) -> String {
        var buf = [UInt8](repeating: 0, count: 8192)
        var result = Data()
        var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)

        while poll(&pfd, 1, timeout) > 0 {
            let n = read(fd, &buf, buf.count)
            if n <= 0 { break }
            result.append(contentsOf: buf[..<n])
            /* Short timeout for subsequent reads so we don't
             * block forever waiting for more data that won't
             * come.  The first poll uses the caller's timeout
             * to wait for the shell to produce output. */
            pfd.revents = 0
        }
        return String(data: result, encoding: .utf8) ?? ""
    }

    private static func escape(_ s: String) -> String {
        s.replacingOccurrences(of: "\n", with: "\\n")
         .replacingOccurrences(of: "\r", with: "\\r")
         .replacingOccurrences(of: "\u{1B}", with: "\\e")
    }

    private static func log(_ msg: String) {
        print("INTEGRATION_TEST: \(msg)")
        fflush(stdout)
    }

    private static func fail(_ msg: String) -> Never {
        log("FAIL: \(msg)")
        exit(1)
    }
}
