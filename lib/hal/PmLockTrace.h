#pragma once

#include <cstdint>
#include <string>

void setBtLockTraceConnectionParams(uint16_t intervalUnits, uint16_t latency);
std::string formatBtLockTraceDiagnostics();
