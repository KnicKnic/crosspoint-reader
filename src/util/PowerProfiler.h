#pragma once

#include <string>
#include <vector>

namespace PowerProfiler {

struct Snapshot {
  std::vector<std::string> summaryLines;
  std::vector<std::string> pmModeLines;
  std::vector<std::string> taskLines;
  std::vector<std::string> lockLines;
  std::vector<std::string> timerLines;
  std::string rawLockDump;
  std::string rawTimerDump;
};

Snapshot collect();
std::vector<std::string> buildDisplayLines(const Snapshot& snapshot);
void printSerial();

}  // namespace PowerProfiler
