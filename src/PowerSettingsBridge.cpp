#include "PowerSettingsBridge.h"

#include <HalPowerManager.h>
#include <HalPowerRuntimeConfig.h>

#include "CrossPointSettings.h"

void applyPowerSettingsToHal() {
  halPowerConfig.powerSavingEnabled = SETTINGS.powerSavingEnabled != 0;
  halPowerConfig.idlePowerSavingDelaySec = SETTINGS.idlePowerSavingDelaySec;
  halPowerConfig.lowPowerFrequencyMhz = SETTINGS.lowPowerFrequencyMhz;
  halPowerConfig.maxCpuFrequencyMhz = SETTINGS.maxCpuFrequencyMhz;
  halPowerConfig.usbPollingEnabled = SETTINGS.usbPollingEnabled != 0;
  halPowerConfig.usbPollIntervalTenths = SETTINGS.usbPollIntervalTenths;
  halPowerConfig.batteryPollingEnabled = SETTINGS.batteryPollingEnabled != 0;
  halPowerConfig.batteryPollIntervalTenths = SETTINGS.batteryPollIntervalTenths;
  halPowerConfig.clockPollingEnabled = SETTINGS.clockPollingEnabled != 0;
  halPowerConfig.clockPollIntervalTenths = SETTINGS.clockPollIntervalTenths;
  halPowerConfig.tiltPollingEnabled = SETTINGS.tiltPollingEnabled != 0;
  halPowerConfig.tiltPollIntervalMs = SETTINGS.tiltPollIntervalMs;

  powerManager.applyRuntimeSettings();
}
