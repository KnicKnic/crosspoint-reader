#pragma once

#include <cstdint>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class PowerSettingsActivity final : public Activity {
 public:
  explicit PowerSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PowerSettings", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Item : uint8_t {
    CpuPowerSaving,
    IdleDelay,
    LowPowerFreq,
    MaxCpuFreq,
    AutoLightSleep,
    UsbPolling,
    UsbPollInterval,
    BatteryPolling,
    BatteryPollInterval,
    ClockPolling,
    ClockPollInterval,
    TiltPolling,
    TiltPollInterval,
  };

  ButtonNavigator buttonNavigator;
  std::vector<Item> items;
  int selectedIndex = 0;

  void rebuildItems();
  void handleSelection();
  void openIntervalPicker(Item item);
  const char* itemName(Item item) const;
  std::string itemValue(Item item) const;
};
