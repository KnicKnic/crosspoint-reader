#include "PowerStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/PowerProfiler.h"

namespace {
constexpr unsigned long REFRESH_INTERVAL_MS = 5000;
}

void PowerStatsActivity::onEnter() {
  Activity::onEnter();
  refresh(true);
  requestUpdate();
}

void PowerStatsActivity::refresh(bool printToSerial) {
  displayLines = PowerProfiler::buildDisplayLines(PowerProfiler::collect());
  if (printToSerial) PowerProfiler::printSerial();
  lastRefreshMs = millis();
}

void PowerStatsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    refresh(true);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    if (scrollOffset + 1 < displayLines.size()) {
      scrollOffset++;
      requestUpdate();
    }
  });

  buttonNavigator.onPreviousRelease([this] {
    if (scrollOffset > 0) {
      scrollOffset--;
      requestUpdate();
    }
  });

  if (millis() - lastRefreshMs >= REFRESH_INTERVAL_MS) {
    refresh(false);
    scrollOffset = std::min(scrollOffset, displayLines.empty() ? size_t{0} : displayLines.size() - 1);
    requestUpdate();
  }
}

void PowerStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_POWER_STATS));

  const int left = metrics.contentSidePadding;
  const int right = pageWidth - metrics.contentSidePadding;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int lineHeight = std::max(12, renderer.getLineHeight(SMALL_FONT_ID) + 2);
  const int rows = std::max(1, (bottom - top) / lineHeight);

  for (int row = 0; row < rows; row++) {
    const size_t idx = scrollOffset + static_cast<size_t>(row);
    if (idx >= displayLines.size()) break;

    const int y = top + row * lineHeight;
    if (y + lineHeight > bottom) break;

    const auto& sourceLine = displayLines[idx];
    if (sourceLine.empty()) continue;

    const std::string line = renderer.truncatedText(SMALL_FONT_ID, sourceLine.c_str(), right - left);
    renderer.drawText(SMALL_FONT_ID, left, y, line.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPDATE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
