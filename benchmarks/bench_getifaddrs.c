#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>

#define INET_ADDRSTRLEN 16

/* Fallback strlcpy */
size_t my_strlcpy(char *dst, const char *src, size_t siz)
{
        char *d = dst;
        const char *s = src;
        size_t n = siz;
        if (n != 0) {
                while (--n != 0) {
                        if ((*d++ = *s++) == '\0')
                                break;
                }
        }
        if (n == 0) {
                if (siz != 0)
                        *d = '\0';
                while (*s++)
                        ;
        }
        return(s - src - 1);
}

static void get_ip_address_old(const char *ifname, char *ip_buf, size_t buf_size) {
    struct ifaddrs *ifaddr, *ifa;
    my_strlcpy(ip_buf, "Unknown", buf_size);
    if (getifaddrs(&ifaddr) == -1) return;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_INET && strcmp(ifa->ifa_name, ifname) == 0) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip_buf, buf_size);
            break;
        }
    }
    freeifaddrs(ifaddr);
}

static void get_ip_address_new(struct ifaddrs *ifaddr, const char *ifname, char *ip_buf, size_t buf_size) {
    struct ifaddrs *ifa;
    my_strlcpy(ip_buf, "Unknown", buf_size);
    if (ifaddr == NULL) return;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_INET && strcmp(ifa->ifa_name, ifname) == 0) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip_buf, buf_size);
            break;
        }
    }
}

int main() {
    int iterations = 1000;
    int interfaces = 10;
    char ip_buf[INET_ADDRSTRLEN];

    struct timeval start, end;

    // Baseline
    gettimeofday(&start, NULL);
    for (int i = 0; i < iterations; i++) {
        for (int j = 0; j < interfaces; j++) {
            get_ip_address_old("eth0", ip_buf, sizeof(ip_buf));
        }
    }
    gettimeofday(&end, NULL);
    double elapsed_old = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Baseline (N+1 getifaddrs): %.6f seconds\n", elapsed_old);

    // Optimized
    gettimeofday(&start, NULL);
    for (int i = 0; i < iterations; i++) {
        struct ifaddrs *ifaddr = NULL;
        getifaddrs(&ifaddr);
        for (int j = 0; j < interfaces; j++) {
            get_ip_address_new(ifaddr, "eth0", ip_buf, sizeof(ip_buf));
        }
        if (ifaddr) freeifaddrs(ifaddr);
    }
    gettimeofday(&end, NULL);
    double elapsed_new = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Optimized (1 getifaddrs per cycle): %.6f seconds\n", elapsed_new);
    printf("Improvement: %.2fx faster\n", elapsed_old / elapsed_new);

    return 0;
}
