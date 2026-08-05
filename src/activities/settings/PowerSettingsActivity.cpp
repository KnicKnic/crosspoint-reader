#include "PowerSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalTiltSensor.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "PowerSettingsBridge.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"

namespace {
std::string formatMsFromTenths(uint8_t tenths) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%u ms", static_cast<unsigned int>(tenths) * 100u);
  return buf;
}

std::string formatMs(uint8_t ms) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%u ms", static_cast<unsigned int>(ms));
  return buf;
}

std::string formatSeconds(uint8_t seconds) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%u sec", static_cast<unsigned int>(seconds));
  return buf;
}

std::string formatMhz(uint8_t mhz) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%u MHz", static_cast<unsigned int>(mhz));
  return buf;
}

std::string onOff(uint8_t value) { return value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF); }

void applyAndSave() {
  applyPowerSettingsToHal();
  SETTINGS.saveToFile();
}
}  // namespace

void PowerSettingsActivity::onEnter() {
  Activity::onEnter();
  rebuildItems();
  selectedIndex = 0;
  requestUpdate();
}

void PowerSettingsActivity::rebuildItems() {
  items = {
      Item::CpuPowerSaving,
      Item::IdleDelay,
      Item::LowPowerFreq,
      Item::MaxCpuFreq,
      Item::AutoLightSleep,
  };

  if (gpio.deviceIsX3()) {
    items.push_back(Item::UsbPolling);
    items.push_back(Item::UsbPollInterval);
    items.push_back(Item::BatteryPolling);
    items.push_back(Item::BatteryPollInterval);
  }
  if (halClock.isAvailable()) {
    items.push_back(Item::ClockPolling);
    items.push_back(Item::ClockPollInterval);
  }
  if (halTiltSensor.isAvailable()) {
    items.push_back(Item::TiltPolling);
    items.push_back(Item::TiltPollInterval);
  }
}

void PowerSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(items.size()));
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(items.size()));
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(items.size()));
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(items.size()));
    requestUpdate();
  });
}

void PowerSettingsActivity::handleSelection() {
  if (items.empty()) return;

  const Item item = items[std::min(selectedIndex, static_cast<int>(items.size()) - 1)];
  switch (item) {
    case Item::CpuPowerSaving:
      SETTINGS.powerSavingEnabled = (SETTINGS.powerSavingEnabled + 1) % 2;
      applyAndSave();
      return;
    case Item::AutoLightSleep:
      SETTINGS.autoLightSleep = (SETTINGS.autoLightSleep + 1) % 2;
      applyPowerSettingsToHal();
      powerManager.setAutoLightSleep(SETTINGS.autoLightSleep != 0);
      SETTINGS.saveToFile();
      return;
    case Item::UsbPolling:
      SETTINGS.usbPollingEnabled = (SETTINGS.usbPollingEnabled + 1) % 2;
      applyAndSave();
      return;
    case Item::BatteryPolling:
      SETTINGS.batteryPollingEnabled = (SETTINGS.batteryPollingEnabled + 1) % 2;
      applyAndSave();
      return;
    case Item::ClockPolling:
      SETTINGS.clockPollingEnabled = (SETTINGS.clockPollingEnabled + 1) % 2;
      applyAndSave();
      return;
    case Item::TiltPolling:
      SETTINGS.tiltPollingEnabled = (SETTINGS.tiltPollingEnabled + 1) % 2;
      applyAndSave();
      return;
    case Item::IdleDelay:
    case Item::LowPowerFreq:
    case Item::MaxCpuFreq:
    case Item::UsbPollInterval:
    case Item::BatteryPollInterval:
    case Item::ClockPollInterval:
    case Item::TiltPollInterval:
      openIntervalPicker(item);
      return;
  }
}

void PowerSettingsActivity::openIntervalPicker(Item item) {
  int initial = 0;
  int min = 0;
  int max = 0;
  int smallStep = 0;
  int largeStep = 0;
  StrId title = StrId::STR_POWER_SETTINGS;
  StrId format = StrId::STR_NONE_OPT;

  switch (item) {
    case Item::IdleDelay:
      initial = SETTINGS.idlePowerSavingDelaySec;
      min = 1;
      max = 30;
      smallStep = 1;
      largeStep = 5;
      title = StrId::STR_IDLE_POWER_DELAY;
      format = StrId::STR_SECONDS_VALUE_FORMAT;
      break;
    case Item::LowPowerFreq:
      initial = SETTINGS.lowPowerFrequencyMhz;
      min = 10;
      max = 160;
      smallStep = 10;
      largeStep = 40;
      title = StrId::STR_LOW_POWER_FREQ;
      format = StrId::STR_MHZ_VALUE_FORMAT;
      break;
    case Item::MaxCpuFreq:
      initial = SETTINGS.maxCpuFrequencyMhz;
      min = 80;
      max = 240;
      smallStep = 10;
      largeStep = 40;
      title = StrId::STR_MAX_CPU_FREQ;
      format = StrId::STR_MHZ_VALUE_FORMAT;
      break;
    case Item::UsbPollInterval:
      initial = SETTINGS.usbPollIntervalTenths * 100;
      min = 100;
      max = 20000;
      smallStep = 100;
      largeStep = 1000;
      title = StrId::STR_USB_POLL_INTERVAL;
      format = StrId::STR_MS_VALUE_FORMAT;
      break;
    case Item::BatteryPollInterval:
      initial = SETTINGS.batteryPollIntervalTenths * 100;
      min = 100;
      max = 20000;
      smallStep = 100;
      largeStep = 1000;
      title = StrId::STR_BATTERY_POLL_INTERVAL;
      format = StrId::STR_MS_VALUE_FORMAT;
      break;
    case Item::ClockPollInterval:
      initial = SETTINGS.clockPollIntervalTenths * 100;
      min = 1000;
      max = 25000;
      smallStep = 1000;
      largeStep = 5000;
      title = StrId::STR_CLOCK_POLL_INTERVAL;
      format = StrId::STR_MS_VALUE_FORMAT;
      break;
    case Item::TiltPollInterval:
      initial = SETTINGS.tiltPollIntervalMs;
      min = 20;
      max = 250;
      smallStep = 5;
      largeStep = 25;
      title = StrId::STR_TILT_POLL_INTERVAL;
      format = StrId::STR_MS_VALUE_FORMAT;
      break;
    default:
      return;
  }

  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(renderer, mappedInput, "PowerInterval", title, initial, min, max,
                                                  smallStep, largeStep, format, false, true),
      [this, item](const ActivityResult& result) {
        if (!result.isCancelled) {
          const int value = static_cast<int>(std::get<IntervalResult>(result.data).value);
          switch (item) {
            case Item::IdleDelay:
              SETTINGS.idlePowerSavingDelaySec = static_cast<uint8_t>(value);
              break;
            case Item::LowPowerFreq:
              SETTINGS.lowPowerFrequencyMhz = static_cast<uint8_t>(value);
              break;
            case Item::MaxCpuFreq:
              SETTINGS.maxCpuFrequencyMhz = static_cast<uint8_t>(value);
              break;
            case Item::UsbPollInterval:
              SETTINGS.usbPollIntervalTenths = static_cast<uint8_t>(std::clamp(value / 100, 1, 200));
              break;
            case Item::BatteryPollInterval:
              SETTINGS.batteryPollIntervalTenths = static_cast<uint8_t>(std::clamp(value / 100, 1, 200));
              break;
            case Item::ClockPollInterval:
              SETTINGS.clockPollIntervalTenths = static_cast<uint8_t>(std::clamp(value / 100, 10, 250));
              break;
            case Item::TiltPollInterval:
              SETTINGS.tiltPollIntervalMs = static_cast<uint8_t>(value);
              break;
            default:
              break;
          }
          applyAndSave();
        }
        requestUpdate();
      });
}

const char* PowerSettingsActivity::itemName(Item item) const {
  switch (item) {
    case Item::CpuPowerSaving:
      return tr(STR_CPU_POWER_SAVING);
    case Item::IdleDelay:
      return tr(STR_IDLE_POWER_DELAY);
    case Item::LowPowerFreq:
      return tr(STR_LOW_POWER_FREQ);
    case Item::MaxCpuFreq:
      return tr(STR_MAX_CPU_FREQ);
    case Item::AutoLightSleep:
      return tr(STR_AUTO_LIGHT_SLEEP);
    case Item::UsbPolling:
      return tr(STR_USB_POLLING);
    case Item::UsbPollInterval:
      return tr(STR_USB_POLL_INTERVAL);
    case Item::BatteryPolling:
      return tr(STR_BATTERY_POLLING);
    case Item::BatteryPollInterval:
      return tr(STR_BATTERY_POLL_INTERVAL);
    case Item::ClockPolling:
      return tr(STR_CLOCK_POLLING);
    case Item::ClockPollInterval:
      return tr(STR_CLOCK_POLL_INTERVAL);
    case Item::TiltPolling:
      return tr(STR_TILT_POLLING);
    case Item::TiltPollInterval:
      return tr(STR_TILT_POLL_INTERVAL);
  }
  return "";
}

std::string PowerSettingsActivity::itemValue(Item item) const {
  switch (item) {
    case Item::CpuPowerSaving:
      return onOff(SETTINGS.powerSavingEnabled);
    case Item::IdleDelay:
      return formatSeconds(SETTINGS.idlePowerSavingDelaySec);
    case Item::LowPowerFreq:
      return formatMhz(SETTINGS.lowPowerFrequencyMhz);
    case Item::MaxCpuFreq:
      return formatMhz(SETTINGS.maxCpuFrequencyMhz);
    case Item::AutoLightSleep:
      return onOff(SETTINGS.autoLightSleep);
    case Item::UsbPolling:
      return onOff(SETTINGS.usbPollingEnabled);
    case Item::UsbPollInterval:
      return formatMsFromTenths(SETTINGS.usbPollIntervalTenths);
    case Item::BatteryPolling:
      return onOff(SETTINGS.batteryPollingEnabled);
    case Item::BatteryPollInterval:
      return formatMsFromTenths(SETTINGS.batteryPollIntervalTenths);
    case Item::ClockPolling:
      return onOff(SETTINGS.clockPollingEnabled);
    case Item::ClockPollInterval:
      return formatMsFromTenths(SETTINGS.clockPollIntervalTenths);
    case Item::TiltPolling:
      return onOff(SETTINGS.tiltPollingEnabled);
    case Item::TiltPollInterval:
      return formatMs(SETTINGS.tiltPollIntervalMs);
  }
  return "";
}

void PowerSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_POWER_SETTINGS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(items.size()), selectedIndex,
      [this](int index) { return std::string(itemName(items[index])); }, nullptr, nullptr,
      [this](int index) { return itemValue(items[index]); }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
