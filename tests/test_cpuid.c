#include <sys/types.h>
#include <stdio.h>
#include <stdint.h>
#include <machine/cpufunc.h>

void get_cpu_brand(char *brand) {
    u_int regs[4];
    do_cpuid(0x80000000, regs);
    if (regs[0] >= 0x80000004) {
        uint32_t *brand_ptr = (uint32_t *)brand;
        do_cpuid(0x80000002, regs);
        brand_ptr[0] = regs[0]; brand_ptr[1] = regs[1]; brand_ptr[2] = regs[2]; brand_ptr[3] = regs[3];
        do_cpuid(0x80000003, regs);
        brand_ptr[4] = regs[0]; brand_ptr[5] = regs[1]; brand_ptr[6] = regs[2]; brand_ptr[7] = regs[3];
        do_cpuid(0x80000004, regs);
        brand_ptr[8] = regs[0]; brand_ptr[9] = regs[1]; brand_ptr[10] = regs[2]; brand_ptr[11] = regs[3];
        brand[48] = '\0';
    } else {
        brand[0] = '\0';
    }
}

int main() {
    char brand[49] = {0};
    get_cpu_brand(brand);
    printf("CPU Brand: %s\n", brand);
    return 0;
}
