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

## License

This project is licensed under the 2-Clause BSD License. See the [LICENSE](LICENSE) file for details.

## Version

v0.1.0
