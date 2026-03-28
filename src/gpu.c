/**
 * @file gpu_monitor.c
 * @brief Professional GPU Monitoring Tool for FreeBSD
 * 
 * LIBRARIES USED:
 * - stdio.h:  Standard Input/Output for printing to terminal and pipe operations.
 * - stdlib.h: Standard Library for process control (exit codes) and memory management.
 * - string.h: String manipulation for parsing hardware names and buffer handling.
 * - unistd.h: Unix Standard for the sleep() function and process-level constants.
 * - signal.h: To catch interrupts (Ctrl+C) so we can restore the terminal state.
 * - sys/types.h & sys/sysctl.h: Interface to the FreeBSD Kernel for hardware telemetry.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/sysctl.h>

/**
 * ANSI ESCAPE CODES:
 * These are special character sequences that control terminal behavior.
 * Syntax: \033 (Octal for ESC) followed by [ and a formatting code.
 */
#define RESET "\033[0m"         // Resets all colors and styles
#define CYAN "\033[36m"          // Standard Cyan text
#define GREEN "\033[32m"         // Standard Green text
#define YELLOW "\033[33m"        // Standard Yellow text
#define RED "\033[31m"           // Standard Red text
#define MAGENTA "\033[35m"       // Standard Magenta text
#define CLEAR_SCREEN "\033[2J\033[H" // [2J clears screen, [H moves cursor to Top-Left (Home)

/**
 * GLOBAL VOLATILE VARIABLE:
 * 'sig_atomic_t' ensures the variable is read/written in a single CPU instruction,
 * preventing race conditions during signal handling. 'volatile' tells the compiler
 * not to optimize the variable away, as it changes outside the normal program flow.
 */
volatile sig_atomic_t keep_running = 1;

/**
 * SIGNAL HANDLER:
 * Purpose: Allows the program to shut down gracefully.
 * When the user presses Ctrl+C, the kernel sends SIGINT. This function catches it.
 */
void sigint_handler(int dummy) {
    keep_running = 0; // Flip the switch to break the main loop
}

/**
 * DATA STRUCTURE:
 * Encapsulates all telemetry for a single GPU unit.
 * Logic: Grouping related data (Utilization, VRAM, Temperature) into a struct 
 * makes the code modular and allows the same 'print' function to handle multiple GPUs.
 */
typedef struct {
    char name[128];     // Hardware model name
    char type[32];      // "Integrated" vs "Dedicated"
    float util;         // Percentage of core usage
    long vram_used;     // Used memory in MiB (changed to long for precise calculation)
    long vram_total;    // Total available memory in MiB
    int temp;           // Temperature in Celsius
    int available;      // Boolean flag: Is the driver responding?
} GPUInfo;

/**
 * HARDWARE IDENTIFICATION (pciconf):
 * 'pciconf' is the FreeBSD utility to query the PCI bus.
 * SYSCALL: popen() creates a pipe between the C program and a shell command.
 * LOGIC: We look for the 'device' line under the VGA controller to get the real name.
 */
void get_gpu_name(const char *device_id, char *name, size_t size) {
    char command[256];
    // Regex: ^[[:space:]]+device matches the specific indented name line in pciconf output
    snprintf(command, sizeof(command), 
             "pciconf -lv %s | grep -E \"^[[:space:]]+device\" | head -n 1 | cut -d \"'\" -f 2", 
             device_id);
    
    FILE *fp = popen(command, "r"); // "r" means we want to read the command's output
    if (fp && fgets(name, size, fp)) {
        name[strcspn(name, "\n")] = 0; // Remove trailing newline for clean UI
        pclose(fp); // Close pipe to avoid resource leaks
    } else {
        strncpy(name, "Unknown GPU", size);
        if (fp) pclose(fp);
    }
}

/**
 * NVIDIA TELEMETRY (nvidia-smi):
 * Purpose: Communicates with the NVIDIA Binary Driver.
 * SYNTAX: --query-gpu allows specific metrics. --format=csv,noheader,nounits 
 * provides raw numbers that are easy for C to parse using sscanf().
 */
void update_nvidia_info(GPUInfo *gpu) {
    FILE *fp = popen("nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu "
                     "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (fp) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), fp)) {
            // sscanf parses formatted text back into variables
            int used, total;
            if (sscanf(buffer, "%f, %d, %d, %d", &gpu->util, &used, &total, &gpu->temp) == 4) {
                gpu->vram_used = used;
                gpu->vram_total = total;
                gpu->available = 1;
            } else {
                gpu->available = 0; // Likely driver mismatch or state error
            }
        } else {
            gpu->available = 0;
        }
        pclose(fp);
    } else {
        gpu->available = 0;
    }
}

/**
 * INTEL TELEMETRY (sysctl + kernel internal info):
 * 
 * INTERPRETING INTEGRATED VRAM ON FREEBSD:
 * 1. "Stolen Memory": This is system RAM reserved by the BIOS for the GPU at boot.
 *    It is represented by the kernel node 'hw.intel_graphics_stolen_size'.
 * 2. "GEM Objects": Graphics Execution Manager (GEM) handles the memory life-cycle.
 *    The number of active objects in 'vm.uma.drm_i915_gem_object.stats.current' 
 *    multiplied by the base object size gives a proxy of active allocations.
 * 
 * LOGIC: We combine these values to show a meaningful "Hardware Dedicated" vs "Dynamic Used" metric.
 */
void update_intel_info(GPUInfo *gpu) {
    int temp_raw;
    size_t len = sizeof(temp_raw);
    
    // 1. Temperature Retrieval
    if (sysctlbyname("hw.acpi.thermal.tz0.temperature", &temp_raw, &len, NULL, 0) == 0) {
        gpu->temp = (temp_raw - 2732) / 10;
        gpu->available = 1;
    } else {
        gpu->temp = 0;
        gpu->available = 1;
    }

    // 2. VRAM Retrieval (Total Stolen Memory)
    long stolen_bytes = 0;
    len = sizeof(stolen_bytes);
    if (sysctlbyname("hw.intel_graphics_stolen_size", &stolen_bytes, &len, NULL, 0) == 0) {
        gpu->vram_total = stolen_bytes / (1024 * 1024); // Convert Bytes to MiB
    } else {
        gpu->vram_total = 0;
    }

    // 3. Current Dynamic Allocations (GEM Proxy)
    unsigned int gem_objects = 0;
    len = sizeof(gem_objects);
    // UMA (Unified Memory Architecture) tracks the active "slots" for graphics objects
    if (sysctlbyname("vm.uma.drm_i915_gem_object.stats.current", &gem_objects, &len, NULL, 0) == 0) {
        // Average GEM object size is typically 4KB (page size), but many are larger.
        // We use a safe lower-bound estimate based on active objects.
        gpu->vram_used = (gem_objects * 4096) / (1024 * 1024);
    } else {
        gpu->vram_used = -1;
    }

    // 4. Utilization (Driver Limitation)
    gpu->util = -1.0f; 
}

/**
 * UI RENDERING:
 * Logic: Separates data gathering from presentation. 
 * This function handles color coding and the dynamic utilization bar.
 */
void print_gpu_card(GPUInfo *gpu) {
    printf(CYAN "---------------------------------------------------------\n" RESET);
    printf(GREEN " GPU Type: " RESET "%-12s | " GREEN "Name: " RESET "%s\n", gpu->type, gpu->name);
    
    if (!gpu->available) {
        printf(RED " Status:   [DRIVER NOT RESPONDING]\n" RESET);
        return;
    }

    // UTILIZATION BAR:
    if (gpu->util >= 0) {
        printf(YELLOW " Util:     " RESET "%5.1f%% ", gpu->util);
        printf("[");
        int bars = (int)(gpu->util / 5);
        for (int i = 0; i < 20; i++) {
            if (i < bars) printf(GREEN "#" RESET);
            else printf(".");
        }
        printf("]\n");
    } else {
        printf(YELLOW " Util:     " RESET "N/A (Driver limitation on FreeBSD)\n");
    }

    // VRAM USAGE (Refined for Iris):
    if (strcmp(gpu->type, "Integrated") == 0) {
        if (gpu->vram_used >= 0) {
            printf(MAGENTA " VRAM:     " RESET "%ld MiB (Active GEM) / %ld MiB (Stolen/Reserved)\n", 
                   gpu->vram_used, gpu->vram_total);
        } else {
            printf(MAGENTA " VRAM:     " RESET "Shared Dynamic RAM\n");
        }
    } else {
        // Dedicated GPU logic (NVIDIA)
        if (gpu->vram_used >= 0 && gpu->vram_total > 0) {
            float vram_pct = (float)gpu->vram_used / gpu->vram_total * 100.0f;
            printf(MAGENTA " VRAM:     " RESET "%ld / %ld MiB (%5.1f%%)\n", 
                   gpu->vram_used, gpu->vram_total, vram_pct);
        }
    }

    // TEMPERATURE COLOR LOGIC:
    if (gpu->temp > 0) {
        char *temp_color = (gpu->temp > 80) ? RED : (gpu->temp > 60) ? YELLOW : GREEN;
        printf(RED " Temp:     " RESET "%s%d°C" RESET "\n", temp_color, gpu->temp);
    } else {
        printf(RED " Temp:     " RESET "Sensor Unavailable\n");
    }
}

/**
 * MAIN EXECUTION LOOP:
 * Purpose: Orchestrates the initialization, monitoring, and shutdown.
 */
int main() {
    // 1. Register the interrupt signal handler
    signal(SIGINT, sigint_handler);

    // 2. Initialize GPU structures
    GPUInfo intel, nvidia;
    
    strcpy(intel.type, "Integrated");
    get_gpu_name("vgapci0", intel.name, sizeof(intel.name));
    
    strcpy(nvidia.type, "Dedicated");
    get_gpu_name("vgapci1", nvidia.name, sizeof(nvidia.name));

    // 3. Live Monitoring Loop
    while (keep_running) {
        update_intel_info(&intel);
        update_nvidia_info(&nvidia);

        printf(CLEAR_SCREEN);
        printf(CYAN "=========================================================\n" RESET);
        printf(GREEN "             FreeBSD GPU Monitoring Tool                 \n" RESET);
        printf(CYAN "=========================================================\n" RESET);
        printf(" Sampling: 1.0s | PID: %d | Press Ctrl+C to stop\n\n", getpid());

        print_gpu_card(&intel);
        printf("\n");
        print_gpu_card(&nvidia);

        fflush(stdout);
        sleep(1);
    }

    // 4. Cleanup Phase
    printf(RESET CLEAR_SCREEN);
    printf(GREEN "Monitoring terminated. Terminal restored.\n" RESET);
    return 0;
}
