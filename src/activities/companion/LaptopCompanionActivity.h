#pragma once

#include "../Activity.h"

#include <HalPowerManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string>

#include "LaptopCompanionView.h"

class LaptopCompanionActivity final : public Activity {
  SemaphoreHandle_t viewStateMutex = nullptr;
  LaptopCompanionView::State viewState;
  LaptopCompanionView::Page activePage = LaptopCompanionView::Page::Status;
  unsigned long noHostConnectedSinceMs = 0;
  unsigned long lastPowerDiagnosticAtMs = 0;
  HalPowerManager::LightSleepStats lastPowerDiagnosticStats;
  HalPowerManager::PmLockTimingStats lastPowerDiagnosticPmLockStats;
  bool hasLastPowerDiagnosticStats = false;
  bool hasLastPowerDiagnosticPmLockStats = false;
  bool previousSerialLogOutputEnabled = false;
  bool serialLogOutputQuieted = false;
  unsigned long lastRenderDurationMs = 0;

  void lockViewState() const;
  void unlockViewState() const;
  void quietSerialLogOutput();
  void restoreSerialLogOutput();
  void updateNoHostTimer(bool connected);
  bool shouldHoldWakeForCompanion() const;
  void updatePowerDiagnostics(bool forceLog);

 public:
  explicit LaptopCompanionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~LaptopCompanionActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;
  bool suppressAutoDeepSleep() override;
  bool ownsPowerManagement() override;
  bool isCompanionActivity() const override { return true; }
};
