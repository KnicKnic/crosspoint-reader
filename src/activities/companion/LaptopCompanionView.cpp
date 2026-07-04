#include "LaptopCompanionView.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "companion/CompanionProtocol.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace LaptopCompanionView {
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

void render(GfxRenderer& renderer, MappedInputManager& mappedInput, bool hostConnected,
            const std::string& statusMessage, const std::string& microphoneMessage,
            const std::string& cameraMessage) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int contentX = metrics.contentSidePadding;
  const int contentWidth = pageWidth - metrics.contentSidePadding * 2;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 20;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LAPTOP_COMPANION));

  renderer.drawCenteredText(UI_12_FONT_ID, y, hostConnected ? "Connected" : "Disconnected", true,
                            EpdFontFamily::BOLD);
  y += 60;

  renderer.drawText(UI_10_FONT_ID, contentX, y, "Teams");
  renderer.drawText(UI_10_FONT_ID, contentX + 150, y, statusMessage.c_str(), true, EpdFontFamily::BOLD);
  y += 42;

  renderer.drawText(UI_10_FONT_ID, contentX, y, "Microphone");
  renderer.drawText(UI_10_FONT_ID, contentX + 150, y, microphoneMessage.c_str(), true, EpdFontFamily::BOLD);
  y += 42;

  renderer.drawText(UI_10_FONT_ID, contentX, y, "Camera");
  renderer.drawText(UI_10_FONT_ID, contentX + 150, y, cameraMessage.c_str(), true, EpdFontFamily::BOLD);
  y += 62;

  const auto wrapped =
      renderer.wrappedText(SMALL_FONT_ID, "Open the WPF host app on Windows, then connect over BLE.", contentWidth, 3);
  for (const auto& line : wrapped) {
    renderer.drawText(SMALL_FONT_ID, contentX, y, line.c_str());
    y += renderer.getLineHeight(SMALL_FONT_ID);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
}  // namespace LaptopCompanionView
