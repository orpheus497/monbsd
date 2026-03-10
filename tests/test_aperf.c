#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/cpuctl.h>
#include <time.h>

int main() {
    int fd = open("/dev/cpuctl0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    cpuctl_msr_args_t mperf_args = { .msr = 0xE7 };
    cpuctl_msr_args_t aperf_args = { .msr = 0xE8 };

    if (ioctl(fd, CPUCTL_RDMSR, &mperf_args) == 0 && ioctl(fd, CPUCTL_RDMSR, &aperf_args) == 0) {
        uint64_t m1 = mperf_args.data;
        uint64_t a1 = aperf_args.data;
        usleep(500000); // 500ms
        ioctl(fd, CPUCTL_RDMSR, &mperf_args);
        ioctl(fd, CPUCTL_RDMSR, &aperf_args);
        uint64_t m2 = mperf_args.data;
        uint64_t a2 = aperf_args.data;
        
        double ratio = (double)(a2 - a1) / (m2 - m1);
        printf("APERF/MPERF ratio: %f\n", ratio);
    } else {
        perror("ioctl");
    }
    close(fd);
    return 0;
}
