#pragma once

#include <Arduino.h>

#include <cstdint>

class HalPowerStats {
 public:
  enum class Probe : uint8_t {
    BatteryPercent,
    UsbCharge,
    RtcClock,
    ImuGyro,
    X3Fingerprint,
    Count,
  };

  struct Counter {
    uint32_t queries = 0;
    uint64_t totalUs = 0;
  };

  class ScopedProbe {
   public:
    explicit ScopedProbe(Probe probe);
    ~ScopedProbe();

    ScopedProbe(const ScopedProbe&) = delete;
    ScopedProbe& operator=(const ScopedProbe&) = delete;

   private:
    Probe probe_;
    uint64_t startUs_;
  };

  static void add(Probe probe, uint64_t elapsedUs);
  static Counter get(Probe probe);
  static const char* name(Probe probe);
};

