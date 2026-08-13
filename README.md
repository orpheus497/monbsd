# monbsd - FreeBSD System Monitor

`monbsd` is a lightweight, terminal-based system monitor for FreeBSD laptops. It focuses on thermal performance, power management, and core hardware status in a compact terminal layout.

## Features

- **Thermal Monitoring:** Real-time CPU temperature tracking, plus GPU telemetry where supported.
- **Power Management:** Displays ACPI Cx state information, battery status, fan status, and `powerd` / `powerdxx` process detection.
- **CPU & GPU Insights:** Live CPU frequency tracking, logical core count, GPU model detection, and per-GPU telemetry for supported devices.
- **Network & Storage:** Monitors active network interfaces with RX/TX rates and shows disk usage for selected FreeBSD mount points.
- **Software Inventory:** Counts installed `pkg` packages, ports-tree packages, Linux compatibility binaries, and user-local executables.
- **Laptop Centric:** Automatically resizes to the terminal and keeps the layout readable on smaller screens.

## Prerequisites

- **FreeBSD:** Developed and tested on FreeBSD.
- **FreeBSD release:** FreeBSD 14.0 or later is required because startup uses `clearenv(3)`.
- **Architecture:** Full telemetry (MSR CPU temperatures, APERF/MPERF live frequency, direct PCI device counting) requires **amd64 or i386**. Other architectures build and run with reduced telemetry.
- **Permissions:** For full telemetry, `monbsd` opens `/dev/cpuctl0` and `/dev/io` at startup. It is recommended to install it with setuid root (`make install`) or run it with `sudo`. Privileges are permanently dropped before the UI starts; only the two pre-opened, close-on-exec device descriptors are retained.
- **Kernel module:** MSR CPU temperatures and APERF/MPERF live frequency require the `cpuctl` module - `kldload cpuctl`, or `cpuctl_load="YES"` in `/boot/loader.conf`. Without it, ACPI/sysctl fallbacks are used.

Without `/dev/cpuctl0` / `/dev/io` access, `monbsd` still runs with reduced telemetry:

| Probe | Without device access |
|-------|-----------------------|
| CPU temperature | ACPI thermal zone sysctl (or `N/A` if none) |
| Live frequency | `dev.cpu.0.freq` |
| PCI device count | enumerated via `pciconf -l` |
| Everything else | unchanged |

## Installation

To build and install `monbsd` as a system command:

```bash
make
sudo make install
```

This installs the binary to `/usr/local/bin/monbsd` setuid root. That privilege is only needed to open `/dev/cpuctl0` and `/dev/io` at startup; the program drops privileges before the UI starts.

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

`monbsd` is currently zero-config. It automatically detects hardware and active network interfaces.

## Notes

- **"Ports" count:** packages whose `pkg` repository name is exactly `local` are counted as ports-tree installs. Packages built by poudriere/synth carry the builder's repository name and are not counted. `pkg` data refreshes about every 60 seconds; a failed query keeps the last known value, showing `N/A` until a query succeeds.
- **Power daemons:** `powerd` and `powerdxx` are detected by an exact-name process scan via the `kern.proc` sysctl. Non-root runs need the default `security.bsd.see_other_uids=1` setting to see root-owned daemons.
- **GPU telemetry:** The program enumerates GPUs from `pciconf -lv`. NVIDIA devices get temperature, utilization, and VRAM data through `nvidia-smi` when available. Non-NVIDIA GPUs currently expose model detection and DRM frequency sysctls where supported.

## Development

```bash
make tests   # build the test programs
make check   # run tests; hardware tests SKIP (exit 77) without /dev access
make bench   # build the benchmarks
```

`make check` includes x86-specific tests (`test_compile`, `test_cpuid`, `test_cpuctl`, `test_aperf`, and `test_pci`) that exercise the privileged telemetry paths.

## License

This project is licensed under the 2-Clause BSD License. See the [LICENSE](LICENSE) file for details.

## Version

v0.1.2
