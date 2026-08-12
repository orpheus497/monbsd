# monbsd v0.1.0 — Code Review Remediation Plan

This plan addresses every finding from the v0.1.0 code review (S1–S3, C1–C8, L1–L7)
plus five additional issues discovered while analyzing the code against the review
(N1–N5). All line references are to `src/monbsd.c` r1092 unless noted.

---

## 0. Newly discovered issues (not in the review)

| ID | Issue | Evidence | Disposition |
|----|-------|----------|-------------|
| N1 | **Tree does not compile as-is.** `sanitize_str()` is called but never defined; `struct ieee80211req` / `IEEE80211_IOC_SSID` are used without `<net/if_ieee80211.h>`. The `monbsd` binary at repo root is stale (predates the half-merged SSID work). | `src/monbsd.c:200` (call), `:185-193` (ieee80211req use), includes at `:1-32` (no `net/if_ieee80211.h`) | Resolved by S3 (define `sanitize_str`) + L3 (add include, wire feature) |
| N2 | **Cached device fds must be `O_CLOEXEC`.** S1's open-once pattern keeps `/dev/io` and `/dev/cpuctl0` open for life; without close-on-exec, unprivileged `popen_safe()` children inherit them → I/O-port + MSR access as the invoking user (privesc). | `popen_safe()` at `:323-344` | Mandatory part of S1 design |
| N3 | `print_val()` label clamp goes negative at w=5..7: `lbl_len > w - 6` assigns `lbl_len = -1`; negative precision is ignored by `printf`, printing the full label and breaking the border. Same family as C8. | `:806` | Fixed alongside C8 |
| N4 | PCI count of `-1` (non-root / non-x86) renders as `-1 devices`. | `:466`, `:863-864` | Render "N/A" when `< 0` (folds into S1/L2 degradation paths) |
| N5 | Review claims "`exit(1)` inside `count_dir_executables()`" and "privileges temporarily dropped in `count_dir_executables()`" — neither exists in the code. No action possible/needed; S1 makes it moot. | `:296-321` | Documented; no code change |

---

## 1. Findings matrix

| ID | Finding | Fix location(s) | Effort | Risk |
|----|---------|-----------------|--------|------|
| S1 | Root held for process lifetime | `main()` `:1007`, `direct_cpu_temp()` `:225`, `direct_cpu_live_freq()` `:246`, `direct_pci_count()` `:268` | Medium | Medium (telemetry regressions if fd-caching assumption fails) |
| S2 | No SIGINT/SIGTERM/SIGHUP handlers | `main()` `:1071`, main loop `:1074` | Low | Low |
| S3 | Unsanitized system strings | new helper; call sites `:391`, `:413`, `:495`, `:497`, `:545`, `:585` | Low | Low |
| C1 | "Cores" = logical threads | `direct_cpu_cores()` `:218`, `gather_data()` `:429`, label `:852` | Low | Low |
| C2 | nvidia-smi blocks render loop | `gather_data()` `:615-648` | Medium | Medium (threading) |
| C3 | freq_levels "3600/70000 MHz" | `render_thermal_power_box()` `:944-950` | Trivial | None |
| C4 | Header off-center (24 vs 26) | `render()` `:999-1000` | Trivial | None |
| C5 | Duplicate disk rows | `gather_data()` `:778-790` | Low | None |
| C6 | Battery magic values | `gather_data()` `:536-542` | Low | None |
| C7 | Ports "local" substring | `update_pkg_counts_thread()` `:374-376` | Low | None |
| C8 | print_val negative precision | `print_val()` `:802-816` | Low | None |
| L1 | No isatty() guard | `main()` `:1059-1070` | Trivial | None |
| L2 | x86-only, no guards | includes `:23-25`, `direct_*()` `:218-294`, brand `:401-412` | Medium | Low |
| L3 | Dead/half-merged SSID | `:51-59`, `:173-203`, iface loop `:688-719`, render `:961-975` | Medium | Low |
| L4 | Makefile gaps | `Makefile` | Low | None |
| L5 | Div-by-zero f_blocks==0 | `gather_data()` `:784-786` | Trivial | None |
| L6 | GNU pattern rule vs bmake | `Makefile:20-21` | Trivial | None |
| L7 | Man/README gaps | `monbsd.8`, `README.md` | Low | None |

---

## 2. Detailed designs

### S1 — Permanent privilege drop after startup fd acquisition

**Current state.** euid root for the entire process lifetime. `/dev/cpuctl0` is opened
and closed on *every* probe in `direct_cpu_temp()` (`:226`) and `direct_cpu_live_freq()`
(`:248`); `/dev/io` likewise in `direct_pci_count()` (`:269`). All rendering/parsing code
runs as root.

**Why the cached-fd pattern is safe on FreeBSD.** Credential checks happen at `open(2)`
time (devfs node permissions; `PRIV_IO` for `/dev/io`). `CPUCTL_RDMSR` ioctls on an
already-open fd and userland `inl`/`outl` (I/O bitmap permission granted per-process at
`/dev/io` open) do not re-check credentials. This is the classic Xorg privilege-separation
pattern.

**Design.**

1. File-scope globals:
   ```c
   static int g_cpuctl_fd = -1;   /* /dev/cpuctl0, open for process lifetime */
   static int g_io_fd = -1;       /* /dev/io, open for process lifetime */
   ```
2. In `main()`, ordering is significant:
   1. Resolve home dir (unprivileged; uses real uid — unaffected by later drop). *unchanged*
   2. Validate `TERM`. *unchanged*
   3. `clearenv()` + re-inject `PATH`/`TERM`. *unchanged*
   4. **Open** `g_cpuctl_fd = open("/dev/cpuctl0", O_RDWR | O_CLOEXEC)` and
      `g_io_fd = open("/dev/io", O_RDWR | O_CLOEXEC)` while still euid root. Failure of
      either open is non-fatal (fallback paths exist) — record and continue.
   5. **Drop permanently:** if `geteuid() == 0`, then
      `setresgid(rgid, rgid, rgid)` followed by `setresuid(ruid, ruid, ruid)`;
      on failure, `fprintf(stderr, …)` + `exit(1)` (fail closed — never run the UI as root).
   6. isatty guard (L1), raw mode, signals, loop. *as before*
3. Refactor the three probes to use the cached fds:
   - `direct_cpu_temp()`: `if (g_cpuctl_fd >= 0) { ioctl(…0x1A2…); ioctl(…0x19C…); }`
     else fall through to the existing `hw.acpi.thermal.tz0.temperature` sysctl fallback.
   - `direct_cpu_live_freq()`: use `g_cpuctl_fd`; else `return fallback_freq`.
   - `direct_pci_count()`: `if (g_io_fd < 0) return -1;` then the existing scan (keep the
     fd open; remove per-call open/close).
4. `popen_safe()` keeps its child-side `setresgid/setresuid` to real ids — after S1 this is
   a harmless no-op, retained as defense-in-depth.
5. N4 follow-through: render `PCI Devices:` as "N/A" when `d->pci_device_count < 0`.

**N2 (mandatory).** Both opens use `O_CLOEXEC` so `fork()`+`execv()` children
(pkg/pciconf/swapinfo/nvidia-smi) never inherit the privileged descriptors.

**Degradation matrix (non-setuid / non-root execution).**

| Probe | Without root |
|-------|--------------|
| CPU temp | ACPI thermal zone sysctl (already present) |
| Live freq | `dev.cpu.0.freq` fallback (already present) |
| PCI count | "N/A" (new, N4) |
| Everything else | Unchanged (all sysctls/subprocesses work unprivileged) |

---

### S2 — Fatal-signal handlers with terminal restoration

Raw mode clears `ISIG` (`:165`), so Ctrl-C arrives as byte `0x03` (already handled at
`:1087`), but external `kill` bypasses `atexit(disable_raw_mode)` → user's terminal left
with echo off / cursor hidden.

**Design.**

```c
static volatile sig_atomic_t exit_signo = 0;

static void handle_exit_signal(int sig) { exit_signo = sig; }   /* async-signal-safe */
```

- Install via `sigaction()` (no `SA_RESTART`, so `usleep()`/`read()` return early) for
  `SIGINT`, `SIGTERM`, `SIGHUP` after `enable_raw_mode()` next to the existing
  `SIGWINCH` install (`:1071`).
- Loop-top check in the `while (1)` loop (`:1074`): if `exit_signo != 0` →
  `clear_screen(); exit(128 + exit_signo);`. `exit()` runs the registered
  `disable_raw_mode()` → termios restored, cursor shown. Worst-case latency one tick
  (~100 ms) since `VMIN=0/VTIME=0` keeps `read()` non-blocking.
- Exit status convention: `0` for `q`/`Q`/Ctrl-C byte; `128+signo` for signal death;
  `1` for startup failures. Documented in man page (L7).
- Detached pkg/nvidia threads require no join; `exit()` from the main thread terminates
  them. No shared state is written by the atexit handler.

---

### S3 — Sanitize system strings before terminal output (also fixes N1)

**Design.** One helper, defined before `refresh_wifi_ssid()` (`:180`) so the existing
call at `:200` compiles:

```c
/* Replace terminal-hostile control bytes (incl. ESC) with spaces.
 * Bytes >= 0x80 are left intact so valid UTF-8 survives. */
static void sanitize_str(char *s) {
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) *s = ' ';
    }
}
```

**Call sites** (review-named sources + cheap extras):

| String | Sanitize after | Line |
|--------|----------------|------|
| `d->host` | `gethostname()` | `:391` |
| `d->cpu_model` | `strlcpy` from brand/hw.model | `:413` |
| `g_cache[i].model` (pciconf parse) | `strlcpy` | `:585` |
| `d->cx_lowest` | sysctl read | `:495` |
| `d->cx_usage` | sysctl read | `:497` |
| `d->freq_levels` | sysctl read | `:545` |
| SSID output | existing call | `:200` |

Program-generated constants (`thermal_state`, `fan_status`, `bat_*`, IP addresses) are
not attacker-influenced; left as-is.

---

### C1 — Report logical CPUs as "Threads"

`direct_cpu_cores()` (`:218-223`) returns CPUID leaf-1 EBX[23:16] = addressable logical
processor IDs (2× cores on HT).

**Design.**
- Primary source: `kern.smp.cpus` sysctl (arch-neutral, exact logical CPU count):
  ```c
  int ncpu = 0; size_t sz = sizeof(ncpu);
  if (sysctlbyname("kern.smp.cpus", &ncpu, &sz, NULL, 0) != 0 || ncpu <= 0)
      ncpu = direct_cpu_cores();   /* x86 fallback; returns >= 1 */
  d->cpu_threads = ncpu;
  ```
- Rename struct field `cpu_cores` → `cpu_threads` (`:76`, uses at `:429`, `:851-852`).
- Relabel UI `"Cores:"` → `"Threads:"` (`:852`). `direct_cpu_cores()` is retained as the
  fallback, wrapped in the L2 x86 guard (non-x86 fallback is simply `1`).

### C2 — nvidia-smi on a detached thread (mirror the pkg pattern)

**Current state.** `gather_data()` runs `nvidia-smi` synchronously every 5 ticks
(`:615-648`); 100–300 ms subprocess latency stalls the 100 ms render loop.

**Design** (file-scope cache + atomics, same acquire/release idiom as pkg):

```c
struct nv_sample { float util; int mem_used, mem_total, temp; int valid; };
static struct nv_sample g_nv_samples[MAX_GPUS];
static int g_nv_samples_ready = 0;    /* atomic: release-store / acquire-load */
static int g_nv_thread_running = 0;   /* atomic guard, like g_pkg_thread_running */
```

- `update_nvidia_thread()`: `popen_safe(NVIDIA_SMI_PATH, …)` into a *local* sample array
  (parsing logic identical to today's sscanf loop), copy to `g_nv_samples`,
  `__atomic_store_n(&g_nv_samples_ready, 1, __ATOMIC_RELEASE)`, then clear
  `g_nv_thread_running` with release ordering.
- `gather_data()`: when `has_nvidia_smi` && ≥1 NVIDIA GPU && `tick_count % 5 == 0` &&
  not running → spawn detached pthread. Every tick, if `g_nv_samples_ready`
  (acquire), map samples onto GPUs using the existing nth-NVIDIA-index logic (`:632-635`)
  and populate `util_pct/vram_*/temp_c`.
- Persistence change: sample application happens *every* tick from the cache, so GPU
  fields no longer blank between probes (today they reset every 5 ticks at `:611-612`).
- Sysctl fallbacks unchanged: `dev.nvidia.N.temperature` when the sample is invalid;
  `dev.drm*/dev.drmn*` freq for non-NVIDIA (`:650-673`).
- The one-shot `pciconf -lv` model scan (`:556-604`) stays synchronous: bounded, runs
  once at startup.
- fork-in-thread safety: `popen_safe()`'s child calls only async-signal-safe functions
  (`close/dup2/open/setres*/execv/_exit`) before `execv` — same as the existing pkg
  thread.

### C3 — freq_levels power suffix

In `render_thermal_power_box()` (`:944-950`), after tokenizing each level:

```c
char *slash = strchr(level, '/');
if (slash != NULL) *slash = '\0';   /* strip "/power_mW" suffix */
snprintf(l_buf, sizeof(l_buf), "%s MHz", level);
```

### C4 — Header centering

`║ FreeBSD System Monitor ║` = 26 display columns; `╚` + 24×`═` + `╝` = 26.
`render()` (`:999-1000`): change `(term_width - 24) / 2` → `(term_width - 26) / 2`
on both lines.

### C5 — Disk target dedup

In `gather_data()` (`:778-790`), skip targets equal to any earlier target
(covers `home_path == "/zroot"` etc.):

```c
for (int j = 0; j < 5; j++) {
    if (targets[j][0] == '\0') continue;
    int dup = 0;
    for (int k = 0; k < j; k++)
        if (strcmp(targets[j], targets[k]) == 0) { dup = 1; break; }
    if (dup) continue;
    …
}
```

### C6 — Battery state bitmask

`hw.acpi.battery.state` is a bitmask (bit0 discharging, bit1 charging, bit2 critical),
not an enum. Replace `:537-541`:

```c
if (itmp == 0) {
    strlcpy(d->bat_source, "AC Power", …); strlcpy(d->bat_state, "Full", …);
} else {
    strlcpy(d->bat_source, (itmp & 1) ? "Battery" : "AC Power", …);
    if (itmp & 4)      strlcpy(d->bat_state, "Critical", …);
    else if (itmp & 1) strlcpy(d->bat_state, "Discharging", …);
    else if (itmp & 2) strlcpy(d->bat_state, "Charging", …);
    else               strlcpy(d->bat_state, "Unknown", …);
}
```

This removes the inverted `itmp == 7 → "AC Power"` nonsense (7 = discharging|charging|
critical).

### C7 — Exact ports-repository match

`pkg query %r` prints one repository name per line. In `update_pkg_counts_thread()`
(`:374-376`), replace the `strstr` with a trimmed exact compare:

```c
line[strcspn(line, "\r\n")] = '\0';
char *p = line;
while (isspace((unsigned char)*p)) p++;
size_t L = strlen(p);
while (L > 0 && isspace((unsigned char)p[L - 1])) p[--L] = '\0';
if (strcmp(p, "local") == 0) count++;
```

### C8 + N3 — print_val() hardening

`:802-816`. Two negative-precision edges:
1. Truncation branch: `".." ` form requires `avail >= 4`; for `avail ∈ {1,2,3}` print a
   bare truncation: `printf(" %.*s", avail - 1, val);` (`avail <= 0` already returns at
   `:809`).
2. N3: clamp the label: after `if (lbl_len > w - 6) lbl_len = w - 6;` add
   `if (lbl_len < 0) lbl_len = 0;` (guards w=5..7).

### L1 — isatty() guard

In `main()`, after env scrub, before `enable_raw_mode()` (`:1070`):

```c
if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    fprintf(stderr, "monbsd: stdin and stdout must be a terminal\n");
    exit(1);
}
```

### L2 — Architecture guards

- At the includes block (`:23-25`):
  ```c
  #if defined(__amd64__) || defined(__i386__)
  #define MONBSD_X86 1
  #include <machine/cpufunc.h>
  #include <sys/ioccom.h>
  #include <sys/cpuctl.h>
  #else
  #define MONBSD_X86 0
  #endif
  ```
- Guard bodies: `direct_cpu_cores()` CPUID read (non-x86 → `return 1`);
  `direct_cpu_temp()`/`direct_cpu_live_freq()` MSR blocks (non-x86 → sysctl/fallback);
  `direct_pci_count()` port scan (non-x86 → `return -1`, rendered "N/A" per N4);
  CPU-brand CPUID block in `gather_data()` (`:401-412`) (non-x86 → `hw.model` sysctl).
- `g_cpuctl_fd`/`g_io_fd` opens in `main()` are harmless on non-x86 (fail → -1) but are
  also guarded for clarity.
- Docs: README Prerequisites + man page note that MSR temps, APERF/MPERF live frequency
  and direct PCI counting require amd64/i386; other architectures build and run with
  reduced telemetry.

### L3 — Complete the half-merged SSID feature (fixes N1 compile blocker)

Infrastructure exists (`ssid[64]` field `:57`, `g_ssid_cache` `:177`,
`refresh_wifi_ssid()` `:180-203`) but is never called and never rendered.

1. Add `#include <net/if_ieee80211.h>` after `<net/if.h>` (`:21`) for
   `struct ieee80211req` / `IEEE80211_IOC_SSID` / `SIOCG80211`.
2. Add small cache helpers: `ssid_cache_lookup(name)` / `ssid_cache_store(name, ssid)`
   over `g_ssid_cache`.
3. In the iface loop (`:697-702`): default `d->ifaces[i].ssid[0] = '\0'`; when
   `is_wifi`, look up the cache; refresh via `refresh_wifi_ssid()` every 50 ticks or on
   cache miss, then store and copy into `d->ifaces[i].ssid`. (The `SIOCG80211` ioctl is
   a cheap in-kernel call — no subprocess — so inline refresh at 5 s cadence is fine.)
4. Render (`:965` area): after the `IP:` line,
   `if (d->ifaces[i].is_wifi && d->ifaces[i].ssid[0]) print_val(…, "SSID:", …);`

### L4 + L6 — Makefile

- **L6:** replace the GNU-style pattern rule (`Makefile:20-21`) with a portable
  single-suffix rule — bmake treats `.c:` as a suffix transform, GNU make treats it as
  equivalent to `%: %.c`; one rule covers both `tests/` and `benchmarks/`:
  ```make
  .SUFFIXES: .c
  .c:
  	${CC} ${CFLAGS} $< -o $@
  ```
- **L4 additions:**
  ```make
  BENCH_SRCS= benchmarks/bench.c benchmarks/bench_getifaddrs.c benchmarks/bench_opt.c
  BENCH= $(BENCH_SRCS:.c=)

  bench: ${BENCH}

  check: tests
  	@pass=0; skip=0; fail=0; \
  	for t in ${TESTS}; do \
  		printf '== %s: ' "$$t"; \
  		./$$t; rc=$$?; \
  		if [ $$rc -eq 0 ]; then pass=$$((pass+1)); echo ok; \
  		elif [ $$rc -eq 77 ]; then skip=$$((skip+1)); echo SKIP; \
  		else fail=$$((fail+1)); echo FAIL; fi; \
  	done; \
  	echo "pass=$$pass skip=$$skip fail=$$fail"; \
  	[ $$fail -eq 0 ]

  install-user: ${TARGET}
  	@if [ -n "$$HOME" ]; then \
  		mkdir -p "$$HOME/.local/bin"; \
  		install -m 755 ${TARGET} "$$HOME/.local/bin/${TARGET}"; \
  		echo "installed (no setuid: MSR/PCI telemetry limited)"; \
  	else \
  		echo "\$$HOME is not set or empty; skipping user install."; \
  	fi
  ```
  Extend `clean` to remove `${BENCH}`; extend `.PHONY` with `bench check install-user`.
- Patch `tests/test_pci.c` and `tests/test_aperf.c` to exit **77** (automake skip
  convention) when `open()` of `/dev/io` resp. `/dev/cpuctl0` fails with
  `EACCES`/`EPERM`/`ENOENT`; keep real failures as 1. (`test_cpuctl.c` is compile-only;
  `test_statfs.c`/`test_uptime.c` need no privileges; `test_compile.c`/`test_cpuid.c`
  are x86-only — document that `make check` is an x86 target, matching L2.)

### L5 — statfs divide-by-zero

`gather_data()` `:783-787`: only accept a match with `f_blocks > 0`:

```c
if (strcmp(fs[i].f_mntonname, targets[j]) == 0) {
    if (fs[i].f_blocks > 0) {
        …fill entry…
        d->disk_count++;
    }
    break;
}
```

### L7 — Documentation

**`monbsd.8`:**
- New `EXIT STATUS` section: `0` user quit (`q`/`Q`/Ctrl-C); `1` startup failure
  (non-terminal stdin/stdout, environment sanitization failure, privilege-drop failure);
  `128+signo` terminated by signal after terminal restoration.
- New `ENVIRONMENT` section: `TERM` (validated against `[A-Za-z0-9._+-]`, max 64 chars,
  re-injected after scrubbing), `PATH` (reset to a fixed system list), `SUDO_UID`
  (consulted to resolve the invoking user's home directory when run via sudo); all other
  variables are cleared via `clearenv()`.
- Update `PRIVILEGES`: describe the open-then-drop model — setuid root is needed only to
  open `/dev/cpuctl0` and `/dev/io` at startup; privileges are permanently dropped before
  the UI starts; without them the tool runs with reduced telemetry.
- Add architecture note (full telemetry on amd64/i386).
- Bump `.Dd`.

**`README.md`:** Prerequisites gain the amd64/i386 note + non-root degradation table;
Installation documents `make install-user`; new short "Development" section documenting
`make tests`, `make check`, `make bench`.

**Housekeeping:** bump `VERSION` to `"0.1.1"` (`:34`) and add a CHANGELOG entry
grouping fixes by review ID.

---

## 3. Implementation sequence

1. **Unblock the build** — S3 helper + L3 wiring (resolves N1). *Everything else depends on a compilable tree.*
2. **S1** privilege lifecycle (+N2 O_CLOEXEC, +N4 "N/A"). Highest security impact; touches the same probes L2 guards will wrap — do S1 first, then L2 guards around the refactored bodies.
3. **S2** signal handlers. Small, independent.
4. **C2** nvidia thread. Largest self-contained change.
5. **C1, C3–C8, L1, L5** — single-site edits, any order.
6. **L2** arch guards.
7. **L4/L6** Makefile + test skip patches.
8. **L7** docs, CHANGELOG, version bump.
9. **Verification** (below).

## 4. Verification (requires approval to run make)

1. `make clean && make` — warning-free under `-Wall -Wextra` (clang, FreeBSD/amd64).
2. `make check` as a normal user — hardware tests report SKIP (77), rest pass; then
   `sudo make check` — all pass.
3. `make bench` builds all three benchmarks.
4. Runtime smoke (setuid install):
   - CPU temp / live freq / PCI count still populate (proves cached-fd pattern works
     after the drop).
   - `kill -TERM <pid>` / `kill -HUP <pid>` → terminal restored (echo on, cursor
     visible), shell reports exit status 143/129.
   - Resize to ~20 columns → box borders intact (C8/N3).
   - NVIDIA box updates without visible stutter (C2); ports count matches
     `pkg query %r | grep -cx local` (C7).
   - WiFi interface shows SSID line (L3).
5. `make install-user` / `make uninstall-user` round-trip.
6. Manual: build on arm64 hardware if available (L2 guards) — otherwise at minimum
   `cc -fsyntax-only` with the x86 paths forced off via a scratch `-UMONBSD_X86` test
   harness is *not* equivalent; treat arm64 build as a follow-up manual check.

## 5. Risks and mitigations

| Risk | Mitigation |
|------|------------|
| RDMSR/inl/outl silently fail post-drop on some FreeBSD version | Graceful fallbacks already exist (ACPI thermal, `dev.cpu.0.freq`, PCI "N/A"); smoke test per §4.4 confirms |
| nvidia thread races on sample array | Local array in thread → single release-store of ready flag; acquire-load by reader; reader copies before use |
| `check` runner flags permission failures as failures | Exit-77 skip convention in the two hardware tests |
| Behavior change: "Cores" → "Threads" label | Documented in CHANGELOG; value semantics now honest |
| SSID ioctl unsupported on non-802.11 `wlan*` names | `refresh_wifi_ssid()` already fails soft (empty string → line hidden) |
