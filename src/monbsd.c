#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_mib.h>
#include <net80211/ieee80211_ioctl.h>
#include <sys/vmmeter.h>
#include <dev/acpica/acpiio.h>
#if defined(__amd64__) || defined(__i386__)
#define MONBSD_X86 1
#include <machine/cpufunc.h>
#include <sys/ioccom.h>
#include <sys/cpuctl.h>
#else
#define MONBSD_X86 0
#endif
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/user.h>
#include <pthread.h>
#include <sys/wait.h>

/*
 * monbsd - terminal-based system monitor for FreeBSD laptops.
 *
 * Architecture
 * ------------
 *  1. Terminal control  - raw-mode setup/teardown, cursor/color escapes,
 *                          SIGWINCH-driven resize handling.
 *  2. Hardware access   - direct MSR reads via /dev/cpuctl0 and raw PCI
 *                          config-space I/O via /dev/io. Both descriptors
 *                          must be opened with an elevated effective uid
 *                          (see "Privilege model" below).
 *  3. Subprocess helpers - privilege-dropping fork/exec wrappers used to
 *                          invoke read-only system utilities (pkg,
 *                          pciconf, swapinfo, nvidia-smi) without a shell.
 *  4. Data gathering     - gather_data() populates a single, reused
 *                          struct mon_data snapshot per tick by combining
 *                          sysctl(3) queries, the helpers above, and a
 *                          small ring buffer (history[]) used to derive
 *                          rates (CPU%, network throughput) between ticks.
 *  5. Rendering          - stateless draw_box/print_val/print_bar and the
 *                          render_*_box helpers turn a struct mon_data
 *                          snapshot into ANSI
 *                          escape sequences; render() is the entry point.
 *  6. main()             - sanitizes the environment, resolves the
 *                          invoking (pre-sudo) user's home directory,
 *                          acquires the privileged device descriptors,
 *                          drops privileges, then drives the raw-mode
 *                          render loop.
 *
 * Privilege model
 * ----------------
 * monbsd is normally installed setuid root (see the Makefile's `install`
 * target) so that it can open /dev/cpuctl0 and /dev/io. Those two
 * descriptors are opened once, early in main(), while the effective uid
 * is still root, and with O_CLOEXEC so that no forked child inherits
 * them. Because credentials are checked at open(2) time, the later
 * RDMSR ioctls and I/O-port reads keep working once the descriptors are
 * held. main() then permanently drops the effective, real and saved gid
 * and uid back to the invoking user's (setresgid/setresuid) before the
 * UI starts, and treats a failed drop as fatal - so the render loop does
 * not run with an elevated effective uid.
 *
 * Note that the drop lowers privileges to the *real* uid, so when monbsd
 * is genuinely invoked by root (via sudo, say) there is nothing to drop
 * and the process legitimately stays root. Two narrower mechanisms limit
 * exposure regardless of how the program was started:
 *   - count_dir_executables() lowers the effective uid to the real uid
 *     for the duration of each scan, so its file-access checks carry the
 *     invoking user's authority rather than root's; it restores the
 *     previous euid afterwards (or exits if it cannot).
 *   - popen_safe() permanently drops both uid and gid (via setresuid/
 *     setresgid) in the forked child *before* exec, so subprocesses like
 *     pkg(8) or pciconf(8) never run with root privileges. This only
 *     affects the child; the parent's credentials are unchanged.
 *
 * Memory model
 * ------------
 * There is no long-lived heap allocation on the hot path: struct
 * mon_data is a single fixed-size, statically-sized instance owned by
 * main() and passed by pointer; all string fields use fixed-size buffers
 * filled with strlcpy()/snprintf() (never sprintf/strcpy). The only
 * heap allocations are short-lived and always freed on every path:
 * getfsstat()'s struct statfs array in gather_data(), and the TERM
 * string duplicated in main(). The background threads
 * (update_pkg_counts_thread, update_nvidia_thread) are created
 * PTHREAD_CREATE_DETACHED, so they need no explicit join/cleanup.
 */

#define VERSION "0.1.2"
#define HISTORY_SIZE 10
#define MAX_DISKS 8
#define MAX_NET_IF 4
#define MAX_GPUS 2
#define NVIDIA_SMI_PATH "/usr/local/bin/nvidia-smi"

static int g_cpuctl_fd = -1;   /* /dev/cpuctl0, open for process lifetime */
static int g_io_fd = -1;       /* /dev/io, open for process lifetime */

struct gpu_data {
    char model[128];
    double freq_mhz;
    double temp_c;
    double util_pct;      /* GPU core utilization % (-1 = unavailable) */
    long vram_used_mib;   /* VRAM used in MiB (-1 = unavailable) */
    long vram_total_mib;  /* VRAM total in MiB (-1 = unavailable) */
    int active;
};

struct net_iface_data {
    char name[32];
    char ip[INET_ADDRSTRLEN];
    double rx_rate_kb, tx_rate_kb;
    double total_rx_gb, total_tx_gb;
    int is_wifi;
    char ssid[64];
    int active;
};

struct disk_entry {
    char mount[MAXPATHLEN];
    long long total_bytes;
    long long used_bytes;
    double usage;
};

/*
 * A single point-in-time snapshot of everything the UI renders. Owned
 * by main() as one persistent instance and refilled in place by
 * gather_data() every tick (never reallocated) - see the "Memory
 * model" note at the top of this file. All string members are
 * fixed-size buffers written only through strlcpy()/snprintf().
 */
struct mon_data {
    char time_str[16];
    char date_str[16];
    char host[MAXHOSTNAMELEN];
    char uptime_str[32];
    double load[3];
    char cpu_model[256];
    double cpu_freq_ghz; 
    int cpu_threads;
    double cpu_usage;
    long long mem_total, mem_used;
    double mem_usage;
    int pkg_count, ports_count, linux_count;
    int user_bin_count;
    int pci_device_count;

    double cpu_temp;
    char thermal_state[32];
    double live_freq_mhz; 
    int freq_trend; 
    char fan_status[32];
    char cx_lowest[16];
    char cx_usage[128];
    int powerd_running;
    int powerdxx_running;
    char bat_source[32];
    int bat_life;
    char bat_state[32];
    int has_battery;
    char freq_levels[1024];

    struct gpu_data gpus[MAX_GPUS];
    int gpu_count;

    struct net_iface_data ifaces[MAX_NET_IF];
    int if_count;

    long long swap_total, swap_used;
    double swap_usage;
    struct disk_entry disks[MAX_DISKS];
    int disk_count;
    char home_path[MAXPATHLEN];
    char home_dir[MAXPATHLEN];
};

struct iface_history {
    char name[32];
    long long rx, tx;
    struct timespec ts;
};

/*
 * Fixed-size ring buffer of past CPU/network counters, used to derive
 * rates (CPU load %, network KB/s) as a delta against a sample from
 * `history[HISTORY_SIZE - 1]` ticks ago (see the `oidx` lookups in
 * gather_data()). `valid` guards each slot until it has been written
 * at least once, so early ticks (before the ring buffer has wrapped)
 * skip rate calculations instead of reading uninitialized data.
 */
struct {
    long cp_time[CPUSTATES];
    struct iface_history ifaces[MAX_NET_IF];
    int if_count;
    struct timespec ts;
    int valid;
} history[HISTORY_SIZE];
int hist_idx = 0;

struct termios orig_termios;
int term_width = 120, term_height = 40;
volatile sig_atomic_t resize_pending = 0;
static volatile sig_atomic_t exit_signo = 0;
unsigned int tick_count = 0;

/* ===================== Terminal control & raw mode ===================== */

void move_cursor(int y, int x) { printf("\033[%d;%dH", y, x); }
void set_color(int color) { printf("\033[%dm", color); }
void reset_color() { printf("\033[0m"); }
void clear_screen() { printf("\033[2J\033[H"); }

static void draw_heading(int y, int x, int w, int color, const char *text) {
    if (y < 1 || w < 1) return;
    move_cursor(y, x);
    if (color > 0) set_color(color);
    printf("%-*.*s", w, w, text);
    if (color > 0) reset_color();
}

void get_terminal_size() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        term_width = ws.ws_col;
        term_height = ws.ws_row;
    }
}

void handle_sigwinch(int sig) { (void)sig; resize_pending = 1; }

static void handle_exit_signal(int sig) { exit_signo = sig; }   /* async-signal-safe */

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h");
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l");
}

/* Replace terminal-hostile control bytes (incl. ESC) with spaces.
 * Bytes >= 0x80 are left intact so valid UTF-8 survives. */
static void sanitize_str(char *s) {
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) *s = ' ';
    }
}

struct ssid_cache_entry {
    char name[32];
    char ssid[64];
    unsigned int last_tick;
};
static struct ssid_cache_entry g_ssid_cache[MAX_NET_IF];
static int g_ssid_cache_count = 0;

static struct ssid_cache_entry *ssid_cache_lookup(const char *name) {
    for (int i = 0; i < g_ssid_cache_count; i++)
        if (strcmp(g_ssid_cache[i].name, name) == 0)
            return &g_ssid_cache[i];
    return NULL;
}

static void ssid_cache_store(const char *name, const char *ssid, unsigned int tick) {
    struct ssid_cache_entry *e = ssid_cache_lookup(name);
    if (e == NULL && g_ssid_cache_count < MAX_NET_IF)
        e = &g_ssid_cache[g_ssid_cache_count++];
    if (e != NULL) {
        strlcpy(e->name, name, sizeof(e->name));
        strlcpy(e->ssid, ssid, sizeof(e->ssid));
        e->last_tick = tick;
    }
}

static void refresh_wifi_ssid(const char *ifname, char *out, size_t out_len) {
    if (out_len == 0) return;
    out[0] = '\0';
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct ieee80211req ireq;
    char buf[64];
    memset(&ireq, 0, sizeof(ireq));
    memset(buf, 0, sizeof(buf));
    strlcpy(ireq.i_name, ifname, sizeof(ireq.i_name));
    ireq.i_type = IEEE80211_IOC_SSID;
    ireq.i_val = -1;
    ireq.i_data = buf;
    ireq.i_len = sizeof(buf);
    if (ioctl(s, SIOCG80211, &ireq) == 0 && ireq.i_len > 0) {
        size_t n = (size_t)ireq.i_len;
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        if (n >= out_len) n = out_len - 1;
        memcpy(out, buf, n);
        out[n] = '\0';
        sanitize_str(out);
    }
    close(s);
}

static void get_ip_address(struct ifaddrs *ifaddr, const char *ifname, char *ip_buf, size_t buf_size) {
    struct ifaddrs *ifa;
    strlcpy(ip_buf, "Unknown", buf_size);
    if (ifaddr == NULL) return;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_INET && strcmp(ifa->ifa_name, ifname) == 0) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip_buf, buf_size);
            break;
        }
    }
}

/* ========= Direct hardware access (via the pre-opened privileged fds) ========= */

/* Logical processor (thread) count per package, from CPUID leaf 1 EBX[23:16],
 * cached after the first successful read. Used only as a fallback when the
 * kern.smp.cpus sysctl fails; returns >= 1 on x86 and 1 elsewhere. */
int direct_cpu_cores() {
#if MONBSD_X86
    static int cached_cores = 0;
    if (cached_cores > 0) return cached_cores;

    u_int regs[4];
    do_cpuid(1, regs);
    int cores = (regs[1] >> 16) & 0xFF;
    cached_cores = cores > 0 ? cores : 1;
    return cached_cores;
#else
    return 1;
#endif
}

/*
 * CPU package temperature in degrees Celsius, or -1.0 if unavailable.
 * Tries an MSR read via /dev/cpuctl0 first, falling back to the ACPI
 * thermal-zone sysctl if the MSR path (or file) isn't available.
 */
double direct_cpu_temp() {
#if MONBSD_X86
    if (g_cpuctl_fd >= 0) {
        cpuctl_msr_args_t args;
        args.msr = 0x1A2; // MSR_TEMPERATURE_TARGET
        int tjmax = 100;
        if (ioctl(g_cpuctl_fd, CPUCTL_RDMSR, &args) == 0) tjmax = (args.data >> 16) & 0xFF;
        args.msr = 0x19C; // IA32_THERM_STATUS
        if (ioctl(g_cpuctl_fd, CPUCTL_RDMSR, &args) == 0) {
            int temp_offset = (args.data >> 16) & 0x7F;
            return (double)(tjmax - temp_offset);
        }
    }
#endif
    int temp; size_t size = sizeof(temp);
    if (sysctlbyname("hw.acpi.thermal.tz0.temperature", &temp, &size, NULL, 0) == 0)
        return (temp - 2732.0) / 10.0;
    return -1.0;
}

/*
 * Instantaneous CPU frequency (MHz) derived from the change in the
 * APERF/MPERF MSR pair since the previous call. Returns fallback_freq
 * on the first call (no prior sample) or if /dev/cpuctl0 is unavailable.
 * Keeps its previous-sample state in static locals, so it is not
 * reentrant/thread-safe; it is only ever called from gather_data() on
 * the main thread.
 */
double direct_cpu_live_freq(double fallback_freq, double base_freq) {
#if !MONBSD_X86
    (void)base_freq;
#endif
#if MONBSD_X86
    static uint64_t last_mperf = 0, last_aperf = 0;
    if (g_cpuctl_fd >= 0) {
        cpuctl_msr_args_t m_args = { .msr = 0xE7 }; // MPERF
        cpuctl_msr_args_t a_args = { .msr = 0xE8 }; // APERF
        if (ioctl(g_cpuctl_fd, CPUCTL_RDMSR, &m_args) == 0 && ioctl(g_cpuctl_fd, CPUCTL_RDMSR, &a_args) == 0) {
            uint64_t mperf = m_args.data;
            uint64_t aperf = a_args.data;
            double freq = fallback_freq;
            if (last_mperf != 0 && mperf > last_mperf && aperf > last_aperf) {
                freq = base_freq * ((double)(aperf - last_aperf) / (mperf - last_mperf));
            }
            last_mperf = mperf; last_aperf = aperf;
            return freq;
        }
    }
#endif
    return fallback_freq;
}

/* Unprivileged, architecture-neutral fallback used below when the direct
 * config-space scan is unavailable. */
static int pciconf_pci_count(void);

/*
 * Counts populated PCI functions by brute-force scanning all 256 buses
 * and 32 devices (plus up to 8 functions on multi-function devices)
 * via raw CF8/CFC config-space I/O through the cached /dev/io
 * descriptor. On non-x86 builds, or when /dev/io could not be opened,
 * it falls back to pciconf_pci_count(); returns -1 if that fails too.
 * This is a deliberately exhaustive scan, but not a hot-path call -
 * gather_data() only invokes it every ~100 ticks.
 */
int direct_pci_count() {
#if MONBSD_X86
    if (g_io_fd >= 0) {
        int count = 0;
        for (int bus = 0; bus < 256; bus++) {
            for (int dev = 0; dev < 32; dev++) {
                uint32_t address = (1 << 31) | (bus << 16) | (dev << 11) | 0;
                outl(0xCF8, address);
                uint32_t val = inl(0xCFC);
                if (val != 0xFFFFFFFF && val != 0) {
                    count++;
                    outl(0xCF8, address | 0x0C);
                    uint32_t hdr = inl(0xCFC);
                    if (hdr & 0x00800000) {
                        for (int func = 1; func < 8; func++) {
                            address = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8);
                            outl(0xCF8, address);
                            val = inl(0xCFC);
                            if (val != 0xFFFFFFFF && val != 0) count++;
                        }
                    }
                }
            }
        }
        return count;
    }
#endif
    /* Unprivileged, arch-neutral fallback: pciconf(8) enumerates PCI
     * devices for any user on any FreeBSD architecture. */
    return pciconf_pci_count();
}

/* ==================== Privilege-dropping helper functions ==================== */

/*
 * Counts executable regular files directly inside `path`. Temporarily
 * drops the effective uid to the real (invoking) uid for the duration
 * of the scan, so a setuid-root monbsd cannot be used to enumerate or
 * probe files in the invoking user's home directory that the user
 * themselves could not already access. Any failure to restore the
 * original euid afterwards is treated as fatal (exit(1)) rather than
 * silently continuing with the wrong privilege level.
 */
static int count_dir_executables(const char *path) {
    uid_t orig_euid = geteuid();
    uid_t ruid = getuid();

    if (seteuid(ruid) != 0)
        return 0;

    DIR *dir = opendir(path);
    if (!dir) {
        if (seteuid(orig_euid) != 0) exit(1);
        return 0;
    }
    int count = 0;
    struct dirent *e;
    int dfd = dirfd(dir);
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;

        if (e->d_type != DT_UNKNOWN && e->d_type != DT_REG && e->d_type != DT_LNK)
            continue;

        if (e->d_type == DT_REG) {
            if (faccessat(dfd, e->d_name, X_OK, 0) == 0)
                count++;
        } else {
            if (faccessat(dfd, e->d_name, X_OK, 0) == 0) {
                struct stat st;
                if (fstatat(dfd, e->d_name, &st, 0) == 0 && S_ISREG(st.st_mode))
                    count++;
            }
        }
    }
    closedir(dir);

    if (seteuid(orig_euid) != 0) exit(1);

    return count;
}

/* ========================= Subprocess helpers ========================= */

/*
 * popen(3)-like helper that execv()s `path` directly (no shell, so no
 * shell-metacharacter injection risk) and permanently drops the child
 * to the real uid/gid before exec, so subprocesses never inherit
 * monbsd's setuid-root privilege. The child's stderr is redirected to
 * /dev/null to avoid corrupting the parent's raw-mode terminal display.
 * On success, returns a FILE* open for reading the child's stdout and
 * stores its pid in *pid_out; the caller must eventually call
 * pclose_safe() to reap the child and avoid a zombie process. Returns
 * NULL on failure with no fd/resource left open.
 */
static FILE *popen_safe(const char *path, char *const argv[], pid_t *pid_out) {
    int pipe_fds[2];
    if (pipe(pipe_fds) == -1) return NULL;
    pid_t pid = fork();
    if (pid == -1) { close(pipe_fds[0]); close(pipe_fds[1]); return NULL; }
    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[1]);
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null != -1) { dup2(dev_null, STDERR_FILENO); close(dev_null); }
        gid_t rgid = getgid();
        uid_t ruid = getuid();
        if (setresgid(rgid, rgid, rgid) == -1) _exit(1);
        if (setresuid(ruid, ruid, ruid) == -1) _exit(1);
        execv(path, argv);
        _exit(1);
    }
    close(pipe_fds[1]);
    *pid_out = pid;
    FILE *fp = fdopen(pipe_fds[0], "r");
    if (fp == NULL) {
        close(pipe_fds[0]);
        (void)waitpid(pid, NULL, 0);
    }
    return fp;
}

/* Closes a popen_safe() pipe and reaps its child, preventing zombies. */
static int pclose_safe(FILE *fp, pid_t pid) {
    fclose(fp);
    int status;
    if (waitpid(pid, &status, 0) == -1) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Count PCI devices via pciconf(8): one output line per enumerated
 * device/function. Unprivileged and architecture-neutral; used when the
 * direct /dev/io port scan is unavailable. Returns -1 on failure. */
static int pciconf_pci_count(void) {
    pid_t p_pid;
    char *pciconf_argv[] = {"pciconf", "-l", NULL};
    FILE *fp = popen_safe("/usr/sbin/pciconf", pciconf_argv, &p_pid);
    if (fp == NULL) return -1;
    int count = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp))
        if (line[0] != '\n' && line[0] != '\0') count++;
    if (pclose_safe(fp, p_pid) != 0) return -1;
    return count;
}

/* ===================== Background package-count thread ===================== */
/*
 * pkg(8) queries are slow enough to stall the render loop, so they run
 * on a detached background thread every ~600 ticks (see pkg_ticks in
 * gather_data()). g_pkg_count/g_ports_count/g_pkg_thread_running are
 * the only state shared with the main thread, and all access to them
 * goes through __atomic_*, so no mutex/join is required: the main
 * thread just reads the last-published values each tick. Both counters
 * start at -1, meaning "no successful query yet", and render as "N/A"
 * until a query succeeds.
 */
static int g_pkg_count = -1;    /* -1 = no successful query yet (rendered N/A) */
static int g_ports_count = -1;
static int g_pkg_thread_running = 0;

static void *update_pkg_counts_thread(void *arg) {
    (void)arg;
    pid_t p_pid;
    char *pkg_info_argv[] = {"pkg", "info", "-q", NULL};
    FILE *fp = popen_safe("/usr/local/sbin/pkg", pkg_info_argv, &p_pid);
    if (fp) {
        int count = 0;
        char line[256];
        while (fgets(line, sizeof(line), fp)) count++;
        if (pclose_safe(fp, p_pid) == 0)
            __atomic_store_n(&g_pkg_count, count, __ATOMIC_RELAXED);
    }
    /* Current pkg requires a suffix on the repository format ('%rn');
     * older releases accepted the bare '%r'. Try the modern form first,
     * retry once with the legacy form on non-zero exit, and only store on
     * success so a failed query never overwrites a last-known-good count. */
    const char *repo_fmts[] = {"%rn", "%r"};
    for (size_t a = 0; a < sizeof(repo_fmts) / sizeof(repo_fmts[0]); a++) {
        char *pkg_query_argv[] = {"pkg", "query", (char *)repo_fmts[a], NULL};
        fp = popen_safe("/usr/local/sbin/pkg", pkg_query_argv, &p_pid);
        if (fp == NULL) break;
        int count = 0;
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *p = line;
            while (isspace((unsigned char)*p)) p++;
            size_t L = strlen(p);
            while (L > 0 && isspace((unsigned char)p[L - 1])) p[--L] = '\0';
            if (strcmp(p, "local") == 0) count++;
        }
        if (pclose_safe(fp, p_pid) == 0) {
            __atomic_store_n(&g_ports_count, count, __ATOMIC_RELAXED);
            break;
        }
    }

    // Ensure memory is visibly updated before clearing the flag
    __atomic_store_n(&g_pkg_thread_running, 0, __ATOMIC_RELEASE);
    return NULL;
}

struct nv_sample { float util; int mem_used, mem_total, temp; int valid; };
static struct nv_sample g_nv_samples[MAX_GPUS];
static int g_nv_samples_ready = 0;    /* atomic: release-store / acquire-load */
static int g_nv_thread_running = 0;   /* atomic guard, like g_pkg_thread_running */
static pthread_mutex_t g_nv_samples_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int g_nv_samples_tick = 0;
static char g_privileged_note[128];
static int g_privileged_note_visible = 0;

static void *update_nvidia_thread(void *arg) {
    (void)arg;
    struct nv_sample local[MAX_GPUS];
    memset(local, 0, sizeof(local));
    pid_t p_pid;
    char *nvidia_argv[] = {
        "nvidia-smi",
        "--query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu",
        "--format=csv,noheader,nounits",
        NULL
    };
    FILE *fp = popen_safe(NVIDIA_SMI_PATH, nvidia_argv, &p_pid);
    if (fp) {
        char sbuf[256];
        int nv_line = 0;
        while (fgets(sbuf, sizeof(sbuf), fp) && nv_line < MAX_GPUS) {
            float util; int mem_used, mem_total, gtemp;
            if (sscanf(sbuf, " %f , %d , %d , %d", &util, &mem_used, &mem_total, &gtemp) == 4) {
                local[nv_line].util = util;
                local[nv_line].mem_used = mem_used;
                local[nv_line].mem_total = mem_total;
                local[nv_line].temp = gtemp;
                local[nv_line].valid = 1;
                nv_line++;
            }
        }
        if (pclose_safe(fp, p_pid) == 0) {
            unsigned int sample_tick = __atomic_load_n(&tick_count, __ATOMIC_ACQUIRE);
            pthread_mutex_lock(&g_nv_samples_mutex);
            memcpy(g_nv_samples, local, sizeof(g_nv_samples));
            g_nv_samples_tick = sample_tick;
            pthread_mutex_unlock(&g_nv_samples_mutex);
            __atomic_store_n(&g_nv_samples_ready, 1, __ATOMIC_RELEASE);
        } else {
            pthread_mutex_lock(&g_nv_samples_mutex);
            memset(g_nv_samples, 0, sizeof(g_nv_samples));
            g_nv_samples_tick = 0;
            pthread_mutex_unlock(&g_nv_samples_mutex);
            __atomic_store_n(&g_nv_samples_ready, 0, __ATOMIC_RELEASE);
        }
    } else {
        pthread_mutex_lock(&g_nv_samples_mutex);
        memset(g_nv_samples, 0, sizeof(g_nv_samples));
        g_nv_samples_tick = 0;
        pthread_mutex_unlock(&g_nv_samples_mutex);
        __atomic_store_n(&g_nv_samples_ready, 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_nv_thread_running, 0, __ATOMIC_RELEASE);
    return NULL;
}

/* ============================ Data gathering ============================ */
/*
 * Refreshes every field of `d` for one render tick. Cheap sysctl(3)
 * reads happen every tick; expensive operations (pkg counts, PCI scan,
 * home-directory executable counts, GPU/swap/disk enumeration) are
 * cached in function-local statics and refreshed only every N ticks or
 * on the first call, to keep the render loop responsive. `d` is a
 * single long-lived instance reused every tick, not reallocated, so
 * fields that a given refresh path skips simply retain their previous
 * value rather than being reset.
 */
void gather_data(struct mon_data *d) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(d->time_str, sizeof(d->time_str), "%H:%M:%S", t);
    strftime(d->date_str, sizeof(d->date_str), "%Y-%m-%d", t);
    gethostname(d->host, sizeof(d->host));
    d->host[sizeof(d->host) - 1] = '\0';
    sanitize_str(d->host);

    struct timeval boottime; size_t size = sizeof(boottime);
    if (sysctlbyname("kern.boottime", &boottime, &size, NULL, 0) == 0) {
        long upt = now - boottime.tv_sec;
        snprintf(d->uptime_str, sizeof(d->uptime_str), "%ldh %ldm", upt / 3600, (upt % 3600) / 60);
    }
    getloadavg(d->load, 3);

    static char cpu_brand[256] = "";
    if (cpu_brand[0] == '\0') {
#if MONBSD_X86
        u_int regs[4]; do_cpuid(0x80000000, regs);
        if (regs[0] >= 0x80000004) {
            uint32_t *brand = (uint32_t *)cpu_brand;
            do_cpuid(0x80000002, regs); brand[0] = regs[0]; brand[1] = regs[1]; brand[2] = regs[2]; brand[3] = regs[3];
            do_cpuid(0x80000003, regs); brand[4] = regs[0]; brand[5] = regs[1]; brand[6] = regs[2]; brand[7] = regs[3];
            do_cpuid(0x80000004, regs); brand[8] = regs[0]; brand[9] = regs[1]; brand[10] = regs[2]; brand[11] = regs[3];
            cpu_brand[48] = '\0';
        } else {
            size = sizeof(cpu_brand); sysctlbyname("hw.model", cpu_brand, &size, NULL, 0);
        }
#else
        size = sizeof(cpu_brand); sysctlbyname("hw.model", cpu_brand, &size, NULL, 0);
#endif
    }
    strlcpy(d->cpu_model, cpu_brand, sizeof(d->cpu_model));
    sanitize_str(d->cpu_model);

    int freq; size = sizeof(freq);
    if (sysctlbyname("dev.cpu.0.freq", &freq, &size, NULL, 0) == 0) {
        if (tick_count % 5 == 0) {
            uint64_t tsc_freq = 0; size_t tsc_sz = sizeof(tsc_freq);
            sysctlbyname("machdep.tsc_freq", &tsc_freq, &tsc_sz, NULL, 0);
            double base_mhz = tsc_freq > 0 ? (double)tsc_freq / 1000000.0 : (double)freq;
            
            double new_live = direct_cpu_live_freq((double)freq, base_mhz);
            if (d->live_freq_mhz > 0) d->freq_trend = (new_live > d->live_freq_mhz ? 1 : (new_live < d->live_freq_mhz ? -1 : 0));
            d->live_freq_mhz = new_live;
        }
        if (tick_count % 10 == 0) d->cpu_freq_ghz = freq / 1000.0;
    }
    
    int ncpu = 0; size = sizeof(ncpu);
    if (sysctlbyname("kern.smp.cpus", &ncpu, &size, NULL, 0) != 0 || ncpu <= 0)
        ncpu = direct_cpu_cores();   /* x86 fallback; returns >= 1 */
    d->cpu_threads = ncpu;

    static int hw_initialized = 0;
    static long long cached_mem_total = 0;
    static int cached_pagesize = 0;

    if (!hw_initialized) {
        size = sizeof(cached_mem_total); sysctlbyname("hw.physmem", &cached_mem_total, &size, NULL, 0);
        size = sizeof(cached_pagesize); sysctlbyname("hw.pagesize", &cached_pagesize, &size, NULL, 0);
        hw_initialized = 1;
    }

    d->mem_total = cached_mem_total;
    unsigned int active = 0, wire = 0, v_free = 0;
    int pagesize = cached_pagesize;
    size = sizeof(active); sysctlbyname("vm.stats.vm.v_active_count", &active, &size, NULL, 0);
    size = sizeof(wire); sysctlbyname("vm.stats.vm.v_wire_count", &wire, &size, NULL, 0);
    size = sizeof(v_free); sysctlbyname("vm.stats.vm.v_free_count", &v_free, &size, NULL, 0);
    /* Widen to long long before adding to avoid unsigned-int wraparound
     * on systems with an extreme page count, then guard mem_total==0
     * (e.g. hw.physmem sysctl failed) to avoid a divide-by-zero. */
    d->mem_used = ((long long)active + (long long)wire) * pagesize;
    d->mem_usage = (d->mem_total > 0) ? (100.0 * d->mem_used / d->mem_total) : 0.0;

    static int pkg_ticks = 0;
    if (pkg_ticks-- <= 0) {
        pkg_ticks = 600;

        if (__atomic_load_n(&g_pkg_thread_running, __ATOMIC_ACQUIRE) == 0) {
            __atomic_store_n(&g_pkg_thread_running, 1, __ATOMIC_RELEASE);
            pthread_t t;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&t, &attr, update_pkg_counts_thread, NULL) != 0) {
                __atomic_store_n(&g_pkg_thread_running, 0, __ATOMIC_RELEASE);
            }
            pthread_attr_destroy(&attr);
        }
    }

    static int soft_ticks = 0;
    if (soft_ticks-- <= 0) {
        soft_ticks = 10;

        d->linux_count = 0;
        DIR *dir = opendir("/compat/linux/usr/bin");
        if (dir) { struct dirent *e; while ((e = readdir(dir))) if (e->d_name[0] != '.') d->linux_count++; closedir(dir); }

        static int cached_pci_count = -1;
        if (cached_pci_count == -1 || tick_count % 100 == 0) {
            int current_count = direct_pci_count();
            if (current_count >= 0) {
                cached_pci_count = current_count;
            }
        }
        d->pci_device_count = cached_pci_count;

        static int cached_user_bin_count = -1;
        if (cached_user_bin_count == -1 || tick_count % 100 == 0) {
            int current_user_bin_count = 0;
            if (d->home_dir[0]) {
                char probe[MAXPATHLEN];
                snprintf(probe, sizeof(probe), "%s/.local/bin", d->home_dir);
                current_user_bin_count += count_dir_executables(probe);
                snprintf(probe, sizeof(probe), "%s/bin", d->home_dir);
                current_user_bin_count += count_dir_executables(probe);
                snprintf(probe, sizeof(probe), "%s/local/bin", d->home_dir);
                current_user_bin_count += count_dir_executables(probe);
            }
            cached_user_bin_count = current_user_bin_count;
        }
        d->user_bin_count = cached_user_bin_count;
    }

    d->pkg_count = __atomic_load_n(&g_pkg_count, __ATOMIC_RELAXED);
    d->ports_count = __atomic_load_n(&g_ports_count, __ATOMIC_RELAXED);

    d->cpu_temp = direct_cpu_temp();
    
    int itmp; size = sizeof(itmp);
    if (sysctlbyname("hw.acpi.thermal.tz0.passive_cooling", &itmp, &size, NULL, 0) == 0) strlcpy(d->thermal_state, itmp > 0 ? "Passive" : "Active", sizeof(d->thermal_state));
    else strlcpy(d->thermal_state, "Normal", sizeof(d->thermal_state));

    if (sysctlbyname("dev.acpi_ibm.0.fan_speed", &itmp, &size, NULL, 0) != 0)
        if (sysctlbyname("dev.aibs.0.fan0.speed", &itmp, &size, NULL, 0) != 0) itmp = -1;
    if (itmp >= 0) snprintf(d->fan_status, sizeof(d->fan_status), "%d RPM", itmp);
    else strlcpy(d->fan_status, "No sensor detected", sizeof(d->fan_status));

    size = sizeof(d->cx_lowest);
    if (sysctlbyname("hw.acpi.cpu.cx_lowest", d->cx_lowest, &size, NULL, 0) != 0) strlcpy(d->cx_lowest, "N/A", sizeof(d->cx_lowest));
    sanitize_str(d->cx_lowest);
    size = sizeof(d->cx_usage);
    if (sysctlbyname("dev.cpu.0.cx_usage", d->cx_usage, &size, NULL, 0) != 0) strlcpy(d->cx_usage, "N/A", sizeof(d->cx_usage));
    sanitize_str(d->cx_usage);

    static int cached_powerd = 0;
    static int cached_powerdxx = 0;

    if (tick_count % 20 == 0) {
        /* Name-exact process scan via the kern.proc sysctl: unprivileged
         * under the default security.bsd.see_other_uids=1, immune to pidfile
         * permissions and PID recycling, and matches only "powerd"/"powerdxx"
         * exactly. */
        int mib[3];
        size_t len;
        struct kinfo_proc *kp;

        mib[0] = CTL_KERN;
        mib[1] = KERN_PROC;
        mib[2] = KERN_PROC_ALL;

        if (sysctl(mib, 3, NULL, &len, NULL, 0) == 0) {
            len = len * 4 / 3; /* Allocate extra buffer to prevent race conditions */
            kp = malloc(len);
            if (kp != NULL) {
                if (sysctl(mib, 3, kp, &len, NULL, 0) == 0) {
                    int found_powerd = 0;
                    int found_powerdxx = 0;
                    int nproc = len / sizeof(struct kinfo_proc);
                    for (int i = 0; i < nproc; i++) {
                        if (strcmp(kp[i].ki_comm, "powerd") == 0) found_powerd = 1;
                        else if (strcmp(kp[i].ki_comm, "powerdxx") == 0) found_powerdxx = 1;
                        if (found_powerd && found_powerdxx) break;
                    }
                    cached_powerd = found_powerd;
                    cached_powerdxx = found_powerdxx;
                }
                free(kp);
            }
        }
    }

    d->powerd_running = cached_powerd;
    d->powerdxx_running = cached_powerdxx;

    size = sizeof(itmp);
    if (sysctlbyname("hw.acpi.battery.state", &itmp, &size, NULL, 0) == 0) {
        if (itmp & ACPI_BATT_STAT_NOT_PRESENT) {
            d->has_battery = 0;
            d->bat_life = -1;
            strlcpy(d->bat_source, "AC Power", sizeof(d->bat_source));
            strlcpy(d->bat_state, "No battery", sizeof(d->bat_state));
        } else {
            d->has_battery = 1;
            size = sizeof(d->bat_life);
            if (sysctlbyname("hw.acpi.battery.life", &d->bat_life, &size, NULL, 0) != 0)
                d->bat_life = -1;
            if ((itmp & ACPI_BATT_STAT_DISCHARG) && (itmp & ACPI_BATT_STAT_CHARGING)) {
                strlcpy(d->bat_source, "AC Power", sizeof(d->bat_source));
                strlcpy(d->bat_state, "Invalid", sizeof(d->bat_state));
            } else if (itmp & ACPI_BATT_STAT_CRITICAL) {
                strlcpy(d->bat_source, "Battery", sizeof(d->bat_source));
                strlcpy(d->bat_state, "Critical", sizeof(d->bat_state));
            } else if (itmp & ACPI_BATT_STAT_DISCHARG) {
                strlcpy(d->bat_source, "Battery", sizeof(d->bat_source));
                strlcpy(d->bat_state, "Discharging", sizeof(d->bat_state));
            } else if (itmp & ACPI_BATT_STAT_CHARGING) {
                strlcpy(d->bat_source, "AC Power", sizeof(d->bat_source));
                strlcpy(d->bat_state, "Charging", sizeof(d->bat_state));
            } else {
                strlcpy(d->bat_source, "AC Power", sizeof(d->bat_source));
                if (d->bat_life >= 100)
                    strlcpy(d->bat_state, "Full", sizeof(d->bat_state));
                else
                    strlcpy(d->bat_state, "Idle", sizeof(d->bat_state));
            }
        }
    } else {
        d->has_battery = 0;
        d->bat_life = -1;
        strlcpy(d->bat_source, "AC Power", sizeof(d->bat_source));
        strlcpy(d->bat_state, "No battery", sizeof(d->bat_state));
    }

    size = sizeof(d->freq_levels);
    if (sysctlbyname("dev.cpu.0.freq_levels", d->freq_levels, &size, NULL, 0) == 0)
        sanitize_str(d->freq_levels);

    {
        static struct gpu_info_cache {
            char model[128];
            int is_nvidia;
        } g_cache[MAX_GPUS];
        static int g_cached_count = 0;
        static int g_init = 0;
        static int has_nvidia_smi = -1;

        if (!g_init) {
            pid_t p_pid;
            char *pciconf_argv[] = {"pciconf", "-lv", NULL};
            FILE *fp = popen_safe("/usr/sbin/pciconf", pciconf_argv, &p_pid);
            if (fp) {
                char line[256];
                int in_gpu = 0;
                int pending_nvidia = 0;
                while (fgets(line, sizeof(line), fp)) {
                    if (strstr(line, "class=0x03")) {
                        in_gpu = 1;
                        pending_nvidia = 0;
                        continue;
                    }
                    if (in_gpu && line[0] != ' ' && line[0] != '\t') {
                        in_gpu = 0;
                        pending_nvidia = 0;
                    }
                    if (!in_gpu) continue;
                    if (strstr(line, "vendor") && strstr(line, "=")) {
                        if (strstr(line, "NVIDIA") || strstr(line, "nvidia"))
                            pending_nvidia = 1;
                    }
                    if (strstr(line, "device") && strstr(line, "=") && g_cached_count < MAX_GPUS) {
                        char *start = strchr(line, '\'');
                        if (start) {
                            char *end = strchr(start + 1, '\'');
                            if (end) {
                                *end = '\0';
                                strlcpy(g_cache[g_cached_count].model, start + 1, sizeof(g_cache[g_cached_count].model));
                                sanitize_str(g_cache[g_cached_count].model);
                                g_cache[g_cached_count].is_nvidia = pending_nvidia ||
                                    (strstr(g_cache[g_cached_count].model, "NVIDIA") != NULL) ||
                                    (strstr(g_cache[g_cached_count].model, "GeForce") != NULL) ||
                                    (strstr(g_cache[g_cached_count].model, "Quadro") != NULL) ||
                                    (strstr(g_cache[g_cached_count].model, "Tesla") != NULL);
                                g_cached_count++;
                                in_gpu = 0;
                                pending_nvidia = 0;
                            }
                        }
                    }
                }
                if (pclose_safe(fp, p_pid) != 0)
                    g_cached_count = 0;   /* discard partial scan on failure */
            }
            if (has_nvidia_smi < 0) {
                has_nvidia_smi = (access(NVIDIA_SMI_PATH, X_OK) == 0) ? 1 : 0;
            }
            g_init = 1;
        }
        d->gpu_count = g_cached_count;

        for (int i = 0; i < d->gpu_count; i++) {
            strlcpy(d->gpus[i].model, g_cache[i].model, sizeof(d->gpus[i].model));
            d->gpus[i].active = 1;
            d->gpus[i].freq_mhz = 0; d->gpus[i].temp_c = -1;
            d->gpus[i].util_pct = -1; d->gpus[i].vram_used_mib = -1; d->gpus[i].vram_total_mib = -1;
        }

        int have_nvidia = 0;
        for (int i = 0; i < d->gpu_count; i++)
            if (g_cache[i].is_nvidia) { have_nvidia = 1; break; }

        if (has_nvidia_smi && have_nvidia && tick_count % 5 == 0 &&
            __atomic_load_n(&g_nv_thread_running, __ATOMIC_ACQUIRE) == 0) {
            __atomic_store_n(&g_nv_thread_running, 1, __ATOMIC_RELEASE);
            pthread_t t;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&t, &attr, update_nvidia_thread, NULL) != 0) {
                __atomic_store_n(&g_nv_thread_running, 0, __ATOMIC_RELEASE);
            }
            pthread_attr_destroy(&attr);
        }

        if (__atomic_load_n(&g_nv_samples_ready, __ATOMIC_ACQUIRE)) {
            struct nv_sample samples[MAX_GPUS];
            unsigned int sample_tick;
            pthread_mutex_lock(&g_nv_samples_mutex);
            memcpy(samples, g_nv_samples, sizeof(samples));
            sample_tick = g_nv_samples_tick;
            pthread_mutex_unlock(&g_nv_samples_mutex);
            if (tick_count - sample_tick <= 10) {
                int nv_line = 0;
                for (int i = 0; i < d->gpu_count; i++) {
                    if (!g_cache[i].is_nvidia) continue;
                    if (nv_line < MAX_GPUS && samples[nv_line].valid) {
                        d->gpus[i].util_pct = samples[nv_line].util;
                        d->gpus[i].vram_used_mib = samples[nv_line].mem_used;
                        d->gpus[i].vram_total_mib = samples[nv_line].mem_total;
                        d->gpus[i].temp_c = samples[nv_line].temp;
                    }
                    nv_line++;
                }
            }
        }

        for (int i = 0; i < d->gpu_count; i++) {
            if (g_cache[i].is_nvidia) {
                if (d->gpus[i].util_pct < 0) {
                    char sysnode[64]; int gtemp; size_t sz = sizeof(gtemp);
                    int nv_idx = 0;
                    for (int j = 0; j < i; j++)
                        if (g_cache[j].is_nvidia) nv_idx++;
                    snprintf(sysnode, sizeof(sysnode), "dev.nvidia.%d.temperature", nv_idx);
                    if (sysctlbyname(sysnode, &gtemp, &sz, NULL, 0) == 0) d->gpus[i].temp_c = gtemp;
                }
            } else {
                int gfreq; size_t sz = sizeof(gfreq);
                int drm_idx = 0;
                for (int j = 0; j < i; j++)
                    if (!g_cache[j].is_nvidia) drm_idx++;
                char node[64];
                snprintf(node, sizeof(node), "dev.drm.%d.gt_cur_freq_mhz", drm_idx);
                if (sysctlbyname(node, &gfreq, &sz, NULL, 0) == 0) d->gpus[i].freq_mhz = gfreq;
                else {
                    snprintf(node, sizeof(node), "dev.drmn.%d.gt_cur_freq_mhz", drm_idx);
                    if (sysctlbyname(node, &gfreq, &sz, NULL, 0) == 0) d->gpus[i].freq_mhz = gfreq;
                }
            }
        }
    }

    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long cp_time[CPUSTATES] = {0}; size = sizeof(cp_time); sysctlbyname("kern.cp_time", cp_time, &size, NULL, 0);
    
    int ifc = 0; size = sizeof(ifc); sysctlbyname("net.link.generic.system.ifcount", &ifc, &size, NULL, 0);
    d->if_count = 0;

    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == -1) {
        ifaddr = NULL; // Ensure it's NULL if it fails, although getifaddrs usually doesn't touch it on failure
    }

    for (int i = 1; i <= ifc && d->if_count < MAX_NET_IF; i++) {
        int mib[6] = {CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_IFDATA, i, IFDATA_GENERAL};
        struct ifmibdata ifmd; size = sizeof(ifmd);
        if (sysctl(mib, 6, &ifmd, &size, NULL, 0) == 0) {
            if (strcmp(ifmd.ifmd_name, "lo0") == 0) continue;
            if (ifmd.ifmd_data.ifi_link_state != LINK_STATE_UP) continue;
            char ip_buf[INET_ADDRSTRLEN];
            get_ip_address(ifaddr, ifmd.ifmd_name, ip_buf, sizeof(ip_buf));
            if (strcmp(ip_buf, "Unknown") == 0) continue;
            strlcpy(d->ifaces[d->if_count].name, ifmd.ifmd_name, sizeof(d->ifaces[d->if_count].name));
            strlcpy(d->ifaces[d->if_count].ip, ip_buf, sizeof(d->ifaces[d->if_count].ip));
            d->ifaces[d->if_count].total_rx_gb = ifmd.ifmd_data.ifi_ibytes / (1024.0*1024.0*1024.0);
            d->ifaces[d->if_count].total_tx_gb = ifmd.ifmd_data.ifi_obytes / (1024.0*1024.0*1024.0);
            d->ifaces[d->if_count].is_wifi = (strncmp(ifmd.ifmd_name, "wlan", 4) == 0);
            d->ifaces[d->if_count].active = 1;
            d->ifaces[d->if_count].ssid[0] = '\0';
            if (d->ifaces[d->if_count].is_wifi) {
                struct ssid_cache_entry *se = ssid_cache_lookup(ifmd.ifmd_name);
                if (se == NULL || tick_count - se->last_tick >= 50) {
                    char ssid_buf[64];
                    refresh_wifi_ssid(ifmd.ifmd_name, ssid_buf, sizeof(ssid_buf));
                    ssid_cache_store(ifmd.ifmd_name, ssid_buf, tick_count);
                    se = ssid_cache_lookup(ifmd.ifmd_name);
                }
                if (se != NULL)
                    strlcpy(d->ifaces[d->if_count].ssid, se->ssid, sizeof(d->ifaces[d->if_count].ssid));
            }
            
            int oidx = (hist_idx + 1) % HISTORY_SIZE;
            if (history[oidx].valid) {
                double dt = (ts.tv_sec - history[oidx].ts.tv_sec) + (ts.tv_nsec - history[oidx].ts.tv_nsec) / 1e9;
                if (dt > 0.1) {
                    for(int h=0; h<history[oidx].if_count; h++) {
                        if (strcmp(history[oidx].ifaces[h].name, ifmd.ifmd_name) == 0) {
                            d->ifaces[d->if_count].rx_rate_kb = (ifmd.ifmd_data.ifi_ibytes - history[oidx].ifaces[h].rx) / 1024.0 / dt;
                            d->ifaces[d->if_count].tx_rate_kb = (ifmd.ifmd_data.ifi_obytes - history[oidx].ifaces[h].tx) / 1024.0 / dt;
                            break;
                        }
                    }
                }
            }
            d->if_count++;
        }
    }

    if (ifaddr != NULL) {
        freeifaddrs(ifaddr);
    }

    int oidx = (hist_idx + 1) % HISTORY_SIZE;
    if (history[oidx].valid && tick_count % 10 == 0) {
        double dt = (ts.tv_sec - history[oidx].ts.tv_sec) + (ts.tv_nsec - history[oidx].ts.tv_nsec) / 1e9;
        if (dt > 0.1) {
            long tot = 0, ltot = 0; for (int i = 0; i < CPUSTATES; i++) { tot += cp_time[i]; ltot += history[oidx].cp_time[i]; }
            if (tot > ltot) d->cpu_usage = 100.0 * (1.0 - (double)(cp_time[CP_IDLE] - history[oidx].cp_time[CP_IDLE]) / (tot - ltot));
        }
    }

    memcpy(history[hist_idx].cp_time, cp_time, sizeof(cp_time));
    history[hist_idx].if_count = d->if_count;
    for(int h=0; h<d->if_count; h++) {
        strlcpy(history[hist_idx].ifaces[h].name, d->ifaces[h].name, sizeof(history[hist_idx].ifaces[h].name));
        history[hist_idx].ifaces[h].rx = (long long)(d->ifaces[h].total_rx_gb * 1024.0 * 1024.0 * 1024.0);
        history[hist_idx].ifaces[h].tx = (long long)(d->ifaces[h].total_tx_gb * 1024.0 * 1024.0 * 1024.0);
    }
    history[hist_idx].ts = ts; history[hist_idx].valid = 1; hist_idx = (hist_idx + 1) % HISTORY_SIZE;

    static long long cached_swap_total = 0, cached_swap_used = 0;
    static int swap_init = 0;
    if (!swap_init || tick_count % 50 == 0) {
        pid_t swapinfo_pid;
        char *swapinfo_argv[] = {"swapinfo", "-k", NULL};
        FILE *fsw = popen_safe("/usr/sbin/swapinfo", swapinfo_argv, &swapinfo_pid);
        if (fsw) {
            char line[1024];
            long long total = 0, used = 0;
            if (fgets(line, sizeof(line), fsw)) { // skip header
                while (fgets(line, sizeof(line), fsw)) {
                    if (strncasecmp(line, "Total", 5) == 0) continue;
                    char device[64];
                    long long t, u;
                    if (sscanf(line, "%63s %lld %lld", device, &t, &u) == 3) {
                        total += t * 1024;
                        used += u * 1024;
                    }
                }
            }
            if (pclose_safe(fsw, swapinfo_pid) != -1) {
                cached_swap_total = total;
                cached_swap_used = used;
            }
        }
        swap_init = 1;
    }
    d->swap_total = cached_swap_total; d->swap_used = cached_swap_used;

    d->swap_usage = (d->swap_total > 0) ? (100.0 * d->swap_used / d->swap_total) : 0;

    static struct disk_entry cached_disks[MAX_DISKS];
    static int cached_disk_count = 0;
    static int disk_init = 0;

    if (!disk_init || tick_count % 50 == 0) {
        cached_disk_count = 0;
        int nfs = getfsstat(NULL, 0, MNT_NOWAIT);
        if (nfs > 0 && (size_t)nfs <= SIZE_MAX / sizeof(struct statfs)) {
            struct statfs *fs = malloc(sizeof(struct statfs) * (size_t)nfs);
            if (fs != NULL) {
                nfs = getfsstat(fs, sizeof(struct statfs) * (size_t)nfs, MNT_NOWAIT);
                if (nfs < 0) nfs = 0;
                const char *targets[] = {"/", "/boot/efi", "/tmp", "/zroot", d->home_path};
                for (int j = 0; j < 5; j++) {
                    if (targets[j][0] == '\0') continue;
                    int dup = 0;
                    for (int k = 0; k < j; k++)
                        if (strcmp(targets[j], targets[k]) == 0) { dup = 1; break; }
                    if (dup) continue;
                    for (int i = 0; i < nfs && cached_disk_count < MAX_DISKS; i++) {
                        if (strcmp(fs[i].f_mntonname, targets[j]) == 0) {
                            if (fs[i].f_blocks > 0) {
                                strlcpy(cached_disks[cached_disk_count].mount, fs[i].f_mntonname, sizeof(cached_disks[cached_disk_count].mount));
                                cached_disks[cached_disk_count].total_bytes = (long long)fs[i].f_blocks * fs[i].f_bsize;
                                cached_disks[cached_disk_count].used_bytes = (long long)(fs[i].f_blocks - fs[i].f_bfree) * fs[i].f_bsize;
                                cached_disks[cached_disk_count].usage = 100.0 * cached_disks[cached_disk_count].used_bytes / cached_disks[cached_disk_count].total_bytes;
                                cached_disk_count++;
                            }
                            break;
                        }
                    }
                }
                free(fs);
            }
        }
        disk_init = 1;
    }

    d->disk_count = cached_disk_count;
    for (int i = 0; i < d->disk_count; i++) {
        strlcpy(d->disks[i].mount, cached_disks[i].mount, sizeof(d->disks[i].mount));
        d->disks[i].total_bytes = cached_disks[i].total_bytes;
        d->disks[i].used_bytes = cached_disks[i].used_bytes;
        d->disks[i].usage = cached_disks[i].usage;
    }
}

/* ============================== Rendering ============================== */
/*
 * The draw_box/print_val/print_bar/render_*_box functions below are pure output: they
 * only read from `struct mon_data *d` and write ANSI escape sequences
 * to stdout, never allocating or mutating shared state. Every helper
 * takes an explicit width/height and clips its output to it (see the
 * `w < N` / `r < box_bot` guards throughout), so a too-small terminal
 * degrades to fewer visible rows rather than misrendering.
 */
void draw_box(int y, int x, int h, int w, const char *title) {
    if (w < 5) return;
    int tlen = strlen(title); if (tlen > w - 6) tlen = w - 6;
    move_cursor(y, x); printf("┌─ %.*s ", tlen, title); for (int i = 0; i < w - 5 - tlen; i++) printf("─"); printf("┐");
    for (int i = 1; i < h - 1; i++) { move_cursor(y + i, x); printf("│"); move_cursor(y + i, x + w - 1); printf("│"); }
    move_cursor(y + h - 1, x); printf("└"); for (int i = 0; i < w - 2; i++) printf("─"); printf("┘");
}

void print_val(int y, int x, int w, const char *lbl, const char *val) {
    if (w < 5 || y < 1) return;
    move_cursor(y, x);
    set_color(37);
    int lbl_len = strlen(lbl);
    if (lbl_len > w - 6) lbl_len = w - 6;
    if (lbl_len < 0) lbl_len = 0;
    printf("%.*s", lbl_len, lbl); reset_color();
    int avail = w - lbl_len - 1;
    if (avail <= 0) return;
    int vlen = strlen(val);
    if (vlen > avail - 1) {
        if (avail >= 4) printf(" %*.*s..", avail - 3, avail - 3, val);
        else printf(" %.*s", avail - 1, val);
    } else {
        printf("%*s ", avail, val);
    }
}

void print_bar(int y, int x, int w, double pct, const char *lbl) {
    if (w < 15 || y < 1) return;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    move_cursor(y, x); set_color(37); 
    int lbl_len = strlen(lbl); if (lbl_len > w / 2) lbl_len = w / 2;
    printf("%.*s ", lbl_len, lbl); reset_color();
    int bar_w = w - lbl_len - 10;
    if (bar_w < 5) bar_w = 5;
    printf("[");
    int filled = (int)(pct / 100.0 * bar_w);
    set_color(pct > 80 ? 31 : (pct > 50 ? 33 : 32));
    for (int i = 0; i < filled; i++) printf("█");
    reset_color();
    for (int i = 0; i < bar_w - filled; i++) printf("░");
    printf("] %5.1f%%", pct);
}

static void render_system_box(struct mon_data *d, int box_top, int box_bot, int h, int col_w) {
    draw_box(box_top, 1, h, col_w, "SYSTEM");
    char buf[256];
    int r = box_top + 2;

    print_val(r++, 3, col_w - 4, "Time:", d->time_str);
    print_val(r++, 3, col_w - 4, "Date:", d->date_str);
    print_val(r++, 3, col_w - 4, "Host:", d->host);
    print_val(r++, 3, col_w - 4, "Uptime:", d->uptime_str);
    snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", d->load[0], d->load[1], d->load[2]);
    print_val(r++, 3, col_w - 4, "Load:", buf);
    r++;
    if (r < box_bot) draw_heading(r++, 3, col_w - 4, 36, "CPU");
    if (r < box_bot) draw_heading(r++, 3, col_w - 4, 0, d->cpu_model);
    snprintf(buf, sizeof(buf), "%.2f GHz", d->cpu_freq_ghz);
    if (r < box_bot) print_val(r++, 3, col_w - 4, "Frequency:", buf);
    snprintf(buf, sizeof(buf), "%d", d->cpu_threads);
    if (r < box_bot) print_val(r++, 3, col_w - 4, "Threads:", buf);
    if (r < box_bot) print_bar(r++, 3, col_w - 4, d->cpu_usage, "Usage");
    r++;
    if (r < box_bot) draw_heading(r++, 3, col_w - 4, 36, "MEMORY");
    snprintf(buf, sizeof(buf), "%.2f GB", d->mem_total / (1024.0*1024.0*1024.0));
    if (r < box_bot) print_val(r++, 3, col_w - 4, "Total:", buf);
    snprintf(buf, sizeof(buf), "%.2f GB", d->mem_used / (1024.0*1024.0*1024.0));
    if (r < box_bot) print_val(r++, 3, col_w - 4, "Used:", buf);
    if (r < box_bot) print_bar(r++, 3, col_w - 4, d->mem_usage, "Usage");
    r++;
    if (r < box_bot) draw_heading(r++, 3, col_w - 4, 36, "SOFTWARE & BUS");
    if (d->pci_device_count >= 0) snprintf(buf, sizeof(buf), "%d devices", d->pci_device_count);
    else strlcpy(buf, "N/A", sizeof(buf));
    if (r < box_bot) print_val(r++, 3, col_w - 4, "PCI Devices:", buf);
    if (g_privileged_note_visible && r < box_bot) print_val(r++, 3, col_w - 4, "Note:", g_privileged_note);
    if (d->pkg_count >= 0) snprintf(buf, sizeof(buf), "%d installed", d->pkg_count);
    else strlcpy(buf, "N/A", sizeof(buf));
    if (r < box_bot) print_val(r++, 3, col_w - 4, "pkg Packages:", buf);
    if (d->ports_count >= 0) snprintf(buf, sizeof(buf), "%d built", d->ports_count);
    else strlcpy(buf, "N/A", sizeof(buf));
    if (r < box_bot) print_val(r++, 3, col_w - 4, "Ports:", buf);
    snprintf(buf, sizeof(buf), "%d program(s)", d->linux_count);
    if (r < box_bot) print_val(r++, 3, col_w - 4, "Linux Compat:", buf);
    if (d->user_bin_count > 0 && r < box_bot) {
        snprintf(buf, sizeof(buf), "%d program(s)", d->user_bin_count);
        print_val(r++, 3, col_w - 4, "User Binaries:", buf);
    }
}

static void render_thermal_power_box(struct mon_data *d, int box_top, int box_bot, int h, int col_w) {
    int c2x = col_w + 1;
    int c2w = col_w;
    int c2inner = c2w - 4;
    draw_box(box_top, c2x, h, c2w, "THERMAL & POWER");
    char buf[256];
    int r = box_top + 2;
    if (r < box_bot) draw_heading(r++, c2x + 2, c2inner, 36, "THERMAL");
    if (d->cpu_temp >= 0) snprintf(buf, sizeof(buf), "%.1f °C", d->cpu_temp);
    else strlcpy(buf, "N/A", sizeof(buf));
    if (r < box_bot) print_val(r++, c2x + 2, c2inner, "CPU Temp:", buf);
    if (r < box_bot) print_val(r++, c2x + 2, c2inner, "State:", d->thermal_state);
    snprintf(buf, sizeof(buf), "%.0f MHz (%c)", d->live_freq_mhz, d->freq_trend > 0 ? '+' : (d->freq_trend < 0 ? '-' : '='));
    if (r < box_bot) print_val(r++, c2x + 2, c2inner, "Live Freq:", buf);
    r++;

    if (r < box_bot) draw_heading(r++, c2x + 2, c2inner, 36, "GPU HARDWARE");
    if (d->gpu_count == 0) {
        if (r < box_bot) print_val(r++, c2x + 2, c2inner, "  Status:", "No GPU detected");
    }
    for (int i = 0; i < d->gpu_count && r < box_bot; i++) {
        draw_heading(r++, c2x + 2, c2inner, 0, d->gpus[i].model);

        if (d->gpus[i].util_pct >= 0) {
            snprintf(buf, sizeof(buf), "%.0f%%", d->gpus[i].util_pct);
            if (d->gpus[i].temp_c >= 0) {
                size_t len = strlen(buf);
                snprintf(buf + len, sizeof(buf) - len, " | %.0f C", d->gpus[i].temp_c);
            }
        } else if (d->gpus[i].temp_c >= 0 && d->gpus[i].freq_mhz > 0) {
            snprintf(buf, sizeof(buf), "%.0f MHz | %.0f C", d->gpus[i].freq_mhz, d->gpus[i].temp_c);
        } else if (d->gpus[i].temp_c >= 0) {
            snprintf(buf, sizeof(buf), "%.0f C", d->gpus[i].temp_c);
        } else if (d->gpus[i].freq_mhz > 0) {
            snprintf(buf, sizeof(buf), "%.0f MHz", d->gpus[i].freq_mhz);
        } else {
            strlcpy(buf, "Active", sizeof(buf));
        }
        if (r < box_bot) print_val(r++, c2x + 2, c2inner, "  Status:", buf);

        if (d->gpus[i].util_pct >= 0 && r < box_bot) {
            print_bar(r++, c2x + 2, c2inner, d->gpus[i].util_pct, "  GPU");
        }

        if (d->gpus[i].vram_used_mib >= 0 && d->gpus[i].vram_total_mib > 0 && r < box_bot) {
            double vram_pct = 100.0 * d->gpus[i].vram_used_mib / d->gpus[i].vram_total_mib;
            snprintf(buf, sizeof(buf), "%ld/%ldM", d->gpus[i].vram_used_mib, d->gpus[i].vram_total_mib);
            print_bar(r++, c2x + 2, c2inner, vram_pct, buf);
        }
    }
    r++;

    if (r < box_bot) draw_heading(r++, c2x + 2, c2inner, 36, "POWER & ACPI");
    if (r < box_bot) print_val(r++, c2x + 2, c2inner, "powerd:", d->powerd_running ? "Running ✓" : "Stopped ✗");
    if (r < box_bot) print_val(r++, c2x + 2, c2inner, "powerdxx:", d->powerdxx_running ? "Running ✓" : "Stopped ✗");
    if (r < box_bot) print_val(r++, c2x + 2, c2inner, "Cx Lowest:", d->cx_lowest);
    if (r < box_bot) {
        char cx_buf[256];
        snprintf(cx_buf, sizeof(cx_buf), "Cx Usage: %s", d->cx_usage);
        draw_heading(r++, c2x + 2, c2inner, 0, cx_buf);
    }
    r++;
    if (r < box_bot) draw_heading(r++, c2x + 2, c2inner, 36, "BATTERY");
    if (r < box_bot) print_val(r++, c2x + 2, c2inner, "Source:", d->bat_source);
    if (d->has_battery && d->bat_life >= 0 && r < box_bot)
        print_bar(r++, c2x + 2, c2inner, (double)d->bat_life, "Bat");
    if (r < box_bot) print_val(r++, c2x + 2, c2inner, "State:", d->bat_state);
    r++;
    if (r < box_bot) draw_heading(r++, c2x + 2, c2inner, 36, "FREQ RANGE");
    char *pp = d->freq_levels;
    while (*pp && r < box_bot) {
        char level[32]; int n; if (sscanf(pp, "%31s%n", level, &n) != 1) break;
        char *slash = strchr(level, '/');
        if (slash != NULL) *slash = '\0';   /* strip "/power_mW" suffix */
        char l_buf[64]; snprintf(l_buf, sizeof(l_buf), "%s MHz", level);
        draw_heading(r++, c2x + 4, c2inner - 2, 0, l_buf);
        pp += n; while (*pp == ' ') pp++;
    }
}

static void render_network_disks_box(struct mon_data *d, int box_top, int box_bot, int h, int col_w) {
    int c3x = 2 * col_w + 1;
    int c3w = term_width - 2 * col_w;
    int c3inner = c3w - 4;
    draw_box(box_top, c3x, h, c3w, "NETWORK & DISKS");
    char buf[256];
    int r = box_top + 2;

    for (int i = 0; i < d->if_count && r < box_bot; i++) {
        char n_buf[64];
        snprintf(n_buf, sizeof(n_buf), "NET: %s (%s)", d->ifaces[i].name, d->ifaces[i].is_wifi ? "WiFi" : "Ethernet");
        draw_heading(r++, c3x + 2, c3inner, 36, n_buf);
        if (r < box_bot) print_val(r++, c3x + 2, c3inner, "IP:", d->ifaces[i].ip);
        if (d->ifaces[i].is_wifi && d->ifaces[i].ssid[0] != '\0' && r < box_bot)
            print_val(r++, c3x + 2, c3inner, "SSID:", d->ifaces[i].ssid);
        snprintf(buf, sizeof(buf), "%.2f KB/s", d->ifaces[i].rx_rate_kb);
        if (r < box_bot) print_val(r++, c3x + 2, c3inner, "Down:", buf);
        snprintf(buf, sizeof(buf), "%.2f KB/s", d->ifaces[i].tx_rate_kb);
        if (r < box_bot) print_val(r++, c3x + 2, c3inner, "Up:", buf);
        snprintf(buf, sizeof(buf), "%.2f GB", d->ifaces[i].total_rx_gb);
        if (r < box_bot) print_val(r++, c3x + 2, c3inner, "Total Rx:", buf);
        snprintf(buf, sizeof(buf), "%.2f GB", d->ifaces[i].total_tx_gb);
        if (r < box_bot) print_val(r++, c3x + 2, c3inner, "Total Tx:", buf);
        r++;
    }

    if (r < box_bot) draw_heading(r++, c3x + 2, c3inner, 36, "SWAP");
    snprintf(buf, sizeof(buf), "%.2f GB", d->swap_total / (1024.0*1024.0*1024.0));
    if (r < box_bot) print_val(r++, c3x + 2, c3inner, "Total:", buf);
    if (r < box_bot) print_bar(r++, c3x + 2, c3inner, d->swap_usage, "Usage");
    r++;

    if (r < box_bot) draw_heading(r++, c3x + 2, c3inner, 36, "DISKS");
    for (int i = 0; i < d->disk_count && r < box_bot; i++) {
        draw_heading(r++, c3x + 2, c3inner, 0, d->disks[i].mount);
        if (r < box_bot) {
            snprintf(buf, sizeof(buf), "%.1f/%.1fG", d->disks[i].used_bytes / (1024.0*1024.0*1024.0), d->disks[i].total_bytes / (1024.0*1024.0*1024.0));
            print_bar(r++, c3x + 2, c3inner, d->disks[i].usage, buf);
        }
    }
}

void render(struct mon_data *d) {
    int col_w = term_width / 3 - 1; if (col_w < 30) col_w = 30;
    int h = term_height - 6;
    int box_top = 5;
    int box_bot = box_top + h - 1;

    move_cursor(2, (term_width - 26) / 2); printf("║ FreeBSD System Monitor ║");
    move_cursor(3, (term_width - 26) / 2); printf("╚════════════════════════╝");

    render_system_box(d, box_top, box_bot, h, col_w);
    render_thermal_power_box(d, box_top, box_bot, h, col_w);
    render_network_disks_box(d, box_top, box_bot, h, col_w);
}

/* ================================ main ================================ */
/*
 * Resolves the invoking (pre-sudo) user's home directory and its mount
 * point, sanitizes the environment down to a fixed PATH plus a
 * validated TERM, then drives the raw-mode render loop until 'q' is
 * pressed. Must run before enable_raw_mode()/gather_data() so that
 * privilege-sensitive setup (env sanitization) happens first.
 */
int main() {
    char resolved_home[MAXPATHLEN] = "";
    char resolved_home_dir[MAXPATHLEN] = "";
    {
        uid_t target_uid = getuid();
        if (target_uid == 0) {
            const char *sudo_uid_str = getenv("SUDO_UID");
            if (sudo_uid_str != NULL) {
                char *endp;
                errno = 0;
                unsigned long v = strtoul(sudo_uid_str, &endp, 10);
                if (errno == 0 &&
                    endp != sudo_uid_str &&
                    *endp == '\0' &&
                    v <= (unsigned long)((uid_t)-1)) {
                    target_uid = (uid_t)v;
                }
            }
        }
        struct passwd *pw = getpwuid(target_uid);
        if (pw != NULL && pw->pw_dir != NULL) {
            strlcpy(resolved_home_dir, pw->pw_dir, sizeof(resolved_home_dir));
            struct statfs home_fs;
            if (statfs(pw->pw_dir, &home_fs) == 0) {
                if (!(home_fs.f_mntonname[0] == '/' && home_fs.f_mntonname[1] == '\0')) {
                    strlcpy(resolved_home, home_fs.f_mntonname, sizeof(resolved_home));
                }
            } else {
                if (!(pw->pw_dir[0] == '/' && pw->pw_dir[1] == '\0')) {
                    strlcpy(resolved_home, pw->pw_dir, sizeof(resolved_home));
                }
            }
        }
    }

    const char *term_env = getenv("TERM");
    char *term = NULL;
    if (term_env != NULL) {
        int valid = 1;
        for (int i = 0; term_env[i] != '\0' && i < 64; i++) {
            char c = term_env[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) &&
                c != '-' && c != '_' && c != '.' && c != '+') {
                valid = 0;
                break;
            }
        }
        if (valid) {
            term = strndup(term_env, 64);
        }
    }

    if (clearenv() != 0 ||
        setenv("PATH", "/bin:/usr/bin:/sbin:/usr/sbin:/usr/local/sbin:/usr/local/bin", 1) != 0 ||
        (term != NULL && setenv("TERM", term, 1) != 0)) {
        fprintf(stderr, "Failed to sanitize environment: %s\n", strerror(errno));
        free(term);
        exit(1);
    }
    free(term);

#if MONBSD_X86
    /* Acquire privileged device fds while still euid root. Credentials are
     * checked at open(2) time, so later RDMSR ioctls and I/O-port access
     * work after the privilege drop. O_CLOEXEC keeps them out of children. */
    g_cpuctl_fd = open("/dev/cpuctl0", O_RDWR | O_CLOEXEC);
    g_io_fd = open("/dev/io", O_RDWR | O_CLOEXEC);
#endif

    /* Drop privileges permanently before starting the UI; fail closed. */
    if (geteuid() == 0) {
        uid_t ruid = getuid();
        gid_t rgid = getgid();
        if (setresgid(rgid, rgid, rgid) != 0 || setresuid(ruid, ruid, ruid) != 0) {
            fprintf(stderr, "monbsd: failed to drop privileges: %s\n", strerror(errno));
            exit(1);
        }
    }

#if MONBSD_X86
    if (g_cpuctl_fd < 0 || g_io_fd < 0) {
        fprintf(stderr, "monbsd: privileged CPU/I/O device access unavailable; some metrics will be disabled\n");
        snprintf(g_privileged_note, sizeof(g_privileged_note),
            "no /dev/cpuctl0 or /dev/io: CPU Temp/PCI via fallback");
        g_privileged_note_visible = 1;
    }
#endif

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "monbsd: stdin and stdout must be a terminal\n");
        exit(1);
    }

    struct mon_data d = {0};
    strlcpy(d.home_path, resolved_home, sizeof(d.home_path));
    strlcpy(d.home_dir, resolved_home_dir, sizeof(d.home_dir));
    enable_raw_mode();
    signal(SIGWINCH, handle_sigwinch);
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handle_exit_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;    /* no SA_RESTART: let usleep()/read() return early */
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);
    }
    get_terminal_size();
    clear_screen();
    while (1) {
        if (exit_signo != 0) { clear_screen(); exit(128 + exit_signo); }
        if (resize_pending) { get_terminal_size(); resize_pending = 0; clear_screen(); }
        gather_data(&d);
        printf("\033[?2026h");  // Begin synchronized update
        printf("\033[?25l");    // Hide cursor during redraw
        render(&d);
        move_cursor(term_height, 1);
        printf("%*s", term_width, "");
        move_cursor(term_height, 1);
        printf(" 'q' to quit | %s | Tick: %u", VERSION, ++tick_count);
        printf("\033[?25h");    // Show cursor
        printf("\033[?2026l");  // End synchronized update
        fflush(stdout);
        char c; if (read(STDIN_FILENO, &c, 1) > 0) if (c == 'q' || c == 'Q' || c == 3) { clear_screen(); exit(0); }
        usleep(100000);
    }
    return 0;
}
