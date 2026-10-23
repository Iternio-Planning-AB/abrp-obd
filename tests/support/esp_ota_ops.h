#pragma once
// Host stand-in for the two IDF OTA calls the firmware-build log line reads.
#include <cstdio>
#include <cstring>

struct esp_partition_t { char label[17]; };
inline esp_partition_t host_app_partition{"app1"};
inline const esp_partition_t* host_running_partition = &host_app_partition;

inline const esp_partition_t* esp_ota_get_running_partition() { return host_running_partition; }
// Like IDF 4.4: up to size - 1 hex digits of the app ELF SHA-256, terminated.
inline int esp_ota_get_app_elf_sha256(char* dst, size_t size) {
  std::snprintf(dst, size, "%s", "0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210");
  return static_cast<int>(std::strlen(dst));
}
