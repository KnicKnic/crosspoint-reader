#include "BatteryDrainActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <InputManager.h>
#include <Wire.h>

#include <cstdio>

#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char BATTERY_LOG_PATH[] = "/batterylog.txt";

struct DrainMode {
  const char* label;
  uint64_t durationUs;
};

constexpr uint64_t minutesToUs(uint64_t minutes) {
  return minutes * 60ULL * 1000ULL * 1000ULL;
}

constexpr DrainMode DRAIN_MODES[] = {
    {"1 minute smoke", minutesToUs(1)},
    {"10 minutes", minutesToUs(10)},
    {"2 hours", minutesToUs(2 * 60)},
    {"24 hours", minutesToUs(24 * 60)},
    {"3 days", minutesToUs(3 * 24 * 60)},
};

constexpr uint8_t REG_TEMPERATURE = 0x06;
constexpr uint8_t REG_VOLTAGE = 0x08;
constexpr uint8_t REG_BATTERY_STATUS = 0x0A;
constexpr uint8_t REG_CURRENT = 0x0C;
constexpr uint8_t REG_REMAINING_CAPACITY = 0x10;
constexpr uint8_t REG_FULL_CHARGE_CAPACITY = 0x12;
constexpr uint8_t REG_AVERAGE_CURRENT = 0x14;
constexpr uint8_t REG_TIME_TO_EMPTY = 0x16;
constexpr uint8_t REG_STANDBY_CURRENT = 0x18;
constexpr uint8_t REG_STANDBY_TIME_TO_EMPTY = 0x1A;
constexpr uint8_t REG_MAX_LOAD_CURRENT = 0x1C;
constexpr uint8_t REG_MAX_LOAD_TIME_TO_EMPTY = 0x1E;
constexpr uint8_t REG_AVERAGE_POWER = 0x24;
constexpr uint8_t REG_INTERNAL_TEMPERATURE = 0x28;
constexpr uint8_t REG_CYCLE_COUNT = 0x2A;
constexpr uint8_t REG_STATE_OF_CHARGE = 0x2C;
constexpr uint8_t REG_STATE_OF_HEALTH = 0x2E;
constexpr uint8_t REG_CHARGE_VOLTAGE = 0x30;
constexpr uint8_t REG_CHARGE_CURRENT = 0x32;
constexpr uint8_t REG_OPERATION_STATUS = 0x3A;
constexpr uint8_t REG_DESIGN_CAPACITY = 0x3C;

float deciKelvinToC(uint16_t raw) {
  return static_cast<float>(raw) / 10.0f - 273.15f;
}

const char* wakeCauseName(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
      return "timer";
    case ESP_SLEEP_WAKEUP_GPIO:
      return "gpio";
    default:
      return "other";
  }
}

void appendLine(std::string& out, const char* name, bool ok, uint16_t raw, const char* unit, long value) {
  char line[96];
  if (ok) {
    snprintf(line, sizeof(line), "%s=%ld%s raw=0x%04X\n", name, value, unit, raw);
  } else {
    snprintf(line, sizeof(line), "%s=ERR\n", name);
  }
  out += line;
}

void appendSignedLine(std::string& out, const char* name, bool ok, uint16_t raw, const char* unit) {
  appendLine(out, name, ok, raw, unit, static_cast<int16_t>(raw));
}

void appendUnsignedLine(std::string& out, const char* name, bool ok, uint16_t raw, const char* unit) {
  appendLine(out, name, ok, raw, unit, raw);
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

void BatteryDrainActivity::onEnter() {
  Activity::onEnter();
  state = State::Idle;
  selectedMode = 0;
  status = "Ready";
  lastResult = "Logs append to /batterylog.txt";
  requestUpdate();
}

const char* BatteryDrainActivity::selectedModeLabel() const {
  return DRAIN_MODES[selectedMode].label;
}

uint64_t BatteryDrainActivity::selectedSleepDurationUs() const {
  return DRAIN_MODES[selectedMode].durationUs;
}

bool BatteryDrainActivity::readBQ27220U16(uint8_t reg, uint16_t& value) const {
  if (!gpio.deviceIsX3()) {
    return false;
  }

  Wire.beginTransmission(I2C_ADDR_BQ27220);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(I2C_ADDR_BQ27220, static_cast<uint8_t>(2)) < 2) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  value = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
  return true;
}

std::string BatteryDrainActivity::buildBatterySnapshot(const char* phase, unsigned long elapsedMs,
                                                       esp_sleep_wakeup_cause_t cause, uint64_t gpioMask) const {
  char line[160];
  std::string out;
  char timeBuf[12] = "ERR";
  if (!halClock.formatTime(timeBuf, sizeof(timeBuf), 48, false, true)) {
    snprintf(timeBuf, sizeof(timeBuf), "N/A");
  }
  char dateBuf[11] = "ERR";
  if (!halClock.formatDate(dateBuf, sizeof(dateBuf))) {
    snprintf(dateBuf, sizeof(dateBuf), "N/A");
  }
  snprintf(line, sizeof(line),
           "\n=== %s rtc_datetime_utc=%sT%s uptime_ms=%lu elapsed_ms=%lu wake=%s gpio_mask=0x%llX ===\n", phase,
           dateBuf, timeBuf, millis(), elapsedMs, wakeCauseName(cause), static_cast<unsigned long long>(gpioMask));
  out += line;

  uint16_t temp = 0;
  uint16_t voltage = 0;
  uint16_t batteryStatus = 0;
  uint16_t current = 0;
  uint16_t remainingCapacity = 0;
  uint16_t fullChargeCapacity = 0;
  uint16_t averageCurrent = 0;
  uint16_t timeToEmpty = 0;
  uint16_t standbyCurrent = 0;
  uint16_t standbyTimeToEmpty = 0;
  uint16_t maxLoadCurrent = 0;
  uint16_t maxLoadTimeToEmpty = 0;
  uint16_t averagePower = 0;
  uint16_t internalTemp = 0;
  uint16_t cycleCount = 0;
  uint16_t soc = 0;
  uint16_t soh = 0;
  uint16_t chargeVoltage = 0;
  uint16_t chargeCurrent = 0;
  uint16_t operationStatus = 0;
  uint16_t designCapacity = 0;

  const bool okTemp = readBQ27220U16(REG_TEMPERATURE, temp);
  const bool okVoltage = readBQ27220U16(REG_VOLTAGE, voltage);
  const bool okBatteryStatus = readBQ27220U16(REG_BATTERY_STATUS, batteryStatus);
  const bool okCurrent = readBQ27220U16(REG_CURRENT, current);
  const bool okRemainingCapacity = readBQ27220U16(REG_REMAINING_CAPACITY, remainingCapacity);
  const bool okFullChargeCapacity = readBQ27220U16(REG_FULL_CHARGE_CAPACITY, fullChargeCapacity);
  const bool okAverageCurrent = readBQ27220U16(REG_AVERAGE_CURRENT, averageCurrent);
  const bool okTimeToEmpty = readBQ27220U16(REG_TIME_TO_EMPTY, timeToEmpty);
  const bool okStandbyCurrent = readBQ27220U16(REG_STANDBY_CURRENT, standbyCurrent);
  const bool okStandbyTimeToEmpty = readBQ27220U16(REG_STANDBY_TIME_TO_EMPTY, standbyTimeToEmpty);
  const bool okMaxLoadCurrent = readBQ27220U16(REG_MAX_LOAD_CURRENT, maxLoadCurrent);
  const bool okMaxLoadTimeToEmpty = readBQ27220U16(REG_MAX_LOAD_TIME_TO_EMPTY, maxLoadTimeToEmpty);
  const bool okAveragePower = readBQ27220U16(REG_AVERAGE_POWER, averagePower);
  const bool okInternalTemp = readBQ27220U16(REG_INTERNAL_TEMPERATURE, internalTemp);
  const bool okCycleCount = readBQ27220U16(REG_CYCLE_COUNT, cycleCount);
  const bool okSoc = readBQ27220U16(REG_STATE_OF_CHARGE, soc);
  const bool okSoh = readBQ27220U16(REG_STATE_OF_HEALTH, soh);
  const bool okChargeVoltage = readBQ27220U16(REG_CHARGE_VOLTAGE, chargeVoltage);
  const bool okChargeCurrent = readBQ27220U16(REG_CHARGE_CURRENT, chargeCurrent);
  const bool okOperationStatus = readBQ27220U16(REG_OPERATION_STATUS, operationStatus);
  const bool okDesignCapacity = readBQ27220U16(REG_DESIGN_CAPACITY, designCapacity);

  if (okTemp) {
    snprintf(line, sizeof(line), "temperature_c=%.1f raw=0x%04X\n", deciKelvinToC(temp), temp);
    out += line;
  } else {
    out += "temperature_c=ERR\n";
  }
  if (okInternalTemp) {
    snprintf(line, sizeof(line), "internal_temperature_c=%.1f raw=0x%04X\n", deciKelvinToC(internalTemp),
             internalTemp);
    out += line;
  } else {
    out += "internal_temperature_c=ERR\n";
  }

  appendUnsignedLine(out, "voltage", okVoltage, voltage, "mV");
  appendSignedLine(out, "current", okCurrent, current, "mA");
  appendSignedLine(out, "average_current", okAverageCurrent, averageCurrent, "mA");
  appendSignedLine(out, "standby_current", okStandbyCurrent, standbyCurrent, "mA");
  appendSignedLine(out, "max_load_current", okMaxLoadCurrent, maxLoadCurrent, "mA");
  appendSignedLine(out, "average_power", okAveragePower, averagePower, "mW");
  appendUnsignedLine(out, "remaining_capacity", okRemainingCapacity, remainingCapacity, "mAh");
  appendUnsignedLine(out, "full_charge_capacity", okFullChargeCapacity, fullChargeCapacity, "mAh");
  appendUnsignedLine(out, "design_capacity", okDesignCapacity, designCapacity, "mAh");
  appendUnsignedLine(out, "state_of_charge", okSoc, soc, "%");
  appendUnsignedLine(out, "state_of_health", okSoh, soh, "%");
  appendUnsignedLine(out, "time_to_empty", okTimeToEmpty, timeToEmpty, "min");
  appendUnsignedLine(out, "standby_time_to_empty", okStandbyTimeToEmpty, standbyTimeToEmpty, "min");
  appendUnsignedLine(out, "max_load_time_to_empty", okMaxLoadTimeToEmpty, maxLoadTimeToEmpty, "min");
  appendUnsignedLine(out, "cycle_count", okCycleCount, cycleCount, "");
  appendUnsignedLine(out, "charge_voltage", okChargeVoltage, chargeVoltage, "mV");
  appendUnsignedLine(out, "charge_current", okChargeCurrent, chargeCurrent, "mA");
  appendUnsignedLine(out, "battery_status", okBatteryStatus, batteryStatus, "");
  appendUnsignedLine(out, "operation_status", okOperationStatus, operationStatus, "");

  return out;
}

bool BatteryDrainActivity::appendLog(const std::string& text) {
  HalFile file = Storage.open(BATTERY_LOG_PATH, O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    return false;
  }
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  file.flush();
  file.close();
  return written == text.size();
}

void BatteryDrainActivity::holdX3BatteryLatch() const {
  if (!gpio.deviceIsX3()) {
    return;
  }
  gpio_hold_dis(X3_BATTERY_LATCH_PIN);
  gpio_set_direction(X3_BATTERY_LATCH_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(X3_BATTERY_LATCH_PIN, 1);
  gpio_hold_en(X3_BATTERY_LATCH_PIN);
}

void BatteryDrainActivity::releaseX3BatteryLatchHold() const {
  if (!gpio.deviceIsX3()) {
    return;
  }
  gpio_hold_dis(X3_BATTERY_LATCH_PIN);
  gpio_set_direction(X3_BATTERY_LATCH_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(X3_BATTERY_LATCH_PIN, 1);
}

void BatteryDrainActivity::runDrainTest() {
  state = State::Running;
  status = "Logging before sleep";
  requestUpdateAndWait();

  const uint64_t sleepDurationUs = selectedSleepDurationUs();
  char runHeader[128];
  snprintf(runHeader, sizeof(runHeader), "\n*** Battery drain test mode=\"%s\" requested_us=%llu ***\n",
           selectedModeLabel(), static_cast<unsigned long long>(sleepDurationUs));
  const std::string before =
      std::string(runHeader) + buildBatterySnapshot("before", 0, ESP_SLEEP_WAKEUP_UNDEFINED, 0);
  if (!appendLog(before)) {
    state = State::Error;
    status = "Failed to write /batterylog.txt";
    requestUpdateAndWait();
    return;
  }

  char statusBuf[96];
  snprintf(statusBuf, sizeof(statusBuf), "Sleeping: %s or GPIO wake", selectedModeLabel());
  status = statusBuf;
  requestUpdateAndWait();
  delay(150);

  powerManager.setPowerSaving(false);
  holdX3BatteryLatch();

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup(sleepDurationUs);

  gpio_wakeup_disable(GPIO_NUM_1);
  gpio_wakeup_disable(GPIO_NUM_2);
  gpio_wakeup_disable(GPIO_NUM_3);
  pinMode(InputManager::BUTTON_ADC_PIN_1, INPUT_PULLUP);
  pinMode(InputManager::BUTTON_ADC_PIN_2, INPUT_PULLUP);
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  const esp_err_t gpio1Err = gpio_wakeup_enable(GPIO_NUM_1, GPIO_INTR_LOW_LEVEL);
  const esp_err_t gpio2Err = gpio_wakeup_enable(GPIO_NUM_2, GPIO_INTR_LOW_LEVEL);
  const esp_err_t gpio3Err = gpio_wakeup_enable(GPIO_NUM_3, GPIO_INTR_LOW_LEVEL);
  const esp_err_t wakeErr = esp_sleep_enable_gpio_wakeup();

  if (gpio1Err != ESP_OK || gpio2Err != ESP_OK || gpio3Err != ESP_OK || wakeErr != ESP_OK) {
    releaseX3BatteryLatchHold();
    char buf[96];
    snprintf(buf, sizeof(buf), "Wake setup failed g1=%d g2=%d g3=%d w=%d", gpio1Err, gpio2Err, gpio3Err, wakeErr);
    status = buf;
    appendLog(std::string("ERROR: ") + status + "\n");
    state = State::Error;
    requestUpdateAndWait();
    return;
  }

  const unsigned long startMs = millis();
  const esp_err_t sleepErr = esp_light_sleep_start();
  const unsigned long elapsedMs = millis() - startMs;
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const uint64_t gpioMask = esp_sleep_get_gpio_wakeup_status();

  releaseX3BatteryLatchHold();
  gpio_wakeup_disable(GPIO_NUM_1);
  gpio_wakeup_disable(GPIO_NUM_2);
  gpio_wakeup_disable(GPIO_NUM_3);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  powerManager.setPowerSaving(false);
  gpio.update();

  std::string after = buildBatterySnapshot("after", elapsedMs, cause, gpioMask);
  if (sleepErr != ESP_OK) {
    char buf[64];
    snprintf(buf, sizeof(buf), "sleep_error=%d\n", sleepErr);
    after += buf;
  }

  if (!appendLog(after)) {
    state = State::Error;
    status = "Woke, but failed to write log";
    requestUpdateAndWait();
    return;
  }

  char summary[128];
  snprintf(summary, sizeof(summary), "%s woke by %s after %lums", selectedModeLabel(), wakeCauseName(cause),
           elapsedMs);
  status = summary;
  lastResult = "Saved before/after snapshots to /batterylog.txt";
  state = State::Complete;
  requestUpdateAndWait();
}

void BatteryDrainActivity::loop() {
  if (state == State::Running) {
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedMode = ButtonNavigator::nextIndex(selectedMode, modeCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedMode = ButtonNavigator::previousIndex(selectedMode, modeCount);
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    runDrainTest();
  }
}

void BatteryDrainActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Battery Drain",
                 selectedModeLabel());

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  drawLine(renderer, y, status.c_str(), EpdFontFamily::BOLD);
  y += 5;
  drawLine(renderer, y, "Select duration", EpdFontFamily::BOLD);
  char line[96];
  for (int i = 0; i < modeCount; ++i) {
    snprintf(line, sizeof(line), "%c %s", selectedMode == i ? '>' : ' ', DRAIN_MODES[i].label);
    drawLine(renderer, y, line, selectedMode == i ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
  y += 5;
  drawLine(renderer, y, "Creates /batterylog.txt");
  drawLine(renderer, y, "Logs BQ27220 before sleep");
  drawLine(renderer, y, "Arms GPIO1, GPIO2, GPIO3 wake");
  drawLine(renderer, y, "Sleeps up to selected duration");
  drawLine(renderer, y, "Logs BQ27220 after wake");
  y += 5;
  for (const auto& wrapped : renderer.wrappedText(UI_10_FONT_ID, lastResult.c_str(),
                                                  pageWidth - metrics.contentSidePadding * 2, 4)) {
    drawLine(renderer, y, wrapped.c_str());
  }

  const auto labels = mappedInput.mapLabels("Back", "Run", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
