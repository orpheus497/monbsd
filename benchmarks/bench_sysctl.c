#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/sysctl.h>

int main() {
    struct timeval start, end;
    long long mem_total, mem_used;
    unsigned int active, wire, v_free;
    int pagesize;
    size_t size;

    gettimeofday(&start, NULL);

    for (int i = 0; i < 100000; i++) {
        size = sizeof(mem_total); sysctlbyname("hw.physmem", &mem_total, &size, NULL, 0);
        size = sizeof(pagesize); sysctlbyname("hw.pagesize", &pagesize, &size, NULL, 0);
        size = sizeof(active); sysctlbyname("vm.stats.vm.v_active_count", &active, &size, NULL, 0);
        sysctlbyname("vm.stats.vm.v_wire_count", &wire, &size, NULL, 0);
        sysctlbyname("vm.stats.vm.v_free_count", &v_free, &size, NULL, 0);
        mem_used = (long long)(active + wire) * pagesize;
    }

    gettimeofday(&end, NULL);
    double elapsed1 = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Uncached loop time: %.6f seconds\n", elapsed1);

    static int hw_initialized = 0;
    static long long cached_mem_total = 0;
    static int cached_pagesize = 0;

    gettimeofday(&start, NULL);

    for (int i = 0; i < 100000; i++) {
        if (!hw_initialized) {
            size = sizeof(cached_mem_total); sysctlbyname("hw.physmem", &cached_mem_total, &size, NULL, 0);
            size = sizeof(cached_pagesize); sysctlbyname("hw.pagesize", &cached_pagesize, &size, NULL, 0);
            hw_initialized = 1;
        }
        mem_total = cached_mem_total;
        pagesize = cached_pagesize;

        size = sizeof(active); sysctlbyname("vm.stats.vm.v_active_count", &active, &size, NULL, 0);
        sysctlbyname("vm.stats.vm.v_wire_count", &wire, &size, NULL, 0);
        sysctlbyname("vm.stats.vm.v_free_count", &v_free, &size, NULL, 0);
        mem_used = (long long)(active + wire) * pagesize;
    }

    gettimeofday(&end, NULL);
    double elapsed2 = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Cached loop time: %.6f seconds\n", elapsed2);

    return 0;
}
