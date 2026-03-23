#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <machine/cpufunc.h>
#include <machine/sysarch.h>

int main() {
    u_int regs[4];
    do_cpuid(0, regs);
    printf("CPUID 0: %08x %08x %08x %08x\n", regs[0], regs[1], regs[2], regs[3]);
    uint64_t tsc = rdtsc();
    printf("TSC: %lu\n", tsc);
    return 0;
}
