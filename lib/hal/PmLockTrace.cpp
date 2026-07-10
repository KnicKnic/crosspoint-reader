#include "PmLockTrace.h"

#include <Logging.h>
#include <esp_cpu.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
esp_err_t __real_esp_pm_lock_create(esp_pm_lock_type_t lock_type, int arg, const char* name,
                                    esp_pm_lock_handle_t* out_handle);
esp_err_t __real_esp_pm_lock_acquire(esp_pm_lock_handle_t handle);
esp_err_t __real_esp_pm_lock_release(esp_pm_lock_handle_t handle);
esp_err_t __real_esp_pm_lock_delete(esp_pm_lock_handle_t handle);
}

namespace {

constexpr size_t MAX_TRACED_LOCKS = 8;
constexpr uint64_t MIN_RELEASE_LOG_US = 30ULL * 1000ULL;
constexpr size_t HOLD_BUCKET_COUNT = 5;

struct LockMeta {
  esp_pm_lock_handle_t handle = nullptr;
  int depth = 0;
  int64_t acquiredAtUs = 0;
  uint64_t sleepGapBeforeAcquireUs = 0;
  uint64_t acquirePeriodUs = 0;
  uintptr_t acquireCallAddr = 0;
  char acquireTaskName[16] = "";
  bool used = false;
};

struct BtLockPrintEvent {
  uint64_t atUs = 0;
  uint64_t startUs = 0;
  uint64_t heldUs = 0;
  uint64_t sleepGapBeforeAcquireUs = 0;
  uint64_t acquirePeriodUs = 0;
  uint64_t releasePeriodUs = 0;
  uint32_t connectionIntervalUs = 0;
  uint16_t connectionLatency = 0;
  uint16_t estimatedEvents = 0;
  uint64_t sequence = 0;
  const char* op = "?";
  uintptr_t acquireCallAddr = 0;
  uintptr_t releaseCallAddr = 0;
  char acquireTaskName[16] = "";
  char releaseTaskName[16] = "";
  int depth = 0;
  esp_err_t err = ESP_OK;
  bool valid = false;
};

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
LockMeta locks[MAX_TRACED_LOCKS];
uint64_t lastAcquireAtUs = 0;
uint64_t lastReleaseAtUs = 0;
uint64_t longHoldCount = 0;
uint64_t longHoldTotalUs = 0;
uint64_t longHoldMaxUs = 0;
uint64_t intervalLongHoldCount = 0;
uint64_t intervalLongHoldTotalUs = 0;
uint64_t intervalLongHoldMaxUs = 0;
uint64_t intervalHoldBuckets[HOLD_BUCKET_COUNT] = {};
uint64_t lastLongHoldUs = 0;
uint64_t lastLongSleepGapUs = 0;
uint64_t lastLongAcquirePeriodUs = 0;
uint64_t lastLongReleasePeriodUs = 0;
uintptr_t lastLongAcquireCallAddr = 0;
uintptr_t lastLongReleaseCallAddr = 0;
char lastLongAcquireTaskName[16] = "";
char lastLongReleaseTaskName[16] = "";
uint32_t negotiatedConnectionIntervalUs = 0;
uint16_t negotiatedConnectionLatency = 0;

bool isBtApbLock(esp_pm_lock_type_t lockType, const char* name) {
  return lockType == ESP_PM_APB_FREQ_MAX && name && strcmp(name, "bt") == 0;
}

LockMeta* findLock(esp_pm_lock_handle_t handle) {
  for (auto& lock : locks) {
    if (lock.used && lock.handle == handle) {
      return &lock;
    }
  }
  return nullptr;
}

void trackLock(esp_pm_lock_handle_t handle) {
  if (!handle) {
    return;
  }

  portENTER_CRITICAL(&mux);
  LockMeta* existing = findLock(handle);
  if (existing) {
    existing->depth = 0;
    existing->acquiredAtUs = 0;
    portEXIT_CRITICAL(&mux);
    return;
  }

  for (auto& lock : locks) {
    if (!lock.used) {
      lock.handle = handle;
      lock.depth = 0;
      lock.acquiredAtUs = 0;
      lock.used = true;
      break;
    }
  }
  portEXIT_CRITICAL(&mux);
}

void untrackLock(esp_pm_lock_handle_t handle) {
  portENTER_CRITICAL(&mux);
  LockMeta* lock = findLock(handle);
  if (lock) {
    *lock = LockMeta{};
  }
  portEXIT_CRITICAL(&mux);
}

void formatUsAsMs(uint64_t us, char* buffer, size_t bufferSize) {
  snprintf(buffer, bufferSize, "%llu.%03llums", static_cast<unsigned long long>(us / 1000ULL),
           static_cast<unsigned long long>(us % 1000ULL));
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

  snprintf(buffer, bufferSize, "%llum", static_cast<unsigned long long>(totalSeconds / 60ULL));
}

uint8_t holdBucketIndex(uint64_t heldUs) {
  if (heldUs < 50ULL * 1000ULL) {
    return 0;
  }
  if (heldUs < 100ULL * 1000ULL) {
    return 1;
  }
  if (heldUs < 200ULL * 1000ULL) {
    return 2;
  }
  if (heldUs < 500ULL * 1000ULL) {
    return 3;
  }
  return 4;
}

uint16_t estimateConnectionEvents(uint64_t periodUs, uint32_t intervalUs) {
  if (periodUs == 0 || intervalUs == 0) {
    return 0;
  }

  const uint64_t roundedEvents = (periodUs + (intervalUs / 2ULL)) / intervalUs;
  return roundedEvents > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(roundedEvents);
}

void copyCurrentTaskName(char* buffer, size_t bufferSize) {
  if (!buffer || bufferSize == 0) {
    return;
  }

  const char* taskName = pcTaskGetName(xTaskGetCurrentTaskHandle());
  if (!taskName) {
    taskName = "?";
  }
  strncpy(buffer, taskName, bufferSize - 1);
  buffer[bufferSize - 1] = '\0';
}

void printBtLockEvent(const BtLockPrintEvent& event) {
#ifdef ENABLE_SERIAL_LOG
  if (!event.valid || !isSerialLogOutputEnabled() || !logSerial) {
    return;
  }

  char at[24];
  char start[24];
  char held[24];
  formatUsAsMs(event.atUs, at, sizeof(at));
  formatUsAsMs(event.startUs, start, sizeof(start));
  formatUsAsMs(event.heldUs, held, sizeof(held));
  char sleepGap[24];
  char acquirePeriod[24];
  char releasePeriod[24];
  formatUsAsMs(event.sleepGapBeforeAcquireUs, sleepGap, sizeof(sleepGap));
  formatUsAsMs(event.acquirePeriodUs, acquirePeriod, sizeof(acquirePeriod));
  formatUsAsMs(event.releasePeriodUs, releasePeriod, sizeof(releasePeriod));

  char line[320];
  const int len = snprintf(line, sizeof(line),
                           "[%lu] [DBG] [BTL] %s#%llu at=%s start=%s held=%s sleepGap=%s acqPer=%s relPer=%s ev=%u lat=%u/%u depth=%d err=%d task=%s/%s pc=0x%08lx/0x%08lx\n",
                           static_cast<unsigned long>(event.atUs / 1000ULL), event.op,
                           static_cast<unsigned long long>(event.sequence), at, start, held, sleepGap, acquirePeriod,
                           releasePeriod, static_cast<unsigned>(event.estimatedEvents),
                           static_cast<unsigned>(event.estimatedEvents > 0 ? event.estimatedEvents - 1 : 0),
                           static_cast<unsigned>(event.connectionLatency), event.depth, static_cast<int>(event.err),
                           event.acquireTaskName, event.releaseTaskName,
                           static_cast<unsigned long>(event.acquireCallAddr),
                           static_cast<unsigned long>(event.releaseCallAddr));
  if (len > 0) {
    const size_t writeLen = len < static_cast<int>(sizeof(line)) ? static_cast<size_t>(len) : sizeof(line) - 1;
    logSerial.write(reinterpret_cast<const uint8_t*>(line), writeLen);
  }
#else
  (void)event;
#endif
}

void recordAcquire(esp_pm_lock_handle_t handle, esp_err_t err) {
  const int64_t nowUs = esp_timer_get_time();
  const uintptr_t callAddr =
      static_cast<uintptr_t>(esp_cpu_get_call_addr(reinterpret_cast<intptr_t>(__builtin_return_address(0))));
  char taskName[16];
  copyCurrentTaskName(taskName, sizeof(taskName));

  portENTER_CRITICAL(&mux);
  LockMeta* lock = findLock(handle);
  if (!lock) {
    portEXIT_CRITICAL(&mux);
    return;
  }

  if (err == ESP_OK) {
    if (lock->depth == 0) {
      lock->acquiredAtUs = nowUs;
      lock->sleepGapBeforeAcquireUs =
          lastReleaseAtUs > 0 && nowUs >= static_cast<int64_t>(lastReleaseAtUs)
              ? static_cast<uint64_t>(nowUs) - lastReleaseAtUs
              : 0;
      lock->acquirePeriodUs =
          lastAcquireAtUs > 0 && nowUs >= static_cast<int64_t>(lastAcquireAtUs)
              ? static_cast<uint64_t>(nowUs) - lastAcquireAtUs
              : 0;
      lastAcquireAtUs = static_cast<uint64_t>(nowUs);
      lock->acquireCallAddr = callAddr;
      strncpy(lock->acquireTaskName, taskName, sizeof(lock->acquireTaskName) - 1);
      lock->acquireTaskName[sizeof(lock->acquireTaskName) - 1] = '\0';
    }
    lock->depth++;
  }
  portEXIT_CRITICAL(&mux);
}

BtLockPrintEvent recordRelease(esp_pm_lock_handle_t handle, esp_err_t err) {
  const int64_t nowUs = esp_timer_get_time();
  const uintptr_t callAddr =
      static_cast<uintptr_t>(esp_cpu_get_call_addr(reinterpret_cast<intptr_t>(__builtin_return_address(0))));
  char taskName[16];
  copyCurrentTaskName(taskName, sizeof(taskName));
  BtLockPrintEvent event;

  portENTER_CRITICAL(&mux);
  LockMeta* lock = findLock(handle);
  if (!lock) {
    portEXIT_CRITICAL(&mux);
    return event;
  }

  uint64_t heldUs = 0;
  uint64_t startUs = 0;
  uint64_t sleepGapBeforeAcquireUs = 0;
  uint64_t acquirePeriodUs = 0;
  uint64_t releasePeriodUs = 0;
  bool finalRelease = false;
  if (err == ESP_OK && lock->depth > 0) {
    if (lock->acquiredAtUs > 0 && nowUs >= lock->acquiredAtUs) {
      startUs = static_cast<uint64_t>(lock->acquiredAtUs);
      heldUs = static_cast<uint64_t>(nowUs - lock->acquiredAtUs);
    }
    sleepGapBeforeAcquireUs = lock->sleepGapBeforeAcquireUs;
    acquirePeriodUs = lock->acquirePeriodUs;
    releasePeriodUs = lastReleaseAtUs > 0 && nowUs >= static_cast<int64_t>(lastReleaseAtUs)
                          ? static_cast<uint64_t>(nowUs) - lastReleaseAtUs
                          : 0;
    lock->depth--;
    if (lock->depth == 0) {
      lock->acquiredAtUs = 0;
      finalRelease = true;
    }
  }

  event.atUs = nowUs > 0 ? static_cast<uint64_t>(nowUs) : 0;
  event.startUs = startUs != 0 ? startUs : event.atUs;
  event.heldUs = heldUs;
  event.sleepGapBeforeAcquireUs = sleepGapBeforeAcquireUs;
  event.acquirePeriodUs = acquirePeriodUs;
  event.releasePeriodUs = releasePeriodUs;
  event.connectionIntervalUs = negotiatedConnectionIntervalUs;
  event.connectionLatency = negotiatedConnectionLatency;
  event.estimatedEvents = estimateConnectionEvents(acquirePeriodUs, negotiatedConnectionIntervalUs);
  event.op = "rel";
  event.acquireCallAddr = lock->acquireCallAddr;
  event.releaseCallAddr = callAddr;
  strncpy(event.acquireTaskName, lock->acquireTaskName, sizeof(event.acquireTaskName) - 1);
  event.acquireTaskName[sizeof(event.acquireTaskName) - 1] = '\0';
  strncpy(event.releaseTaskName, taskName, sizeof(event.releaseTaskName) - 1);
  event.releaseTaskName[sizeof(event.releaseTaskName) - 1] = '\0';
  event.depth = lock->depth;
  event.err = err;
  event.valid = finalRelease && heldUs > MIN_RELEASE_LOG_US;
  if (event.valid) {
    longHoldCount++;
    longHoldTotalUs += heldUs;
    if (heldUs > longHoldMaxUs) {
      longHoldMaxUs = heldUs;
    }
    intervalLongHoldCount++;
    intervalLongHoldTotalUs += heldUs;
    if (heldUs > intervalLongHoldMaxUs) {
      intervalLongHoldMaxUs = heldUs;
    }
    intervalHoldBuckets[holdBucketIndex(heldUs)]++;
    lastLongHoldUs = heldUs;
    lastLongSleepGapUs = sleepGapBeforeAcquireUs;
    lastLongAcquirePeriodUs = acquirePeriodUs;
    lastLongReleasePeriodUs = releasePeriodUs;
    lastLongAcquireCallAddr = event.acquireCallAddr;
    lastLongReleaseCallAddr = event.releaseCallAddr;
    strncpy(lastLongAcquireTaskName, event.acquireTaskName, sizeof(lastLongAcquireTaskName) - 1);
    lastLongAcquireTaskName[sizeof(lastLongAcquireTaskName) - 1] = '\0';
    strncpy(lastLongReleaseTaskName, event.releaseTaskName, sizeof(lastLongReleaseTaskName) - 1);
    lastLongReleaseTaskName[sizeof(lastLongReleaseTaskName) - 1] = '\0';
    event.sequence = longHoldCount;
  }
  if (finalRelease) {
    lastReleaseAtUs = event.atUs;
  }
  portEXIT_CRITICAL(&mux);

  return event;
}

}  // namespace

void setBtLockTraceConnectionParams(uint16_t intervalUnits, uint16_t latency) {
  const uint32_t intervalUs = static_cast<uint32_t>(intervalUnits) * 1250UL;
  portENTER_CRITICAL(&mux);
  negotiatedConnectionIntervalUs = intervalUs;
  negotiatedConnectionLatency = latency;
  portEXIT_CRITICAL(&mux);
}

std::string formatBtLockTraceDiagnostics() {
#ifdef ENABLE_SERIAL_LOG
  uint64_t count = 0;
  uint64_t totalUs = 0;
  uint64_t maxUs = 0;
  uint64_t buckets[HOLD_BUCKET_COUNT] = {};
  uint64_t lastUs = 0;
  uint64_t sleepGapUs = 0;
  uint64_t acquirePeriodUs = 0;
  uint64_t releasePeriodUs = 0;
  uintptr_t acquirePc = 0;
  uintptr_t releasePc = 0;
  uint32_t intervalUs = 0;
  uint16_t latency = 0;
  char acquireTask[16] = "";
  char releaseTask[16] = "";

  portENTER_CRITICAL(&mux);
  count = intervalLongHoldCount;
  totalUs = intervalLongHoldTotalUs;
  maxUs = intervalLongHoldMaxUs;
  for (size_t i = 0; i < HOLD_BUCKET_COUNT; i++) {
    buckets[i] = intervalHoldBuckets[i];
    intervalHoldBuckets[i] = 0;
  }
  intervalLongHoldCount = 0;
  intervalLongHoldTotalUs = 0;
  intervalLongHoldMaxUs = 0;
  lastUs = lastLongHoldUs;
  sleepGapUs = lastLongSleepGapUs;
  acquirePeriodUs = lastLongAcquirePeriodUs;
  releasePeriodUs = lastLongReleasePeriodUs;
  acquirePc = lastLongAcquireCallAddr;
  releasePc = lastLongReleaseCallAddr;
  intervalUs = negotiatedConnectionIntervalUs;
  latency = negotiatedConnectionLatency;
  strncpy(acquireTask, lastLongAcquireTaskName, sizeof(acquireTask) - 1);
  acquireTask[sizeof(acquireTask) - 1] = '\0';
  strncpy(releaseTask, lastLongReleaseTaskName, sizeof(releaseTask) - 1);
  releaseTask[sizeof(releaseTask) - 1] = '\0';
  portEXIT_CRITICAL(&mux);

  if (count == 0) {
    return "BTLD: no >30ms bt holds";
  }

  char total[16];
  char average[16];
  char max[16];
  char last[16];
  char sleepGap[16];
  char acquirePeriod[16];
  char releasePeriod[16];
  formatDurationUs(totalUs, total, sizeof(total));
  formatDurationUs(totalUs / count, average, sizeof(average));
  formatDurationUs(maxUs, max, sizeof(max));
  formatDurationUs(lastUs, last, sizeof(last));
  formatDurationUs(sleepGapUs, sleepGap, sizeof(sleepGap));
  formatDurationUs(acquirePeriodUs, acquirePeriod, sizeof(acquirePeriod));
  formatDurationUs(releasePeriodUs, releasePeriod, sizeof(releasePeriod));
  const uint16_t estimatedEvents = estimateConnectionEvents(acquirePeriodUs, intervalUs);

  char line[240];
  snprintf(line, sizeof(line),
           "BTLD: +%llu %s avg%s max%s last%s gap%s acq%s rel%s ev%u lat%u/%u b%llu/%llu/%llu/%llu/%llu task=%s/%s pc=0x%08lx/0x%08lx",
           static_cast<unsigned long long>(count), total, average, max, last, sleepGap, acquirePeriod, releasePeriod,
           static_cast<unsigned>(estimatedEvents),
           static_cast<unsigned>(estimatedEvents > 0 ? estimatedEvents - 1 : 0), static_cast<unsigned>(latency),
           static_cast<unsigned long long>(buckets[0]), static_cast<unsigned long long>(buckets[1]),
           static_cast<unsigned long long>(buckets[2]), static_cast<unsigned long long>(buckets[3]),
           static_cast<unsigned long long>(buckets[4]), acquireTask, releaseTask,
           static_cast<unsigned long>(acquirePc), static_cast<unsigned long>(releasePc));
  return line;
#else
  return "BTLD: serial log disabled";
#endif
}

extern "C" esp_err_t __wrap_esp_pm_lock_create(esp_pm_lock_type_t lock_type, int arg, const char* name,
                                                esp_pm_lock_handle_t* out_handle) {
  const esp_err_t err = __real_esp_pm_lock_create(lock_type, arg, name, out_handle);
  if (err == ESP_OK && out_handle && isBtApbLock(lock_type, name)) {
    trackLock(*out_handle);
  }
  return err;
}

extern "C" esp_err_t __wrap_esp_pm_lock_acquire(esp_pm_lock_handle_t handle) {
  const esp_err_t err = __real_esp_pm_lock_acquire(handle);
  recordAcquire(handle, err);
  return err;
}

extern "C" esp_err_t __wrap_esp_pm_lock_release(esp_pm_lock_handle_t handle) {
  const esp_err_t err = __real_esp_pm_lock_release(handle);
  printBtLockEvent(recordRelease(handle, err));
  return err;
}

extern "C" esp_err_t __wrap_esp_pm_lock_delete(esp_pm_lock_handle_t handle) {
  untrackLock(handle);
  return __real_esp_pm_lock_delete(handle);
}
