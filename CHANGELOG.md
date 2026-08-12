# Changelog

All notable changes to `monbsd` will be documented in this file.

## [0.1.2] - 2026-08-12

Regression restoration release. IDs reference PLAN.md (R = regression,
F = fix, H = hygiene).

### Fixed
- **R1/F2:** PCI device count no longer reports "N/A" for non-root runs:
  when the direct `/dev/io` configuration-space scan is unavailable the
  count falls back to `pciconf -l`, which works for any user on any
  FreeBSD architecture.
- **R2/F1:** Ports count stuck at 0: current pkg rejects the bare `%r`
  query format, and the failed query silently overwrote the count. The
  repository name is now queried as `%rn` (with a legacy `%r` retry),
  results are only stored when `pkg` exits successfully, and both pkg
  counters render "N/A" until a query succeeds.
- **R3a/F3a:** powerd/powerdxx always showed "Stopped": detection read
  `/var/run/*.pid` (mode 0600, root-owned), unreadable after the privilege
  drop. Reverted to an exact-name `kern.proc` sysctl scan — no subprocess,
  unprivileged, immune to PID recycling.
- **F3b:** CPU temperature renders "N/A" instead of "-1.0 °C" when neither
  MSR (`cpuctl`) nor ACPI thermal zone telemetry exists.
- **F3c:** Battery charge reads are validated (a failed
  `hw.acpi.battery.life` read no longer shows a stale/0% bar); hosts
  without an ACPI battery report "No battery" and hide the charge bar.
- **H2:** nvidia-smi, pciconf (GPU scan) and pkg probe results are only
  committed when the subprocess exits successfully.

### Changed
- pkg/ports refresh cadence is ~60 s (600 ticks) instead of ~5 min.
- The compiled `monbsd` binary is no longer tracked in git; build
  artifacts are covered by `.gitignore`.

## [0.1.1] - 2026-08-12

Code review remediation release. IDs reference the v0.1.0 review findings
(S/C/L) and newly discovered issues (N) in PLAN.md.

### Security
- **S1/N2:** Root privileges are no longer held for the process lifetime.
  `/dev/cpuctl0` and `/dev/io` are opened once at startup with `O_CLOEXEC`
  (so `fork()`+`execv()` children never inherit them), then privileges are
  permanently dropped via `setresgid()`/`setresuid()` before the UI starts;
  failure to drop is fatal (fail closed). MSR reads and PCI config-space
  access continue to work through the cached descriptors.
- **S3/N1:** All system-provided strings shown in the UI (hostname, CPU
  model, GPU model, Cx states, frequency levels, WiFi SSID) are sanitized of
  terminal control characters.

### Fixed
- **N1/L3:** Tree did not compile (missing `sanitize_str()`); completed the
  half-merged SSID feature using the native `<net80211/ieee80211_ioctl.h>`.
- **S2:** SIGINT/SIGTERM/SIGHUP now restore the terminal and exit 128+signo.
- **C1:** "Cores" actually reported logical threads; now sourced from
  `kern.smp.cpus` and honestly labeled "Threads".
- **C2:** `nvidia-smi` ran synchronously in the render loop; it now runs on
  a detached thread (same pattern as the pkg probe), eliminating UI stalls,
  and GPU fields persist between probes.
- **C3:** Frequency levels no longer show the "3600/70000 MHz" power suffix.
- **C4:** Header box is now correctly centered (26 columns, not 24).
- **C5/L5:** Duplicate disk targets are deduplicated; filesystems with zero
  blocks no longer cause a divide-by-zero.
- **C6:** `hw.acpi.battery.state` is decoded as a bitmask
  (discharging/charging/critical) instead of the inverted `== 7` test.
- **C7:** Ports count now matches the repository name "local" exactly
  instead of substring matching.
- **C8/N3:** `print_val()` no longer emits negative printf precision at
  narrow widths; truncation degrades cleanly.
- **N4:** PCI device count renders "N/A" when unavailable instead of
  "-1 devices".
- **L1:** Refuses to run when stdin/stdout are not terminals.
- **L2:** x86-only code paths (CPUID, RDMSR, inl/outl PCI scan) are guarded
  by `MONBSD_X86`; other architectures build and run with reduced telemetry.
- **L6:** GNU-specific pattern rule replaced with a portable `.c:` suffix
  rule (works with both bmake and GNU make).
- **L4:** New `make bench`, `make check` (exit-77 skip convention in the
  hardware tests), and `make install-user` targets.

### Changed
- UI label "Cores:" is now "Threads:" (value semantics are now honest).

## [0.1.0] - 2026-03-11

### Initial Release
- Initial implementation of `monbsd` system monitor.
- CPU/GPU frequency and temperature tracking.
- Network and disk usage monitoring.
- ACPI and battery status tracking.
- Terminal-based UI with resizing support.
- Project restructuring for public release.
- Makefile for easy building and installation.
