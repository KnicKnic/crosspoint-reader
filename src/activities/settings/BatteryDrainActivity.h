#pragma once

#include <string>

#include <driver/gpio.h>
#include <esp_sleep.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BatteryDrainActivity final : public Activity {
  enum class State { Idle, Running, Complete, Error };

  ButtonNavigator buttonNavigator;
  State state = State::Idle;
  int selectedMode = 0;
  std::string status = "Ready";
  std::string lastResult = "No drain test yet";

  static constexpr int modeCount = 5;
  static constexpr gpio_num_t X3_BATTERY_LATCH_PIN = GPIO_NUM_13;

  const char* selectedModeLabel() const;
  uint64_t selectedSleepDurationUs() const;
  bool readBQ27220U16(uint8_t reg, uint16_t& value) const;
  std::string buildBatterySnapshot(const char* phase, unsigned long elapsedMs, esp_sleep_wakeup_cause_t cause,
                                   uint64_t gpioMask) const;
  bool appendLog(const std::string& text);
  void holdX3BatteryLatch() const;
  void releaseX3BatteryLatchHold() const;
  void runDrainTest();

 public:
  explicit BatteryDrainActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Battery Drain", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool suppressAutoDeepSleep() override { return true; }
  bool ownsPowerManagement() override { return true; }
};
