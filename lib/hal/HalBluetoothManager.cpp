#include "HalBluetoothManager.h"

#include <BluetoothDiagnostics.h>
#include <Logging.h>
#include <NimBLEDevice.h>

HalBluetoothManager bluetoothManager;

HalBluetoothManager& HalBluetoothManager::getInstance() { return bluetoothManager; }

bool HalBluetoothManager::enable(const char* deviceName) {
  if (enabled) {
    return true;
  }

  lastError.clear();
  const char* resolvedName = (deviceName && *deviceName) ? deviceName : "CrossPoint";
  if (!NimBLEDevice::init(resolvedName)) {
    lastError = "NimBLE init failed";
    LOG_ERR("BT", "Failed to initialize Bluetooth stack");
    BluetoothDiagnostics::record("bluetooth_enable_failed");
    return false;
  }

  enabled = true;
  BluetoothDiagnostics::record("bluetooth_enabled");
  LOG_INF("BT", "Bluetooth stack enabled as %s", resolvedName);
  return true;
}

void HalBluetoothManager::disable() {
  if (!enabled) {
    return;
  }

  NimBLEDevice::stopAdvertising();
  if (!NimBLEDevice::deinit(true)) {
    lastError = "NimBLE deinit failed";
    LOG_ERR("BT", "Failed to deinitialize Bluetooth stack");
    BluetoothDiagnostics::record("bluetooth_disable_failed");
    return;
  }

  enabled = false;
  lastError.clear();
  BluetoothDiagnostics::record("bluetooth_disabled");
  LOG_INF("BT", "Bluetooth stack disabled");
}
