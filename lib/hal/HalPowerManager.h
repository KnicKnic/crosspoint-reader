#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <Wire.h>
#include <esp_pm.h>
#include <freertos/semphr.h>

#include <cassert>
#include <cstdint>
#include <string>

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
  esp_pm_config_t savedPmConfig = {};
  bool savedPmConfigValid = false;
  bool autoLightSleepConfigured = false;
  esp_pm_lock_handle_t cpuMaxLock = nullptr;
  esp_pm_lock_handle_t apbMaxLock = nullptr;
  esp_pm_lock_handle_t noLightSleepLock = nullptr;

  bool ensurePmLocks();

 public:
  static constexpr uint8_t LIGHT_SLEEP_WAKE_CAUSE_COUNT = 18;
  static constexpr uint8_t LIGHT_SLEEP_WAKE_CAUSE_UNKNOWN_INDEX = LIGHT_SLEEP_WAKE_CAUSE_COUNT - 1;
  static constexpr uint8_t LIGHT_SLEEP_REQUEST_BUCKET_COUNT = 6;

  struct LightSleepStats {
    uint32_t enterCount = 0;
    uint64_t sleptUs = 0;
    uint64_t requestedUs = 0;
    uint64_t uptimeUs = 0;
    uint64_t earlyWakeCount = 0;
    uint64_t wakeCauseCounts[LIGHT_SLEEP_WAKE_CAUSE_COUNT] = {};
    uint64_t requestBucketCounts[LIGHT_SLEEP_REQUEST_BUCKET_COUNT] = {};
  };

  struct PmLockTimingStats {
    uint64_t fullLockCount = 0;
    uint64_t fullLockHeldUs = 0;
    uint64_t peripheralLockCount = 0;
    uint64_t peripheralLockHeldUs = 0;
  };

  static constexpr int LOW_POWER_FREQ = 10;                    // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Configure IDF automatic light sleep. Intended for scoped use by activities
  // that keep radio tasks alive and can tolerate idle-task sleep.
  bool configureAutoLightSleep(bool enabled);
  bool isAutoLightSleepConfigured() const { return autoLightSleepConfigured; }
  LightSleepStats getLightSleepStats() const;
  PmLockTimingStats getPmLockTimingStats() const;
  std::string formatLightSleepStats() const;
  std::string formatEspTimerActivity() const;
  std::string formatTaskRuntimeActivity() const;
  void formatPmLockActivity(std::string& line1, std::string& line2, std::string& line3, std::string& line4,
                            std::string& line5) const;
  void logLightSleepDiagnostics(const char* reason) const;
  static const char* lightSleepWakeCauseName(uint8_t causeIndex);
  static const char* lightSleepRequestBucketName(uint8_t bucketIndex);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;
    bool cpuLockAcquired = false;
    bool apbLockAcquired = false;
    bool noLightSleepLockAcquired = false;
    int64_t noLightSleepLockAcquiredAtUs = 0;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };

  // Keeps APB-backed peripherals stable and prevents automatic light sleep
  // while a short transaction is in progress.
  class PeripheralLock {
    friend class HalPowerManager;
    bool apbLockAcquired = false;
    bool noLightSleepLockAcquired = false;
    int64_t noLightSleepLockAcquiredAtUs = 0;

   public:
    explicit PeripheralLock();
    ~PeripheralLock();

    PeripheralLock(const PeripheralLock&) = delete;
    PeripheralLock& operator=(const PeripheralLock&) = delete;
    PeripheralLock(PeripheralLock&&) = delete;
    PeripheralLock& operator=(PeripheralLock&&) = delete;
  };
};
