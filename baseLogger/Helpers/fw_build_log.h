#pragma once
/*
  The running firmware image, in the flash log (FW event). The Serial banner in setup() is lost without a
  cable, so without this a dump cannot tell a USB-flashed test build from the OTA'd release, or which ELF
  a coredump belongs to.

  #included by logger.ino after logging.h into the single sketch translation unit - NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/
#include <esp_ota_ops.h>

#if ENABLE_LOGGING
// elf= is the app ELF SHA-256 prefix esp_coredump checks (compare with `sha256sum firmware.elf`); part= is
// the slot, since OTA and USB flashes land in different ones.
void logFwBuild() {
  char elf[17];
  esp_ota_get_app_elf_sha256(elf, sizeof(elf));
  const esp_partition_t* running = esp_ota_get_running_partition();
  logEvent("FW build=%s v=%lu elf=%s part=%s", FW_BUILD_TAG, (unsigned long)OTA_FW_VERSION, elf,
    running ? running->label : "?");
}
#endif
