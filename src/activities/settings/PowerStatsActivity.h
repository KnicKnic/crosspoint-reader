#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class PowerStatsActivity final : public Activity {
 public:
  explicit PowerStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PowerStats", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  bool preventAutoSleep() override { return true; }
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  std::vector<std::string> displayLines;
  size_t scrollOffset = 0;
  unsigned long lastRefreshMs = 0;

  void refresh(bool printToSerial = false);
};
