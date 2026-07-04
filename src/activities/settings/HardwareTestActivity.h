#pragma once

#include <string>

#include <driver/gpio.h>
#include <esp_sleep.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class HardwareTestActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedPage = 0;
  int selectedAction = 0;
  unsigned long lastRefreshMs = 0;
  std::string lastButtonEvent = "None";
  std::string lastSleepReport = "No sleep test yet";
  bool sleepPromptVisible = false;
  std::string sleepPrompt;

  static constexpr int actionCount = 8;
  static constexpr int pageCount = 3;

  void updateButtonEvent();
  void nextPage();
  void previousPage();
  void runTimerOnlyLightSleepTest();
  void runTimerNoLatchLightSleepTest();
  void runGpio1LightSleepTest();
  void runGpio2LightSleepTest();
  void runPowerButtonLightSleepTest();
  void runGyroLightSleepTest();
  void runGyroInterruptScan();
  void scanGyroInterruptRoute(bool useInt1, unsigned long durationMs);
  void runLightSleepTest(const char* label, gpio_num_t wakePin, gpio_int_type_t wakeLevel, uint64_t timeoutUs,
                         bool enableGpioWake, bool holdLatchDuringSleep = true);
  std::string buildButtonStateLine() const;
  std::string buildWakeCauseLine(esp_sleep_wakeup_cause_t cause, uint64_t gpioMask, unsigned long elapsedMs) const;
  void renderActionsPage(int& y);
  void renderMotionPage(int& y);
  void renderGpioPage(int& y);

 public:
  explicit HardwareTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Hardware Test", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool suppressAutoDeepSleep() override { return true; }
  bool ownsPowerManagement() override { return true; }
};
