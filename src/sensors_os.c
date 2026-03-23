#include "monbsd.h"
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

// Function: Gather Hostname and Kernel Release information using memory-safe
// strlcpy
void get_os_info(struct system_state *state) {

  // 1. Get Hostname
  char temp_host[MAX_STR_LEN];
  if (gethostname(temp_host, sizeof(temp_host)) == 0) {
    // SAFE COPY: Copy from temp_host into our state cabinet
    strlcpy(state->hostname, temp_host, sizeof(state->hostname));
  } else {
    strlcpy(state->hostname, "Unknown", sizeof(state->hostname));
  }

  // 2. Get OS Kernel Release
  struct utsname sys_info;
  if (uname(&sys_info) == 0) {
    strlcpy(state->os_release, sys_info.release, sizeof(state->os_release));
  } else {
    strlcpy(state->os_release, "Unknown", sizeof(state->os_release));
  }
}
