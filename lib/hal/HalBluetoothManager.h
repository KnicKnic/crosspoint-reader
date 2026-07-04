#pragma once

#include <string>

class HalBluetoothManager {
 public:
  static HalBluetoothManager& getInstance();
  HalBluetoothManager() = default;

  bool enable(const char* deviceName = "CrossPoint");
  void disable();

  bool isEnabled() const { return enabled; }
  const std::string& getLastError() const { return lastError; }

 private:
  bool enabled = false;
  std::string lastError;
};

extern HalBluetoothManager bluetoothManager;
