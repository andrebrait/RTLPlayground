# Namespace bench

Runs the Linux daemon (`make PLATFORM=linux`) in network namespace `sw` with six veth ports
`lan1`..`lan6`. Each peer `h<N>` lives in namespace `host<N>` with `192.0.2.1<N>/24`
(TEST-NET-1). Needs root, and `iproute2 busybox ethtool curl` besides the build dependencies.

    sudo platform/linux/bench/run.sh      # every t_*.sh on a fresh bench: `ok <name>` / `FAIL <name>`, exit 1 on any failure
    docker run --rm --privileged -v "$PWD":/w -w /w rtlplayground-dev platform/linux/bench/run.sh

- `setup.sh` / `teardown.sh`: create / remove the namespaces (teardown also stops the daemon; idempotent, only touches `sw`, `host1..6`).
- `lib.sh`: helpers for tests (`sw_start`, `sw_cmd`, `sw_get`, `in_host`, `assert_eq`, `assert_contains`).
  Until the CLI socket exists, `sw_cmd` types into the daemon's `--console` over a FIFO pair and reads up to the next prompt.
- A test is a `t_*.sh` that sources `lib.sh`, calls `sw_start`, and exits non-zero on failure. `RTLP_BIN` overrides the daemon path.
