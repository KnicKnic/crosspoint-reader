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

  static CompanionBleService& getInstance();

  bool begin();
  void end();
  bool isRunning() const { return running; }
  bool isHostConnected() const { return hostConnected; }
  bool isConnectionHandshakeActive() const;
  void update();
  bool restartAdvertising(const char* reason);
  std::string getStatusText() const;
  HostStatus getHostStatus() const;
  bool consumeStatusChanged();
  bool notifyToggleMuteReleased();
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
  uint16_t hostConnHandle = 0xFFFF;
  unsigned long hostConnectedAtMs = 0;
  unsigned long lastMaintenanceAtMs = 0;
  unsigned long lastStateLogAtMs = 0;
  unsigned long lastAdvertisingRestartAtMs = 0;
  unsigned long pendingAdvertisingRestartAtMs = 0;
  unsigned long lastConnParamRequestAtMs = 0;
  unsigned long responsiveUntilMs = 0;
  bool advertisingRestartPending = false;
  const char* pendingAdvertisingRestartReason = nullptr;
  uint16_t buttonEventSequence = 0;
  HostStatus hostStatus;
  std::function<void()> statusChangedCallback;
};
