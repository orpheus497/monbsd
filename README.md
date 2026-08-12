# monbsd - FreeBSD System Monitor

`monbsd` is a lightweight, terminal-based system monitor specifically designed for FreeBSD laptops. It provides granular monitoring of system health, with a strong focus on thermal performance, power management, and hardware status.

## Features

- **Granular Thermal Monitoring:** Real-time CPU and GPU temperature tracking.
- **Power Management:** Displays ACPI states (Cx usage), battery status, and `powerd`/`powerdxx` status.
- **CPU & GPU Insights:** Live frequency tracking, core counts, and GPU model detection.
- **Network & Storage:** Monitor network interface traffic (RX/TX rates) and disk usage across common FreeBSD mount points.
- **Software Inventory:** Counts of `pkg` packages, ports, and Linux compatibility binaries.
- **Laptop Centric:** Designed to fit nicely on laptop screens with auto-resizing support.

## Prerequisites

- **FreeBSD:** Developed and tested on FreeBSD.
- **Architecture:** Full telemetry (MSR CPU temperatures, APERF/MPERF live frequency, direct PCI device counting) requires **amd64 or i386**. Other architectures build and run with reduced telemetry.
- **Permissions:** For full telemetry, `monbsd` opens `/dev/cpuctl0` and `/dev/io` at startup. It is recommended to install it with setuid root (`make install`) or run it with `sudo`. Privileges are permanently dropped before the UI starts; only the two pre-opened, close-on-exec device descriptors are retained.
- **Kernel module:** MSR CPU temperatures and APERF/MPERF live frequency require the `cpuctl` module — `kldload cpuctl`, or `cpuctl_load="YES"` in `/boot/loader.conf`. Without it, ACPI/sysctl fallbacks are used.

Without `/dev/cpuctl0` / `/dev/io` access, `monbsd` still runs with reduced telemetry:

| Probe | Without device access |
|-------|-----------------------|
| CPU temperature | ACPI thermal zone sysctl (or "N/A" if none) |
| Live frequency | `dev.cpu.0.freq` |
| PCI device count | enumerated via `pciconf -l` (unprivileged, all architectures) |
| Everything else | unchanged |

## Installation

To build and install `monbsd` as a system command:

```bash
make
sudo make install
```

This installs the binary to `/usr/local/bin/monbsd` setuid root (needed only to open `/dev/cpuctl0` and `/dev/io` at startup; privileges are permanently dropped before the UI starts).

For a per-user install without setuid (reduced telemetry):

```bash
make install-user
```

This installs to `~/.local/bin/monbsd`. Remove it with `make uninstall-user`.

## Usage

Simply run:

```bash
monbsd
```

Press `q` to exit.

## Configuration

`monbsd` is currently zero-config. It automatically detects hardware and network interfaces.

## Notes

- **"Ports" count:** packages whose `pkg` repository name is exactly `local` (ports-tree installs). Packages built by poudriere/synth carry the builder's repository name and are not counted. pkg data refreshes about every 60 seconds; a failed query keeps the last known value, showing "N/A" until a query succeeds.
- **powerd/powerdxx:** detected by an exact-name process scan via the `kern.proc` sysctl. Non-root runs need the default `security.bsd.see_other_uids=1` setting to see root-owned daemons.

## Development

```bash
make tests   # build the test programs
make check   # run tests; hardware tests SKIP (exit 77) without /dev access
make bench   # build the benchmarks
```

`make check` is an x86 (amd64/i386) target: `test_compile`, `test_cpuid`, `test_cpuctl`, `test_aperf`, and `test_pci` exercise x86-specific interfaces.

## License

This project is licensed under the 2-Clause BSD License. See the [LICENSE](LICENSE) file for details.

## Version

v0.1.2
