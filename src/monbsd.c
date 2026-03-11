#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
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
#include <sys/vmmeter.h>
#include <machine/cpufunc.h>
#include <sys/ioccom.h>
#include <sys/cpuctl.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pwd.h>

#define VERSION "0.1.0"
#define HISTORY_SIZE 10
#define MAX_DISKS 8
#define MAX_NET_IF 4
#define MAX_GPUS 2

struct gpu_data {
    char model[128];
    double freq_mhz;
    double temp_c;
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

struct mon_data {
    char time_str[16];
    char date_str[16];
    char host[MAXHOSTNAMELEN];
    char uptime_str[32];
    double load[3];
    char cpu_model[256];
    double cpu_freq_ghz; 
    int cpu_cores;
    double cpu_usage;
    long long mem_total, mem_used;
    double mem_usage;
    int pkg_count, ports_count, linux_count;
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
};

struct iface_history {
    char name[32];
    long long rx, tx;
    struct timespec ts;
    int valid;
};

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
unsigned int tick_count = 0;

void move_cursor(int y, int x) { printf("\033[%d;%dH", y, x); }
void set_color(int color) { printf("\033[%dm", color); }
void reset_color() { printf("\033[0m"); }
void clear_screen() { printf("\033[2J\033[H"); }

void get_terminal_size() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        term_width = ws.ws_col;
        term_height = ws.ws_row;
    }
}

void handle_sigwinch(int sig) { (void)sig; resize_pending = 1; }

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

void get_ip_address(const char *ifname, char *ip_buf) {
    struct ifaddrs *ifaddr, *ifa;
    strcpy(ip_buf, "Unknown");
    if (getifaddrs(&ifaddr) == -1) return;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_INET && strcmp(ifa->ifa_name, ifname) == 0) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip_buf, INET_ADDRSTRLEN);
            break;
        }
    }
    freeifaddrs(ifaddr);
}

int direct_cpu_cores() {
    u_int regs[4];
    do_cpuid(1, regs);
    int cores = (regs[1] >> 16) & 0xFF;
    return cores > 0 ? cores : 1;
}

double direct_cpu_temp() {
    int fd = open("/dev/cpuctl0", O_RDWR);
    if (fd >= 0) {
        cpuctl_msr_args_t args;
        args.msr = 0x1A2; // MSR_TEMPERATURE_TARGET
        int tjmax = 100;
        if (ioctl(fd, CPUCTL_RDMSR, &args) == 0) tjmax = (args.data >> 16) & 0xFF;
        args.msr = 0x19C; // IA32_THERM_STATUS
        if (ioctl(fd, CPUCTL_RDMSR, &args) == 0) {
            close(fd);
            int temp_offset = (args.data >> 16) & 0x7F;
            return (double)(tjmax - temp_offset);
        }
        close(fd);
    }
    int temp; size_t size = sizeof(temp);
    if (sysctlbyname("hw.acpi.thermal.tz0.temperature", &temp, &size, NULL, 0) == 0)
        return (temp - 2732.0) / 10.0;
    return -1.0;
}

double direct_cpu_live_freq(double fallback_freq, double base_freq) {
    static uint64_t last_mperf = 0, last_aperf = 0;
    int fd = open("/dev/cpuctl0", O_RDWR);
    if (fd >= 0) {
        cpuctl_msr_args_t m_args = { .msr = 0xE7 }; // MPERF
        cpuctl_msr_args_t a_args = { .msr = 0xE8 }; // APERF
        if (ioctl(fd, CPUCTL_RDMSR, &m_args) == 0 && ioctl(fd, CPUCTL_RDMSR, &a_args) == 0) {
            close(fd);
            uint64_t mperf = m_args.data;
            uint64_t aperf = a_args.data;
            double freq = fallback_freq;
            if (last_mperf != 0 && mperf > last_mperf && aperf > last_aperf) {
                freq = base_freq * ((double)(aperf - last_aperf) / (mperf - last_mperf));
            }
            last_mperf = mperf; last_aperf = aperf;
            return freq;
        }
        close(fd);
    }
    return fallback_freq;
}

int direct_pci_count() {
    int fd = open("/dev/io", O_RDWR);
    if (fd < 0) return -1;
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
    close(fd);
    return count;
}

void gather_data(struct mon_data *d) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(d->time_str, sizeof(d->time_str), "%H:%M:%S", t);
    strftime(d->date_str, sizeof(d->date_str), "%Y-%m-%d", t);
    gethostname(d->host, sizeof(d->host));

    struct timeval boottime; size_t size = sizeof(boottime);
    if (sysctlbyname("kern.boottime", &boottime, &size, NULL, 0) == 0) {
        long upt = now - boottime.tv_sec;
        snprintf(d->uptime_str, sizeof(d->uptime_str), "%ldh %ldm", upt / 3600, (upt % 3600) / 60);
    }
    getloadavg(d->load, 3);

    static char cpu_brand[256] = "";
    if (cpu_brand[0] == '\0') {
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
    }
    strcpy(d->cpu_model, cpu_brand);

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
    
    d->cpu_cores = direct_cpu_cores();

    size = sizeof(d->mem_total); sysctlbyname("hw.physmem", &d->mem_total, &size, NULL, 0);
    unsigned int active, wire, v_free; int pagesize; size = sizeof(pagesize); sysctlbyname("hw.pagesize", &pagesize, &size, NULL, 0);
    size = sizeof(active); sysctlbyname("vm.stats.vm.v_active_count", &active, &size, NULL, 0);
    sysctlbyname("vm.stats.vm.v_wire_count", &wire, &size, NULL, 0);
    sysctlbyname("vm.stats.vm.v_free_count", &v_free, &size, NULL, 0);
    d->mem_used = (long long)(active + wire) * pagesize;
    d->mem_usage = 100.0 * d->mem_used / d->mem_total;

    static int soft_ticks = 0;
    if (soft_ticks-- <= 0) {
        soft_ticks = 10;
        FILE *fp = popen("/usr/sbin/pkg info -q 2>/dev/null | /usr/bin/wc -l", "r");
        if (fp) { fscanf(fp, "%d", &d->pkg_count); pclose(fp); }
        fp = popen("/usr/sbin/pkg query '%r' 2>/dev/null | /usr/bin/grep -ic 'local'", "r");
        if (fp) { fscanf(fp, "%d", &d->ports_count); pclose(fp); }
        d->linux_count = 0;
        DIR *dir = opendir("/compat/linux/usr/bin");
        if (dir) { struct dirent *e; while ((e = readdir(dir))) if (e->d_name[0] != '.') d->linux_count++; closedir(dir); }
        d->pci_device_count = direct_pci_count();
    }

    d->cpu_temp = direct_cpu_temp();
    
    int itmp; size = sizeof(itmp);
    if (sysctlbyname("hw.acpi.thermal.tz0.passive_cooling", &itmp, &size, NULL, 0) == 0) strcpy(d->thermal_state, itmp > 0 ? "Passive" : "Active");
    else strcpy(d->thermal_state, "Normal");

    if (sysctlbyname("dev.acpi_ibm.0.fan_speed", &itmp, &size, NULL, 0) != 0)
        if (sysctlbyname("dev.aibs.0.fan0.speed", &itmp, &size, NULL, 0) != 0) itmp = -1;
    if (itmp >= 0) snprintf(d->fan_status, sizeof(d->fan_status), "%d RPM", itmp);
    else strcpy(d->fan_status, "No sensor detected");

    size = sizeof(d->cx_lowest);
    if (sysctlbyname("hw.acpi.cpu.cx_lowest", d->cx_lowest, &size, NULL, 0) != 0) strcpy(d->cx_lowest, "N/A");
    size = sizeof(d->cx_usage);
    if (sysctlbyname("dev.cpu.0.cx_usage", d->cx_usage, &size, NULL, 0) != 0) strcpy(d->cx_usage, "N/A");

    /* TODO: replace remaining shell-based execution (system() and popen() calls in gather_data())
     * with fork()/execve() using an explicitly constructed environ[] for maximum safety in a
     * setuid binary (see powerd_running, powerdxx_running, and all popen() call sites), and
     * track this work in a dedicated issue so it is not overlooked. */
    d->powerd_running = (system("/usr/bin/pgrep -q -x powerd || /usr/bin/pgrep -x powerd >/dev/null 2>&1") == 0);
    d->powerdxx_running = (system("/usr/bin/pgrep -q -x powerdxx || /usr/bin/pgrep -x powerdxx >/dev/null 2>&1") == 0);

    size = sizeof(itmp);
    if (sysctlbyname("hw.acpi.battery.state", &itmp, &size, NULL, 0) == 0) {
        if (itmp == 7) strcpy(d->bat_source, "AC Power"); else if (itmp == 1) strcpy(d->bat_source, "Battery"); else strcpy(d->bat_source, "AC Power");
        if (itmp == 0) strcpy(d->bat_state, "Full");
        else if (itmp & 1) strcpy(d->bat_state, "Discharging");
        else if (itmp & 2) strcpy(d->bat_state, "Charging");
        else strcpy(d->bat_state, "Unknown");
    } else { strcpy(d->bat_source, "AC Power"); strcpy(d->bat_state, "N/A"); }
    size = sizeof(d->bat_life); sysctlbyname("hw.acpi.battery.life", &d->bat_life, &size, NULL, 0);

    size = sizeof(d->freq_levels); sysctlbyname("dev.cpu.0.freq_levels", d->freq_levels, &size, NULL, 0);

    // GPU Monitoring - Live Update every 500ms
    if (tick_count % 5 == 0) {
        d->gpu_count = 0;
        static struct gpu_info_cache {
            char model[128];
            int unit;
            int is_nvidia;
        } g_cache[MAX_GPUS];
        static int g_init = 0;
        if (!g_init) {
            FILE *fp = popen("/usr/sbin/pciconf -lv 2>/dev/null | /usr/bin/grep -A 2 'class=0x03'", "r");
            if (fp) {
                char line[256];
                int g_count = 0;
                while (fgets(line, sizeof(line), fp) && g_count < MAX_GPUS) {
                    if (strstr(line, "device     =")) {
                        char *start = strchr(line, '\'');
                        if (start) {
                            char *end = strchr(start + 1, '\'');
                            if (end) {
                                *end = '\0';
                                strncpy(g_cache[g_count].model, start + 1, 127);
                                g_cache[g_count].is_nvidia = (strstr(line, "NVIDIA") != NULL || strstr(g_cache[g_count].model, "NVIDIA") != NULL);
                                g_cache[g_count].unit = g_count;
                                g_count++;
                            }
                        }
                    }
                }
                pclose(fp);
                d->gpu_count = g_count;
            }
            g_init = 1;
        } else {
            // Count from cache
            for(int i=0; i<MAX_GPUS; i++) if(g_cache[i].model[0]) d->gpu_count++;
        }
        for (int i = 0; i < d->gpu_count; i++) {
            strcpy(d->gpus[i].model, g_cache[i].model);
            d->gpus[i].active = 1;
            d->gpus[i].freq_mhz = 0; d->gpus[i].temp_c = -1;
            if (g_cache[i].is_nvidia) {
                char sysnode[64];
                snprintf(sysnode, sizeof(sysnode), "dev.nvidia.%d.temperature", g_cache[i].unit);
                int gtemp; size_t sz = sizeof(gtemp);
                if (sysctlbyname(sysnode, &gtemp, &sz, NULL, 0) == 0) d->gpus[i].temp_c = gtemp;
            } else {
                int gfreq; size_t sz = sizeof(gfreq);
                if (sysctlbyname("dev.drm.0.gt_cur_freq_mhz", &gfreq, &sz, NULL, 0) == 0) d->gpus[i].freq_mhz = gfreq;
                else if (sysctlbyname("dev.drmn.0.gt_cur_freq_mhz", &gfreq, &sz, NULL, 0) == 0) d->gpus[i].freq_mhz = gfreq;
            }
        }
    }

    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long cp_time[CPUSTATES]; size = sizeof(cp_time); sysctlbyname("kern.cp_time", cp_time, &size, NULL, 0);
    
    int ifc; size = sizeof(ifc); sysctlbyname("net.link.generic.system.ifcount", &ifc, &size, NULL, 0);
    d->if_count = 0;
    for (int i = 1; i <= ifc && d->if_count < MAX_NET_IF; i++) {
        int mib[6] = {CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_IFDATA, i, IFDATA_GENERAL};
        struct ifmibdata ifmd; size = sizeof(ifmd);
        if (sysctl(mib, 6, &ifmd, &size, NULL, 0) == 0) {
            if (strcmp(ifmd.ifmd_name, "lo0") == 0) continue;
            strcpy(d->ifaces[d->if_count].name, ifmd.ifmd_name);
            d->ifaces[d->if_count].total_rx_gb = ifmd.ifmd_data.ifi_ibytes / (1024.0*1024.0*1024.0);
            d->ifaces[d->if_count].total_tx_gb = ifmd.ifmd_data.ifi_obytes / (1024.0*1024.0*1024.0);
            d->ifaces[d->if_count].is_wifi = (strncmp(ifmd.ifmd_name, "wlan", 4) == 0);
            d->ifaces[d->if_count].active = (ifmd.ifmd_data.ifi_link_state == LINK_STATE_UP);
            get_ip_address(ifmd.ifmd_name, d->ifaces[d->if_count].ip);
            
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
        strcpy(history[hist_idx].ifaces[h].name, d->ifaces[h].name);
        history[hist_idx].ifaces[h].rx = (long long)(d->ifaces[h].total_rx_gb * 1024.0 * 1024.0 * 1024.0);
        history[hist_idx].ifaces[h].tx = (long long)(d->ifaces[h].total_tx_gb * 1024.0 * 1024.0 * 1024.0);
    }
    history[hist_idx].ts = ts; history[hist_idx].valid = 1; hist_idx = (hist_idx + 1) % HISTORY_SIZE;

    FILE *fsw = popen("/usr/sbin/swapinfo -k 2>/dev/null", "r"); d->swap_total = d->swap_used = 0;
    if (fsw) { char line[256]; fgets(line, 256, fsw); while (fgets(line, 256, fsw)) { long t, u; if (sscanf(line, "%*s %ld %ld", &t, &u) == 2) { d->swap_total += (long long)t * 1024; d->swap_used += (long long)u * 1024; } } pclose(fsw); }
    d->swap_usage = (d->swap_total > 0) ? (100.0 * d->swap_used / d->swap_total) : 0;

    int nfs = getfsstat(NULL, 0, MNT_NOWAIT);
    struct statfs *fs = malloc(sizeof(struct statfs) * nfs);
    nfs = getfsstat(fs, sizeof(struct statfs) * nfs, MNT_NOWAIT);
    d->disk_count = 0;
    const char *targets[] = {"/", "/boot/efi", "/tmp", "/zroot", d->home_path};
    for (int j = 0; j < 5; j++) {
        if (targets[j][0] == '\0') continue;
        for (int i = 0; i < nfs && d->disk_count < MAX_DISKS; i++) {
            if (strcmp(fs[i].f_mntonname, targets[j]) == 0) {
                strcpy(d->disks[d->disk_count].mount, fs[i].f_mntonname);
                d->disks[d->disk_count].total_bytes = (long long)fs[i].f_blocks * fs[i].f_bsize;
                d->disks[d->disk_count].used_bytes = (long long)(fs[i].f_blocks - fs[i].f_bfree) * fs[i].f_bsize;
                d->disks[d->disk_count].usage = 100.0 * d->disks[d->disk_count].used_bytes / d->disks[d->disk_count].total_bytes;
                d->disk_count++; break;
            }
        }
    }
    free(fs);
}

void draw_box(int y, int x, int h, int w, const char *title) {
    if (w < 5) return;
    int tlen = strlen(title); if (tlen > w - 6) tlen = w - 6;
    move_cursor(y, x); printf("┌─ %.*s ", tlen, title); for (int i = 0; i < w - 5 - tlen; i++) printf("─"); printf("┐");
    for (int i = 1; i < h - 1; i++) { move_cursor(y + i, x); printf("│"); move_cursor(y + i, x + w - 1); printf("│"); }
    move_cursor(y + h - 1, x); printf("└"); for (int i = 0; i < w - 2; i++) printf("─"); printf("┘");
}

void print_val(int y, int x, int w, const char *lbl, const char *val) {
    if (w < 5) return;
    move_cursor(y, x); set_color(37); 
    int lbl_len = strlen(lbl); if (lbl_len > w - 6) lbl_len = w - 6;
    printf("%.*s", lbl_len, lbl); reset_color();
    int avail = w - lbl_len - 3;
    if (avail <= 0) return;
    int vlen = strlen(val);
    if (vlen > avail) {
        move_cursor(y, x + w - avail - 2); printf("%.*s..", avail - 2, val);
    } else {
        move_cursor(y, x + w - vlen - 2); printf("%s", val);
    }
}

void print_bar(int y, int x, int w, double pct, const char *lbl) {
    if (w < 15) return;
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

void render(struct mon_data *d) {
    int col_w = term_width / 3 - 1; if (col_w < 30) col_w = 30;
    int h = term_height - 6;
    move_cursor(2, (term_width - 24) / 2); printf("║ FreeBSD System Monitor ║");
    move_cursor(3, (term_width - 24) / 2); printf("╚════════════════════════╝");

    draw_box(5, 1, h, col_w, "SYSTEM");
    char buf[128];
    print_val(7, 3, col_w - 4, "Time:", d->time_str);
    print_val(8, 3, col_w - 4, "Date:", d->date_str);
    print_val(9, 3, col_w - 4, "Host:", d->host);
    print_val(10, 3, col_w - 4, "Uptime:", d->uptime_str);
    snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", d->load[0], d->load[1], d->load[2]);
    print_val(11, 3, col_w - 4, "Load:", buf);
    move_cursor(13, 3); set_color(36); printf("CPU"); reset_color();
    move_cursor(14, 3); printf("%.*s", col_w - 6, d->cpu_model);
    snprintf(buf, sizeof(buf), "%.2f GHz", d->cpu_freq_ghz); print_val(15, 3, col_w - 4, "Frequency:", buf);
    snprintf(buf, sizeof(buf), "%d", d->cpu_cores); print_val(16, 3, col_w - 4, "Cores:", buf);
    print_bar(17, 3, col_w - 4, d->cpu_usage, "Usage");
    move_cursor(19, 3); set_color(36); printf("MEMORY"); reset_color();
    snprintf(buf, sizeof(buf), "%.2f GB", d->mem_total / (1024.0*1024.0*1024.0)); print_val(20, 3, col_w - 4, "Total:", buf);
    snprintf(buf, sizeof(buf), "%.2f GB", d->mem_used / (1024.0*1024.0*1024.0)); print_val(21, 3, col_w - 4, "Used:", buf);
    print_bar(22, 3, col_w - 4, d->mem_usage, "Usage");
    move_cursor(24, 3); set_color(36); printf("SOFTWARE & BUS"); reset_color();
    snprintf(buf, sizeof(buf), "%d devices", d->pci_device_count); print_val(25, 3, col_w - 4, "PCI Devices:", buf);
    snprintf(buf, sizeof(buf), "%d installed", d->pkg_count); print_val(26, 3, col_w - 4, "pkg Packages:", buf);
    snprintf(buf, sizeof(buf), "%d built", d->ports_count); print_val(27, 3, col_w - 4, "Ports:", buf);
    snprintf(buf, sizeof(buf), "%d program(s)", d->linux_count); print_val(28, 3, col_w - 4, "Linux Compat:", buf);

    draw_box(5, col_w + 1, h, col_w, "THERMAL & POWER");
    move_cursor(7, col_w + 3); set_color(36); printf("THERMAL"); reset_color();
    snprintf(buf, sizeof(buf), "%.1f °C", d->cpu_temp); print_val(8, col_w + 3, col_w - 4, "CPU Temp:", buf);
    print_val(9, col_w + 3, col_w - 4, "State:", d->thermal_state);
    snprintf(buf, sizeof(buf), "%.0f MHz (%c)", d->live_freq_mhz, d->freq_trend > 0 ? '+' : (d->freq_trend < 0 ? '-' : '='));
    print_val(10, col_w + 3, col_w - 4, "Live Freq:", buf);
    move_cursor(12, col_w + 3); set_color(36); printf("GPU HARDWARE"); reset_color();
    for (int i = 0; i < d->gpu_count; i++) {
        int r = 13 + i * 2; if (r > h + 3) break;
        move_cursor(r, col_w + 3); printf("%.*s", col_w - 6, d->gpus[i].model);
        if (d->gpus[i].temp_c >= 0 && d->gpus[i].freq_mhz > 0) snprintf(buf, sizeof(buf), "%.0f MHz | %.0f C", d->gpus[i].freq_mhz, d->gpus[i].temp_c);
        else if (d->gpus[i].temp_c >= 0) snprintf(buf, sizeof(buf), "%.0f C", d->gpus[i].temp_c);
        else if (d->gpus[i].freq_mhz > 0) snprintf(buf, sizeof(buf), "%.0f MHz", d->gpus[i].freq_mhz);
        else strcpy(buf, "Active");
        print_val(r + 1, col_w + 3, col_w - 4, "  Status:", buf);
    }
    move_cursor(18, col_w + 3); set_color(36); printf("POWER & ACPI"); reset_color();
    print_val(19, col_w + 3, col_w - 4, "powerd:", d->powerd_running ? "Running ✓" : "Stopped ✗");
    print_val(20, col_w + 3, col_w - 4, "powerdxx:", d->powerdxx_running ? "Running ✓" : "Stopped ✗");
    print_val(21, col_w + 3, col_w - 4, "Cx Lowest:", d->cx_lowest);
    move_cursor(22, col_w + 3); printf("Cx Usage: %.*s", col_w - 14, d->cx_usage);
    move_cursor(24, col_w + 3); set_color(36); printf("BATTERY"); reset_color();
    print_val(25, col_w + 3, col_w - 4, "Source:", d->bat_source);
    print_bar(26, col_w + 3, col_w - 4, (double)d->bat_life, "Bat");
    print_val(27, col_w + 3, col_w - 4, "State:", d->bat_state);
    move_cursor(29, col_w + 3); set_color(36); printf("FREQ RANGE"); reset_color();
    char *pp = d->freq_levels; int row = 30;
    while (*pp && row < h + 4) {
        char level[32]; int n; if (sscanf(pp, "%31s%n", level, &n) != 1) break;
        move_cursor(row++, col_w + 5); printf("%s MHz", level); pp += n; while (*pp == ' ') pp++;
    }

    draw_box(5, 2 * col_w + 1, h, term_width - 2 * col_w, "NETWORK & DISKS");
    int net_row = 7;
    for(int i=0; i<d->if_count; i++) {
        move_cursor(net_row++, 2 * col_w + 3); set_color(36); printf("NET: %s (%s)", d->ifaces[i].name, d->ifaces[i].is_wifi ? "WiFi" : "Ethernet"); reset_color();
        print_val(net_row++, 2 * col_w + 3, term_width - 2 * col_w - 4, "IP:", d->ifaces[i].ip);
        snprintf(buf, sizeof(buf), "%.2f KB/s", d->ifaces[i].rx_rate_kb); print_val(net_row++, 2 * col_w + 3, term_width - 2 * col_w - 4, "Down:", buf);
        snprintf(buf, sizeof(buf), "%.2f KB/s", d->ifaces[i].tx_rate_kb); print_val(net_row++, 2 * col_w + 3, term_width - 2 * col_w - 4, "Up:", buf);
        if (net_row > 18) break; 
    }
    move_cursor(21, 2 * col_w + 3); set_color(36); printf("SWAP"); reset_color();
    snprintf(buf, sizeof(buf), "%.2f GB", d->swap_total / (1024.0*1024.0*1024.0)); print_val(22, 2 * col_w + 3, term_width - 2 * col_w - 4, "Total:", buf);
    print_bar(23, 2 * col_w + 3, term_width - 2 * col_w - 4, d->swap_usage, "Usage");
    move_cursor(25, 2 * col_w + 3); set_color(36); printf("DISKS"); reset_color();
    for (int i = 0; i < d->disk_count; i++) {
        if (26 + i * 2 > h + 3) break;
        move_cursor(26 + i * 2, 2 * col_w + 3); printf("%.*s", (term_width - 2 * col_w - 4), d->disks[i].mount);
        snprintf(buf, sizeof(buf), "%.1f/%.1fG", d->disks[i].used_bytes / (1024.0*1024.0*1024.0), d->disks[i].total_bytes / (1024.0*1024.0*1024.0));
        print_bar(27 + i * 2, 2 * col_w + 3, term_width - 2 * col_w - 4, d->disks[i].usage, buf);
    }
}

int main() {
    /* Resolve the real user's home directory from the system user database
     * before sanitizing the environment.  When running under sudo, SUDO_UID
     * identifies the invoking user; otherwise the process's real UID is used.
     * Using getpwuid() avoids trusting attacker-controlled environment
     * variables (HOME/USER/SUDO_USER) in a setuid-root binary. */
    char resolved_home[MAXPATHLEN] = "";
    {
        uid_t target_uid = getuid();
        /* Only trust SUDO_UID when running as root; an unprivileged caller
         * must not be able to influence which home directory is monitored. */
        if (target_uid == 0) {
            const char *sudo_uid_str = getenv("SUDO_UID");
            if (sudo_uid_str != NULL) {
                char *endp;
                errno = 0;
                unsigned long v = strtoul(sudo_uid_str, &endp, 10);
                if (errno != ERANGE && *endp == '\0' && v <= (unsigned long)((uid_t)-1))
                    target_uid = (uid_t)v;
            }
        }
        struct passwd *pw = getpwuid(target_uid);
        if (pw != NULL && pw->pw_dir != NULL) {
            /* Disk matching compares against f_mntonname (the mountpoint), not
             * the home directory path itself.  Use statfs() to find the
             * mountpoint that contains the home directory. */
            struct statfs home_fs;
            if (statfs(pw->pw_dir, &home_fs) == 0)
                strncpy(resolved_home, home_fs.f_mntonname, sizeof(resolved_home) - 1);
            else
                strncpy(resolved_home, pw->pw_dir, sizeof(resolved_home) - 1);
        }
    }

    /* Sanitize environment for setuid safety: use an allowlist approach so no
     * dangerous variable (LD_*, IFS, ENV, BASH_ENV, locale, etc.) is missed.
     * Only TERM is restored; home-directory detection uses resolved_home which
     * was derived from the user database, not the environment. */
    const char *term_env = getenv("TERM");
    /* Bound TERM to a reasonable length to prevent DoS via oversized values. */
    char *term = (term_env != NULL) ? strndup(term_env, 64) : NULL;
    if (clearenv() != 0 ||
        setenv("PATH", "/bin:/usr/bin:/sbin:/usr/sbin:/usr/local/sbin", 1) != 0 ||
        (term != NULL && setenv("TERM", term, 1) != 0)) {
        fprintf(stderr, "Failed to sanitize environment: %s\n", strerror(errno));
        free(term);
        exit(1);
    }
    free(term);
    struct mon_data d = {0};
    strncpy(d.home_path, resolved_home, sizeof(d.home_path) - 1);
    enable_raw_mode();
    signal(SIGWINCH, handle_sigwinch);
    get_terminal_size();
    clear_screen();
    while (1) {
        if (resize_pending) { get_terminal_size(); resize_pending = 0; clear_screen(); }
        gather_data(&d);
        render(&d);
        move_cursor(term_height, 1); printf(" 'q' to quit | %s | Tick: %u", VERSION, ++tick_count); fflush(stdout);
        char c; if (read(STDIN_FILENO, &c, 1) > 0) if (c == 'q' || c == 'Q' || c == 3) { clear_screen(); exit(0); }
        usleep(100000);
    }
    return 0;
}
