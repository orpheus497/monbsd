#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

void simulate_direct_pci_count() {
    usleep(50000); // 50ms to simulate pciconf or exhaustive scan
}

int main(int argc, char **argv) {
    int optimize = (argc > 1);
    struct timeval start, end;
    gettimeofday(&start, NULL);

    int tick_count = 0;
    static int cached_pci_count = -1;

    for (int i = 0; i < 500; i++) {
        tick_count++;
        // The original logic:
        if (!optimize) {
            if (cached_pci_count == -1 || tick_count % 100 == 0) {
                simulate_direct_pci_count();
                cached_pci_count = 42;
            }
        } else {
            // The optimized logic:
            if (cached_pci_count == -1) {
                simulate_direct_pci_count();
                cached_pci_count = 42;
            }
        }
        usleep(1000); // 1ms per tick
    }

    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("%s loop time: %.3f seconds\n", optimize ? "Optimized" : "Baseline", elapsed);
    return 0;
}
