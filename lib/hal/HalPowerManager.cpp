#include "HalPowerManager.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_sleep.h>

#include <algorithm>
#include <cassert>

#include "HalGPIO.h"

namespace {
constexpr gpio_num_t BATTERY_LATCH_PIN = GPIO_NUM_13;
}

HalPowerManager powerManager;  // Singleton instance

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

#if CONFIG_PM_ENABLE
  esp_err_t err = configurePm(false);
  if (err == ESP_OK) {
    err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "crosspoint-active", &cpuMaxLock);
    if (err == ESP_OK) {
      pmConfigured = true;
      LOG_INF("PWR", "ESP-IDF power management enabled: max=%d MHz min=%d MHz autosleep=off",
              getConfiguredMaxFrequencyMhz(), getConfiguredMinFrequencyMhz());
    } else {
      LOG_ERR("PWR", "Failed to create CPU PM lock: %s", esp_err_to_name(err));
    }
  } else {
    LOG_ERR("PWR", "Failed to configure ESP-IDF power management: %s", esp_err_to_name(err));
  }
#endif
}

#if CONFIG_PM_ENABLE
esp_err_t HalPowerManager::configurePm(bool lightSleepEnable) {
  esp_pm_config_t pmConfig = {};
  pmConfig.max_freq_mhz = DESIRED_MAX_FREQ;
  pmConfig.min_freq_mhz = LOW_POWER_FREQ;
  pmConfig.light_sleep_enable = lightSleepEnable;

  return esp_pm_configure(&pmConfig);
}
#endif

void HalPowerManager::setAutoLightSleep(bool enabled) {
#if CONFIG_PM_ENABLE
  if (!pmConfigured) {
    autoLightSleepEnabled = false;
    return;
  }
  if (enabled == autoLightSleepEnabled) {
    return;
  }

#ifdef ENABLE_SERIAL_LOG
  if (enabled) {
    if (logSerial) {
      LOG_INF("PWR", "Disabling serial before enabling automatic light sleep");
      logSerial.flush();
      delay(20);
    }
    logSerial.end();
    serialSuspendedForLightSleep = true;
  }
#endif

  if (enabled) {
    gpio_hold_dis(BATTERY_LATCH_PIN);
    gpio_set_direction(BATTERY_LATCH_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(BATTERY_LATCH_PIN, 1);
    gpio_hold_en(BATTERY_LATCH_PIN);
  }

  const esp_err_t err = configurePm(enabled);
  if (err == ESP_OK) {
    autoLightSleepEnabled = enabled;
#ifdef ENABLE_SERIAL_LOG
    if (!enabled && serialSuspendedForLightSleep) {
      logSerial.begin(115200);
      logSerial.setTxTimeoutMs(1);
      serialSuspendedForLightSleep = false;
      LOG_INF("PWR", "Serial restored after disabling automatic light sleep");
    }
#endif
    LOG_INF("PWR", "Automatic light sleep %s", enabled ? "enabled" : "disabled");
  } else {
#ifdef ENABLE_SERIAL_LOG
    if (enabled && serialSuspendedForLightSleep) {
      logSerial.begin(115200);
      logSerial.setTxTimeoutMs(1);
      serialSuspendedForLightSleep = false;
    }
#endif
    LOG_ERR("PWR", "Failed to %s automatic light sleep: %s", enabled ? "enable" : "disable", esp_err_to_name(err));
  }
#else
  (void)enabled;
#endif
}

bool HalPowerManager::isAutoLightSleepEnabled() const {
#if CONFIG_PM_ENABLE
  return autoLightSleepEnabled;
#else
  return false;
#endif
}

bool HalPowerManager::isPowerManagementConfigured() const {
#if CONFIG_PM_ENABLE
  return pmConfigured;
#else
  return false;
#endif
}

int HalPowerManager::getConfiguredMaxFrequencyMhz() const {
#if CONFIG_PM_ENABLE
  if (pmConfigured) {
    esp_pm_config_t config = {};
    if (esp_pm_get_configuration(&config) == ESP_OK) return config.max_freq_mhz;
  }
#endif
  return normalFreq;
}

int HalPowerManager::getConfiguredMinFrequencyMhz() const {
#if CONFIG_PM_ENABLE
  if (pmConfigured) {
    esp_pm_config_t config = {};
    if (esp_pm_get_configuration(&config) == ESP_OK) return config.min_freq_mhz;
  }
#endif
  return isLowPower ? LOW_POWER_FREQ : normalFreq;
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

void HalPowerManager::startDeepSleep(HalGPIO& gpio) {
#if CONFIG_PM_ENABLE
  if (pmConfigured && cpuMaxLock != nullptr && !cpuMaxLockAcquired) {
    LOG_DBG("PWR", "Acquiring CPU max-frequency PM lock before deep sleep");
    if (esp_pm_lock_acquire(cpuMaxLock) == ESP_OK) {
      cpuMaxLockAcquired = true;
      isLowPower = false;
    }
  }
#else
  setPowerSaving(false);
#endif

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
  gpio_hold_dis(BATTERY_LATCH_PIN);
  gpio_set_direction(BATTERY_LATCH_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(BATTERY_LATCH_PIN, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(BATTERY_LATCH_PIN);
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
    //powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
