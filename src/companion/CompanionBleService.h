#pragma once

#include <cstdint>
#include <functional>
#include <string>

class NimBLECharacteristic;
class NimBLEServer;

class CompanionBleService {
 public:
  enum class ConnectionPowerProfile : uint8_t {
    Unknown,
    Responsive,
    Idle,
  };

  struct HostStatus {
    bool teamsDetected = false;
    uint8_t microphone = 0;
    uint8_t camera = 0;
    std::string message;
  };

  struct ActivityStats {
    uint32_t updateCalls = 0;
    uint32_t maintenanceRuns = 0;
    uint32_t gapConnects = 0;
    uint32_t gapDisconnects = 0;
    uint32_t connParamRequests = 0;
    uint32_t connParamUpdates = 0;
    uint32_t hostWrites = 0;
    uint32_t hostStateChanges = 0;
    uint32_t buttonSubscribes = 0;
    uint32_t buttonNotifications = 0;
    uint32_t advertisingRestarts = 0;
    uint32_t stateLogs = 0;
    uint32_t statusNotifications = 0;
  };

  static CompanionBleService& getInstance();

  bool begin();
  void end();
  bool isRunning() const { return running; }
  bool isHostConnected() const { return hostConnected; }
  bool isAdvertising() const;
  bool isConnectionHandshakeActive() const;
  unsigned long getNextUpdateDelayMs(unsigned long now) const;
  void update();
  bool restartAdvertising(const char* reason);
  std::string getStatusText() const;
  HostStatus getHostStatus() const;
  bool consumeStatusChanged();
  bool notifyToggleMuteReleased();
  std::string formatTimingDiagnostics() const;
  std::string formatActivityDeltaDiagnostics();
  void setStatusChangedCallback(std::function<void()> callback);

  void onHostConnected();
  void onHostConnected(uint16_t connHandle);
  void onHostDisconnected();
  void onConnParamsUpdated(uint16_t interval, uint16_t latency, uint16_t timeout);
  void onHostTeamsStateWritten(NimBLECharacteristic* characteristic);
  void onHostMicrophoneStateWritten(NimBLECharacteristic* characteristic);
  void onHostCameraStateWritten(NimBLECharacteristic* characteristic);
  void onHostStatusMessageWritten(NimBLECharacteristic* characteristic);
  void onButtonEventSubscribed(bool subscribed);
  void scheduleAdvertisingRestart(const char* reason, unsigned long delayMs);

 private:
  CompanionBleService() = default;

  void disconnectConnectedHosts();
  bool hasConnectedHosts() const;
  void resetSessionState();
  void requestConnectionParams(ConnectionPowerProfile profile, const char* reason);
  void requestIdleConnectionParamsIfReady(const char* reason);
  void publishHostStateValues();
  void notifyStatusChanged();
  void publishDeviceInfo();
  void publishButtonEvent(uint8_t buttonId, uint8_t action);
  void logStateSnapshot(const char* reason);

  NimBLEServer* server = nullptr;
  NimBLECharacteristic* hostTeamsStateCharacteristic = nullptr;
  NimBLECharacteristic* hostMicrophoneStateCharacteristic = nullptr;
  NimBLECharacteristic* hostCameraStateCharacteristic = nullptr;
  NimBLECharacteristic* hostStatusMessageCharacteristic = nullptr;
  NimBLECharacteristic* buttonEventCharacteristic = nullptr;
  NimBLECharacteristic* deviceInfoCharacteristic = nullptr;
  bool running = false;
  bool hostConnected = false;
  bool hostStateReceived = false;
  bool buttonEventSubscribed = false;
  bool ownsBluetoothStack = false;
  bool statusChanged = false;
  ConnectionPowerProfile connectionProfile = ConnectionPowerProfile::Unknown;
  ConnectionPowerProfile requestedConnectionProfile = ConnectionPowerProfile::Unknown;
  uint16_t hostConnHandle = 0xFFFF;
  uint16_t requestedConnIntervalMin = 0;
  uint16_t requestedConnIntervalMax = 0;
  uint16_t requestedConnLatency = 0;
  uint16_t requestedConnTimeout = 0;
  uint16_t negotiatedConnInterval = 0;
  uint16_t negotiatedConnLatency = 0;
  uint16_t negotiatedConnTimeout = 0;
  unsigned long hostConnectedAtMs = 0;
  unsigned long lastMaintenanceAtMs = 0;
  unsigned long lastStateLogAtMs = 0;
  unsigned long lastAdvertisingRestartAtMs = 0;
  unsigned long pendingAdvertisingRestartAtMs = 0;
  unsigned long lastConnParamRequestAtMs = 0;
  unsigned long responsiveUntilMs = 0;
  bool advertisingRestartPending = false;
  bool hasNegotiatedConnParams = false;
  const char* pendingAdvertisingRestartReason = nullptr;
  uint16_t buttonEventSequence = 0;
  HostStatus hostStatus;
  ActivityStats activityStats;
  ActivityStats previousActivityStats;
  bool hasPreviousActivityStats = false;
  std::function<void()> statusChangedCallback;
};
