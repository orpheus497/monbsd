#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

void simulate_slow_subprocess() {
    usleep(150000); // 150ms delay representing pkg info etc.
}

int main() {
    struct timeval start, end;
    gettimeofday(&start, NULL);

    int soft_ticks = 0;
    for (int i = 0; i < 50; i++) {
        usleep(10000); // 10ms frame time

        if (soft_ticks-- <= 0) {
            soft_ticks = 10;
            simulate_slow_subprocess();
        }
    }

    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Baseline loop time: %.3f seconds\n", elapsed);
    return 0;
}
