#ifndef MONBSD_H
#define MONBSD_H

// Pillar 1: The Brain
// This struct will hold every piece of data our monitor collects.
struct system_state {
  int pci_device_count;
};

// Function Prototypes

// This funciton will scan the motherbard and return the number of connected
// devices
int scan_pci_bus(void);

#endif // MONBSD_H
