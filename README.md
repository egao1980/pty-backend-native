# pty-backend-native

[`pty-protocol`](https://github.com/egao1980/pty-protocol) backend over `libpty-cl`, a small C library with one ABI on every OS. Loading it sets `*pty-backend*` unless a backend is already selected.

| OS | How |
|----|-----|
| Linux, macOS | `posix_openpt`, then `fork`. The child runs only async-signal-safe C: reset signals, `setsid`, open the slave, `TIOCSCTTY`, set the window size, `dup2`, `chdir`, a `PATH` walk over a buffer built before the fork, then `execve`. Exec failures come back as errno over a `CLOEXEC` pipe. Reads use `poll`, or `select` on macOS, where `poll` rejects ttys. |
| Windows 10 1809+ | ConPTY (`CreatePseudoConsole`, loaded from kernel32) with `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`. Arguments are quoted per `CommandLineToArgvW`. `ClosePseudoConsole` runs on its own thread, because it can block until the last output is read. `:int` sends Ctrl-C; `:term` and `:kill` terminate. |

No Lisp code runs in a forked child.

The child gets `STARTF_USESTDHANDLES` with invalid std handles, so it never inherits a redirected parent's handles; its console output always goes to the pseudo console.

## Overlays

`linux/amd64` and `linux/arm64` (glibc ≥ 2.31) get `libpty-cl.so`, `darwin/arm64` gets `libpty-cl.dylib`, and `windows/amd64` gets `pty-cl.dll`. `scripts/build-pty-cl.sh [os-arch …]` builds them with `zig cc`, which cross-compiles every target from one host. `PTY_CL_NATIVE=/dir` points the loader elsewhere.

## License

MIT
