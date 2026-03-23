#include "monbsd.h"
#include <stdio.h>

int main(void) {
  printf("monbsd core engine v0.2.0 initialized.\n");

  // Initialize the State
  struct system_state state = {0};

  // --- OS SENSOR ---
  // We pass the memory address (&) of 'state' to the function
  get_os_info(&state);

  // --- HARDWARE SENSOR ---
  state.pci_device_count = scan_pci_bus();

  // --- UI OUTPUT ---
  printf("----------------------------------------\n");
  printf(" Hostname:    %s\n", state.hostname);
  printf(" OS Kernel:   FreeBSD %s\n", state.os_release);

  if (state.pci_device_count >= 0) {
    printf(" PCI Devices: %d\n", state.pci_device_count);
  } else {
    printf(" PCI Devices: [Access Denied]\n");
  }
  printf("----------------------------------------\n");

  return 0;
}
