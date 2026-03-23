# monbsd

**monbsd** is a lightweight, terminal-based system monitor specifically designed for FreeBSD. It provides real-time insights into your hardware and operating system status, including CPU/GPU performance, network traffic, disk usage, and power management.

Originally developed as a single-file utility, `monbsd` is currently being restructured into a modern, modular engine (v0.2.0+) to enhance maintainability and extensibility.

---

## 🚀 Features

- **CPU Monitoring:** Frequency tracking (Live & Base), core count, usage statistics, and thermal data.
- **GPU Hardware:** Support for NVIDIA and Intel/integrated GPUs, tracking frequencies and temperatures.
- **Network Stats:** Real-time RX/TX rates, total throughput, and IP address identification for multiple interfaces (WiFi & Ethernet).
- **Disk & Storage:** Mount point usage tracking and SWAP monitoring.
- **Power & ACPI:** Battery life/state, power source detection, and ACPI cooling states.
- **Hardware Bus:** Low-level PCI device scanning via direct motherboard communication.
- **Software Info:** Quick overview of installed `pkg` packages, Ports, and Linux compatibility binaries.

---

## 🏗️ Architecture (v0.2.0)

The project follows a "Pillar" architecture to separate concerns:

- **The Brain (`include/monbsd.h`):** Defines the `system_state` struct, a central cabinet for all collected data.
- **Hardware Pillar (`src/hw_pci.c`):** Handles raw communication with hardware (e.g., PCI bus scanning via `/dev/io`).
- **OS Pillar (`src/sensors_os.c`):** Gathers information from the FreeBSD kernel using `sysctl` and system calls.
- **Core Engine (`src/main.c`):** Orchestrates data collection and handles the user interface.

---

## 🛠️ Building & Installation

### Prerequisites

You must be running **FreeBSD**. Building requires standard development tools (`cc`, `make`).

### Build

To compile the project:

```bash
make
```

### Run

Because `monbsd` accesses raw hardware memory (e.g., `/dev/io`, `/dev/cpuctl`), it requires root privileges:

```bash
sudo ./monbsd
```

### Installation

To install `monbsd` to your system with the necessary `setuid` permissions for hardware access:

```bash
sudo make install
```

---

## 📂 Project Structure

- `src/`: Core source files for the modular engine.
- `include/`: Header files defining the data structures and prototypes.
- `mon_old/`: The original stable single-file implementation (v0.1.0) and its legacy tests.
- `docs/`: Additional documentation and man pages.
- `tests/`: Modern test suite (under development).

---

## 📜 License

This project is licensed under the terms found in the `LICENSE` file.
