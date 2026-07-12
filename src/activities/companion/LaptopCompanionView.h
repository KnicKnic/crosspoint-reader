#pragma once

#include <cstdint>
#include <string>

class GfxRenderer;
class MappedInputManager;

namespace LaptopCompanionView {
enum class Page : uint8_t { Status, Diagnostics };

struct State {
  bool hostConnected = false;
  bool inputControlsVisible = true;
  std::string statusMessage = "Waiting for host";
  std::string microphoneMessage = "Unknown";
  std::string cameraMessage = "Unknown";
  std::string powerStatsMessage;
  std::string powerDeltaMessage;
  std::string powerAccountingMessage;
  std::string wakeCauseMessage;
  std::string powerTimerMessage;
  std::string bleAdvertiseMessage;
  std::string bleConnectionMessage;
  std::string bleActivityMessage;
  std::string btLockTraceMessage;
  std::string powerTaskMessage;
  std::string renderCostMessage;
  std::string pmLockMessage3;
  std::string pmLockMessage4;
  std::string pmLockMessage5;
};

const char* triStateText(uint8_t state, const char* offText, const char* onText);

void resetRenderCache();
bool renderIfChanged(GfxRenderer& renderer, MappedInputManager& mappedInput, Page page, const State& state);
}  // namespace LaptopCompanionView
