#include <sys/param.h>
#include <sys/mount.h>
#include <stdio.h>

int main() {
    struct statfs st;
    if (statfs("/", &st) == 0) {
        printf("Root: blocks=%lu, bfree=%lu, bavail=%lu, bsize=%u\n", 
               (unsigned long)st.f_blocks, (unsigned long)st.f_bfree, (unsigned long)st.f_bavail, (unsigned int)st.f_bsize);
        double total = (double)st.f_blocks * st.f_bsize / (1024.0*1024.0*1024.0);
        double free = (double)st.f_bfree * st.f_bsize / (1024.0*1024.0*1024.0);
        double used = total - free;
        printf("Root: total=%.2fG, used=%.2fG, avail=%.2fG\n", total, used, free);
    }
    return 0;
}
