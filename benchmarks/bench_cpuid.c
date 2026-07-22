#include <stdio.h>
#include <sys/time.h>

#if defined(__i386__) || defined(__x86_64__)
static inline void cpuid(unsigned int info, unsigned int *eax, unsigned int *ebx, unsigned int *ecx, unsigned int *edx) {
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(info)
    );
}
#endif

int direct_cpu_cores_uncached() {
    unsigned int eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    int cores = (ebx >> 16) & 0xFF;
    return cores > 0 ? cores : 1;
}

int direct_cpu_cores_cached() {
    static int cached_cores = 0;
    if (cached_cores > 0) return cached_cores;
    unsigned int eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    int cores = (ebx >> 16) & 0xFF;
    cached_cores = cores > 0 ? cores : 1;
    return cached_cores;
}

int main() {
    struct timeval start, end;
    long iterations = 10000000;
    volatile int dummy = 0;

    gettimeofday(&start, NULL);
    for (long i = 0; i < iterations; i++) {
        dummy = direct_cpu_cores_uncached();
    }
    gettimeofday(&end, NULL);
    double elapsed_uncached = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;

    gettimeofday(&start, NULL);
    for (long i = 0; i < iterations; i++) {
        dummy = direct_cpu_cores_cached();
    }
    gettimeofday(&end, NULL);
    double elapsed_cached = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;

    printf("Uncached CPUID time for %ld iterations: %.6f seconds\n", iterations, elapsed_uncached);
    printf("Cached CPUID time for %ld iterations: %.6f seconds\n", iterations, elapsed_cached);
    printf("Improvement: %.2fx faster\n", elapsed_uncached / elapsed_cached);

    return 0;
}
