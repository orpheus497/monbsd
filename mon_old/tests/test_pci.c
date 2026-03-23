#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <machine/cpufunc.h>

uint32_t pci_read(int bus, int slot, int func, int offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xfc) | ((uint32_t)0x80000000));
    outl(0xCF8, address);
    return inl(0xCFC);
}

int main() {
    int fd = open("/dev/io", O_RDWR);
    if (fd < 0) {
        perror("open /dev/io");
        return 1;
    }
    // Scan bus 0, device 0, function 0
    uint32_t vendor_device = pci_read(0, 0, 0, 0);
    printf("Bus 0 Dev 0 Func 0 Vendor/Device: %08x\n", vendor_device);
    return 0;
}
