#pragma once

#include <string>

namespace BluetoothDiagnostics {

void recordBoot();
void record(const char* event, const char* detail = nullptr);
void recordf(const char* event, const char* format, ...);
void setStorageFlushSuppressed(bool suppressed);
void flushToStorage(bool force = false);
std::string snapshot();
std::string persistedSnapshot();

}  // namespace BluetoothDiagnostics
