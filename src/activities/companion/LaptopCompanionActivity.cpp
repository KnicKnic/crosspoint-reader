#include "LaptopCompanionActivity.h"

#include <Arduino.h>

#include "MappedInputManager.h"
#include "LaptopCompanionView.h"
#include "companion/CompanionBleService.h"

namespace {
constexpr unsigned long NO_HOST_WAKE_GRACE_MS = 10UL * 60UL * 1000UL;
}  // namespace

void LaptopCompanionActivity::onEnter() {
  Activity::onEnter();
  hostConnected = false;
  noHostConnectedSinceMs = 0;
  statusMessage = "Waiting for host";
  microphoneMessage = "Unknown";
  cameraMessage = "Unknown";

  auto& service = CompanionBleService::getInstance();
  service.setStatusChangedCallback([this] { requestUpdate(); });
  if (!service.begin()) {
    statusMessage = "BLE start failed";
  } else {
    statusMessage = service.getStatusText();
  }
  hostConnected = service.isHostConnected();
  updateNoHostTimer(hostConnected);
  requestUpdate();
}

void LaptopCompanionActivity::onExit() {
  CompanionBleService::getInstance().setStatusChangedCallback(nullptr);
  CompanionBleService::getInstance().end();
  Activity::onExit();
}

void LaptopCompanionActivity::loop() {
  auto& service = CompanionBleService::getInstance();
  service.update();
  updateNoHostTimer(service.isHostConnected());
  if (service.consumeStatusChanged()) {
    const auto hostStatus = service.getHostStatus();
    hostConnected = service.isHostConnected();
    statusMessage = service.getStatusText();
    microphoneMessage = LaptopCompanionView::triStateText(hostStatus.microphone, "Muted", "Live");
    cameraMessage = LaptopCompanionView::triStateText(hostStatus.camera, "Off", "Active");
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    statusMessage = service.notifyToggleMuteReleased() ? "Mute button sent" : "Host not connected";
    requestUpdate();
  }
}

bool LaptopCompanionActivity::suppressAutoDeepSleep() {
  return shouldHoldWakeForCompanion();
}

bool LaptopCompanionActivity::preventAutoSleep() {
  return shouldHoldWakeForCompanion();
}

void LaptopCompanionActivity::updateNoHostTimer(bool connected) {
  hostConnected = connected;
  if (hostConnected) {
    noHostConnectedSinceMs = 0;
    return;
  }

  if (noHostConnectedSinceMs == 0) {
    noHostConnectedSinceMs = millis();
  }
}

bool LaptopCompanionActivity::shouldHoldWakeForCompanion() const {
  const auto& service = CompanionBleService::getInstance();
  if (!service.isRunning()) {
    return false;
  }

  if (service.isHostConnected()) {
    return true;
  }

  if (noHostConnectedSinceMs == 0) {
    return true;
  }

  return millis() - noHostConnectedSinceMs < NO_HOST_WAKE_GRACE_MS;
}

void LaptopCompanionActivity::render(RenderLock&&) {
  LaptopCompanionView::render(renderer, mappedInput, hostConnected, statusMessage, microphoneMessage, cameraMessage);
}
