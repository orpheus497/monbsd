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
- **Permissions:** `monbsd` requires access to `/dev/cpuctl` and `/dev/io` for direct hardware monitoring. It is recommended to install it with setuid root or run it with `sudo`.

## Installation

To build and install `monbsd` as a system command:

```bash
make
sudo make install
```

This will install the binary to `/usr/local/bin/monbsd` with the necessary permissions.

## Usage

Simply run:

```bash
monbsd
```

Press `q` to exit.

## Configuration

`monbsd` is currently zero-config. It automatically detects hardware and network interfaces.

## Architecture & Security Model

`monbsd` is a single-source-file (`src/monbsd.c`) program, organized top to bottom into clearly
commented sections: terminal control, direct hardware access, subprocess helpers, the background
package-count thread, data gathering, and rendering. See the file header comment in
`src/monbsd.c` for a full breakdown.

Because `monbsd` reads CPU MSRs and PCI configuration space directly, it is normally installed
setuid root, and it runs with an elevated effective UID for its entire lifetime — there is no
global privilege drop at startup. Two narrower mechanisms limit the exposure that comes with
that instead:

- Any operation that touches the invoking user's files (e.g. counting executables in
  `~/.local/bin`) temporarily drops the effective UID back to the real UID for the duration of
  that scan only, then restores it (or exits, rather than continuing at the wrong privilege
  level, if the restore fails).
- All subprocesses (`pkg`, `pciconf`, `swapinfo`, `nvidia-smi`) are launched with `fork`/`execv`
  directly (never a shell), and the *forked child* permanently drops both UID and GID to the
  real, unprivileged user before `exec` — this does not affect the main process's own elevated
  UID.

All string handling uses fixed-size buffers filled via `strlcpy()`/`snprintf()` (never
`sprintf`/`strcpy`), and the only heap allocations on the data path are short-lived and freed on
every code path (see the "Memory model" note in `src/monbsd.c`).

## License

This project is licensed under the 2-Clause BSD License. See the [LICENSE](LICENSE) file for details.

## Version

v0.1.0
