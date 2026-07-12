#include "HardwareTestActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalTiltSensor.h>
#include <InputManager.h>
#include <Logging.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstdarg>
#include <iterator>

#include "CrossPointSettings.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr uint64_t LIGHT_SLEEP_TIMEOUT_US = 10ULL * 1000ULL * 1000ULL;
constexpr uint32_t AUTO_LIGHT_SLEEP_BLOCK_MS = 30UL * 1000UL;
constexpr unsigned long REFRESH_INTERVAL_MS = 350;
constexpr unsigned long MOTION_TEST_SETTLE_MS = 2500;
constexpr gpio_num_t X3_BATTERY_LATCH_PIN = GPIO_NUM_13;
constexpr uint32_t AUTO_LIGHT_SLEEP_BUTTON_NOTIFY = 0x01;

volatile TaskHandle_t autoLightSleepWaitTask = nullptr;

void IRAM_ATTR autoLightSleepButtonInterrupt() {
  TaskHandle_t task = autoLightSleepWaitTask;
  if (!task) {
    return;
  }

  autoLightSleepWaitTask = nullptr;
  BaseType_t higherPriorityTaskWoken = pdFALSE;
  xTaskNotifyFromISR(task, AUTO_LIGHT_SLEEP_BUTTON_NOTIFY, eSetBits, &higherPriorityTaskWoken);
  if (higherPriorityTaskWoken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

esp_err_t enableAutoLightSleepButtonWakeForTest() {
  if (!gpio.deviceIsX3()) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  gpio_wakeup_disable(GPIO_NUM_1);
  gpio_wakeup_disable(GPIO_NUM_2);
  gpio_wakeup_disable(GPIO_NUM_3);

  pinMode(InputManager::BUTTON_ADC_PIN_1, INPUT_PULLUP);
  pinMode(InputManager::BUTTON_ADC_PIN_2, INPUT_PULLUP);
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(InputManager::BUTTON_ADC_PIN_1), autoLightSleepButtonInterrupt, ONLOW_WE);
  attachInterrupt(digitalPinToInterrupt(InputManager::BUTTON_ADC_PIN_2), autoLightSleepButtonInterrupt, ONLOW_WE);
  attachInterrupt(digitalPinToInterrupt(InputManager::POWER_BUTTON_PIN), autoLightSleepButtonInterrupt, ONLOW_WE);

  return esp_sleep_enable_gpio_wakeup();
}

void disableAutoLightSleepButtonWakeForTest() {
  detachInterrupt(digitalPinToInterrupt(InputManager::BUTTON_ADC_PIN_1));
  detachInterrupt(digitalPinToInterrupt(InputManager::BUTTON_ADC_PIN_2));
  detachInterrupt(digitalPinToInterrupt(InputManager::POWER_BUTTON_PIN));

  gpio_wakeup_disable(GPIO_NUM_1);
  gpio_wakeup_disable(GPIO_NUM_2);
  gpio_wakeup_disable(GPIO_NUM_3);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
}

const char* rawButtonName(uint8_t idx) {
  return InputManager::getButtonName(idx);
}

bool anyButtonPressed() {
  for (uint8_t idx = HalGPIO::BTN_BACK; idx <= HalGPIO::BTN_POWER; ++idx) {
    if (gpio.isPressed(idx)) {
      return true;
    }
  }
  return false;
}

void debugSerialPrintln(const char* text = "") {
#ifdef ENABLE_SERIAL_LOG
  if (logSerial) {
    logSerial.println(text);
  }
#else
  (void)text;
#endif
}

void debugSerialPrintf(const char* format, ...) {
#ifdef ENABLE_SERIAL_LOG
  if (!logSerial) {
    return;
  }

  char buf[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  logSerial.print(buf);
#else
  (void)format;
#endif
}

void debugSerialFlush() {
#ifdef ENABLE_SERIAL_LOG
  if (logSerial) {
    logSerial.flush();
  }
#endif
}

void drawLine(GfxRenderer& renderer, int& y, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const int maxWidth = renderer.getScreenWidth() - x * 2;
  const std::string clipped = renderer.truncatedText(UI_10_FONT_ID, text, maxWidth, style);
  renderer.drawText(UI_10_FONT_ID, x, y, clipped.c_str(), true, style);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 3;
}
}  // namespace

void HardwareTestActivity::onEnter() {
  Activity::onEnter();
  selectedPage = 0;
  selectedAction = 0;
  lastRefreshMs = 0;
  lastButtonEvent = "None";
  lastSleepReport = "No sleep test yet";
  lastSleepStatsLine1.clear();
  lastSleepStatsLine2.clear();
  lastSleepStatsLine3.clear();
  sleepPromptVisible = false;
  halTiltSensor.wake();
  requestUpdate();
}

void HardwareTestActivity::onExit() {
  halTiltSensor.disableWakeOnMotion();
  halTiltSensor.deepSleep();
  Activity::onExit();
}

void HardwareTestActivity::nextPage() {
  selectedPage = ButtonNavigator::nextIndex(selectedPage, pageCount);
  requestUpdate();
}

void HardwareTestActivity::previousPage() {
  selectedPage = ButtonNavigator::previousIndex(selectedPage, pageCount);
  requestUpdate();
}

void HardwareTestActivity::updateButtonEvent() {
  for (uint8_t idx = HalGPIO::BTN_BACK; idx <= HalGPIO::BTN_POWER; ++idx) {
    if (gpio.wasPressed(idx)) {
      lastButtonEvent = std::string(rawButtonName(idx)) + " pressed";
    } else if (gpio.wasReleased(idx)) {
      lastButtonEvent = std::string(rawButtonName(idx)) + " released";
    }
  }
}

void HardwareTestActivity::loop() {
  updateButtonEvent();

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    nextPage();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    previousPage();
    return;
  }

  if (selectedPage == 0) {
    buttonNavigator.onNextRelease([this] {
      selectedAction = ButtonNavigator::nextIndex(selectedAction, actionCount);
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      selectedAction = ButtonNavigator::previousIndex(selectedAction, actionCount);
      requestUpdate();
    });
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (selectedPage != 0) {
      selectedPage = 0;
      requestUpdate();
    } else {
      activityManager.goHome();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedPage != 0) {
      requestUpdate();
      return;
    }

    switch (selectedAction) {
      case 0:
        requestUpdate();
        break;
      case 1:
        runTimerOnlyLightSleepTest();
        break;
      case 2:
        runAutoLightSleepBlockTest();
        break;
      case 3:
        runTimerNoLatchLightSleepTest();
        break;
      case 4:
        runGpio1LightSleepTest();
        break;
      case 5:
        runGpio2LightSleepTest();
        break;
      case 6:
        runPowerButtonLightSleepTest();
        break;
      case 7:
        runGyroLightSleepTest();
        break;
      case 8:
        runGyroInterruptScan();
        break;
      default:
        break;
    }
  }

  if (selectedPage != 0 && millis() - lastRefreshMs >= REFRESH_INTERVAL_MS) {
    lastRefreshMs = millis();
    requestUpdate();
  }
}

void HardwareTestActivity::runTimerOnlyLightSleepTest() {
  sleepPrompt = "Sleeping: timer only";
  runLightSleepTest("Timer-only wake", GPIO_NUM_NC, GPIO_INTR_DISABLE, LIGHT_SLEEP_TIMEOUT_US, false);
}

void HardwareTestActivity::runAutoLightSleepBlockTest() {
  LOG_INF("HWT", "Starting auto light sleep block test");
  debugSerialPrintf("HWT_AUTO_LS_START:timeout_ms=%lu\n", static_cast<unsigned long>(AUTO_LIGHT_SLEEP_BLOCK_MS));
  debugSerialFlush();

  powerManager.setPowerSaving(false);
  sleepPromptVisible = true;
  sleepPrompt = "Release buttons";
  requestUpdateAndWait();

  while (anyButtonPressed()) {
    gpio.update();
    delay(20);
  }
  delay(80);
  gpio.update();

  sleepPrompt = "Auto LS: press button or wait 30s";
  requestUpdateAndWait();
  delay(150);

#ifdef ENABLE_SERIAL_LOG
  const bool previousSerialLogOutputEnabled = isSerialLogOutputEnabled();
  if (logSerial) {
    logSerial.flush();
  }
  setSerialLogOutputEnabled(false);
  if (logSerial) {
    logSerial.flush();
    logSerial.end();
  }
#else
  const bool previousSerialLogOutputEnabled = false;
#endif

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  const esp_err_t buttonWakeErr = enableAutoLightSleepButtonWakeForTest();

  uint32_t discardedNotifyBits = 0;
  xTaskNotifyWait(UINT32_MAX, UINT32_MAX, &discardedNotifyBits, 0);
  autoLightSleepWaitTask = xTaskGetCurrentTaskHandle();

  const auto before = powerManager.getLightSleepStats();
  const int64_t startUsSigned = esp_timer_get_time();

  bool autoLightSleepEnabled = buttonWakeErr == ESP_OK && powerManager.configureAutoLightSleep(true);
  uint32_t notifyBits = 0;
  BaseType_t notified = pdFALSE;
  if (autoLightSleepEnabled) {
    notified = xTaskNotifyWait(0, UINT32_MAX, &notifyBits, pdMS_TO_TICKS(AUTO_LIGHT_SLEEP_BLOCK_MS));
  }

  const int64_t endUsSigned = esp_timer_get_time();
  const auto after = powerManager.getLightSleepStats();

  autoLightSleepWaitTask = nullptr;
  disableAutoLightSleepButtonWakeForTest();
  powerManager.configureAutoLightSleep(false);
  powerManager.setPowerSaving(false);
  gpio.update();

#ifdef ENABLE_SERIAL_LOG
  if (previousSerialLogOutputEnabled) {
    logSerial.begin(115200);
    logSerial.setTxTimeoutMs(1);
  }
  setSerialLogOutputEnabled(previousSerialLogOutputEnabled);
#else
  (void)previousSerialLogOutputEnabled;
#endif

  const uint64_t elapsedUs =
      endUsSigned > startUsSigned ? static_cast<uint64_t>(endUsSigned - startUsSigned) : 0ULL;
  const uint64_t sleptUs = after.sleptUs >= before.sleptUs ? after.sleptUs - before.sleptUs : 0ULL;
  const uint64_t awakeUs = elapsedUs > sleptUs ? elapsedUs - sleptUs : 0ULL;
  const uint32_t sleepEntries = after.enterCount >= before.enterCount ? after.enterCount - before.enterCount : 0;
  const uint64_t earlyWakeCount =
      after.earlyWakeCount >= before.earlyWakeCount ? after.earlyWakeCount - before.earlyWakeCount : 0ULL;

  char buf[192];
  if (!autoLightSleepEnabled) {
    snprintf(buf, sizeof(buf), "Auto LS setup failed wake=%d", static_cast<int>(buttonWakeErr));
    lastSleepStatsLine1.clear();
    lastSleepStatsLine2.clear();
    lastSleepStatsLine3.clear();
  } else {
    const char* wakeText = notified == pdTRUE ? "button" : "30s timer";
    const uint64_t sleptPercentTenths = elapsedUs > 0 ? (sleptUs * 1000ULL) / elapsedUs : 0ULL;
    snprintf(buf, sizeof(buf), "Auto LS woke by %s", wakeText);

    char line[96];
    snprintf(line, sizeof(line), "Elapsed: %llums", static_cast<unsigned long long>(elapsedUs / 1000ULL));
    lastSleepStatsLine1 = line;
    snprintf(line, sizeof(line), "Slept: %llums  Awake: %llums",
             static_cast<unsigned long long>(sleptUs / 1000ULL),
             static_cast<unsigned long long>(awakeUs / 1000ULL));
    lastSleepStatsLine2 = line;
    snprintf(line, sizeof(line), "Sleep: %llu.%01llu%%  Entries: +%lu  Early: +%llu",
             static_cast<unsigned long long>(sleptPercentTenths / 10ULL),
             static_cast<unsigned long long>(sleptPercentTenths % 10ULL), static_cast<unsigned long>(sleepEntries),
             static_cast<unsigned long long>(earlyWakeCount));
    lastSleepStatsLine3 = line;
  }
  lastSleepReport = buf;

  sleepPromptVisible = false;
  LOG_INF("HWT", "%s", lastSleepReport.c_str());
  debugSerialPrintf("HWT_AUTO_LS_END:%s\n", lastSleepReport.c_str());
  debugSerialFlush();
  requestUpdateAndWait();
}

void HardwareTestActivity::runTimerNoLatchLightSleepTest() {
  sleepPrompt = "Sleeping: timer, no latch hold";
  runLightSleepTest("Timer no latch hold", GPIO_NUM_NC, GPIO_INTR_DISABLE, LIGHT_SLEEP_TIMEOUT_US, false, false);
}

void HardwareTestActivity::runGpio1LightSleepTest() {
  sleepPrompt = "Sleeping: press front buttons";
  runLightSleepTest("GPIO1 low wake", GPIO_NUM_1, GPIO_INTR_LOW_LEVEL, LIGHT_SLEEP_TIMEOUT_US, true);
}

void HardwareTestActivity::runGpio2LightSleepTest() {
  sleepPrompt = "Sleeping: press GPIO2 buttons";
  runLightSleepTest("GPIO2 low wake", GPIO_NUM_2, GPIO_INTR_LOW_LEVEL, LIGHT_SLEEP_TIMEOUT_US, true);
}

void HardwareTestActivity::runPowerButtonLightSleepTest() {
  sleepPrompt = "Sleeping: press power button";
  runLightSleepTest("GPIO3 power low wake", GPIO_NUM_3, GPIO_INTR_LOW_LEVEL, LIGHT_SLEEP_TIMEOUT_US, true);
}

void HardwareTestActivity::runGyroLightSleepTest() {
  sleepPrompt = "Release buttons; hold still";
  sleepPromptVisible = true;
  requestUpdateAndWait();
  const unsigned long settleStart = millis();
  while (millis() - settleStart < MOTION_TEST_SETTLE_MS) {
    gpio.update();
    delay(20);
  }

  sleepPrompt = "Sleeping: move X3 for gyro WoM";

  if (!halTiltSensor.enableWakeOnMotionInt1(60, 8, true)) {
    lastSleepReport = "Gyro WoM setup failed";
    sleepPromptVisible = false;
    requestUpdate();
    return;
  }

  runLightSleepTest("Gyro WoM via GPIO3 low", GPIO_NUM_3, GPIO_INTR_LOW_LEVEL, LIGHT_SLEEP_TIMEOUT_US, true);
  halTiltSensor.disableWakeOnMotion();
  halTiltSensor.wake();
}

void HardwareTestActivity::runGyroInterruptScan() {
  lastSleepStatsLine1.clear();
  lastSleepStatsLine2.clear();
  lastSleepStatsLine3.clear();
  sleepPrompt = "Release buttons; hold still";
  sleepPromptVisible = true;
  requestUpdateAndWait();
  const unsigned long settleStart = millis();
  while (millis() - settleStart < MOTION_TEST_SETTLE_MS) {
    gpio.update();
    delay(20);
  }

  sleepPrompt = "Move X3: scanning gyro INT pins";
  requestUpdateAndWait();

  debugSerialPrintln("HWT_GYRO_SCAN_BEGIN");
  debugSerialFlush();

  scanGyroInterruptRoute(true, 8000);
  delay(250);
  scanGyroInterruptRoute(false, 8000);

  halTiltSensor.disableWakeOnMotion();
  halTiltSensor.wake();

  sleepPromptVisible = false;
  lastSleepReport = "Gyro INT scan complete; check serial log";
  debugSerialPrintln("HWT_GYRO_SCAN_END");
  debugSerialFlush();
  requestUpdateAndWait();
}

void HardwareTestActivity::scanGyroInterruptRoute(bool useInt1, unsigned long durationMs) {
  static constexpr int candidatePins[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 13, 20, 21};

  halTiltSensor.disableWakeOnMotion();
  delay(25);

  const char* route = useInt1 ? "INT1" : "INT2";
  if (!halTiltSensor.enableWakeOnMotionInterrupt(useInt1, 60, 8, true)) {
    debugSerialPrintf("HWT_GYRO_SCAN_SETUP_FAILED:%s\n", route);
    debugSerialFlush();
    return;
  }

  uint8_t statusInt = 0;
  uint8_t status0 = 0;
  uint8_t status1 = 0;
  halTiltSensor.readStatus(statusInt, status0, status1);

  uint32_t previousLevels = 0;
  for (size_t i = 0; i < std::size(candidatePins); ++i) {
    if (digitalRead(candidatePins[i]) == HIGH) {
      previousLevels |= (1UL << i);
    }
  }

  debugSerialPrintf("HWT_GYRO_SCAN_ROUTE:%s statusInt=0x%02X status0=0x%02X status1=0x%02X levels=0x%08lX pins=",
                    route, statusInt, status0, status1, static_cast<unsigned long>(previousLevels));
  for (size_t i = 0; i < std::size(candidatePins); ++i) {
    debugSerialPrintf("%s%d:%d", i == 0 ? "" : ",", candidatePins[i], (previousLevels & (1UL << i)) ? 1 : 0);
  }
  debugSerialPrintln();
  debugSerialFlush();

  const unsigned long startMs = millis();
  while (millis() - startMs < durationMs) {
    delay(100);

    uint32_t levels = 0;
    for (size_t i = 0; i < std::size(candidatePins); ++i) {
      if (digitalRead(candidatePins[i]) == HIGH) {
        levels |= (1UL << i);
      }
    }

    halTiltSensor.readStatus(statusInt, status0, status1);
    if (levels != previousLevels || (status1 & 0x04)) {
      debugSerialPrintf(
          "HWT_GYRO_SCAN_EVENT:%s t=%lums statusInt=0x%02X status0=0x%02X status1=0x%02X levels=0x%08lX pins=", route,
          millis() - startMs, statusInt, status0, status1, static_cast<unsigned long>(levels));
      for (size_t i = 0; i < std::size(candidatePins); ++i) {
        const bool level = (levels & (1UL << i)) != 0;
        const bool changed = ((levels ^ previousLevels) & (1UL << i)) != 0;
        debugSerialPrintf("%s%d:%d%s", i == 0 ? "" : ",", candidatePins[i], level ? 1 : 0, changed ? "*" : "");
      }
      debugSerialPrintln();
      debugSerialFlush();
      previousLevels = levels;
    }
  }

  halTiltSensor.disableWakeOnMotion();
  debugSerialPrintf("HWT_GYRO_SCAN_ROUTE_DONE:%s\n", route);
  debugSerialFlush();
}

void HardwareTestActivity::runLightSleepTest(const char* label, gpio_num_t wakePin, gpio_int_type_t wakeLevel,
                                             uint64_t timeoutUs, bool enableGpioWake, bool holdLatchDuringSleep) {
  lastSleepStatsLine1.clear();
  lastSleepStatsLine2.clear();
  lastSleepStatsLine3.clear();
  LOG_INF("HWT", "Starting light sleep test: %s pin=%d level=%d gpio=%d", label, static_cast<int>(wakePin),
          static_cast<int>(wakeLevel), enableGpioWake);
  debugSerialPrintf("HWT_SLEEP_START:%s pin=%d gpio=%d timeout_us=%llu\n", label, static_cast<int>(wakePin),
                    enableGpioWake, static_cast<unsigned long long>(timeoutUs));
  debugSerialFlush();

  powerManager.setPowerSaving(false);
  sleepPromptVisible = true;
  requestUpdateAndWait();
  delay(150);

  if (holdLatchDuringSleep && gpio.deviceIsX3()) {
    gpio_hold_dis(X3_BATTERY_LATCH_PIN);
    gpio_set_direction(X3_BATTERY_LATCH_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(X3_BATTERY_LATCH_PIN, 1);
    gpio_hold_en(X3_BATTERY_LATCH_PIN);
  }

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup(timeoutUs);

  esp_err_t gpioErr = ESP_OK;
  esp_err_t wakeErr = ESP_OK;
  if (enableGpioWake) {
    gpio_wakeup_disable(wakePin);
    pinMode(static_cast<uint8_t>(wakePin), INPUT_PULLUP);
    gpioErr = gpio_wakeup_enable(wakePin, wakeLevel);
    wakeErr = esp_sleep_enable_gpio_wakeup();
  }

  if (gpioErr != ESP_OK || wakeErr != ESP_OK) {
    if (gpio.deviceIsX3()) {
      gpio_hold_dis(X3_BATTERY_LATCH_PIN);
      gpio_set_direction(X3_BATTERY_LATCH_PIN, GPIO_MODE_OUTPUT);
      gpio_set_level(X3_BATTERY_LATCH_PIN, 1);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "%s setup failed g=%d w=%d", label, gpioErr, wakeErr);
    lastSleepReport = buf;
    sleepPromptVisible = false;
    requestUpdateAndWait();
    return;
  }

  const unsigned long startMs = millis();
  const esp_err_t sleepErr = esp_light_sleep_start();
  const unsigned long elapsedMs = millis() - startMs;

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const uint64_t gpioMask = esp_sleep_get_gpio_wakeup_status();

  if (gpio.deviceIsX3()) {
    gpio_hold_dis(X3_BATTERY_LATCH_PIN);
    gpio_set_direction(X3_BATTERY_LATCH_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(X3_BATTERY_LATCH_PIN, 1);
  }

  if (enableGpioWake) {
    gpio_wakeup_disable(wakePin);
  }
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  powerManager.setPowerSaving(false);
  gpio.update();

  if (sleepErr != ESP_OK) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s sleep failed err=%d", label, sleepErr);
    lastSleepReport = buf;
  } else {
    lastSleepReport = std::string(label) + ": " + buildWakeCauseLine(cause, gpioMask, elapsedMs);
  }

  sleepPromptVisible = false;
  LOG_INF("HWT", "%s", lastSleepReport.c_str());
  debugSerialPrintf("HWT_SLEEP_END:%s\n", lastSleepReport.c_str());
  debugSerialFlush();
  requestUpdateAndWait();
}

std::string HardwareTestActivity::buildWakeCauseLine(esp_sleep_wakeup_cause_t cause, uint64_t gpioMask,
                                                     unsigned long elapsedMs) const {
  char buf[128];
  const char* causeText = "other";
  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    causeText = "GPIO";
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    causeText = "timer";
  }

  snprintf(buf, sizeof(buf), "%s after %lums mask=0x%llX", causeText, elapsedMs,
           static_cast<unsigned long long>(gpioMask));
  return buf;
}

std::string HardwareTestActivity::buildButtonStateLine() const {
  std::string pressed;
  for (uint8_t idx = HalGPIO::BTN_BACK; idx <= HalGPIO::BTN_POWER; ++idx) {
    if (gpio.isPressed(idx)) {
      if (!pressed.empty()) {
        pressed += " ";
      }
      pressed += rawButtonName(idx);
    }
  }
  return pressed.empty() ? "Pressed: none" : "Pressed: " + pressed;
}

void HardwareTestActivity::renderActionsPage(int& y) {
  char line[128];
  drawLine(renderer, y, "Page 1/3: Actions", EpdFontFamily::BOLD);

  const char* actions[actionCount] = {"Refresh readings",
                                      "Light sleep: timer only",
                                      "Auto LS: buttons/30s",
                                      "Light sleep: timer no latch",
                                      "Light sleep: GPIO1 low",
                                      "Light sleep: GPIO2 low",
                                      "Light sleep: power button",
                                      "Light sleep: gyro WoM GPIO3",
                                      "Gyro INT scan"};
  for (int i = 0; i < actionCount; ++i) {
    snprintf(line, sizeof(line), "%c %s", selectedAction == i ? '>' : ' ', actions[i]);
    drawLine(renderer, y, line, selectedAction == i ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  y += 5;
  const auto& metrics = UITheme::getInstance().getMetrics();
  for (const auto& wrapped : renderer.wrappedText(UI_10_FONT_ID, lastSleepReport.c_str(),
                                                  renderer.getScreenWidth() - metrics.contentSidePadding * 2, 3)) {
    drawLine(renderer, y, wrapped.c_str());
  }
  if (!lastSleepStatsLine1.empty()) {
    drawLine(renderer, y, lastSleepStatsLine1.c_str());
  }
  if (!lastSleepStatsLine2.empty()) {
    drawLine(renderer, y, lastSleepStatsLine2.c_str());
  }
  if (!lastSleepStatsLine3.empty()) {
    drawLine(renderer, y, lastSleepStatsLine3.c_str());
  }
}

void HardwareTestActivity::renderMotionPage(int& y) {
  drawLine(renderer, y, "Page 2/3: Gyro / QMI", EpdFontFamily::BOLD);

  float gx = 0;
  float gy = 0;
  float gz = 0;
  const bool gyroOk = halTiltSensor.readGyroDps(gx, gy, gz);

  uint8_t statusInt = 0;
  uint8_t status0 = 0;
  uint8_t status1 = 0;
  halTiltSensor.readStatus(statusInt, status0, status1);

  char line[128];
  snprintf(line, sizeof(line), "Gyro: %s", gyroOk ? "OK" : "N/A");
  drawLine(renderer, y, line);
  snprintf(line, sizeof(line), "X: %7.1f dps", gx);
  drawLine(renderer, y, line);
  snprintf(line, sizeof(line), "Y: %7.1f dps", gy);
  drawLine(renderer, y, line);
  snprintf(line, sizeof(line), "Z: %7.1f dps", gz);
  drawLine(renderer, y, line);
  y += 5;
  snprintf(line, sizeof(line), "QMI status int: 0x%02X", statusInt);
  drawLine(renderer, y, line);
  snprintf(line, sizeof(line), "QMI status0: 0x%02X", status0);
  drawLine(renderer, y, line);
  snprintf(line, sizeof(line), "QMI status1: 0x%02X", status1);
  drawLine(renderer, y, line);
}

void HardwareTestActivity::renderGpioPage(int& y) {
  drawLine(renderer, y, "Page 3/3: GPIO / Buttons", EpdFontFamily::BOLD);

  const int adc1 = analogRead(InputManager::BUTTON_ADC_PIN_1);
  const int dig1 = digitalRead(InputManager::BUTTON_ADC_PIN_1);
  const int adc2 = analogRead(InputManager::BUTTON_ADC_PIN_2);
  const int dig2 = digitalRead(InputManager::BUTTON_ADC_PIN_2);
  const int dig3 = digitalRead(InputManager::POWER_BUTTON_PIN);

  char line[128];
  snprintf(line, sizeof(line), "GPIO1 ADC=%4d digital=%d", adc1, dig1);
  drawLine(renderer, y, line);
  snprintf(line, sizeof(line), "GPIO2 ADC=%4d digital=%d", adc2, dig2);
  drawLine(renderer, y, line);
  snprintf(line, sizeof(line), "GPIO3 power digital=%d", dig3);
  drawLine(renderer, y, line);
  y += 5;
  drawLine(renderer, y, buildButtonStateLine().c_str());
  snprintf(line, sizeof(line), "Last event: %s", lastButtonEvent.c_str());
  drawLine(renderer, y, line);
}

void HardwareTestActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Hardware Test",
                 gpio.deviceIsX3() ? "X3" : "X4");

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (sleepPromptVisible) {
    drawLine(renderer, y, sleepPrompt.c_str(), EpdFontFamily::BOLD);
    drawLine(renderer, y, "Wake sources: selected GPIO or 10s timer");
    renderer.displayBuffer();
    return;
  }

  switch (selectedPage) {
    case 1:
      renderMotionPage(y);
      break;
    case 2:
      renderGpioPage(y);
      break;
    case 0:
    default:
      renderActionsPage(y);
      break;
  }

  const char* confirmLabel = selectedPage == 0 ? "Run" : "Refresh";
  const auto labels = mappedInput.mapLabels("Back", confirmLabel, "Prev", "Next");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
