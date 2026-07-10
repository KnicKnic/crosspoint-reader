#pragma once

#include <cstdint>
#include <string>

class GfxRenderer;
class MappedInputManager;

namespace LaptopCompanionView {
const char* triStateText(uint8_t state, const char* offText, const char* onText);

void render(GfxRenderer& renderer, MappedInputManager& mappedInput, bool hostConnected, const std::string& statusMessage,
            const std::string& microphoneMessage, const std::string& cameraMessage,
            const std::string& powerStatsMessage, const std::string& powerDeltaMessage,
            const std::string& powerTimerMessage);
}  // namespace LaptopCompanionView
