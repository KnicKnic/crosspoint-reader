#include "HalPowerStats.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include <cstddef>

namespace {
constexpr size_t kProbeCount = static_cast<size_t>(HalPowerStats::Probe::Count);
HalPowerStats::Counter counters[kProbeCount];
portMUX_TYPE countersMux = portMUX_INITIALIZER_UNLOCKED;

size_t indexOf(HalPowerStats::Probe probe) {
  const size_t index = static_cast<size_t>(probe);
  return index < kProbeCount ? index : 0;
}
}  // namespace

HalPowerStats::ScopedProbe::ScopedProbe(Probe probe) : probe_(probe), startUs_(esp_timer_get_time()) {}

HalPowerStats::ScopedProbe::~ScopedProbe() {
  const uint64_t nowUs = esp_timer_get_time();
  HalPowerStats::add(probe_, nowUs - startUs_);
}

void HalPowerStats::add(Probe probe, uint64_t elapsedUs) {
  const size_t index = indexOf(probe);
  portENTER_CRITICAL(&countersMux);
  counters[index].queries++;
  counters[index].totalUs += elapsedUs;
  portEXIT_CRITICAL(&countersMux);
}

HalPowerStats::Counter HalPowerStats::get(Probe probe) {
  const size_t index = indexOf(probe);
  Counter snapshot;
  portENTER_CRITICAL(&countersMux);
  snapshot = counters[index];
  portEXIT_CRITICAL(&countersMux);
  return snapshot;
}

const char* HalPowerStats::name(Probe probe) {
  switch (probe) {
    case Probe::BatteryPercent:
      return "battery";
    case Probe::UsbCharge:
      return "usb-charge";
    case Probe::RtcClock:
      return "rtc";
    case Probe::ImuGyro:
      return "imu";
    case Probe::X3Fingerprint:
      return "x3-probe";
    case Probe::Count:
      break;
  }
  return "unknown";
}
