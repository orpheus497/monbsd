#include "monbsd.h"
#include <stdio.h>

int main(void) {
  printf("monbsd core engine v0.2.0 initialized.\n");

  // 1. Initialize the State (The Brain) to pure zeroes
  // This creates our filing cabinet and ensures no garbage memory exists.
  struct system_state state = {0};

  printf("Scanning PCI Bus for hardware...\n");

  // 2. Call the hardware sensor
  // We execute the function and store the result inside our state struct
  state.pci_device_count = scan_pci_bus();

  // 3. Evaluate the result
  // If the sensor returned -1, we know the hardware rejected us.
  if (state.pci_device_count < 0) {
    printf("Hardware scan failed. Ensure you have proper permissions.\n");
    return 1; // Return 1 indicates to the OS that the program crashed/failed
  }

  // 4. Success Output
  printf("SUCCESS: Motherboard reported %d active PCI devices/functions.\n",
         state.pci_device_count);

  return 0; // Return 0 indicates clean exit
}
