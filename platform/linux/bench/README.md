# Namespace bench

Runs the Linux daemon (`make PLATFORM=linux`) in network namespace `rtlp-sw` with six veth ports
`lan1`..`lan6`. Each peer `h<N>` lives in namespace `rtlp-h<N>` with `192.0.2.1<N>/24`
(TEST-NET-1). Needs root, and `iproute2 busybox ethtool curl python3 openssl` besides the build dependencies.

    sudo platform/linux/bench/run.sh      # every t_*.sh on a fresh bench: `ok <name>` / `FAIL <name>`, exit 1 on any failure
    docker run --rm --privileged -v "$PWD":/w -w /w rtlplayground-dev platform/linux/bench/run.sh

- `setup.sh` / `teardown.sh`: create / remove the namespaces (teardown also stops the daemon; idempotent; deletes only the namespaces `setup.sh` recorded in `$RUN/owner`, and `--force` also the `rtlp-*` names of a stale bench; other namespaces are never touched).
- `lib.sh`: helpers for tests (`sw_start`, `sw_cmd`, `sw_console`, `sw_get`, `in_host`, `assert_eq`, `assert_contains`).
  `sw_cmd` runs a command through the CLI socket with `swcli`; `sw_console` types it into the daemon's `--console` over a FIFO pair and reads up to the next prompt. `sw_get <path>` fetches a page from the web UI inside `rtlp-sw` (127.0.0.1:80), logging in with `$SW_PASS` first. The daemon's stderr is `$RUN/daemon.log`.
- A test is a `t_*.sh` that sources `lib.sh`, calls `sw_start`, and exits non-zero on failure. `RTLP_BIN` overrides the daemon path.
