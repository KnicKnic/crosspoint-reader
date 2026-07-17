#include "HalPowerManager.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_sleep.h>

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

namespace {
void logHeapSnapshot(const char* label) {
  LOG_INF("PWR", "%s heap: free=%d total=%d minFree=%d maxAlloc=%d", label, ESP.getFreeHeap(), ESP.getHeapSize(),
          ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
}
}  // namespace

void HalPowerManager::begin() {
  if (gpio.deviceIsX3()) {
    // X3 uses an I2C fuel gauge for battery monitoring.
    // I2C init must come AFTER gpio.begin() so early hardware detection/probes are finished.
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    Wire.setTimeOut(4);
    _batteryUseI2C = true;
  } else {
    pinMode(BAT_GPIO0, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::configureMemoryTesting(bool memoryTesting, bool autoLightSleep) {
  logHeapSnapshot("Memory test configure start");
  LOG_INF("PWR", "Memory testing setting=%s, auto light sleep setting=%s", memoryTesting ? "on" : "off",
          autoLightSleep ? "on" : "off");

  if (!memoryTesting) {
    memoryTestingActive = false;
    autoLightSleepActive = false;
    setPmLocks(true, true);
    LOG_INF("PWR", "Memory testing disabled");
    logHeapSnapshot("Memory test configure done");
    return;
  }

#if CROSSPOINT_HAL_HAS_ESP_PM
  esp_pm_config_t config = {};
  config.max_freq_mhz = normalFreq > 0 ? normalFreq : getCpuFrequencyMhz();
  config.min_freq_mhz = LOW_POWER_FREQ;
  config.light_sleep_enable = autoLightSleep;

  const esp_err_t configureResult = esp_pm_configure(&config);
  if (configureResult != ESP_OK) {
    LOG_ERR("PWR", "esp_pm_configure failed: %s", esp_err_to_name(configureResult));
    memoryTestingActive = false;
    autoLightSleepActive = false;
    logHeapSnapshot("After failed PM configure");
    logHeapSnapshot("Memory test configure done");
    return;
  }

  logHeapSnapshot("After PM configure");

  if (!pmCpuLock) {
    const esp_err_t result = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "crosspoint_cpu", &pmCpuLock);
    if (result != ESP_OK) {
      LOG_ERR("PWR", "CPU PM lock create failed: %s", esp_err_to_name(result));
    } else {
      LOG_INF("PWR", "CPU PM lock created");
    }
  } else {
    LOG_INF("PWR", "CPU PM lock already exists");
  }
  logHeapSnapshot("After CPU PM lock setup");

  if (autoLightSleep && !pmNoLightSleepLock) {
    const esp_err_t result = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "crosspoint_awake", &pmNoLightSleepLock);
    if (result != ESP_OK) {
      LOG_ERR("PWR", "No-light-sleep PM lock create failed: %s", esp_err_to_name(result));
    } else {
      LOG_INF("PWR", "No-light-sleep PM lock created");
    }
  } else if (autoLightSleep) {
    LOG_INF("PWR", "No-light-sleep PM lock already exists");
  } else {
    LOG_INF("PWR", "No-light-sleep PM lock not requested");
  }
  logHeapSnapshot("After no-light-sleep PM lock setup");

  memoryTestingActive = pmCpuLock != nullptr;
  autoLightSleepActive = memoryTestingActive && autoLightSleep && pmNoLightSleepLock != nullptr;
  setPmLocks(memoryTestingActive, autoLightSleepActive);
  logHeapSnapshot("After PM lock apply");
  if (autoLightSleepActive) {
    LOG_INF("PWR", "No-light-sleep PM lock is sticky and will remain held until reboot");
  }
  LOG_INF("PWR", "Memory testing %s, auto light sleep %s", memoryTestingActive ? "enabled" : "disabled",
          autoLightSleepActive ? "enabled" : "disabled");
  logHeapSnapshot("Memory test configure done");
#else
  LOG_ERR("PWR", "Memory testing requested but this build does not have CONFIG_PM_ENABLE");
  memoryTestingActive = false;
  autoLightSleepActive = false;
  logHeapSnapshot("After skipped PM configure");
  logHeapSnapshot("Memory test configure done");
#endif
}

void HalPowerManager::setPmLocks(bool cpuMax, bool noLightSleep) {
#if CROSSPOINT_HAL_HAS_ESP_PM
  if (pmCpuLock) {
    if (cpuMax && !pmCpuLockHeld) {
      const esp_err_t result = esp_pm_lock_acquire(pmCpuLock);
      if (result == ESP_OK) pmCpuLockHeld = true;
    } else if (!cpuMax && pmCpuLockHeld) {
      const esp_err_t result = esp_pm_lock_release(pmCpuLock);
      if (result == ESP_OK) pmCpuLockHeld = false;
    }
  }

  if (pmNoLightSleepLock) {
    if (noLightSleep && !pmNoLightSleepLockHeld) {
      const esp_err_t result = esp_pm_lock_acquire(pmNoLightSleepLock);
      if (result == ESP_OK) pmNoLightSleepLockHeld = true;
    }
  }
#else
  (void)cpuMax;
  (void)noLightSleep;
#endif
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (memoryTestingActive) {
    const bool lowPowerAllowed = mode == None && enabled;
    setPmLocks(!lowPowerAllowed, autoLightSleepActive);
    isLowPower = lowPowerAllowed;
    return;
  }

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

  // Pre-sleep routines from the original firmware
  // GPIO13 is connected to battery latch MOSFET, we need to make sure it's low during sleep
  // Note that this means the MCU will be completely powered off during sleep, including RTC
  constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
  gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SPIWP, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(GPIO_SPIWP);
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  // Arm the wakeup trigger *after* the button is released
  // Note: this is only useful for waking up on USB power. On battery, the MCU will be completely powered off, so the
  // power button is hard-wired to briefly provide power to the MCU, waking it up regardless of the wakeup source
  // configuration
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  if (_batteryUseI2C) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    // Read SOC directly from I2C fuel gauge (16-bit LE register).
    // On I2C error, keep last known value to avoid UI jitter/slowdowns.
    Wire.beginTransmission(I2C_ADDR_BQ27220);
    Wire.write(BQ27220_SOC_REG);
    if (Wire.endTransmission(false) != 0) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    Wire.requestFrom(I2C_ADDR_BQ27220, (uint8_t)2);
    if (Wire.available() < 2) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    const uint16_t soc = (hi << 8) | lo;
    _batteryCachedPercent = soc > 100 ? 100 : soc;
    _batteryLastPollMs = now;
    return _batteryCachedPercent;
  }
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
