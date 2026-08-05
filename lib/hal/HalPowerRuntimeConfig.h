#pragma once

#include <cstdint>

struct HalPowerRuntimeConfig {
  bool powerSavingEnabled = true;
  uint8_t idlePowerSavingDelaySec = 3;
  uint8_t lowPowerFrequencyMhz = 10;
  uint8_t maxCpuFrequencyMhz = 160;
  bool usbPollingEnabled = true;
  uint8_t usbPollIntervalTenths = 10;
  bool batteryPollingEnabled = true;
  uint8_t batteryPollIntervalTenths = 15;
  bool clockPollingEnabled = true;
  uint8_t clockPollIntervalTenths = 100;
  bool tiltPollingEnabled = true;
  uint8_t tiltPollIntervalMs = 50;
};

extern HalPowerRuntimeConfig halPowerConfig;

