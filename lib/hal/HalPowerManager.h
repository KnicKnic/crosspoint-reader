#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <Wire.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

#if CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  // I2C fuel gauge configuration for X3 battery monitoring
  bool _batteryUseI2C = false;                   // True if using I2C fuel gauge (X3), false for ADC (X4)
  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode
#if CONFIG_PM_ENABLE
  esp_pm_lock_handle_t cpuMaxLock = nullptr;
  esp_pm_lock_handle_t noLightSleepLock = nullptr;
  bool cpuMaxLockAcquired = false;
  bool pmConfigured = false;
  bool autoLightSleepEnabled = false;
  bool serialSuspendedForLightSleep = false;
  esp_err_t configurePm(bool lightSleepEnable);
#endif

 public:
  static constexpr int DESIRED_MAX_FREQ = 160;                 // MHz
  static constexpr int LOW_POWER_FREQ = 10;                    // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);
  void applyRuntimeSettings();
  unsigned long getIdlePowerSavingMs() const;

  void setAutoLightSleep(bool enabled);
  bool isAutoLightSleepEnabled() const;
  bool isPowerManagementConfigured() const;
  int getConfiguredMaxFrequencyMhz() const;
  int getConfiguredMinFrequencyMhz() const;

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio);

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;
    bool cpuLockAcquired = false;
    bool lightSleepLockAcquired = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
