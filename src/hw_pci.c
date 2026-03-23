#include "monbsd.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <machine/cpufunc.h>
#include <sys/types.h>

/* Function: Talk directly to the motherboard's PCI controller to count the
 * devices
 */

int scan_pci_bus(void) {

  /*  1. OPEN THE HARDWARE DOOR
   *  We ask the kernel to open the raw Input/Output memory of the motherboard
   *  O_RDWR means "Open for Reading and Writing"
   */

  int fd = open("/dev/io", O_RDWR);
  if (fd < 0) {
    perror("FATAL: Failed to open /dev/io. Are you running as root?");
    return -1;
  }

  int device_count = 0;

  /*  THE PCI BUS GRID
   *  The PCI standard allows up to 256 buses, 32 devices per bus, and 8
   * functions per device
   */

  for (int bus = 0; bus < 256; bus++) {
    for (int dev = 0; dev < 32; dev++) {

      /*  3. BITWISE SHIFTING: Building the Memory Address
       *  The motherboard expects a 32-bit (uint32_t) address formatted exactly
       * like this:
       * Bit 31: Enable Bit (Must be 1)
       * Bits 23-16: Bus Number
       * Bits 15-11: Device Number
       * Bits 10-8: Function Number
       * We use '<<' (Bitwise Shift Leg) to slide the numbers into the correct
       * slots. We use '|' (Bitwise OR) to glue the slots together into one
       * 32-bit number
       */

      uint32_t address = (1 << 31) | (bus << 16) | (dev << 11) | 0;

      /* 4. TALKING TO THE CPU
       * outl (Out Long): Sends our 32-bit address to port 0xCF8 (The PCI
       * Command Port)
       */

      outl(0xCF8, address);

      /* inl (In Long): Reads the motherboard's response from port 0xCFC (The
       * PCI Data Port)
       */

      uint32_t val = inl(0xCFC);

      /* 0xFFFFFFFF means "Nobody is home" Any other number means that a device
       * responded
       */

      if (val != 0xFFFFFFFF && val != 0) {
        device_count++;

        /* Inspect to see if device is Multi-Function (like GPU with built-in
         * Audio)
         */

        outl(0xCF8, address | 0x0C); // Check Header Type register
        uint32_t hdr = inl(0xCFC);

        // Bitwise AND (&): We check if the 23rd bit is flipped (0x008000000)
        if (hdr & 0x008000000) {
          for (int func = 1; func < 8; func++) {

            /* Build the address again, but this time including the function
             * number
             */

            address = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8);
            outl(0xCF8, address);
            val = inl(0xCFC);
            if (val != 0xFFFFFFFF && val != 0) {
              device_count++;
            }
          }
        }
      }
    }
  }

  // 5. CLOSE THE DOOR
  close(fd);

  return device_count;
}
