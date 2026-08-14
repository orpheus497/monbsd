# Changelog

All notable changes to `monbsd` will be documented in this file.

## [Unreleased]

### Documentation
- Added a file-level architecture/privilege-model/memory-model overview and section banners
  throughout `src/monbsd.c`, plus doc comments on every non-trivial function and on the
  `struct mon_data` / history ring-buffer data structures.
- Documented the privilege-dropping and setuid-root security model in `README.md`.

### Fixed
- Guarded memory-usage percentage against a divide-by-zero (and resulting NaN display) when the
  `hw.physmem` sysctl fails and `mem_total` is 0.
- Guarded per-disk usage percentage against a divide-by-zero when a filesystem reports 0 total
  blocks.
- Widened the active+wired page-count sum to `long long` before multiplying by the page size, to
  avoid a theoretical 32-bit unsigned integer wraparound when computing `mem_used` on systems
  with very large memory.

## [0.1.0] - 2026-03-11

### Initial Release
- Initial implementation of `monbsd` system monitor.
- CPU/GPU frequency and temperature tracking.
- Network and disk usage monitoring.
- ACPI and battery status tracking.
- Terminal-based UI with resizing support.
- Project restructuring for public release.
- Makefile for easy building and installation.
