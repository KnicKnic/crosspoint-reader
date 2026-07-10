#include "HalBluetoothManager.h"

#include <BluetoothDiagnostics.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <esp_bt.h>

#include <cstdio>

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

#if defined(CONFIG_BT_CTRL_MODEM_SLEEP) && CONFIG_BT_CTRL_MODEM_SLEEP
  const esp_err_t sleepErr = esp_bt_sleep_enable();
  if (sleepErr == ESP_OK) {
    modemSleepEnabled = true;
    BluetoothDiagnostics::record("bluetooth_modem_sleep_enabled");
    LOG_INF("BT", "Bluetooth modem sleep enabled");
  } else {
    modemSleepEnabled = false;
    LOG_ERR("BT", "Failed to enable Bluetooth modem sleep: %d", sleepErr);
    BluetoothDiagnostics::recordf("bluetooth_modem_sleep_enable_failed", "err=%d", sleepErr);
  }
#endif

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
#if defined(CONFIG_BT_CTRL_MODEM_SLEEP) && CONFIG_BT_CTRL_MODEM_SLEEP
  if (modemSleepEnabled) {
    const esp_err_t sleepErr = esp_bt_sleep_disable();
    if (sleepErr != ESP_OK) {
      LOG_ERR("BT", "Failed to disable Bluetooth modem sleep: %d", sleepErr);
      BluetoothDiagnostics::recordf("bluetooth_modem_sleep_disable_failed", "err=%d", sleepErr);
    }
    modemSleepEnabled = false;
  }
#endif
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

bool HalBluetoothManager::isControllerSleeping() const {
#if defined(CONFIG_BT_CTRL_MODEM_SLEEP) && CONFIG_BT_CTRL_MODEM_SLEEP
  return enabled && modemSleepEnabled && esp_bt_controller_is_sleeping();
#else
  return false;
#endif
}

std::string HalBluetoothManager::formatPowerState() const {
  char buf[96];
#if defined(CONFIG_BT_CTRL_MODEM_SLEEP) && CONFIG_BT_CTRL_MODEM_SLEEP
  const int lpclk = enabled ? static_cast<int>(esp_bt_get_lpclk_src()) : -1;
  snprintf(buf, sizeof(buf), "BT power enabled=%d modem_sleep=%d controller_sleeping=%d lpclk=%d", enabled,
           modemSleepEnabled, isControllerSleeping(), lpclk);
#else
  snprintf(buf, sizeof(buf), "BT power enabled=%d modem_sleep=unsupported controller_sleeping=0 lpclk=-1", enabled);
#endif
  return buf;
}
