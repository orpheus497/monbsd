#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <stdio.h>
#include <time.h>

int main() {
    struct timeval boottime;
    size_t size = sizeof(boottime);
    if (sysctlbyname("kern.boottime", &boottime, &size, NULL, 0) != -1) {
        time_t now = time(NULL);
        time_t uptime = now - boottime.tv_sec;
        printf("Uptime: %ld seconds\n", (long)uptime);
    }
    return 0;
}
