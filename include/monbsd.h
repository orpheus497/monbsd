#ifndef MONBSD_H
#define MONBSD_H

#define MAX_STR_LEN 256

// Pillar 1: The Brain
// This struct will hold every piece of data our monitor collects.
struct system_state {
  // Hardware Sensors
  int pci_device_count;

  // OS Sensors
  char hostname[MAX_STR_LEN];
  char os_release[MAX_STR_LEN];
};

// Function Prototypes

// This funciton will scan the motherbard and return the number of connected
// devices
int scan_pci_bus(void);
void get_os_info(struct system_state *state);

#endif // MONBSD_H
