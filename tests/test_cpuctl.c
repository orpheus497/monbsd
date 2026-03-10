#include <sys/types.h>
#include <sys/ioccom.h>
#include <sys/cpuctl.h>

int main() {
    cpuctl_msr_args_t args;
    args.msr = 0x19C;
    return 0;
}
