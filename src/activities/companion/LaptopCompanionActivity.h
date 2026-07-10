#pragma once

#include "../Activity.h"

#include <HalPowerManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string>

class LaptopCompanionActivity final : public Activity {
  struct ViewState {
    bool hostConnected = false;
    std::string statusMessage = "Waiting for host";
    std::string microphoneMessage = "Unknown";
    std::string cameraMessage = "Unknown";
    std::string powerStatsMessage;
    std::string powerDeltaMessage;
    std::string powerTimerMessage;

    bool operator==(const ViewState& other) const {
      return hostConnected == other.hostConnected && statusMessage == other.statusMessage &&
             microphoneMessage == other.microphoneMessage && cameraMessage == other.cameraMessage &&
             powerStatsMessage == other.powerStatsMessage && powerDeltaMessage == other.powerDeltaMessage &&
             powerTimerMessage == other.powerTimerMessage;
    }
  };

  SemaphoreHandle_t viewStateMutex = nullptr;
  ViewState viewState;
  ViewState lastRenderedViewState;
  bool hasLastRenderedViewState = false;
  unsigned long noHostConnectedSinceMs = 0;
  unsigned long lastPowerDiagnosticAtMs = 0;
  HalPowerManager::LightSleepStats lastPowerDiagnosticStats;
  bool hasLastPowerDiagnosticStats = false;
  bool previousSerialLogOutputEnabled = false;
  bool serialLogOutputQuieted = false;

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
