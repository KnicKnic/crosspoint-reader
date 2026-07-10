#include "LaptopCompanionActivity.h"

#include <Arduino.h>
#include <esp_log.h>

#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <PmLockTrace.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "LaptopCompanionView.h"
#include "companion/CompanionBleService.h"

namespace {
constexpr unsigned long NO_HOST_WAKE_GRACE_MS = 10UL * 60UL * 1000UL;
constexpr unsigned long POWER_DIAGNOSTIC_INTERVAL_MS = 20UL * 1000UL;
constexpr unsigned long LOOP_DELAY_MS = 100UL;
constexpr bool COMPANION_POWER_BUTTON_LIGHT_SLEEP_WAKE_ENABLED = false;
constexpr bool COMPANION_DISABLE_BLE_EXPERIMENT = false;
constexpr bool COMPANION_SERIAL_DIAGNOSTICS_ENABLED = false;

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

  const uint64_t percentTenths = stats.uptimeUs > 0 ? (stats.sleptUs * 1000ULL) / stats.uptimeUs : 0;

  char line[96];
  snprintf(line, sizeof(line), "Total: %lu x, sleep %s / %s (%llu.%01llu%%)",
           static_cast<unsigned long>(stats.enterCount), slept, uptime,
           static_cast<unsigned long long>(percentTenths / 10ULL),
           static_cast<unsigned long long>(percentTenths % 10ULL));
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

std::string formatPowerAccountingLine(const HalPowerManager::LightSleepStats& current,
                                      const HalPowerManager::LightSleepStats& previous, bool hasPrevious) {
  const uint64_t sleptDelta =
      hasPrevious && current.sleptUs >= previous.sleptUs ? current.sleptUs - previous.sleptUs : 0;
  const uint64_t requestedDelta =
      hasPrevious && current.requestedUs >= previous.requestedUs ? current.requestedUs - previous.requestedUs : 0;
  const uint64_t elapsedDelta =
      hasPrevious && current.uptimeUs >= previous.uptimeUs ? current.uptimeUs - previous.uptimeUs : 0;
  const uint64_t awakeDelta = elapsedDelta > sleptDelta ? elapsedDelta - sleptDelta : 0;
  const uint64_t requestedNotSleptDelta = requestedDelta > sleptDelta ? requestedDelta - sleptDelta : 0;
  const uint64_t awakePercentTenths = elapsedDelta > 0 ? (awakeDelta * 1000ULL) / elapsedDelta : 0;
  const uint64_t lostPercentTenths =
      elapsedDelta > 0 ? (requestedNotSleptDelta * 1000ULL) / elapsedDelta : 0;

  char awake[16];
  char requested[16];
  char lost[16];
  formatDurationUs(awakeDelta, awake, sizeof(awake));
  formatDurationUs(requestedDelta, requested, sizeof(requested));
  formatDurationUs(requestedNotSleptDelta, lost, sizeof(lost));

  char line[112];
  snprintf(line, sizeof(line), "Acct: awake %s %llu.%llu%% req %s lost %s %llu.%llu%%", awake,
           static_cast<unsigned long long>(awakePercentTenths / 10ULL),
           static_cast<unsigned long long>(awakePercentTenths % 10ULL), requested, lost,
           static_cast<unsigned long long>(lostPercentTenths / 10ULL),
           static_cast<unsigned long long>(lostPercentTenths % 10ULL));
  return line;
}

std::string formatBlockCostLine(unsigned long renderDurationMs,
                                const HalPowerManager::PmLockTimingStats& currentLocks,
                                const HalPowerManager::PmLockTimingStats& previousLocks, bool hasPreviousLocks) {
  const uint64_t fullCountDelta =
      hasPreviousLocks && currentLocks.fullLockCount >= previousLocks.fullLockCount
          ? currentLocks.fullLockCount - previousLocks.fullLockCount
          : 0;
  const uint64_t fullHeldDelta =
      hasPreviousLocks && currentLocks.fullLockHeldUs >= previousLocks.fullLockHeldUs
          ? currentLocks.fullLockHeldUs - previousLocks.fullLockHeldUs
          : 0;
  const uint64_t peripheralCountDelta =
      hasPreviousLocks && currentLocks.peripheralLockCount >= previousLocks.peripheralLockCount
          ? currentLocks.peripheralLockCount - previousLocks.peripheralLockCount
          : 0;
  const uint64_t peripheralHeldDelta =
      hasPreviousLocks && currentLocks.peripheralLockHeldUs >= previousLocks.peripheralLockHeldUs
          ? currentLocks.peripheralLockHeldUs - previousLocks.peripheralLockHeldUs
          : 0;

  char render[16];
  char full[16];
  char peripheral[16];
  formatDurationUs(static_cast<uint64_t>(renderDurationMs) * 1000ULL, render, sizeof(render));
  formatDurationUs(fullHeldDelta, full, sizeof(full));
  formatDurationUs(peripheralHeldDelta, peripheral, sizeof(peripheral));

  char line[96];
  snprintf(line, sizeof(line), "Block: render %s full %llu/%s per %llu/%s", render,
           static_cast<unsigned long long>(fullCountDelta), full,
           static_cast<unsigned long long>(peripheralCountDelta), peripheral);
  return line;
}

void configureSerialDiagnosticsOutput() {
#ifdef ENABLE_SERIAL_LOG
  const char* debugTags[] = {
      "NimBLE",
      "NimBLEDevice",
      "NimBLEServer",
      "NimBLEAdvertising",
      "NimBLECharacteristic",
      "NimBLEService",
      "NimBLEUtils",
      "BTDM_INIT",
      "BTDM_CONTROLLER",
      "BT_HCI",
      "BTDM",
      "BT_BTM",
      "BT_BTC",
      "BT_APPL",
  };
  for (const char* tag : debugTags) {
    esp_log_level_set(tag, ESP_LOG_DEBUG);
  }

  LOG_INF("COMP", "Serial diagnostics using standard serial setting; auto light sleep disabled");
#ifdef CONFIG_NIMBLE_CPP_LOG_LEVEL
  LOG_INF("COMP", "NimBLE C++ diagnostics compiled at level %d", CONFIG_NIMBLE_CPP_LOG_LEVEL);
#endif
#ifdef CONFIG_BT_NIMBLE_LOG_LEVEL
  LOG_INF("COMP", "NimBLE host diagnostics compiled at level %d", CONFIG_BT_NIMBLE_LOG_LEVEL);
#endif
#endif
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
  if (COMPANION_SERIAL_DIAGNOSTICS_ENABLED) {
    configureSerialDiagnosticsOutput();
  } else {
    quietSerialLogOutput();
  }
  noHostConnectedSinceMs = 0;
  lastPowerDiagnosticAtMs = 0;
  lockViewState();
  viewState.hostConnected = false;
  viewState.statusMessage = "Waiting for host";
  viewState.microphoneMessage = "Unknown";
  viewState.cameraMessage = "Unknown";
  viewState.powerStatsMessage = powerManager.formatLightSleepStats();
  viewState.powerDeltaMessage = "Delta: +0 x, sleep 0ms / 0ms (0.0%)";
  viewState.powerAccountingMessage = "Acct: baseline";
  viewState.wakeCauseMessage = "Req: baseline";
  viewState.powerTimerMessage = "Timers: baseline";
  viewState.bleAdvertiseMessage = COMPANION_DISABLE_BLE_EXPERIMENT ? "BLEA: disabled experiment" : "BLEA: baseline";
  viewState.bleConnectionMessage = COMPANION_DISABLE_BLE_EXPERIMENT ? "BLEC: disabled experiment" : "BLEC: baseline";
  viewState.bleActivityMessage = COMPANION_DISABLE_BLE_EXPERIMENT ? "BLED: disabled experiment" : "BLED: baseline";
  viewState.btLockTraceMessage = "BTLD: baseline";
  viewState.powerTaskMessage = "PM1: baseline";
  viewState.renderCostMessage = "PM2: baseline";
  viewState.pmLockMessage3 = "PM3: baseline";
  viewState.pmLockMessage4 = "PM4: baseline";
  viewState.pmLockMessage5 = "PM5: baseline";
  activePage = LaptopCompanionView::Page::Status;
  unlockViewState();
  LaptopCompanionView::resetRenderCache();
  hasLastPowerDiagnosticStats = false;
  hasLastPowerDiagnosticPmLockStats = false;
  lastRenderDurationMs = 0;

  if (COMPANION_POWER_BUTTON_LIGHT_SLEEP_WAKE_ENABLED) {
    gpio.enableX3LightSleepPowerButtonWake(nullptr);
  }
  if (COMPANION_SERIAL_DIAGNOSTICS_ENABLED) {
    powerManager.configureAutoLightSleep(false);
  } else if (!powerManager.configureAutoLightSleep(true)) {
    LOG_ERR("COMP", "Auto light sleep enable failed");
  }

  if (COMPANION_DISABLE_BLE_EXPERIMENT) {
    lockViewState();
    viewState.statusMessage = "BLE disabled experiment";
    unlockViewState();
    LOG_INF("COMP", "BLE/NimBLE startup skipped for sleep experiment");
  } else {
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
  }
  updateNoHostTimer(false);
  updatePowerDiagnostics(true);
  requestUpdate();
}

void LaptopCompanionActivity::onExit() {
  if (!COMPANION_DISABLE_BLE_EXPERIMENT) {
    CompanionBleService::getInstance().setStatusChangedCallback(nullptr);
    CompanionBleService::getInstance().end();
  }
  if (COMPANION_POWER_BUTTON_LIGHT_SLEEP_WAKE_ENABLED) {
    gpio.disableX3LightSleepButtonWake();
  }
  powerManager.configureAutoLightSleep(false);
  restoreSerialLogOutput();
  Activity::onExit();
}

void LaptopCompanionActivity::loop() {
  auto& service = CompanionBleService::getInstance();
  if (!COMPANION_DISABLE_BLE_EXPERIMENT) {
    service.update();
  }
  updatePowerDiagnostics(false);
  updateNoHostTimer(!COMPANION_DISABLE_BLE_EXPERIMENT && service.isHostConnected());
  if (!COMPANION_DISABLE_BLE_EXPERIMENT && service.consumeStatusChanged()) {
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    lockViewState();
    activePage = activePage == LaptopCompanionView::Page::Status ? LaptopCompanionView::Page::Diagnostics
                                                                 : LaptopCompanionView::Page::Status;
    unlockViewState();
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const bool sent = !COMPANION_DISABLE_BLE_EXPERIMENT && service.notifyToggleMuteReleased();
    lockViewState();
    viewState.statusMessage =
        COMPANION_DISABLE_BLE_EXPERIMENT ? "BLE disabled experiment" : (sent ? "Mute button sent" : "Host not connected");
    unlockViewState();
    requestUpdate();
  }

  delay(LOOP_DELAY_MS);
}

bool LaptopCompanionActivity::suppressAutoDeepSleep() {
  if (COMPANION_SERIAL_DIAGNOSTICS_ENABLED) {
    return true;
  }
  if (COMPANION_DISABLE_BLE_EXPERIMENT) {
    return true;
  }
  return shouldHoldWakeForCompanion();
}

bool LaptopCompanionActivity::preventAutoSleep() {
  if (COMPANION_SERIAL_DIAGNOSTICS_ENABLED) {
    return true;
  }
  if (COMPANION_DISABLE_BLE_EXPERIMENT) {
    return true;
  }
  return shouldHoldWakeForCompanion();
}

bool LaptopCompanionActivity::ownsPowerManagement() {
  return COMPANION_SERIAL_DIAGNOSTICS_ENABLED || powerManager.isAutoLightSleepConfigured() ||
         CompanionBleService::getInstance().isRunning();
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
  const auto pmLockStats = powerManager.getPmLockTimingStats();
  const std::string statsLine = formatPowerStatsLine(stats);
  const std::string deltaLine = formatPowerDeltaLine(stats, lastPowerDiagnosticStats, hasLastPowerDiagnosticStats);
  const std::string accountingLine =
      formatPowerAccountingLine(stats, lastPowerDiagnosticStats, hasLastPowerDiagnosticStats);
  const std::string wakeCauseLine =
      formatWakeCauseDeltaLine(stats, lastPowerDiagnosticStats, hasLastPowerDiagnosticStats);
  const std::string timerLine = powerManager.formatEspTimerActivity();
  const std::string bleTiming =
      COMPANION_DISABLE_BLE_EXPERIMENT ? "BLEA: disabled experiment\nBLEC: disabled experiment"
                                       : CompanionBleService::getInstance().formatTimingDiagnostics();
  const size_t bleLineBreak = bleTiming.find('\n');
  const std::string bleAdvertiseLine =
      bleLineBreak == std::string::npos ? bleTiming : bleTiming.substr(0, bleLineBreak);
  const std::string bleConnectionLine =
      bleLineBreak == std::string::npos ? "BLEC: unavailable" : bleTiming.substr(bleLineBreak + 1);
  const std::string bleActivityLine =
      COMPANION_DISABLE_BLE_EXPERIMENT ? "BLED: disabled experiment"
                                       : CompanionBleService::getInstance().formatActivityDeltaDiagnostics();
  const std::string btLockTraceLine = formatBtLockTraceDiagnostics();
  std::string pmLockLine1;
  std::string pmLockLine2;
  std::string pmLockLine3;
  std::string pmLockLine4;
  std::string pmLockLine5;
  powerManager.formatPmLockActivity(pmLockLine1, pmLockLine2, pmLockLine3, pmLockLine4, pmLockLine5);

  lockViewState();
  viewState.powerStatsMessage = statsLine;
  viewState.powerDeltaMessage = deltaLine;
  viewState.powerAccountingMessage = accountingLine;
  viewState.wakeCauseMessage = wakeCauseLine;
  viewState.powerTimerMessage = timerLine;
  viewState.bleAdvertiseMessage = bleAdvertiseLine;
  viewState.bleConnectionMessage = bleConnectionLine;
  viewState.bleActivityMessage = bleActivityLine;
  viewState.btLockTraceMessage = btLockTraceLine;
  viewState.powerTaskMessage = pmLockLine1;
  viewState.renderCostMessage = pmLockLine2;
  viewState.pmLockMessage3 = pmLockLine3;
  viewState.pmLockMessage4 = pmLockLine4;
  viewState.pmLockMessage5 = pmLockLine5;
  unlockViewState();

  lastPowerDiagnosticStats = stats;
  lastPowerDiagnosticPmLockStats = pmLockStats;
  hasLastPowerDiagnosticStats = true;
  hasLastPowerDiagnosticPmLockStats = true;
  lastPowerDiagnosticAtMs = now;
  LOG_INF("COMP", "%s", accountingLine.c_str());
  LOG_INF("COMP", "%s", wakeCauseLine.c_str());
  LOG_INF("COMP", "%s", bleAdvertiseLine.c_str());
  LOG_INF("COMP", "%s", bleConnectionLine.c_str());
  LOG_INF("COMP", "%s", bleActivityLine.c_str());
  LOG_INF("COMP", "%s", btLockTraceLine.c_str());
  LOG_INF("COMP", "%s", pmLockLine1.c_str());
  LOG_INF("COMP", "%s", pmLockLine2.c_str());
  LOG_INF("COMP", "%s", pmLockLine3.c_str());
  LOG_INF("COMP", "%s", pmLockLine4.c_str());
  LOG_INF("COMP", "%s", pmLockLine5.c_str());
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
  LaptopCompanionView::State snapshot;
  LaptopCompanionView::Page snapshotPage;
  lockViewState();
  snapshot = viewState;
  snapshotPage = activePage;
  unlockViewState();

  const unsigned long renderStartMs = millis();
  if (LaptopCompanionView::renderIfChanged(renderer, mappedInput, snapshotPage, snapshot)) {
    lastRenderDurationMs = millis() - renderStartMs;
  }
}
