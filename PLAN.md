# monbsd — Regression Analysis & Restoration Plan (v0.1.1 → v0.1.2)

**Scope.** This document supersedes the v0.1.0 remediation plan (S1–S3, C1–C8,
L1–L7, N1–N5), which is **fully implemented** on this branch and now lives in
`CHANGELOG.md` under `[0.1.1]`. What follows is (1) a review of this branch
versus `main`, (2) a deep root-cause analysis of the three regressions now
visible in the UI — **PCI shows "N/A"**, **Ports shows "0"**, and the **power
subsystem is misreported** — and (3) a comprehensive, FreeBSD-only restoration
plan. All line references are to the current `src/monbsd.c` (1309 lines)
unless noted.

Target audience/platform: **FreeBSD only**. Every mechanism proposed here is a
native FreeBSD interface (`pciconf(8)`, `pkg-query(8)`, `sysctl(3)` /
`kern.proc`, ACPI sysctls, `/dev/io`, `/dev/cpuctl0`, devfs). No Linux-isms,
no cross-platform shims.

---

## 1. Branch review: `perf/optimize-getifaddrs-*` vs `main`

The branch contains two bodies of work:

1. **Merged from `main`** (`33bcdbc`) — a batch of bot-generated "performance /
   security" PRs (#21–#37).
2. **Branch-local** (`9ea2af1`, `d3bff5c`, `eb2af17`) — the v0.1.0 review
   remediation (privilege-drop lifecycle, sanitization, arch guards, SSID
   completion, signal handlers, nvidia thread, UI fixes).

### 1.1 Verdicts on the merged `main` commits

| Commit | Change | Verdict |
|--------|--------|---------|
| `e6d25af` | TERM sanitization before setuid install | Sound. Keep. |
| `9fb0076`/`b6bed17` | pkg / blocking subprocesses on detached threads | Sound pattern (release/acquire flags, detached pthreads). Keep. |
| `852be84` | `d_type` fast path in `count_dir_executables()` | Sound. Keep. |
| `f86287f` | NULL-deref fix in `getfsstat` handling | Sound. Keep. |
| `5b6107a`/`a3b549c` | Uninitialized-variable fixes | Sound. Keep. |
| `29fa495` | Privilege drop inside `count_dir_executables()` | Now a harmless no-op after S1 (euid == ruid permanently). Keep; optionally simplify later. |
| `4cd9cd6` | Cache CPU core count | Superseded by C1 (`kern.smp.cpus` each tick, `src/monbsd.c:559-562`). Keep. |
| `43e98b3` | Cache `user_bin_count` directory traversal | Sound. Keep. |
| `9158028` | Cache `hw.physmem`/`hw.pagesize` | Sound. Keep. |
| `7bacc56` | Reduce disk query frequency (50-tick cache) | Sound. Keep. |
| `9dab7a8`/`2daa087` | N+1 `getifaddrs` fix (single call per tick) | Sound. Keep. |
| `0cc37f6` (#36) | pkg queries every **3000 ticks (~5 min)** | Harmful *amplifier*, not a root cause — see **R2**. Any failed pkg probe now persists for 5 minutes. |
| `747d8ae` (#37) | powerd/powerdxx detection via **pidfile + `kill(pid,0)`** | **REGRESSION — root cause of R3a.** Replaced a correct, unprivileged `KERN_PROC` exact-name scan with a pidfile read that fails for any non-root process. Revert per §3.3. |

### 1.2 Verdicts on branch-local (v0.1.1 remediation) changes

All remediation items landed as designed in the old plan. Two of them,
however, **interact** with the merged optimizations to produce the visible
breakage:

- **S1 (open-once + permanent privilege drop, `src/monbsd.c:1250-1266`)** is
  correct security design, but it converts every privileged-resource failure
  into a *permanent, startup-time* failure: if `/dev/io` cannot be opened at
  startup (non-setuid binary, non-root user), PCI is dead for the whole run.
  Combined with **N4** ("render -1 as N/A", `:1040-1041`), the user-facing
  result is `PCI Devices: N/A` — **R1**.
- **N4's "N/A"** is the right *rendering*, but the plan never supplied an
  unprivileged PCI counting fallback, even though one already exists in the
  process: `pciconf(8)` is spawned at startup for GPU detection (`:696-738`)
  and works for any user on any FreeBSD architecture.
- The old plan's degradation matrix assumed "ACPI/sysctl fallbacks exist for
  everything that matters." That assumption is false for PCI counting and was
  never revisited after the merge brought in #36/#37.

### 1.3 New hygiene issue introduced on this branch

**H1 — A compiled `monbsd` binary is committed at the repo root** (added by
`eb2af17`, 46 248 bytes; see `git diff main...HEAD --stat`). Consequences:
stale-binary execution produces exactly the kind of "phantom regressions"
being chased here; the binary is architecture-specific; it desynchronizes
from `src/monbsd.c` on every edit. It must be removed from the index and
covered by `.gitignore`.

---

## 2. Regression root-cause analysis

### R1 — `PCI Devices: N/A`

**Code path.** `main()` opens `g_io_fd = open("/dev/io", O_RDWR|O_CLOEXEC)`
once, while euid root, then permanently drops privileges
(`src/monbsd.c:1254-1266`). `direct_pci_count()` (`:318-346`) returns `-1`
immediately when `g_io_fd < 0`. The cache in `gather_data()` (`:608-615`)
never updates on `-1`, and the renderer (`:1040-1041`) prints **"N/A"**.

**Why it breaks in practice.** devfs creates `/dev/io` as
`crw------- root wheel`. Therefore:

- plain `make && ./monbsd` (non-root) → `open()` fails with `EACCES` → N/A;
- `make install-user` (mode 0755, no setuid — `Makefile:47-54`) → N/A,
  permanently, by design of that target;
- the **committed repo-root binary** (H1) run unprivileged → N/A;
- only the setuid install (`make install`, `Makefile:66-72`, mode 4755) or
  `sudo monbsd` opens the fd, and the cached-fd pattern then correctly
  survives the privilege drop (I/O-port permission is granted per-process at
  `open(2)` time — the Xorg pattern).

**Root cause.** The direct CF8/CFC port scan is the *only* PCI enumeration
source, and it is hard-gated on a root-only device node. On FreeBSD the
canonical, unprivileged, arch-neutral enumerator already exists and is
already used by this very program: `pciconf -l` (`:697`). The old plan's N4
designed the "N/A" rendering but never wired the fallback.

**Contrast with `main`:** `main` opened `/dev/io` per call and also returned
`-1` on failure, but rendered the raw value (`-1 devices`) — wrong, yet
"numeric". The branch made the failure honest without restoring the data.

### R2 — `Ports: 0 built`

**Code path.** `update_pkg_counts_thread()` (`:421-453`) runs
`pkg query %r` via `popen_safe()` and counts lines; the C7 fix tightened the
match from `strstr(line, "local")` to an exact, whitespace-trimmed
`strcmp(p, "local")` (`:438-445`). Result stored to `g_ports_count`
(`:446`), copied to the UI every tick (`:635`), rendered at `:1045-1046`.

**Root cause — the query itself fails.** On the installed pkg, a bare `%r`
format is rejected: pkg reports
`Invalid query: '%r' should be followed by: n, o, v` and exits non-zero,
printing nothing to stdout. (Observed directly on this host.) The thread
then counts **zero lines and stores 0**. Note what this rules out:

- **C7 is not the cause.** `strstr` vs `strcmp` is irrelevant when pkg emits
  no lines at all.
- The failure is **invisible**: `popen_safe()` redirects the child's stderr
  to `/dev/null` (`:396-397`), and the `pclose_safe()` exit status is
  **ignored** at both store sites (`:431`, `:447`).

**Amplifiers.**

1. **A2 — failures overwrite good data, and stick.** `__atomic_store_n(&g_ports_count, count)` runs unconditionally whenever `fork+exec` succeeded, regardless of pkg's exit status. Combined with **#36's 3000-tick (≈5-minute) cadence** (`:583-598`), a single bad query pins the display at 0 for five minutes; a permanently failing query pins it forever.
2. **A3 — no "unknown" state.** The counters initialize to `0` (`:417-418`), so "not yet queried" and "queried, got zero" are indistinguishable. The UI has no N/A path for pkg/ports (unlike PCI post-N4).

**Semantics note (pre-existing, worth documenting, not a bug):** the
repository name `local` is what pkg records for packages installed outside
any configured repository (classic ports-tree `make install`). Packages
built by poudriere/synth carry that builder's repository name instead and
will never be counted by *any* repo-name match. The man page should state
what "Ports: N built" actually means.

### R3 — Power subsystem incorrectly handled

The "THERMAL & POWER" box (`render_thermal_power_box()`, `:1055-1131`) is fed
by several probes; the dominant breakage is powerd/powerdxx detection.

#### R3a — `powerd: Stopped ✗` / `powerdxx: Stopped ✗` while powerd is running

**What changed.** Commit `747d8ae` (merged from `main`) replaced the previous
implementation — a `KERN_PROC` sysctl scan doing an **exact `ki_comm` name
match** for `powerd`/`powerdxx` (visible in `747d8ae^:src/monbsd.c:496-523`) —
with `check_pid_file_liveness()` (`:494-507`): read an integer from a pidfile,
`kill(pid, 0)`, treat `EPERM` as alive.

**Why it fails on FreeBSD.**

1. **Permissions.** `powerd(8)` writes `/var/run/powerd.pid` mode `0600`
   root:wheel (observed on this host). After the S1 privilege drop — and for
   every non-root run — `fopen()` (`:495`) fails with `EACCES` and the
   function returns 0. The `kill(pid,0)`/`EPERM` fallback is never even
   reached, because the pid itself is unreadable. **This check cannot work
   for the exact invocation modes S1 now mandates** (drop after open) nor for
   the documented non-setuid install path. It only works for a process
   running as root — which this program, by design, no longer is 100 ms after
   startup.
2. **powerdxx's pidfile is not guaranteed.** `/var/run/powerdxx.pid` does not
   exist on this host; whether it exists at all depends on how powerdxx was
   started (it is a ports package with its own rc integration, not base
   `powerd(8)`). A pidfile-based check is fundamentally unreliable here.
3. **PID recycling false positives.** Even when readable, the check validates
   *liveness of a number*, not the process name. A recycled PID belonging to
   an unrelated daemon reports `Running ✓`. The previous `ki_comm` exact
   match had no such failure mode (and also avoided substring matches against
   e.g. `upowerd`).
4. **First-boot staleness.** The check runs at `tick_count % 20 == 0`
   (`:658-661`) — i.e., immediately at tick 0 — fine — but a false result is
   now *sticky for the whole session* because it never becomes readable later.

**Root cause summary:** an "optimization" replaced a correct, unprivileged,
name-exact kernel query with a cheaper check that requires privileges the
program deliberately relinquishes. The `KERN_PROC_ALL` cost it saved is one
sysctl every 2 s — negligible.

#### R3b — Silent thermal/live-freq degradation when `cpuctl` is absent

`/dev/cpuctl0` does not exist unless the `cpuctl(4)` module is loaded
(absent on this host). Then `g_cpuctl_fd == -1`, `direct_cpu_temp()`
(`:274-292`) falls back to `hw.acpi.thermal.tz0.temperature`, and
`direct_cpu_live_freq()` (`:294-316`) falls back to `dev.cpu.0.freq`. These
fallbacks are intentional and correct — but (a) the degradation is invisible
to the user, and (b) if the ACPI thermal zone is also absent, temp renders
`-1.0 °C` (`:291`, `:1063`) — an ugly artifact with no "N/A" handling.
Restoration is: render-aware handling + documentation (`kldload cpuctl`,
`cpuctl_load="YES"` in `loader.conf`), not new probe machinery.

#### R3c — Battery minor defects

- `hw.acpi.battery.life` sysctl result is discarded (`:680`); on machines
  where it fails, `bat_life` keeps its previous (or zero-initialized) value
  and the bar renders a bogus `0%`/`stale%` (`:1118`).
- When `hw.acpi.battery.state` is absent (desktops without a BAT device), the
  box shows `Source: AC Power`, `State: N/A`, **and a 0% bar** — the bar
  should be suppressed when there is no battery.
- The C6 bitmask decode itself (`:667-679`) is correct on FreeBSD
  (`hw.acpi.battery.state`: bit0 discharging, bit1 charging, bit2 critical)
  and matches observed behavior (`state=0`, `life=100` on AC →
  `AC Power`/`Full`). No change needed there.

---

## 3. Restoration designs (FreeBSD-only)

### F1 — Restore the Ports count (fixes R2)

**Location:** `update_pkg_counts_thread()` (`src/monbsd.c:421-453`), counter
declarations (`:417-418`), renderer (`:1043-1046`), scheduler (`:583-598`).

1. **Version-proof query format.** pkg's own error enumerates the valid
   suffixes (`n`, `o`, `v`); the repository *name* is `%rn`. To remain
   correct across pkg releases, attempt in order:
   - `pkg query '%rn'` → repository name per package;
   - on non-zero exit, retry once with the legacy bare `pkg query '%r'`.
   Keep the C7 exact, trimmed `strcmp(p, "local")` match — it is correct.
2. **Gate stores on exit status.** For *both* pkg probes, only
   `__atomic_store_n()` when `pclose_safe()` returns `0`. A failed probe must
   leave the last-known value intact.
3. **Introduce an "unknown" state.** Initialize `g_pkg_count` and
   `g_ports_count` to `-1`; render `N/A` (matching the PCI N/A idiom) while
   negative. Distinguishes "query broken" from "genuinely zero".
4. **Cadence.** Keep the #36 thread pattern, but reduce the refresh from
   3000 to **600 ticks (~60 s)**: cheap (two short-lived subprocesses per
   minute, off the render path) and bounds error visibility. Store-gating
   (step 2) makes the cadence argument moot for correctness; this is for
   freshness only.
5. **Docs.** `monbsd.8`: state that "Ports" counts packages whose pkg
   repository name is exactly `local` (ports-tree installs); poudriere/synth
   builds report their own repository names and are not counted.

### F2 — Restore the PCI device count (fixes R1)

**Location:** `direct_pci_count()` (`:318-346`), cache site (`:608-615`),
renderer (`:1040-1041`), `main()` opens (`:1250-1256`).

1. **Add an unprivileged, arch-neutral enumerator.** New helper
   `pciconf_pci_count()`: `popen_safe("/usr/sbin/pciconf", {"pciconf","-l"})`,
   count non-empty output lines, exit-status-gated (F1 step 2 pattern),
   return `-1` on failure. `pciconf(8)` is base-system, readable by any user,
   and works on every FreeBSD architecture (x86, arm64, riscv, powerpc).
2. **Probe order.** Rename/keep the entry point as the single PCI source:
   - x86 with `g_io_fd >= 0` → direct CF8/CFC scan (fast, no subprocess);
   - otherwise → `pciconf_pci_count()`;
   - both fail → `-1` → render `N/A` (existing N4 idiom).
3. **Keep the S1 model unchanged.** `/dev/io` is still opened once pre-drop
   with `O_CLOEXEC`; the setuid install keeps the fast path. Non-root and
   `install-user` runs now get a *correct* count via pciconf instead of N/A.
4. **Semantics note.** The port scan counts config-space functions;
   `pciconf -l` lists one line per enumerated device/function. Counts are
   comparable in magnitude but may differ slightly (bridges, hot-plug
   visibility) — the metric is "devices on the PCI bus", and pciconf is the
   authoritative FreeBSD view of it. Document in the man page.
5. **(Optional, later)** Fold the count into the existing one-shot
   `pciconf -lv` GPU scan (`:696-738`) so the PCI count and GPU models come
   from a single subprocess at startup; refresh at the existing
   `tick_count % 100` cadence. Defer unless desired — correctness first.

### F3 — Restore power telemetry (fixes R3a/R3b/R3c)

**Location:** `check_pid_file_liveness()` (`:494-507`), powerd block
(`:655-664`), battery block (`:666-680`), battery render (`:1116-1119`),
temp render (`:1063`).

1. **F3a — revert to the name-exact kernel scan.** Delete
   `check_pid_file_liveness()` and restore the pre-`747d8ae` implementation
   (proven correct on this codebase): every 20 ticks, `sysctl` `CTL_KERN /
   KERN_PROC / KERN_PROC_ALL`, iterate `struct kinfo_proc`, set
   `cached_powerd` / `cached_powerdxx` on exact `strcmp(ki_comm, "powerd")` /
   `strcmp(ki_comm, "powerdxx") == 0`. Properties:
   - unprivileged under the FreeBSD default (`security.bsd.see_other_uids=1`);
   - no subprocess, no pidfile, no permissions failure mode after the S1 drop;
   - exact-name match: no PID-recycle false positives, no `upowerd`
     substring matches;
   - cost: one sysctl every 2 s.
   Caveat to document: with `security.bsd.see_other_uids=0`, a non-root
   monbsd cannot see root's daemons; in that configuration the previous
   pidfile check is *also* useless (0600), so nothing is lost — the UI will
   show `Stopped ✗`, which the man page will explain. (Optional refinement:
   if the sysctl succeeds but finds neither name, *and* the pidfile is
   readable, fall back to the pidfile liveness check. Only add if a real
   configuration demands it.)
2. **F3b — render-aware thermal degradation.** If `direct_cpu_temp()`
   returns `< 0`, render `N/A` instead of `-1.0 °C`. Document in `monbsd.8`
   and `README.md` that MSR temps / APERF-MPERF live frequency require
   `kldload cpuctl` (and the setuid install); otherwise ACPI thermal zone and
   `dev.cpu.0.freq` are used, and where those are absent the fields show
   `N/A`.
3. **F3c — battery correctness.**
   - Check the `hw.acpi.battery.life` sysctl result; on failure set
     `d->bat_life = -1`.
   - Track battery presence: `has_battery = (sysctl(hw.acpi.battery.state)
     succeeded)`. When absent: `Source: AC Power`, `State: No battery`, and
     **skip the Bat bar** (`:1118`) or render `N/A`. When present but life
     unknown (`-1`), skip just the bar.

### F4 — Hygiene (H1 + fallout)

1. **H1:** `git rm --cached monbsd`; delete the file; add `.gitignore`
   covering `/monbsd`, `/tests/test_*` binaries, `/benchmarks/bench*`
   binaries. The Makefile already builds all of these from source.
2. **H2 (generalized F1-step-2):** audit every `pclose_safe()` call site
   (`:431`, `:447`, `:486`, `:738`, `:911`) — pkg, nvidia-smi, pciconf,
   swapinfo — and only commit parsed results on exit status `0`. (swapinfo
   already gates its cache at `:911`; make the others match.)
3. **Docs:** `monbsd.8` and `README.md` gain a "Telemetry sources and
   privileges" table (below); CHANGELOG `[0.1.2]` entry; bump `VERSION` to
   `"0.1.2"` (`:41`).

---

## 4. Findings matrix (this plan)

| ID | Symptom | Root cause | Fix | Effort | Risk |
|----|---------|------------|-----|--------|------|
| R1 | `PCI Devices: N/A` | `/dev/io` root-only; no unprivileged fallback | F2: pciconf fallback | Low | Low |
| R2 | `Ports: 0 built` | pkg rejects bare `%r`; store not gated on exit status; 5-min stickiness | F1: `%rn`+legacy retry, gate stores, `-1`=N/A, 600-tick cadence | Low | Low |
| R3a | powerd/powerdxx `Stopped ✗` | #37 pidfile check vs 0600 root pidfile + S1 drop; no name match; powerdxx pidfile unreliable | F3a: restore `KERN_PROC` exact-name scan | Low | Low |
| R3b | `-1.0 °C` / silent temp-freq degradation | cpuctl module absent; no render handling for `-1` | F3b: N/A rendering + docs | Trivial | None |
| R3c | Bogus battery bar on battery-less/ACPI-less hosts | `battery.life` result ignored; bar unconditional | F3c: presence flag + `-1` handling | Trivial | None |
| H1 | Stale committed binary at repo root | `eb2af17` added `monbsd` to git | F4.1: remove + `.gitignore` | Trivial | None |
| H2 | Failed subprocesses overwrite good data | `pclose_safe()` statuses unchecked | F4.2: gate all parse stores | Low | Low |

---

## 5. Implementation sequence

1. **F3a** (powerd revert) — highest user-visible impact, self-contained.
2. **F1** (ports) — query format, store gating, `-1` init, cadence, renderer.
3. **F2** (PCI) — pciconf fallback + probe order.
4. **F3b/F3c** (thermal render, battery) — small, independent.
5. **F4** (hygiene: binary removal, `.gitignore`, pclose audit, docs,
   CHANGELOG, version bump to 0.1.2).
6. Verification per §6.

Order rationale: 1–3 are the three reported regressions; each is an
independent single-site change with no cross-dependencies.

---

## 6. Verification (FreeBSD, requires approval to run make)

1. `make clean && make` — warning-free under `-Wall -Wextra`.
2. `make check` as a normal user (hardware tests SKIP with 77), then
   `sudo make check`.
3. Runtime smoke, **as a normal (non-root) user** — the configuration where
   all three regressions manifest:
   - `PCI Devices:` shows a number matching `pciconf -l | wc -l` (± bridge
     semantics), not `N/A`.
   - `Ports:` matches `pkg query '%rn' | grep -cx local`; with pkg
     deliberately made to fail (e.g., `PATH`-shadowed bogus pkg in a scratch
     test) the counter holds its last value / shows `N/A`, never a false `0`.
   - `service powerd onestart` → UI shows `powerd: Running ✓` within ~2 s;
     `service powerd onestop` → `Stopped ✓`. Same for powerdxx if installed.
   - Battery-less host: no `0%` bar; `State: No battery` (or `N/A`).
   - With `kldunload cpuctl` (if loaded): temp shows ACPI value or `N/A`,
     never `-1.0 °C`.
4. Runtime smoke, **setuid install** (`sudo make install`): direct port scan
   path confirmed (PCI count present), MSR temp/live-freq present when
   cpuctl is loaded, and all non-root results above remain correct — the two
   privilege paths must agree.
5. `make install-user` / `make uninstall-user` round-trip; confirm the
   install message matches the new telemetry table (PCI now works
   unprivileged via pciconf).
6. Confirm `git status` shows no tracked binaries; `make clean` leaves the
   tree pristine.

---

## 7. Telemetry sources & privileges (to be documented in `monbsd.8`/`README.md`)

| Field | Source | Root/setuid needed? | Degradation |
|-------|--------|---------------------|-------------|
| CPU temp | MSR via `/dev/cpuctl0` (cached fd) → ACPI `hw.acpi.thermal.tz0.temperature` | Only for MSR path | ACPI value, else `N/A` |
| Live freq | APERF/MPERF via `/dev/cpuctl0` → `dev.cpu.0.freq` | Only for MSR path | sysctl value |
| PCI devices | CF8/CFC scan via `/dev/io` (cached fd) → **`pciconf -l`** (new) | Only for direct scan | pciconf count (equal standing), else `N/A` |
| Ports/pkg | `pkg info -q`, `pkg query '%rn'` (thread, exit-gated) | No | `N/A` on query failure |
| powerd/powerdxx | `KERN_PROC_ALL` exact `ki_comm` match | No (default `see_other_uids=1`) | `Stopped ✗` + man-page caveat |
| Battery | `hw.acpi.battery.{state,life}` | No | `No battery` / bar suppressed |
| GPU | `pciconf -lv` (once), `nvidia-smi` (thread), `dev.nvidia.*`/`dev.drm.*` sysctls | No | Partial fields |
| Disks/swap | `getfsstat`, `swapinfo -k` (cached 50 ticks) | No | Full |
| Network/SSID | `ifmibdata` sysctl, `getifaddrs`, `SIOCG80211` ioctl | No | Full |

---

## 8. Risks and mitigations

| Risk | Mitigation |
|------|------------|
| `%rn` unsupported on some older pkg | Legacy bare-`%r` retry preserves prior behavior; worst case equals today's failure, now rendered honestly as `N/A` |
| `pciconf -l` count differs slightly from port-scan count | Documented semantics (§3.2 step 4); direct scan retained as primary on x86+root so setuid installs see no change |
| `KERN_PROC_ALL` overhead concern (the original #37 motivation) | One sysctl per 2 s (20-tick cadence retained); measured cost trivial vs. the per-tick render work |
| `security.bsd.see_other_uids=0` hardening hides powerd from non-root | Man-page caveat; pidfile method was equally broken there (0600), so no regression vs. any prior state |
| Behavior change: Ports/pkg can now show `N/A` | CHANGELOG entry; honest unknown-state beats false `0` |
