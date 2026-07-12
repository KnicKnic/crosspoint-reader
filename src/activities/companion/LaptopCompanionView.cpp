#include "LaptopCompanionView.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "companion/CompanionProtocol.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace LaptopCompanionView {
namespace {
bool hasLastRenderedState = false;
Page lastRenderedPage = Page::Status;
State lastRenderedState;
uint32_t renderWriteCount = 0;

bool statusPageMatches(const State& a, const State& b) {
  return a.hostConnected == b.hostConnected && a.inputControlsVisible == b.inputControlsVisible &&
         a.statusMessage == b.statusMessage &&
         a.microphoneMessage == b.microphoneMessage && a.cameraMessage == b.cameraMessage;
}

bool diagnosticsPageMatches(const State& a, const State& b) {
  return a.inputControlsVisible == b.inputControlsVisible && a.powerStatsMessage == b.powerStatsMessage &&
         a.powerDeltaMessage == b.powerDeltaMessage &&
         a.powerAccountingMessage == b.powerAccountingMessage && a.wakeCauseMessage == b.wakeCauseMessage &&
         a.powerTimerMessage == b.powerTimerMessage && a.bleAdvertiseMessage == b.bleAdvertiseMessage &&
         a.bleConnectionMessage == b.bleConnectionMessage && a.bleActivityMessage == b.bleActivityMessage &&
         a.btLockTraceMessage == b.btLockTraceMessage && a.powerTaskMessage == b.powerTaskMessage &&
         a.renderCostMessage == b.renderCostMessage && a.pmLockMessage3 == b.pmLockMessage3 &&
         a.pmLockMessage4 == b.pmLockMessage4 && a.pmLockMessage5 == b.pmLockMessage5;
}

bool activePageMatches(Page page, const State& a, const State& b) {
  return page == Page::Diagnostics ? diagnosticsPageMatches(a, b) : statusPageMatches(a, b);
}

void drawTruncatedLine(GfxRenderer& renderer, int fontId, int x, int& y, int width, const std::string& text) {
  const auto line = renderer.truncatedText(fontId, text.c_str(), width);
  renderer.drawText(fontId, x, y, line.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(fontId);
}

void drawRenderWriteCount(GfxRenderer& renderer, int x, uint32_t count) {
  char line[32];
  snprintf(line, sizeof(line), "Render writes: %lu", static_cast<unsigned long>(count));
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID) -
                metrics.verticalSpacing;
  renderer.drawText(SMALL_FONT_ID, x, y, line, true, EpdFontFamily::BOLD);
}

void drawStatusPage(GfxRenderer& renderer, MappedInputManager& mappedInput, const State& state) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int contentX = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 20;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LAPTOP_COMPANION));

  renderer.drawCenteredText(UI_12_FONT_ID, y, state.hostConnected ? "Connected" : "Disconnected", true,
                            EpdFontFamily::BOLD);
  y += 60;

  renderer.drawText(UI_10_FONT_ID, contentX, y, "Teams");
  renderer.drawText(UI_10_FONT_ID, contentX + 150, y, state.statusMessage.c_str(), true, EpdFontFamily::BOLD);
  y += 42;

  renderer.drawText(UI_10_FONT_ID, contentX, y, "Microphone");
  renderer.drawText(UI_10_FONT_ID, contentX + 150, y, state.microphoneMessage.c_str(), true, EpdFontFamily::BOLD);
  y += 42;

  renderer.drawText(UI_10_FONT_ID, contentX, y, "Camera");
  renderer.drawText(UI_10_FONT_ID, contentX + 150, y, state.cameraMessage.c_str(), true, EpdFontFamily::BOLD);

  drawRenderWriteCount(renderer, contentX, renderWriteCount);

  const auto labels =
      mappedInput.mapLabels(state.inputControlsVisible ? tr(STR_BACK) : "",
                            state.inputControlsVisible ? tr(STR_TOGGLE) : "", "", "Stats");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void drawDiagnosticsPage(GfxRenderer& renderer, MappedInputManager& mappedInput, const State& state) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int contentX = metrics.contentSidePadding;
  const int contentWidth = pageWidth - metrics.contentSidePadding * 2;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 8;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Companion Stats");

  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.powerStatsMessage);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.powerDeltaMessage);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.powerAccountingMessage);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.wakeCauseMessage);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.powerTimerMessage);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.bleAdvertiseMessage);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.bleConnectionMessage);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.bleActivityMessage);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.btLockTraceMessage);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.powerTaskMessage);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.renderCostMessage);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.pmLockMessage3);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.pmLockMessage4);
  drawTruncatedLine(renderer, SMALL_FONT_ID, contentX, y, contentWidth, state.pmLockMessage5);

  drawRenderWriteCount(renderer, contentX, renderWriteCount);

  const auto labels =
      mappedInput.mapLabels(state.inputControlsVisible ? tr(STR_BACK) : "",
                            state.inputControlsVisible ? tr(STR_TOGGLE) : "", "", "Main");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
}  // namespace

const char* triStateText(uint8_t state, const char* offText, const char* onText) {
  switch (state) {
    case static_cast<uint8_t>(CompanionProtocol::TriState::Off):
      return offText;
    case static_cast<uint8_t>(CompanionProtocol::TriState::On):
      return onText;
    default:
      return "Unknown";
  }
}

void resetRenderCache() {
  hasLastRenderedState = false;
  lastRenderedPage = Page::Status;
  lastRenderedState = State{};
  renderWriteCount = 0;
}

bool renderIfChanged(GfxRenderer& renderer, MappedInputManager& mappedInput, Page page, const State& state) {
  if (hasLastRenderedState && page == lastRenderedPage && activePageMatches(page, state, lastRenderedState)) {
    LOG_DBG("COMP", "Render skipped; active companion page unchanged");
    return false;
  }

  lastRenderedPage = page;
  lastRenderedState = state;
  hasLastRenderedState = true;
  renderWriteCount++;

  HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
  if (page == Page::Diagnostics) {
    drawDiagnosticsPage(renderer, mappedInput, state);
  } else {
    drawStatusPage(renderer, mappedInput, state);
  }
  renderer.displayBuffer();
  return true;
}
}  // namespace LaptopCompanionView
