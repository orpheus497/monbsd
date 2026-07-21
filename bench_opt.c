#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

void simulate_slow_subprocess() {
    usleep(150000); // 150ms delay representing pkg info etc.
}

int cached_pkg_count = 0;

void *background_thread(void *arg) {
    while (1) {
        simulate_slow_subprocess();
        cached_pkg_count = 1;
        sleep(1); // update every 1 second
    }
    return NULL;
}

int main() {
    pthread_t tid;
    pthread_create(&tid, NULL, background_thread, NULL);
    pthread_detach(tid);

    struct timeval start, end;
    gettimeofday(&start, NULL);

    int soft_ticks = 0;
    for (int i = 0; i < 50; i++) {
        usleep(10000); // 10ms frame time

        if (soft_ticks-- <= 0) {
            soft_ticks = 10;
            // Now we just read cached_pkg_count instead of blocking
        }
    }

    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Optimized loop time: %.3f seconds\n", elapsed);
    return 0;
}
