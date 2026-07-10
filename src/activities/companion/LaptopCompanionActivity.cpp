#include "LaptopCompanionActivity.h"

#include <Arduino.h>

#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "LaptopCompanionView.h"
#include "companion/CompanionBleService.h"

namespace {
constexpr unsigned long NO_HOST_WAKE_GRACE_MS = 10UL * 60UL * 1000UL;
constexpr unsigned long POWER_DIAGNOSTIC_INTERVAL_MS = 20UL * 1000UL;
constexpr unsigned long LOOP_DELAY_MS = 1000UL;
constexpr bool COMPANION_POWER_BUTTON_LIGHT_SLEEP_WAKE_ENABLED = false;

struct NamedDelta {
  const char* name = "";
  uint64_t count = 0;
};

void formatDurationUs(uint64_t durationUs, char* buffer, size_t bufferSize) {
  const uint64_t totalMs = durationUs / 1000ULL;
  if (totalMs < 1000ULL) {
    snprintf(buffer, bufferSize, "%llums", static_cast<unsigned long long>(totalMs));
    return;
  }

  const uint64_t totalSeconds = totalMs / 1000ULL;
  if (totalSeconds < 60ULL) {
    snprintf(buffer, bufferSize, "%llus", static_cast<unsigned long long>(totalSeconds));
    return;
  }

  const uint64_t minutes = totalSeconds / 60ULL;
  const uint64_t seconds = totalSeconds % 60ULL;
  if (minutes < 60ULL) {
    snprintf(buffer, bufferSize, "%llum%02llus", static_cast<unsigned long long>(minutes),
             static_cast<unsigned long long>(seconds));
    return;
  }

  const uint64_t hours = minutes / 60ULL;
  const uint64_t remainingMinutes = minutes % 60ULL;
  snprintf(buffer, bufferSize, "%lluh%02llum", static_cast<unsigned long long>(hours),
           static_cast<unsigned long long>(remainingMinutes));
}

std::string formatPowerStatsLine(const HalPowerManager::LightSleepStats& stats) {
  char slept[16];
  char uptime[16];
  formatDurationUs(stats.sleptUs, slept, sizeof(slept));
  formatDurationUs(stats.uptimeUs, uptime, sizeof(uptime));

  char line[80];
  snprintf(line, sizeof(line), "Total: %lu x, sleep %s, boot %s", static_cast<unsigned long>(stats.enterCount), slept,
           uptime);
  return line;
}

std::string formatWakeCauseDeltaLine(const HalPowerManager::LightSleepStats& current,
                                     const HalPowerManager::LightSleepStats& previous, bool hasPrevious) {
  NamedDelta top[5] = {};
  if (hasPrevious) {
    for (uint8_t i = 1; i < HalPowerManager::LIGHT_SLEEP_WAKE_CAUSE_COUNT; i++) {
      const uint64_t currentCount = current.wakeCauseCounts[i];
      const uint64_t previousCount = previous.wakeCauseCounts[i];
      const uint64_t delta = currentCount >= previousCount ? currentCount - previousCount : 0;
      if (delta == 0) {
        continue;
      }

      for (uint8_t slot = 0; slot < 5; slot++) {
        if (delta <= top[slot].count) {
          continue;
        }
        for (uint8_t shift = 4; shift > slot; shift--) {
          top[shift] = top[shift - 1];
        }
        top[slot] = NamedDelta{.name = HalPowerManager::lightSleepWakeCauseName(i), .count = delta};
        break;
      }
    }
  }

  std::string line;
  if (top[0].count == 0) {
    const uint64_t earlyDelta =
        hasPrevious && current.earlyWakeCount >= previous.earlyWakeCount ? current.earlyWakeCount - previous.earlyWakeCount
                                                                         : 0;
    line = "Req:";
    if (earlyDelta > 0) {
      char earlyPart[28];
      snprintf(earlyPart, sizeof(earlyPart), " early+%llu", static_cast<unsigned long long>(earlyDelta));
      line += earlyPart;
    }

    NamedDelta requestTop[3] = {};
    if (hasPrevious) {
      for (uint8_t i = 0; i < HalPowerManager::LIGHT_SLEEP_REQUEST_BUCKET_COUNT; i++) {
        const uint64_t currentCount = current.requestBucketCounts[i];
        const uint64_t previousCount = previous.requestBucketCounts[i];
        const uint64_t delta = currentCount >= previousCount ? currentCount - previousCount : 0;
        if (delta == 0) {
          continue;
        }

        for (uint8_t slot = 0; slot < 3; slot++) {
          if (delta <= requestTop[slot].count) {
            continue;
          }
          for (uint8_t shift = 2; shift > slot; shift--) {
            requestTop[shift] = requestTop[shift - 1];
          }
          requestTop[slot] = NamedDelta{.name = HalPowerManager::lightSleepRequestBucketName(i), .count = delta};
          break;
        }
      }
    }

    if (earlyDelta == 0 && requestTop[0].count == 0) {
      line += " none";
      return line;
    }

    for (const auto& item : requestTop) {
      if (item.count == 0) {
        break;
      }
      char part[32];
      snprintf(part, sizeof(part), " %s+%llu", item.name, static_cast<unsigned long long>(item.count));
      line += part;
    }
    return line;
  } else {
    line = "Wake:";
  }

  for (const auto& item : top) {
    if (item.count == 0) {
      break;
    }
    char part[32];
    snprintf(part, sizeof(part), " %s+%llu", item.name, static_cast<unsigned long long>(item.count));
    line += part;
  }
  return line;
}

std::string formatPowerDeltaLine(const HalPowerManager::LightSleepStats& current,
                                 const HalPowerManager::LightSleepStats& previous, bool hasPrevious) {
  const uint32_t enterDelta =
      hasPrevious && current.enterCount >= previous.enterCount ? current.enterCount - previous.enterCount : 0;
  const uint64_t sleptDelta =
      hasPrevious && current.sleptUs >= previous.sleptUs ? current.sleptUs - previous.sleptUs : 0;
  const uint64_t elapsedDelta =
      hasPrevious && current.uptimeUs >= previous.uptimeUs ? current.uptimeUs - previous.uptimeUs : 0;
  const uint64_t percentTenths = elapsedDelta > 0 ? (sleptDelta * 1000ULL) / elapsedDelta : 0;

  char slept[16];
  char elapsed[16];
  formatDurationUs(sleptDelta, slept, sizeof(slept));
  formatDurationUs(elapsedDelta, elapsed, sizeof(elapsed));

  char line[96];
  snprintf(line, sizeof(line), "Delta: +%lu x, sleep %s / %s (%llu.%01llu%%)", static_cast<unsigned long>(enterDelta),
           slept, elapsed, static_cast<unsigned long long>(percentTenths / 10ULL),
           static_cast<unsigned long long>(percentTenths % 10ULL));
  return line;
}
}  // namespace

LaptopCompanionActivity::LaptopCompanionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("LaptopCompanion", renderer, mappedInput), viewStateMutex(xSemaphoreCreateMutex()) {
  if (!viewStateMutex) {
    LOG_ERR("COMP", "View state mutex create failed");
  }
}

LaptopCompanionActivity::~LaptopCompanionActivity() {
  restoreSerialLogOutput();
  if (viewStateMutex) {
    vSemaphoreDelete(viewStateMutex);
    viewStateMutex = nullptr;
  }
}

void LaptopCompanionActivity::lockViewState() const {
  if (viewStateMutex) {
    xSemaphoreTake(viewStateMutex, portMAX_DELAY);
  }
}

void LaptopCompanionActivity::unlockViewState() const {
  if (viewStateMutex) {
    xSemaphoreGive(viewStateMutex);
  }
}

void LaptopCompanionActivity::quietSerialLogOutput() {
  if (serialLogOutputQuieted) {
    return;
  }

  previousSerialLogOutputEnabled = isSerialLogOutputEnabled();
  serialLogOutputQuieted = true;
  if (!previousSerialLogOutputEnabled) {
    return;
  }

  LOG_INF("COMP", "Serial log output disabled for laptop companion sleep measurement");
  setSerialLogOutputEnabled(false);
}

void LaptopCompanionActivity::restoreSerialLogOutput() {
  if (!serialLogOutputQuieted) {
    return;
  }

  setSerialLogOutputEnabled(previousSerialLogOutputEnabled);
  if (previousSerialLogOutputEnabled) {
    LOG_INF("COMP", "Serial log output restored after laptop companion");
  }
  serialLogOutputQuieted = false;
}

void LaptopCompanionActivity::onEnter() {
  Activity::onEnter();
  quietSerialLogOutput();
  noHostConnectedSinceMs = 0;
  lastPowerDiagnosticAtMs = 0;
  lockViewState();
  viewState.hostConnected = false;
  viewState.statusMessage = "Waiting for host";
  viewState.microphoneMessage = "Unknown";
  viewState.cameraMessage = "Unknown";
  viewState.powerStatsMessage = powerManager.formatLightSleepStats();
  viewState.powerDeltaMessage = "Delta: +0 x, sleep 0ms / 0ms (0.0%)";
  viewState.powerTimerMessage = "Timers: baseline";
  hasLastRenderedViewState = false;
  unlockViewState();
  hasLastPowerDiagnosticStats = false;

  if (COMPANION_POWER_BUTTON_LIGHT_SLEEP_WAKE_ENABLED) {
    gpio.enableX3LightSleepPowerButtonWake(nullptr);
  }
  if (!powerManager.configureAutoLightSleep(true)) {
    LOG_ERR("COMP", "Auto light sleep enable failed");
  }

  auto& service = CompanionBleService::getInstance();
  service.setStatusChangedCallback([this] { requestUpdate(); });
  if (!service.begin()) {
    lockViewState();
    viewState.statusMessage = "BLE start failed";
    unlockViewState();
  } else {
    lockViewState();
    viewState.statusMessage = service.getStatusText();
    unlockViewState();
  }
  updateNoHostTimer(service.isHostConnected());
  updatePowerDiagnostics(true);
  requestUpdate();
}

void LaptopCompanionActivity::onExit() {
  CompanionBleService::getInstance().setStatusChangedCallback(nullptr);
  CompanionBleService::getInstance().end();
  if (COMPANION_POWER_BUTTON_LIGHT_SLEEP_WAKE_ENABLED) {
    gpio.disableX3LightSleepButtonWake();
  }
  powerManager.configureAutoLightSleep(false);
  restoreSerialLogOutput();
  Activity::onExit();
}

void LaptopCompanionActivity::loop() {
  auto& service = CompanionBleService::getInstance();
  service.update();
  updatePowerDiagnostics(false);
  updateNoHostTimer(service.isHostConnected());
  if (service.consumeStatusChanged()) {
    const auto hostStatus = service.getHostStatus();
    lockViewState();
    viewState.hostConnected = service.isHostConnected();
    viewState.statusMessage = service.getStatusText();
    viewState.microphoneMessage = LaptopCompanionView::triStateText(hostStatus.microphone, "Muted", "Live");
    viewState.cameraMessage = LaptopCompanionView::triStateText(hostStatus.camera, "Off", "Active");
    unlockViewState();
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const bool sent = service.notifyToggleMuteReleased();
    lockViewState();
    viewState.statusMessage = sent ? "Mute button sent" : "Host not connected";
    unlockViewState();
    requestUpdate();
  }

  delay(LOOP_DELAY_MS);
}

bool LaptopCompanionActivity::suppressAutoDeepSleep() {
  return shouldHoldWakeForCompanion();
}

bool LaptopCompanionActivity::preventAutoSleep() {
  return shouldHoldWakeForCompanion();
}

bool LaptopCompanionActivity::ownsPowerManagement() {
  return powerManager.isAutoLightSleepConfigured() || CompanionBleService::getInstance().isRunning();
}

void LaptopCompanionActivity::updateNoHostTimer(bool connected) {
  lockViewState();
  viewState.hostConnected = connected;
  unlockViewState();
  if (connected) {
    noHostConnectedSinceMs = 0;
    return;
  }

  if (noHostConnectedSinceMs == 0) {
    noHostConnectedSinceMs = millis();
  }
}

void LaptopCompanionActivity::updatePowerDiagnostics(bool forceLog) {
  const unsigned long now = millis();
  if (!forceLog && lastPowerDiagnosticAtMs != 0 && now - lastPowerDiagnosticAtMs < POWER_DIAGNOSTIC_INTERVAL_MS) {
    return;
  }
  const auto stats = powerManager.getLightSleepStats();
  const std::string statsLine = formatPowerStatsLine(stats);
  const std::string deltaLine = formatPowerDeltaLine(stats, lastPowerDiagnosticStats, hasLastPowerDiagnosticStats);
  const std::string wakeCauseLine =
      formatWakeCauseDeltaLine(stats, lastPowerDiagnosticStats, hasLastPowerDiagnosticStats);
  const std::string timerLine = powerManager.formatEspTimerActivity();

  lockViewState();
  viewState.powerStatsMessage = statsLine;
  viewState.powerDeltaMessage = deltaLine;
  viewState.powerTimerMessage = timerLine;
  unlockViewState();

  lastPowerDiagnosticStats = stats;
  hasLastPowerDiagnosticStats = true;
  lastPowerDiagnosticAtMs = now;
  LOG_INF("COMP", "%s", wakeCauseLine.c_str());
  powerManager.logLightSleepDiagnostics("laptop_companion");
  requestUpdate();
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
  ViewState snapshot;
  bool shouldSkipRender = false;
  lockViewState();
  snapshot = viewState;
  shouldSkipRender = hasLastRenderedViewState && snapshot == lastRenderedViewState;
  if (!shouldSkipRender) {
    lastRenderedViewState = snapshot;
    hasLastRenderedViewState = true;
  }
  unlockViewState();

  if (shouldSkipRender) {
    LOG_DBG("COMP", "Render skipped; view state unchanged");
    return;
  }

  LaptopCompanionView::render(renderer, mappedInput, snapshot.hostConnected, snapshot.statusMessage,
                              snapshot.microphoneMessage, snapshot.cameraMessage, snapshot.powerStatsMessage,
                              snapshot.powerDeltaMessage, snapshot.powerTimerMessage);
}
