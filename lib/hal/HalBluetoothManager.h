#pragma once

#include <string>

class HalBluetoothManager {
 public:
  static HalBluetoothManager& getInstance();
  HalBluetoothManager() = default;

  bool enable(const char* deviceName = "CrossPoint");
  void disable();

  bool isEnabled() const { return enabled; }
  bool isModemSleepEnabled() const { return modemSleepEnabled; }
  bool isControllerSleeping() const;
  std::string formatPowerState() const;
  const std::string& getLastError() const { return lastError; }

 private:
  bool enabled = false;
  bool modemSleepEnabled = false;
  std::string lastError;
};

extern HalBluetoothManager bluetoothManager;
