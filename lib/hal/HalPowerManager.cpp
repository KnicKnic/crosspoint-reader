#include "HalPowerManager.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "HalBluetoothManager.h"
#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

namespace {
constexpr int AUTO_LIGHT_SLEEP_MIN_FREQ = 40;  // ESP32-C3 XTAL floor; valid for IDF DFS on this target.
constexpr gpio_num_t X3_BATTERY_LATCH_PIN = GPIO_NUM_13;
portMUX_TYPE lightSleepStatsMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t lightSleepEnterCount = 0;
uint64_t lightSleepTotalUs = 0;
uint64_t lightSleepRequestedTotalUs = 0;
uint64_t lightSleepEarlyWakeCount = 0;
int64_t lightSleepEnteredAtUs = 0;
uint64_t lightSleepRequestedUs = 0;
uint64_t lightSleepWakeCauseCounts[HalPowerManager::LIGHT_SLEEP_WAKE_CAUSE_COUNT] = {};
uint64_t lightSleepRequestBucketCounts[HalPowerManager::LIGHT_SLEEP_REQUEST_BUCKET_COUNT] = {};
bool lightSleepCallbacksRegistered = false;
bool x3BatteryLatchHeldForLightSleep = false;

constexpr UBaseType_t TASK_RUNTIME_MAX_TASKS = 48;
constexpr uint8_t TASK_RUNTIME_TOP_COUNT = 8;
constexpr uint8_t TIMER_ACTIVITY_MAX_TIMERS = 48;
constexpr uint8_t TIMER_ACTIVITY_TOP_COUNT = 5;
constexpr size_t TIMER_ACTIVITY_NAME_LEN = 21;

struct TaskRuntimeSnapshot {
  TaskHandle_t handle = nullptr;
  char name[configMAX_TASK_NAME_LEN] = {};
  configRUN_TIME_COUNTER_TYPE runtime = 0;
};

struct TaskRuntimeDelta {
  const char* name = "";
  configRUN_TIME_COUNTER_TYPE delta = 0;
  eTaskState state = eInvalid;
  UBaseType_t priority = 0;
  configSTACK_DEPTH_TYPE stackHighWaterMark = 0;
};

TaskRuntimeSnapshot previousTaskRuntime[TASK_RUNTIME_MAX_TASKS];
UBaseType_t previousTaskRuntimeCount = 0;
configRUN_TIME_COUNTER_TYPE previousTaskRuntimeTotal = 0;
bool hasPreviousTaskRuntime = false;

struct TimerActivitySnapshot {
  char name[TIMER_ACTIVITY_NAME_LEN] = {};
  uint64_t triggered = 0;
  uint64_t armed = 0;
  uint64_t callbackTimeUs = 0;
};

struct TimerActivityDelta {
  char name[TIMER_ACTIVITY_NAME_LEN] = {};
  uint64_t triggered = 0;
  uint64_t armed = 0;
  uint64_t callbackTimeUs = 0;
};

TimerActivitySnapshot previousTimerActivity[TIMER_ACTIVITY_MAX_TIMERS];
uint8_t previousTimerActivityCount = 0;
bool hasPreviousTimerActivity = false;

uint8_t lightSleepWakeCauseIndex(esp_sleep_wakeup_cause_t cause) {
  const auto index = static_cast<uint32_t>(cause);
  if (index < HalPowerManager::LIGHT_SLEEP_WAKE_CAUSE_UNKNOWN_INDEX) {
    return static_cast<uint8_t>(index);
  }
  return HalPowerManager::LIGHT_SLEEP_WAKE_CAUSE_UNKNOWN_INDEX;
}

uint8_t lightSleepRequestBucketIndex(uint64_t requestedUs) {
  if (requestedUs < 1000ULL) {
    return 0;
  }
  if (requestedUs < 5000ULL) {
    return 1;
  }
  if (requestedUs < 20000ULL) {
    return 2;
  }
  if (requestedUs < 100000ULL) {
    return 3;
  }
  if (requestedUs < 500000ULL) {
    return 4;
  }
  return 5;
}

bool isEarlyLightSleepWake(uint64_t requestedUs, uint64_t sleptUs) {
  if (requestedUs < 1000ULL) {
    return false;
  }
  const uint64_t pmEarlyWakeMarginUs = 100ULL;
  const uint64_t measurementSlackUs = 500ULL;
  const uint64_t expectedUs = requestedUs > pmEarlyWakeMarginUs ? requestedUs - pmEarlyWakeMarginUs : requestedUs;
  return sleptUs + measurementSlackUs < expectedUs;
}

void holdX3BatteryLatchForLightSleep() {
  if (!gpio.deviceIsX3()) {
    return;
  }

  gpio_hold_dis(X3_BATTERY_LATCH_PIN);
  gpio_set_direction(X3_BATTERY_LATCH_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(X3_BATTERY_LATCH_PIN, 1);
  gpio_hold_en(X3_BATTERY_LATCH_PIN);
  x3BatteryLatchHeldForLightSleep = true;
}

void releaseX3BatteryLatchLightSleepHold() {
  if (!x3BatteryLatchHeldForLightSleep) {
    return;
  }

  gpio_hold_dis(X3_BATTERY_LATCH_PIN);
  gpio_set_direction(X3_BATTERY_LATCH_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(X3_BATTERY_LATCH_PIN, 1);
  x3BatteryLatchHeldForLightSleep = false;
}

std::string trimTimerField(const char* text, size_t length) {
  std::string value(text, length);
  const size_t first = value.find_first_not_of(' ');
  if (first == std::string::npos) {
    return {};
  }
  const size_t last = value.find_last_not_of(' ');
  return value.substr(first, last - first + 1);
}

void shortenTimerName(const char* name, char* buffer, size_t bufferSize) {
  if (bufferSize == 0) {
    return;
  }
  snprintf(buffer, bufferSize, "%s", name && name[0] ? name : "?");
  if (strlen(buffer) <= 10) {
    return;
  }
  buffer[10] = '\0';
}

int findTimerActivity(TimerActivitySnapshot* timers, uint8_t count, const char* name) {
  for (uint8_t i = 0; i < count; i++) {
    if (strncmp(timers[i].name, name, TIMER_ACTIVITY_NAME_LEN) == 0) {
      return i;
    }
  }
  return -1;
}

const TimerActivitySnapshot* findPreviousTimerActivity(const char* name) {
  for (uint8_t i = 0; i < previousTimerActivityCount; i++) {
    if (strncmp(previousTimerActivity[i].name, name, TIMER_ACTIVITY_NAME_LEN) == 0) {
      return &previousTimerActivity[i];
    }
  }
  return nullptr;
}

uint64_t counterDelta(uint64_t current, uint64_t previous) {
  return current >= previous ? current - previous : 0;
}

void insertTimerActivityDelta(TimerActivityDelta topDeltas[TIMER_ACTIVITY_TOP_COUNT],
                              const TimerActivitySnapshot& timer, uint64_t triggeredDelta, uint64_t armedDelta,
                              uint64_t callbackTimeDelta) {
  if (triggeredDelta == 0 && armedDelta == 0) {
    return;
  }

  for (uint8_t i = 0; i < TIMER_ACTIVITY_TOP_COUNT; i++) {
    if (triggeredDelta < topDeltas[i].triggered ||
        (triggeredDelta == topDeltas[i].triggered && armedDelta <= topDeltas[i].armed)) {
      continue;
    }

    for (uint8_t j = TIMER_ACTIVITY_TOP_COUNT - 1; j > i; j--) {
      topDeltas[j] = topDeltas[j - 1];
    }

    snprintf(topDeltas[i].name, sizeof(topDeltas[i].name), "%s", timer.name);
    topDeltas[i].triggered = triggeredDelta;
    topDeltas[i].armed = armedDelta;
    topDeltas[i].callbackTimeUs = callbackTimeDelta;
    return;
  }
}

bool parseTimerDumpLine(const char* line, TimerActivitySnapshot& timer) {
  const size_t length = strlen(line);
  if (length < 22 || strncmp(line, "Name", 4) == 0 || strncmp(line, "Timer stats", 11) == 0) {
    return false;
  }

  const std::string name = trimTimerField(line, 20);
  if (name.empty()) {
    return false;
  }

  unsigned long long period = 0;
  long long alarm = 0;
  unsigned long long armed = 0;
  unsigned long long triggered = 0;
  unsigned long long skipped = 0;
  unsigned long long callbackTime = 0;
  if (sscanf(line + 20, "%llu %lld %llu %llu %llu %llu", &period, &alarm, &armed, &triggered, &skipped,
             &callbackTime) != 6) {
    return false;
  }

  snprintf(timer.name, sizeof(timer.name), "%s", name.c_str());
  timer.triggered = triggered;
  timer.armed = armed;
  timer.callbackTimeUs = callbackTime;
  return true;
}

uint8_t captureTimerActivity(TimerActivitySnapshot* timers, uint8_t maxTimers) {
  char* dump = nullptr;
  size_t dumpSize = 0;
  FILE* stream = open_memstream(&dump, &dumpSize);
  if (!stream) {
    return 0;
  }

  const esp_err_t err = esp_timer_dump(stream);
  fclose(stream);
  if (err != ESP_OK || !dump) {
    free(dump);
    return 0;
  }

  uint8_t count = 0;
  char* cursor = dump;
  while (cursor && *cursor) {
    char* next = strchr(cursor, '\n');
    if (next) {
      *next = '\0';
    }

    TimerActivitySnapshot parsed;
    if (parseTimerDumpLine(cursor, parsed)) {
      const int existing = findTimerActivity(timers, count, parsed.name);
      if (existing >= 0) {
        timers[existing].triggered += parsed.triggered;
        timers[existing].armed += parsed.armed;
        timers[existing].callbackTimeUs += parsed.callbackTimeUs;
      } else if (count < maxTimers) {
        timers[count++] = parsed;
      }
    }

    cursor = next ? next + 1 : nullptr;
  }

  free(dump);
  return count;
}

void storeTimerActivitySnapshot(const TimerActivitySnapshot* timers, uint8_t count) {
  previousTimerActivityCount = std::min(count, TIMER_ACTIVITY_MAX_TIMERS);
  for (uint8_t i = 0; i < previousTimerActivityCount; i++) {
    previousTimerActivity[i] = timers[i];
  }
  hasPreviousTimerActivity = true;
}

void logPmLockDiagnostics() {
  char* dump = nullptr;
  size_t dumpSize = 0;
  FILE* stream = open_memstream(&dump, &dumpSize);
  if (!stream) {
    LOG_INF("PWR", "PM lock dump unavailable: stream allocation failed");
    return;
  }

  const esp_err_t err = esp_pm_dump_locks(stream);
  fclose(stream);
  if (err != ESP_OK || !dump) {
    free(dump);
    LOG_INF("PWR", "PM lock dump unavailable err=%d", err);
    return;
  }

  char* cursor = dump;
  while (cursor && *cursor) {
    char* next = strchr(cursor, '\n');
    if (next) {
      *next = '\0';
    }
    if (cursor[0] != '\0') {
      LOG_INF("PWR", "PM lock: %s", cursor);
    }
    cursor = next ? next + 1 : nullptr;
  }

  free(dump);
}

const TaskRuntimeSnapshot* findPreviousTaskRuntime(TaskHandle_t handle) {
  for (UBaseType_t i = 0; i < previousTaskRuntimeCount; i++) {
    if (previousTaskRuntime[i].handle == handle) {
      return &previousTaskRuntime[i];
    }
  }
  return nullptr;
}

configRUN_TIME_COUNTER_TYPE runtimeDelta(configRUN_TIME_COUNTER_TYPE current, configRUN_TIME_COUNTER_TYPE previous) {
  if (current >= previous) {
    return current - previous;
  }
  return current + (std::numeric_limits<configRUN_TIME_COUNTER_TYPE>::max() - previous) + 1;
}

const char* taskStateName(eTaskState state) {
  switch (state) {
    case eRunning:
      return "running";
    case eReady:
      return "ready";
    case eBlocked:
      return "blocked";
    case eSuspended:
      return "suspended";
    case eDeleted:
      return "deleted";
    case eInvalid:
      return "invalid";
    default:
      return "unknown";
  }
}

void insertTaskRuntimeDelta(TaskRuntimeDelta topDeltas[TASK_RUNTIME_TOP_COUNT], const TaskStatus_t& task,
                            configRUN_TIME_COUNTER_TYPE delta) {
  if (delta == 0) {
    return;
  }

  for (uint8_t i = 0; i < TASK_RUNTIME_TOP_COUNT; i++) {
    if (delta <= topDeltas[i].delta) {
      continue;
    }

    for (uint8_t j = TASK_RUNTIME_TOP_COUNT - 1; j > i; j--) {
      topDeltas[j] = topDeltas[j - 1];
    }

    topDeltas[i] = TaskRuntimeDelta{
        .name = task.pcTaskName ? task.pcTaskName : "",
        .delta = delta,
        .state = task.eCurrentState,
        .priority = task.uxCurrentPriority,
        .stackHighWaterMark = task.usStackHighWaterMark,
    };
    return;
  }
}

void storeTaskRuntimeSnapshot(const TaskStatus_t* tasks, UBaseType_t taskCount,
                              configRUN_TIME_COUNTER_TYPE totalRuntime) {
  previousTaskRuntimeCount = std::min(taskCount, TASK_RUNTIME_MAX_TASKS);
  for (UBaseType_t i = 0; i < previousTaskRuntimeCount; i++) {
    previousTaskRuntime[i].handle = tasks[i].xHandle;
    previousTaskRuntime[i].runtime = tasks[i].ulRunTimeCounter;
    snprintf(previousTaskRuntime[i].name, sizeof(previousTaskRuntime[i].name), "%s",
             tasks[i].pcTaskName ? tasks[i].pcTaskName : "");
  }
  previousTaskRuntimeTotal = totalRuntime;
  hasPreviousTaskRuntime = true;
}

void logTaskRuntimeDiagnostics() {
  TaskStatus_t tasks[TASK_RUNTIME_MAX_TASKS] = {};
  configRUN_TIME_COUNTER_TYPE totalRuntime = 0;
  const UBaseType_t knownTaskCount = uxTaskGetNumberOfTasks();
  const UBaseType_t taskCount = uxTaskGetSystemState(tasks, TASK_RUNTIME_MAX_TASKS, &totalRuntime);
  if (taskCount == 0) {
    LOG_INF("PWR", "Task runtime unavailable tasks=%u cap=%u", static_cast<unsigned>(knownTaskCount),
            static_cast<unsigned>(TASK_RUNTIME_MAX_TASKS));
    return;
  }

  if (!hasPreviousTaskRuntime) {
    storeTaskRuntimeSnapshot(tasks, taskCount, totalRuntime);
    LOG_INF("PWR", "Task runtime baseline captured tasks=%u total=%llu", static_cast<unsigned>(taskCount),
            static_cast<unsigned long long>(totalRuntime));
    return;
  }

  TaskRuntimeDelta topDeltas[TASK_RUNTIME_TOP_COUNT] = {};
  const configRUN_TIME_COUNTER_TYPE totalDelta = runtimeDelta(totalRuntime, previousTaskRuntimeTotal);
  for (UBaseType_t i = 0; i < taskCount; i++) {
    const TaskRuntimeSnapshot* previous = findPreviousTaskRuntime(tasks[i].xHandle);
    if (!previous) {
      insertTaskRuntimeDelta(topDeltas, tasks[i], tasks[i].ulRunTimeCounter);
      continue;
    }
    insertTaskRuntimeDelta(topDeltas, tasks[i], runtimeDelta(tasks[i].ulRunTimeCounter, previous->runtime));
  }

  LOG_INF("PWR", "Task runtime delta tasks=%u known=%u total=%llu", static_cast<unsigned>(taskCount),
          static_cast<unsigned>(knownTaskCount), static_cast<unsigned long long>(totalDelta));
  for (uint8_t i = 0; i < TASK_RUNTIME_TOP_COUNT; i++) {
    if (topDeltas[i].delta == 0) {
      continue;
    }

    const unsigned long long pctX10 =
        totalDelta > 0 ? (static_cast<unsigned long long>(topDeltas[i].delta) * 1000ULL) / totalDelta : 0ULL;
    LOG_INF("PWR", "Task runtime top%u name=%s runtime=%llu pct=%llu.%01llu state=%s prio=%u stack_hwm=%u",
            static_cast<unsigned>(i + 1), topDeltas[i].name,
            static_cast<unsigned long long>(topDeltas[i].delta), pctX10 / 10ULL, pctX10 % 10ULL,
            taskStateName(topDeltas[i].state), static_cast<unsigned>(topDeltas[i].priority),
            static_cast<unsigned>(topDeltas[i].stackHighWaterMark));
  }

  storeTaskRuntimeSnapshot(tasks, taskCount, totalRuntime);
}

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
esp_err_t onLightSleepEnter(int64_t sleepTimeUs, void*) {
  const int64_t nowUs = esp_timer_get_time();
  const uint64_t requestedUs = sleepTimeUs > 0 ? static_cast<uint64_t>(sleepTimeUs) : 0;
  portENTER_CRITICAL(&lightSleepStatsMux);
  lightSleepEnteredAtUs = nowUs;
  lightSleepRequestedUs = requestedUs;
  lightSleepEnterCount++;
  portEXIT_CRITICAL(&lightSleepStatsMux);
  return ESP_OK;
}

esp_err_t onLightSleepExit(int64_t sleptTimeUs, void*) {
  const esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  portENTER_CRITICAL(&lightSleepStatsMux);
  if (lightSleepEnteredAtUs > 0) {
    const uint64_t sleptUs = sleptTimeUs > 0 ? static_cast<uint64_t>(sleptTimeUs) : 0;
    const uint64_t requestedUs = lightSleepRequestedUs;
    lightSleepTotalUs += sleptUs;
    lightSleepRequestedTotalUs += requestedUs;
    lightSleepRequestBucketCounts[lightSleepRequestBucketIndex(requestedUs)]++;
    if (isEarlyLightSleepWake(requestedUs, sleptUs)) {
      lightSleepEarlyWakeCount++;
    }
    lightSleepWakeCauseCounts[lightSleepWakeCauseIndex(wakeupCause)]++;
  }
  lightSleepEnteredAtUs = 0;
  lightSleepRequestedUs = 0;
  portEXIT_CRITICAL(&lightSleepStatsMux);
  return ESP_OK;
}
#endif

bool releasePmLock(esp_pm_lock_handle_t handle, bool& acquired, const char* name) {
  if (!acquired || !handle) {
    return true;
  }
  const esp_err_t err = esp_pm_lock_release(handle);
  acquired = false;
  if (err != ESP_OK) {
    LOG_ERR("PWR", "Failed to release %s PM lock: %d", name, err);
    return false;
  }
  return true;
}

bool acquirePmLock(esp_pm_lock_handle_t handle, bool& acquired, const char* name) {
  if (acquired) {
    return true;
  }
  if (!handle) {
    return false;
  }
  const esp_err_t err = esp_pm_lock_acquire(handle);
  if (err != ESP_OK) {
    LOG_ERR("PWR", "Failed to acquire %s PM lock: %d", name, err);
    return false;
  }
  acquired = true;
  return true;
}

void formatDurationUs(uint64_t durationUs, char* buffer, size_t bufferSize) {
  const uint64_t totalMs = durationUs / 1000ULL;
  if (totalMs < 1000ULL) {
    snprintf(buffer, bufferSize, "%llums", static_cast<unsigned long long>(totalMs));
    return;
  }

  const uint64_t totalSeconds = totalMs / 1000ULL;
  if (totalSeconds < 60ULL) {
    snprintf(buffer, bufferSize, "%llus", static_cast<unsigned long long>(totalSeconds));
    return;
  }

  const uint64_t minutes = totalSeconds / 60ULL;
  const uint64_t seconds = totalSeconds % 60ULL;
  if (minutes < 60ULL) {
    snprintf(buffer, bufferSize, "%llum%02llus", static_cast<unsigned long long>(minutes),
             static_cast<unsigned long long>(seconds));
    return;
  }

  const uint64_t hours = minutes / 60ULL;
  const uint64_t remainingMinutes = minutes % 60ULL;
  snprintf(buffer, bufferSize, "%lluh%02llum", static_cast<unsigned long long>(hours),
           static_cast<unsigned long long>(remainingMinutes));
}
}  // namespace

void HalPowerManager::begin() {
  if (gpio.deviceIsX3()) {
    // X3 uses an I2C fuel gauge for battery monitoring.
    // I2C init must come AFTER gpio.begin() so early hardware detection/probes are finished.
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    Wire.setTimeOut(4);
    _batteryUseI2C = true;
  } else {
    pinMode(BAT_GPIO0, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
  if (!lightSleepCallbacksRegistered) {
    esp_pm_sleep_cbs_register_config_t callbacks = {
        .enter_cb = onLightSleepEnter,
        .exit_cb = onLightSleepExit,
        .enter_cb_user_arg = nullptr,
        .exit_cb_user_arg = nullptr,
        .enter_cb_prior = 0,
        .exit_cb_prior = 0,
    };
    const esp_err_t err = esp_pm_light_sleep_register_cbs(&callbacks);
    if (err == ESP_OK) {
      lightSleepCallbacksRegistered = true;
    } else {
      LOG_ERR("PWR", "Failed to register light sleep callbacks: %d", err);
    }
  }
#else
  LOG_INF("PWR", "Light sleep callbacks disabled; enable CONFIG_PM_LIGHT_SLEEP_CALLBACKS for sleep timing");
#endif
}

bool HalPowerManager::ensurePmLocks() {
  if (cpuMaxLock && apbMaxLock && noLightSleepLock) {
    return true;
  }

  if (!cpuMaxLock) {
    const esp_err_t err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "cp_cpu", &cpuMaxLock);
    if (err != ESP_OK) {
      LOG_ERR("PWR", "Failed to create CPU max PM lock: %d", err);
      return false;
    }
  }
  if (!apbMaxLock) {
    const esp_err_t err = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "cp_apb", &apbMaxLock);
    if (err != ESP_OK) {
      LOG_ERR("PWR", "Failed to create APB max PM lock: %d", err);
      return false;
    }
  }
  if (!noLightSleepLock) {
    const esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "cp_awake", &noLightSleepLock);
    if (err != ESP_OK) {
      LOG_ERR("PWR", "Failed to create no-light-sleep PM lock: %d", err);
      return false;
    }
  }

  return true;
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }
  if (autoLightSleepConfigured) {
    return;
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

bool HalPowerManager::configureAutoLightSleep(bool enabled) {
  if (normalFreq <= 0 || !modeMutex) {
    return false;
  }

  xSemaphoreTake(modeMutex, portMAX_DELAY);

  if (enabled == autoLightSleepConfigured) {
    xSemaphoreGive(modeMutex);
    return true;
  }

  if (enabled && !ensurePmLocks()) {
    xSemaphoreGive(modeMutex);
    return false;
  }

  esp_pm_config_t config = {};
  if (enabled) {
    holdX3BatteryLatchForLightSleep();

    esp_pm_config_t current = {};
    const esp_err_t getErr = esp_pm_get_configuration(&current);
    if (getErr == ESP_OK) {
      savedPmConfig = current;
      savedPmConfigValid = true;
    } else {
      savedPmConfig = esp_pm_config_t{
          .max_freq_mhz = normalFreq,
          .min_freq_mhz = AUTO_LIGHT_SLEEP_MIN_FREQ,
          .light_sleep_enable = false,
      };
      savedPmConfigValid = false;
      LOG_ERR("PWR", "Failed to read PM config before auto light sleep: %d", getErr);
    }

    config = esp_pm_config_t{
        .max_freq_mhz = normalFreq,
        .min_freq_mhz = AUTO_LIGHT_SLEEP_MIN_FREQ,
        .light_sleep_enable = true,
    };
  } else {
    config = savedPmConfigValid ? savedPmConfig
                                : esp_pm_config_t{
                                      .max_freq_mhz = normalFreq,
                                      .min_freq_mhz = AUTO_LIGHT_SLEEP_MIN_FREQ,
                                      .light_sleep_enable = false,
                                  };
    config.light_sleep_enable = false;
    if (config.max_freq_mhz <= 0) {
      config.max_freq_mhz = normalFreq;
    }
    if (config.min_freq_mhz <= 0 || config.min_freq_mhz > config.max_freq_mhz) {
      config.min_freq_mhz = AUTO_LIGHT_SLEEP_MIN_FREQ;
    }
  }

  const esp_err_t err = esp_pm_configure(&config);
  if (err != ESP_OK) {
    LOG_ERR("PWR", "%s auto light sleep failed: %d", enabled ? "Enable" : "Disable", err);
    if (enabled) {
      releaseX3BatteryLatchLightSleepHold();
    }
    xSemaphoreGive(modeMutex);
    return false;
  }

  autoLightSleepConfigured = enabled;
  if (!enabled) {
    savedPmConfigValid = false;
    releaseX3BatteryLatchLightSleepHold();
  }
  isLowPower = false;
  LOG_INF("PWR", "Auto light sleep %s max=%d min=%d", enabled ? "enabled" : "disabled", config.max_freq_mhz,
          config.min_freq_mhz);

  xSemaphoreGive(modeMutex);
  return true;
}

HalPowerManager::LightSleepStats HalPowerManager::getLightSleepStats() const {
  LightSleepStats stats;
  const int64_t nowUs = esp_timer_get_time();
  stats.uptimeUs = nowUs > 0 ? static_cast<uint64_t>(nowUs) : 0;
  portENTER_CRITICAL(&lightSleepStatsMux);
  stats.enterCount = lightSleepEnterCount;
  stats.sleptUs = lightSleepTotalUs;
  stats.requestedUs = lightSleepRequestedTotalUs;
  stats.earlyWakeCount = lightSleepEarlyWakeCount;
  if (lightSleepEnteredAtUs > 0 && nowUs >= lightSleepEnteredAtUs) {
    stats.sleptUs += static_cast<uint64_t>(nowUs - lightSleepEnteredAtUs);
    stats.requestedUs += lightSleepRequestedUs;
  }
  for (uint8_t i = 0; i < HalPowerManager::LIGHT_SLEEP_WAKE_CAUSE_COUNT; i++) {
    stats.wakeCauseCounts[i] = lightSleepWakeCauseCounts[i];
  }
  for (uint8_t i = 0; i < HalPowerManager::LIGHT_SLEEP_REQUEST_BUCKET_COUNT; i++) {
    stats.requestBucketCounts[i] = lightSleepRequestBucketCounts[i];
  }
  portEXIT_CRITICAL(&lightSleepStatsMux);
  return stats;
}

std::string HalPowerManager::formatLightSleepStats() const {
  const auto stats = getLightSleepStats();
  char slept[16];
  char uptime[16];
  formatDurationUs(stats.sleptUs, slept, sizeof(slept));
  formatDurationUs(stats.uptimeUs, uptime, sizeof(uptime));

  char buf[80];
  snprintf(buf, sizeof(buf), "Total: %lu x, sleep %s, boot %s", static_cast<unsigned long>(stats.enterCount), slept,
           uptime);
  return buf;
}

std::string HalPowerManager::formatEspTimerActivity() const {
#if CONFIG_ESP_TIMER_PROFILING
  TimerActivitySnapshot current[TIMER_ACTIVITY_MAX_TIMERS] = {};
  const uint8_t currentCount = captureTimerActivity(current, TIMER_ACTIVITY_MAX_TIMERS);
  if (currentCount == 0) {
    return "Timers: unavailable";
  }

  if (!hasPreviousTimerActivity) {
    storeTimerActivitySnapshot(current, currentCount);
    return "Timers: baseline";
  }

  TimerActivityDelta topDeltas[TIMER_ACTIVITY_TOP_COUNT] = {};
  for (uint8_t i = 0; i < currentCount; i++) {
    const TimerActivitySnapshot* previous = findPreviousTimerActivity(current[i].name);
    const uint64_t triggeredDelta =
        previous ? counterDelta(current[i].triggered, previous->triggered) : current[i].triggered;
    const uint64_t armedDelta = previous ? counterDelta(current[i].armed, previous->armed) : current[i].armed;
    const uint64_t callbackTimeDelta =
        previous ? counterDelta(current[i].callbackTimeUs, previous->callbackTimeUs) : current[i].callbackTimeUs;
    insertTimerActivityDelta(topDeltas, current[i], triggeredDelta, armedDelta, callbackTimeDelta);
  }

  storeTimerActivitySnapshot(current, currentCount);

  std::string line = "Timers:";
  if (topDeltas[0].triggered == 0 && topDeltas[0].armed == 0) {
    line += " none";
    return line;
  }

  for (uint8_t i = 0; i < TIMER_ACTIVITY_TOP_COUNT; i++) {
    if (topDeltas[i].triggered == 0 && topDeltas[i].armed == 0) {
      break;
    }

    char shortName[12];
    shortenTimerName(topDeltas[i].name, shortName, sizeof(shortName));
    char part[32];
    if (topDeltas[i].triggered > 0) {
      snprintf(part, sizeof(part), " %s+%llu", shortName, static_cast<unsigned long long>(topDeltas[i].triggered));
    } else {
      snprintf(part, sizeof(part), " %s armed+%llu", shortName, static_cast<unsigned long long>(topDeltas[i].armed));
    }
    line += part;
  }
  return line;
#else
  return "Timers: profiling off";
#endif
}

const char* HalPowerManager::lightSleepWakeCauseName(uint8_t causeIndex) {
  switch (causeIndex) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "undefined";
    case ESP_SLEEP_WAKEUP_ALL:
      return "all";
    case ESP_SLEEP_WAKEUP_EXT0:
      return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      return "touchpad";
    case ESP_SLEEP_WAKEUP_ULP:
      return "ulp";
    case ESP_SLEEP_WAKEUP_GPIO:
      return "gpio";
    case ESP_SLEEP_WAKEUP_UART:
      return "uart0";
    case ESP_SLEEP_WAKEUP_UART1:
      return "uart1";
    case ESP_SLEEP_WAKEUP_UART2:
      return "uart2";
    case ESP_SLEEP_WAKEUP_WIFI:
      return "wifi";
    case ESP_SLEEP_WAKEUP_COCPU:
      return "cocpu";
    case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:
      return "cocpu_trap";
    case ESP_SLEEP_WAKEUP_BT:
      return "bt";
    case ESP_SLEEP_WAKEUP_VAD:
      return "vad";
    case ESP_SLEEP_WAKEUP_VBAT_UNDER_VOLT:
      return "vbat_under_volt";
    case LIGHT_SLEEP_WAKE_CAUSE_UNKNOWN_INDEX:
      return "unknown";
    default:
      return "invalid";
  }
}

const char* HalPowerManager::lightSleepRequestBucketName(uint8_t bucketIndex) {
  switch (bucketIndex) {
    case 0:
      return "<1ms";
    case 1:
      return "1-5ms";
    case 2:
      return "5-20ms";
    case 3:
      return "20-100ms";
    case 4:
      return "100-500ms";
    case 5:
      return "500ms+";
    default:
      return "invalid";
  }
}

void HalPowerManager::logLightSleepDiagnostics(const char* reason) const {
  const auto stats = getLightSleepStats();
  const std::string btPower = HalBluetoothManager::getInstance().formatPowerState();
  LOG_INF("PWR", "Light sleep stats reason=%s auto=%d count=%lu slept_ms=%llu requested_ms=%llu early=%llu",
          reason ? reason : "",
          autoLightSleepConfigured, static_cast<unsigned long>(stats.enterCount),
          static_cast<unsigned long long>(stats.sleptUs / 1000ULL),
          static_cast<unsigned long long>(stats.requestedUs / 1000ULL),
          static_cast<unsigned long long>(stats.earlyWakeCount));
  for (uint8_t i = 0; i < LIGHT_SLEEP_WAKE_CAUSE_COUNT; i++) {
    if (stats.wakeCauseCounts[i] == 0) {
      continue;
    }
    LOG_INF("PWR", "Light sleep wake total cause=%s(%u) count=%llu", lightSleepWakeCauseName(i),
            static_cast<unsigned>(i), static_cast<unsigned long long>(stats.wakeCauseCounts[i]));
  }
  for (uint8_t i = 0; i < LIGHT_SLEEP_REQUEST_BUCKET_COUNT; i++) {
    if (stats.requestBucketCounts[i] == 0) {
      continue;
    }
    LOG_INF("PWR", "Light sleep request total bucket=%s count=%llu", lightSleepRequestBucketName(i),
            static_cast<unsigned long long>(stats.requestBucketCounts[i]));
  }
  LOG_INF("PWR", "%s", btPower.c_str());
  logTaskRuntimeDiagnostics();
  logPmLockDiagnostics();
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

  // Pre-sleep routines from the original firmware
  // GPIO13 is connected to battery latch MOSFET, we need to make sure it's low during sleep
  // Note that this means the MCU will be completely powered off during sleep, including RTC
  gpio_hold_dis(X3_BATTERY_LATCH_PIN);
  gpio_set_direction(X3_BATTERY_LATCH_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(X3_BATTERY_LATCH_PIN, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(X3_BATTERY_LATCH_PIN);
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  // Arm the wakeup trigger *after* the button is released
  // Note: this is only useful for waking up on USB power. On battery, the MCU will be completely powered off, so the
  // power button is hard-wired to briefly provide power to the MCU, waking it up regardless of the wakeup source
  // configuration
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  if (_batteryUseI2C) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    // Read SOC directly from I2C fuel gauge (16-bit LE register).
    // On I2C error, keep last known value to avoid UI jitter/slowdowns.
    PeripheralLock peripheralLock;
    Wire.beginTransmission(I2C_ADDR_BQ27220);
    Wire.write(BQ27220_SOC_REG);
    if (Wire.endTransmission(false) != 0) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    Wire.requestFrom(I2C_ADDR_BQ27220, (uint8_t)2);
    if (Wire.available() < 2) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    const uint16_t soc = (hi << 8) | lo;
    _batteryCachedPercent = soc > 100 ? 100 : soc;
    _batteryLastPollMs = now;
    return _batteryCachedPercent;
  }
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
    if (powerManager.autoLightSleepConfigured && powerManager.ensurePmLocks()) {
      if (!acquirePmLock(powerManager.cpuMaxLock, cpuLockAcquired, "CPU max") ||
          !acquirePmLock(powerManager.apbMaxLock, apbLockAcquired, "APB max") ||
          !acquirePmLock(powerManager.noLightSleepLock, noLightSleepLockAcquired, "no-light-sleep")) {
        releasePmLock(powerManager.noLightSleepLock, noLightSleepLockAcquired, "no-light-sleep");
        releasePmLock(powerManager.apbMaxLock, apbLockAcquired, "APB max");
        releasePmLock(powerManager.cpuMaxLock, cpuLockAcquired, "CPU max");
        powerManager.currentLockMode = None;
        valid = false;
      }
    }
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    releasePmLock(powerManager.noLightSleepLock, noLightSleepLockAcquired, "no-light-sleep");
    releasePmLock(powerManager.apbMaxLock, apbLockAcquired, "APB max");
    releasePmLock(powerManager.cpuMaxLock, cpuLockAcquired, "CPU max");
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}

HalPowerManager::PeripheralLock::PeripheralLock() {
  if (!powerManager.modeMutex || !powerManager.autoLightSleepConfigured) {
    return;
  }

  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (powerManager.ensurePmLocks()) {
    if (!acquirePmLock(powerManager.apbMaxLock, apbLockAcquired, "APB max") ||
        !acquirePmLock(powerManager.noLightSleepLock, noLightSleepLockAcquired, "no-light-sleep")) {
      releasePmLock(powerManager.noLightSleepLock, noLightSleepLockAcquired, "no-light-sleep");
      releasePmLock(powerManager.apbMaxLock, apbLockAcquired, "APB max");
    }
  }
  xSemaphoreGive(powerManager.modeMutex);
}

HalPowerManager::PeripheralLock::~PeripheralLock() {
  if (!powerManager.modeMutex) {
    return;
  }

  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  releasePmLock(powerManager.noLightSleepLock, noLightSleepLockAcquired, "no-light-sleep");
  releasePmLock(powerManager.apbMaxLock, apbLockAcquired, "APB max");
  xSemaphoreGive(powerManager.modeMutex);
}
