#include "PowerProfiler.h"

#include <Arduino.h>
#include <HalPowerManager.h>
#include <HalPowerStats.h>
#include <Logging.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#if CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr size_t MAX_DISPLAY_LINES_PER_SECTION = 12;
constexpr size_t MAX_TASKS_TRACKED = 256;

struct RankedLine {
  uint64_t score = 0;
  std::string text;
};

struct PmModeStat {
  std::string freq = "--";
  uint64_t timeUs = 0;
  std::string percent = "0%";
  bool seen = false;
};

uint64_t parseUnsigned(const std::string& token) {
  const char* begin = token.c_str();
  char* end = nullptr;
  const auto value = strtoull(begin, &end, 10);
  return end == begin ? 0 : value;
}

std::string formatDurationUs(uint64_t us) {
  uint64_t seconds = us / 1000000ULL;
  const uint64_t hours = seconds / 3600ULL;
  seconds %= 3600ULL;
  const uint64_t minutes = seconds / 60ULL;
  seconds %= 60ULL;

  char formatted[20];
  snprintf(formatted, sizeof(formatted), "%02llu:%02llu:%02llu", static_cast<unsigned long long>(hours),
           static_cast<unsigned long long>(minutes), static_cast<unsigned long long>(seconds));
  return formatted;
}

std::string formatFrequency(std::string freq) {
  if (!freq.empty() && freq.back() == 'M') {
    freq.pop_back();
    freq += " MHz";
  }
  return freq;
}

std::string formatPercent(std::string percent) {
  if (!percent.empty() && percent.back() != '%') percent += "%";
  return percent;
}

std::string formatPmModeLine(const char* label, const PmModeStat& stat, const char* suffix = "") {
  std::string line = std::string(label) + " " + stat.freq + " " + formatDurationUs(stat.timeUs) + " " + stat.percent;
  if (suffix[0] != '\0') {
    line += " ";
    line += suffix;
  }
  return line;
}

std::vector<std::string> splitWords(const std::string& line) {
  std::vector<std::string> words;
  size_t pos = 0;
  while (pos < line.size()) {
    while (pos < line.size() && isspace(static_cast<unsigned char>(line[pos]))) pos++;
    const size_t start = pos;
    while (pos < line.size() && !isspace(static_cast<unsigned char>(line[pos]))) pos++;
    if (pos > start) words.emplace_back(line.substr(start, pos - start));
  }
  return words;
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(std::move(line));
    start = end + 1;
  }
  return lines;
}

std::string captureDump(esp_err_t (*dumpFn)(FILE*)) {
  char* buffer = nullptr;
  size_t size = 0;
  FILE* stream = open_memstream(&buffer, &size);
  if (!stream) return "unable to allocate dump stream\n";

  const esp_err_t err = dumpFn(stream);
  fclose(stream);

  std::string output;
  if (buffer) {
    output.assign(buffer, size);
    free(buffer);
  }
  if (err != ESP_OK && output.empty()) {
    char msg[64];
    snprintf(msg, sizeof(msg), "dump failed: %d\n", static_cast<int>(err));
    output = msg;
  }
  return output;
}

std::vector<std::string> topLockLines(const std::string& dump) {
  std::vector<RankedLine> ranked;
  bool inLockStats = false;
  for (const auto& line : splitLines(dump)) {
    const auto words = splitWords(line);
    if (line == "Lock stats:") {
      inLockStats = true;
      continue;
    }
    if (line == "Mode stats:") break;
    if (!inLockStats || words.size() < 4 || words[0] == "Name") continue;

    RankedLine item;
#if CONFIG_PM_PROFILING
    if (words.size() >= 7) {
      item.score = parseUnsigned(words[words.size() - 2]);
      item.text = formatDurationUs(parseUnsigned(words[5])) + " " + words[6] + " " + words[0] + " " + words[1] +
                  " on=" + words[3] + " cnt=" + words[4];
    } else
#endif
    {
      item.score = parseUnsigned(words.back());
      item.text = line;
    }
    ranked.push_back(std::move(item));
  }

  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.score > b.score; });

  std::vector<std::string> lines;
  for (size_t i = 0; i < ranked.size() && i < MAX_DISPLAY_LINES_PER_SECTION; i++) {
    lines.push_back(ranked[i].text);
  }
  if (lines.empty()) lines.push_back("No PM locks reported");
  return lines;
}

std::vector<std::string> pmModeLines(const std::string& dump) {
  PmModeStat sleep;
  PmModeStat apbMin;
  PmModeStat apbMax;
  PmModeStat cpuMax;
  std::string sleepCounts;
  bool inModeStats = false;
  for (const auto& line : splitLines(dump)) {
    const auto words = splitWords(line);
    if (line == "Mode stats:") {
      inModeStats = true;
      continue;
    }
    if (!inModeStats) continue;
    if (line == "Sleep stats:") continue;
    if (!words.empty() && words[0].rfind("light_sleep_counts:", 0) == 0) {
      sleepCounts = words[0];
      if (words.size() > 1) sleepCounts += " " + words[1];
      continue;
    }
    if (words.size() < 4 || words[0] == "Mode") continue;

    const std::string& mode = words[0];
    PmModeStat* stat = nullptr;
    if (mode == "SLEEP") stat = &sleep;
    else if (mode == "APB_MIN") stat = &apbMin;
    else if (mode == "APB_MAX") stat = &apbMax;
    else if (mode == "CPU_MAX") stat = &cpuMax;
    if (stat == nullptr) continue;

    stat->freq = formatFrequency(words[1]);
    stat->timeUs = parseUnsigned(words[words.size() - 2]);
    stat->percent = formatPercent(words.back());
    stat->seen = true;
  }

  if (!sleep.seen) {
    sleep.freq = apbMin.freq;
    if (sleep.freq == "--") sleep.freq = std::to_string(powerManager.getConfiguredMinFrequencyMhz()) + " MHz";
  }

  std::vector<std::string> lines;
  lines.push_back(formatPmModeLine("CPU max", cpuMax));
  lines.push_back(formatPmModeLine("APB max", apbMax));
  lines.push_back(formatPmModeLine("CPU min", apbMin));
  lines.push_back(formatPmModeLine("Sleep", sleep, sleep.seen ? "" : "off"));
  if (!sleepCounts.empty()) lines.push_back(sleepCounts);
  if (lines.empty()) lines.push_back("PM mode stats unavailable");
  return lines;
}

std::vector<std::string> topTimerLines(const std::string& dump) {
  std::vector<RankedLine> ranked;
  for (const auto& line : splitLines(dump)) {
    const auto words = splitWords(line);
    if (words.size() < 3 || words[0] == "Timer" || words[0] == "Name") continue;

    RankedLine item;
#if CONFIG_ESP_TIMER_PROFILING
    if (words.size() >= 7) {
      item.score = parseUnsigned(words[words.size() - 1]);
      item.text = "per=" + words[1] + "us trig=" + words[4] + " skip=" + words[5] +
                  " cb=" + formatDurationUs(parseUnsigned(words[6])) + " " + words[0];
    } else
#endif
    {
      item.score = parseUnsigned(words.back());
      item.text = line;
    }
    ranked.push_back(std::move(item));
  }

  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.score > b.score; });

  std::vector<std::string> lines;
  for (size_t i = 0; i < ranked.size() && i < MAX_DISPLAY_LINES_PER_SECTION; i++) {
    lines.push_back(ranked[i].text);
  }
  if (lines.empty()) lines.push_back("No ESP timers reported");
  return lines;
}

std::vector<std::string> topTaskLines() {
  std::vector<std::string> lines;
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
  static configRUN_TIME_COUNTER_TYPE previousRuntime[MAX_TASKS_TRACKED] = {};
  static configRUN_TIME_COUNTER_TYPE previousTotalRuntime = 0;

  UBaseType_t taskCount = uxTaskGetNumberOfTasks();
  TaskStatus_t* statuses = static_cast<TaskStatus_t*>(pvPortMalloc(taskCount * sizeof(TaskStatus_t)));
  if (!statuses) {
    lines.push_back("Unable to allocate task snapshot");
    return lines;
  }

  configRUN_TIME_COUNTER_TYPE totalRuntime = 0;
  taskCount = uxTaskGetSystemState(statuses, taskCount, &totalRuntime);
  configRUN_TIME_COUNTER_TYPE periodRuntime = totalRuntime - previousTotalRuntime;
  if (periodRuntime == 0) periodRuntime = totalRuntime == 0 ? 1 : totalRuntime;
  previousTotalRuntime = totalRuntime;

  std::vector<RankedLine> ranked;
  ranked.reserve(taskCount);
  for (UBaseType_t i = 0; i < taskCount; i++) {
    const auto& task = statuses[i];
    configRUN_TIME_COUNTER_TYPE delta = task.ulRunTimeCounter;
    if (task.xTaskNumber < MAX_TASKS_TRACKED) {
      delta -= previousRuntime[task.xTaskNumber];
      previousRuntime[task.xTaskNumber] = task.ulRunTimeCounter;
    }
    const uint32_t loadTenths = static_cast<uint32_t>((static_cast<uint64_t>(delta) * 1000ULL) / periodRuntime);

    char line[96];
    snprintf(line, sizeof(line), "%-16s %3lu.%lu%% prio=%u stack=%lu", task.pcTaskName,
             static_cast<unsigned long>(loadTenths / 10), static_cast<unsigned long>(loadTenths % 10),
             static_cast<unsigned int>(task.uxCurrentPriority), static_cast<unsigned long>(task.usStackHighWaterMark));
    ranked.push_back({delta, line});
  }
  vPortFree(statuses);

  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
  for (size_t i = 0; i < ranked.size() && i < MAX_DISPLAY_LINES_PER_SECTION; i++) {
    lines.push_back(ranked[i].text);
  }
#else
  lines.push_back("FreeRTOS trace facility unavailable");
#endif
  return lines;
}

std::vector<std::string> hardwareProbeLines() {
  std::vector<std::string> lines;
  for (uint8_t i = 0; i < static_cast<uint8_t>(HalPowerStats::Probe::Count); i++) {
    const auto probe = static_cast<HalPowerStats::Probe>(i);
    const auto counter = HalPowerStats::get(probe);
    const unsigned long long avgUs =
        counter.queries == 0 ? 0ULL : static_cast<unsigned long long>(counter.totalUs / counter.queries);
    char line[96];
    snprintf(line, sizeof(line), "%-12s count=%lu time=%s avg=%lluus", HalPowerStats::name(probe),
             static_cast<unsigned long>(counter.queries), formatDurationUs(counter.totalUs).c_str(),
             avgUs);
    lines.emplace_back(line);
  }
  return lines;
}

void appendSection(std::vector<std::string>& out, const std::vector<std::string>& lines) {
  if (!out.empty()) out.emplace_back();
  out.insert(out.end(), lines.begin(), lines.end());
}

}  // namespace

namespace PowerProfiler {

Snapshot collect() {
  Snapshot snapshot;

  char line[96];
  snprintf(line, sizeof(line), "CPU current=%d MHz policy=%d-%d MHz configured-max=%d MHz", getCpuFrequencyMhz(),
           powerManager.getConfiguredMinFrequencyMhz(), powerManager.getConfiguredMaxFrequencyMhz(),
           powerManager.getConfiguredMaxFrequencyMhz());
  snapshot.summaryLines.emplace_back(line);

  snprintf(line, sizeof(line), "DFS=%s tickless=%s auto-light-sleep=%s pm=%s", CONFIG_PM_DFS_INIT_AUTO ? "on" : "off",
           CONFIG_FREERTOS_USE_TICKLESS_IDLE ? "on" : "off",
           powerManager.isAutoLightSleepEnabled() ? "on" : "off",
           powerManager.isPowerManagementConfigured() ? "configured" : "off");
  snapshot.summaryLines.emplace_back(line);

  const int64_t nowUs = esp_timer_get_time();
  const int64_t nextAlarmUs = esp_timer_get_next_alarm();
  const int64_t nextWakeUs = esp_timer_get_next_alarm_for_wake_up();
  snprintf(line, sizeof(line), "Timers now=%lld next=%lld wake=%lld us", static_cast<long long>(nowUs),
           static_cast<long long>(nextAlarmUs), static_cast<long long>(nextWakeUs));
  snapshot.summaryLines.emplace_back(line);

  snprintf(line, sizeof(line), "Heap free=%lu min=%lu maxalloc=%lu", static_cast<unsigned long>(ESP.getFreeHeap()),
           static_cast<unsigned long>(ESP.getMinFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()));
  snapshot.summaryLines.emplace_back(line);

  snapshot.hardwareLines = hardwareProbeLines();

  snapshot.taskLines = topTaskLines();
#if CONFIG_PM_ENABLE
  snapshot.rawLockDump = captureDump(esp_pm_dump_locks);
  snapshot.lockLines = topLockLines(snapshot.rawLockDump);
  snapshot.pmModeLines = pmModeLines(snapshot.rawLockDump);
#else
  snapshot.rawLockDump = "Power management is not enabled\n";
  snapshot.lockLines = {"Power management is not enabled"};
  snapshot.pmModeLines = {"Power management is not enabled"};
#endif
  snapshot.rawTimerDump = captureDump(esp_timer_dump);
  snapshot.timerLines = topTimerLines(snapshot.rawTimerDump);

  return snapshot;
}

std::vector<std::string> buildDisplayLines(const Snapshot& snapshot) {
  std::vector<std::string> lines;
  appendSection(lines, snapshot.summaryLines);
  appendSection(lines, snapshot.hardwareLines);
  appendSection(lines, snapshot.pmModeLines);
  appendSection(lines, snapshot.taskLines);
  appendSection(lines, snapshot.lockLines);
  appendSection(lines, snapshot.timerLines);
  return lines;
}

void printSerial() {
#ifdef ENABLE_SERIAL_LOG
  if (!logSerial) return;
  const Snapshot snapshot = collect();
  logSerial.println();
  logSerial.println("=== POWER PROFILER ===");
  for (const auto& line : buildDisplayLines(snapshot)) logSerial.println(line.c_str());
  logSerial.println("-- PM Locks --");
  logSerial.print(snapshot.rawLockDump.c_str());
  logSerial.println("-- ESP Timers --");
  logSerial.print(snapshot.rawTimerDump.c_str());
  logSerial.println("=== END POWER PROFILER ===");
#endif
}

}  // namespace PowerProfiler
