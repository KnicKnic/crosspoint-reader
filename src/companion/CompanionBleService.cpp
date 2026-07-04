#include "CompanionBleService.h"

#include <Arduino.h>
#include <BluetoothDiagnostics.h>
#include <HalBluetoothManager.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <NimBLEUtils.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "CompanionProtocol.h"

namespace {
constexpr unsigned long COMPANION_MAINTENANCE_INTERVAL_MS = 2000;
constexpr unsigned long COMPANION_STATE_LOG_INTERVAL_MS = 15000;
constexpr unsigned long COMPANION_HANDSHAKE_TIMEOUT_MS = 15000;
constexpr unsigned long COMPANION_ADVERTISING_RESTART_INTERVAL_MS = 5000;
constexpr unsigned long COMPANION_DISCONNECT_ADVERTISING_RESTART_DELAY_MS = 250;
constexpr unsigned long COMPANION_CONN_PARAM_REQUEST_MIN_INTERVAL_MS = 2500;
constexpr unsigned long COMPANION_BUTTON_RESPONSIVE_WINDOW_MS = 5000;
constexpr size_t COMPANION_STATUS_MESSAGE_MAX_LEN = 48;

constexpr uint16_t BLE_CONN_INTERVAL_RESPONSIVE_MIN = 24;  // 30 ms
constexpr uint16_t BLE_CONN_INTERVAL_RESPONSIVE_MAX = 40;  // 50 ms
constexpr uint16_t BLE_CONN_LATENCY_RESPONSIVE = 0;
constexpr uint16_t BLE_CONN_TIMEOUT_RESPONSIVE = 400;      // 4 s

constexpr uint16_t BLE_CONN_INTERVAL_IDLE_MIN = 240;       // 300 ms
constexpr uint16_t BLE_CONN_INTERVAL_IDLE_MAX = 400;       // 500 ms
constexpr uint16_t BLE_CONN_LATENCY_IDLE = 2;
constexpr uint16_t BLE_CONN_TIMEOUT_IDLE = 1000;           // 10 s

enum class HostStateField : uint8_t {
  Teams,
  Microphone,
  Camera,
  Message,
};

const char* hostStateFieldName(HostStateField field) {
  switch (field) {
    case HostStateField::Teams:
      return "teams";
    case HostStateField::Microphone:
      return "microphone";
    case HostStateField::Camera:
      return "camera";
    case HostStateField::Message:
      return "message";
  }
  return "unknown";
}

const char* profileName(CompanionBleService::ConnectionPowerProfile profile) {
  switch (profile) {
    case CompanionBleService::ConnectionPowerProfile::Responsive:
      return "responsive";
    case CompanionBleService::ConnectionPowerProfile::Idle:
      return "idle";
    default:
      return "unknown";
  }
}

CompanionBleService* g_service = nullptr;

class CompanionServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    (void)pServer;
    LOG_INF("COMP", "Host connected address=%s handle=%u", connInfo.getAddress().toString().c_str(),
            static_cast<unsigned>(connInfo.getConnHandle()));
    BluetoothDiagnostics::recordf("companion_host_gap_connected", "addr=%s handle=%u",
                                  connInfo.getAddress().toString().c_str(),
                                  static_cast<unsigned>(connInfo.getConnHandle()));
    if (g_service) {
      g_service->onHostConnected(connInfo.getConnHandle());
    }
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    LOG_INF("COMP", "Host disconnected address=%s handle=%u reason=%d %s", connInfo.getAddress().toString().c_str(),
            static_cast<unsigned>(connInfo.getConnHandle()), reason, NimBLEUtils::returnCodeToString(reason));
    BluetoothDiagnostics::recordf("companion_host_gap_disconnected", "addr=%s handle=%u reason=%d",
                                  connInfo.getAddress().toString().c_str(),
                                  static_cast<unsigned>(connInfo.getConnHandle()), reason);
    if (g_service) {
      g_service->onHostDisconnected();
      g_service->scheduleAdvertisingRestart("disconnect", COMPANION_DISCONNECT_ADVERTISING_RESTART_DELAY_MS);
    }
  }

  void onConnParamsUpdate(NimBLEConnInfo& connInfo) override {
    LOG_INF("COMP", "Host connection params interval=%.1f ms latency=%u timeout=%u ms",
            connInfo.getConnInterval() * 1.25f, static_cast<unsigned>(connInfo.getConnLatency()),
            static_cast<unsigned>(connInfo.getConnTimeout() * 10));
    BluetoothDiagnostics::recordf("companion_conn_params_updated", "itvl=%u latency=%u timeout=%u",
                                  static_cast<unsigned>(connInfo.getConnInterval()),
                                  static_cast<unsigned>(connInfo.getConnLatency()),
                                  static_cast<unsigned>(connInfo.getConnTimeout()));
    if (g_service) {
      g_service->onConnParamsUpdated(connInfo.getConnInterval(), connInfo.getConnLatency(),
                                     connInfo.getConnTimeout());
    }
  }
};

class HostStateCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit HostStateCallbacks(HostStateField field) : field(field) {}

 private:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    const size_t len = characteristic ? characteristic->getValue().size() : 0;
    BluetoothDiagnostics::recordf("companion_host_state_write", "field=%s handle=%u len=%u",
                                  hostStateFieldName(field), static_cast<unsigned>(connInfo.getConnHandle()),
                                  static_cast<unsigned>(len));
    if (!g_service) {
      return;
    }

    switch (field) {
      case HostStateField::Teams:
        g_service->onHostTeamsStateWritten(characteristic);
        break;
      case HostStateField::Microphone:
        g_service->onHostMicrophoneStateWritten(characteristic);
        break;
      case HostStateField::Camera:
        g_service->onHostCameraStateWritten(characteristic);
        break;
      case HostStateField::Message:
        g_service->onHostStatusMessageWritten(characteristic);
        break;
    }
  }

  HostStateField field;
};

class ButtonEventCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    (void)characteristic;
    LOG_INF("COMP", "Button event subscription changed address=%s handle=%u sub=%u",
            connInfo.getAddress().toString().c_str(), static_cast<unsigned>(connInfo.getConnHandle()),
            static_cast<unsigned>(subValue));
    BluetoothDiagnostics::recordf("companion_button_event_subscribe", "handle=%u sub=%u",
                                  static_cast<unsigned>(connInfo.getConnHandle()),
                                  static_cast<unsigned>(subValue));
    if (g_service) {
      g_service->onButtonEventSubscribed(subValue != 0);
    }
  }
};

CompanionServerCallbacks serverCallbacks;
HostStateCallbacks teamsStateCallbacks(HostStateField::Teams);
HostStateCallbacks microphoneStateCallbacks(HostStateField::Microphone);
HostStateCallbacks cameraStateCallbacks(HostStateField::Camera);
HostStateCallbacks statusMessageCallbacks(HostStateField::Message);
ButtonEventCallbacks buttonEventCallbacks;
}  // namespace

CompanionBleService& CompanionBleService::getInstance() {
  static CompanionBleService instance;
  return instance;
}

bool CompanionBleService::begin() {
  if (running) {
    LOG_DBG("COMP", "Companion BLE already running");
    return true;
  }

  resetSessionState();

  auto& btMgr = HalBluetoothManager::getInstance();
  ownsBluetoothStack = !btMgr.isEnabled();
  LOG_INF("COMP", "Starting companion BLE peripheral/GATT server; owns stack=%d", ownsBluetoothStack);
  if (!btMgr.enable("X3 Companion")) {
    LOG_ERR("COMP", "Bluetooth enable failed: %s", btMgr.getLastError().c_str());
    BluetoothDiagnostics::recordf("companion_ble_enable_failed", "msg=%s", btMgr.getLastError().c_str());
    return false;
  }

  g_service = this;
  if (!server) {
    server = NimBLEDevice::createServer();
    if (!server) {
      LOG_ERR("COMP", "Failed to create companion BLE server");
      BluetoothDiagnostics::record("companion_server_create_failed");
      if (ownsBluetoothStack) {
        btMgr.disable();
      }
      ownsBluetoothStack = false;
      return false;
    }

    server->setCallbacks(&serverCallbacks, false);
    auto* service = server->createService(CompanionProtocol::SERVICE_UUID);
    if (!service) {
      LOG_ERR("COMP", "Failed to create companion GATT service");
      BluetoothDiagnostics::record("companion_service_create_failed");
      if (ownsBluetoothStack) {
        btMgr.disable();
      }
      ownsBluetoothStack = false;
      server = nullptr;
      return false;
    }

    constexpr uint32_t stateProperties = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR;
    hostTeamsStateCharacteristic =
        service->createCharacteristic(CompanionProtocol::HOST_TEAMS_STATE_UUID, stateProperties, 1);
    hostMicrophoneStateCharacteristic =
        service->createCharacteristic(CompanionProtocol::HOST_MICROPHONE_STATE_UUID, stateProperties, 1);
    hostCameraStateCharacteristic =
        service->createCharacteristic(CompanionProtocol::HOST_CAMERA_STATE_UUID, stateProperties, 1);
    hostStatusMessageCharacteristic = service->createCharacteristic(
        CompanionProtocol::HOST_STATUS_MESSAGE_UUID, stateProperties, COMPANION_STATUS_MESSAGE_MAX_LEN);
    buttonEventCharacteristic =
        service->createCharacteristic(CompanionProtocol::BUTTON_EVENT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 9);
    deviceInfoCharacteristic =
        service->createCharacteristic(CompanionProtocol::DEVICE_INFO_UUID, NIMBLE_PROPERTY::READ, 2);

    if (!hostTeamsStateCharacteristic || !hostMicrophoneStateCharacteristic || !hostCameraStateCharacteristic ||
        !hostStatusMessageCharacteristic || !buttonEventCharacteristic || !deviceInfoCharacteristic) {
      LOG_ERR("COMP", "Failed to create companion characteristics teams=%p mic=%p camera=%p msg=%p button=%p info=%p",
              hostTeamsStateCharacteristic, hostMicrophoneStateCharacteristic, hostCameraStateCharacteristic,
              hostStatusMessageCharacteristic, buttonEventCharacteristic, deviceInfoCharacteristic);
      BluetoothDiagnostics::record("companion_characteristic_create_failed");
      if (ownsBluetoothStack) {
        btMgr.disable();
      }
      ownsBluetoothStack = false;
      server = nullptr;
      return false;
    }

    hostTeamsStateCharacteristic->setCallbacks(&teamsStateCallbacks);
    hostMicrophoneStateCharacteristic->setCallbacks(&microphoneStateCallbacks);
    hostCameraStateCharacteristic->setCallbacks(&cameraStateCallbacks);
    hostStatusMessageCharacteristic->setCallbacks(&statusMessageCallbacks);
    buttonEventCharacteristic->setCallbacks(&buttonEventCallbacks);
    if (!server->start()) {
      LOG_ERR("COMP", "Failed to start companion GATT server");
      BluetoothDiagnostics::record("companion_gatt_start_failed");
      if (ownsBluetoothStack) {
        btMgr.disable();
      }
      ownsBluetoothStack = false;
      server = nullptr;
      hostTeamsStateCharacteristic = nullptr;
      hostMicrophoneStateCharacteristic = nullptr;
      hostCameraStateCharacteristic = nullptr;
      hostStatusMessageCharacteristic = nullptr;
      buttonEventCharacteristic = nullptr;
      deviceInfoCharacteristic = nullptr;
      return false;
    }
    BluetoothDiagnostics::record("companion_gatt_started");
  }

  publishHostStateValues();
  publishDeviceInfo();

  auto* advertising = NimBLEDevice::getAdvertising();
  advertising->clearData();
  advertising->enableScanResponse(true);
  advertising->addServiceUUID(CompanionProtocol::SERVICE_UUID);
  advertising->setPreferredParams(BLE_CONN_INTERVAL_IDLE_MIN, BLE_CONN_INTERVAL_IDLE_MAX);
  advertising->setName("X3 Companion");
  if (!advertising->start()) {
    LOG_ERR("COMP", "Failed to start companion advertising");
    BluetoothDiagnostics::record("companion_advertising_start_failed");
    if (ownsBluetoothStack) {
      HalBluetoothManager::getInstance().disable();
      server = nullptr;
      hostTeamsStateCharacteristic = nullptr;
      hostMicrophoneStateCharacteristic = nullptr;
      hostCameraStateCharacteristic = nullptr;
      hostStatusMessageCharacteristic = nullptr;
      buttonEventCharacteristic = nullptr;
      deviceInfoCharacteristic = nullptr;
    }
    ownsBluetoothStack = false;
    return false;
  }
  LOG_INF("COMP", "Advertising companion service %s as peripheral", CompanionProtocol::SERVICE_UUID);

  running = true;
  resetSessionState();
  publishHostStateValues();
  advertisingRestartPending = false;
  pendingAdvertisingRestartAtMs = 0;
  pendingAdvertisingRestartReason = nullptr;
  statusChanged = true;
  lastMaintenanceAtMs = millis();
  lastStateLogAtMs = 0;
  lastAdvertisingRestartAtMs = 0;
  BluetoothDiagnostics::record("companion_ble_started");
  LOG_INF("COMP", "Companion BLE GATT server started");
  logStateSnapshot("start");
  return true;
}

void CompanionBleService::end() {
  if (!running) {
    resetSessionState();
    publishHostStateValues();
    statusChanged = true;
    return;
  }

  NimBLEDevice::stopAdvertising();
  running = false;
  advertisingRestartPending = false;
  pendingAdvertisingRestartAtMs = 0;
  pendingAdvertisingRestartReason = nullptr;
  disconnectConnectedHosts();
  resetSessionState();
  publishHostStateValues();
  statusChanged = true;
  BluetoothDiagnostics::record("companion_ble_stopped");
  LOG_INF("COMP", "Companion BLE service stopped; owns stack=%d", ownsBluetoothStack);

  if (ownsBluetoothStack) {
    HalBluetoothManager::getInstance().disable();
    server = nullptr;
    hostTeamsStateCharacteristic = nullptr;
    hostMicrophoneStateCharacteristic = nullptr;
    hostCameraStateCharacteristic = nullptr;
    hostStatusMessageCharacteristic = nullptr;
    buttonEventCharacteristic = nullptr;
    deviceInfoCharacteristic = nullptr;
  }
  ownsBluetoothStack = false;
}

void CompanionBleService::disconnectConnectedHosts() {
  if (!server) {
    return;
  }

  const auto peers = server->getPeerDevices();
  if (peers.empty()) {
    return;
  }

  LOG_INF("COMP", "Disconnecting %u companion host link(s)", static_cast<unsigned>(peers.size()));
  BluetoothDiagnostics::recordf("companion_disconnect_hosts", "count=%u", static_cast<unsigned>(peers.size()));
  for (const auto connHandle : peers) {
    const bool ok = server->disconnect(connHandle);
    LOG_INF("COMP", "Disconnect host handle=%u result=%d", static_cast<unsigned>(connHandle), ok);
  }

  delay(150);
}

bool CompanionBleService::hasConnectedHosts() const {
  if (!server) {
    return false;
  }

  return !server->getPeerDevices().empty();
}

void CompanionBleService::resetSessionState() {
  hostConnected = false;
  hostConnHandle = 0xFFFF;
  hostConnectedAtMs = 0;
  hostStateReceived = false;
  buttonEventSubscribed = false;
  connectionProfile = ConnectionPowerProfile::Unknown;
  lastConnParamRequestAtMs = 0;
  responsiveUntilMs = 0;
  buttonEventSequence = 0;
  hostStatus = HostStatus{};
}

std::string CompanionBleService::getStatusText() const {
  if (!running) {
    return "BLE failed";
  }
  if (hostConnected) {
    return hostStatus.teamsDetected ? "Teams running" : "Host connected";
  }
  return "Waiting for host";
}

CompanionBleService::HostStatus CompanionBleService::getHostStatus() const { return hostStatus; }

bool CompanionBleService::isConnectionHandshakeActive() const {
  return hostConnected && (!buttonEventSubscribed || !hostStateReceived);
}

void CompanionBleService::update() {
  if (!running || !server) {
    return;
  }

  const unsigned long now = millis();
  const bool pendingRestartDue =
      advertisingRestartPending && static_cast<long>(now - pendingAdvertisingRestartAtMs) >= 0;
  if (!pendingRestartDue && now - lastMaintenanceAtMs < COMPANION_MAINTENANCE_INTERVAL_MS) {
    return;
  }
  lastMaintenanceAtMs = now;

  const bool hasPeers = hasConnectedHosts();
  if (advertisingRestartPending && static_cast<long>(now - pendingAdvertisingRestartAtMs) >= 0) {
    const char* reason = pendingAdvertisingRestartReason ? pendingAdvertisingRestartReason : "pending";
    advertisingRestartPending = false;
    pendingAdvertisingRestartReason = nullptr;
    if (restartAdvertising(reason)) {
      return;
    }
    if (!hostConnected) {
      scheduleAdvertisingRestart(reason, COMPANION_MAINTENANCE_INTERVAL_MS);
    }
  }

  if (hostConnected && !hasPeers) {
    LOG_INF("COMP", "Recovering missed host disconnect; no active peer remains");
    BluetoothDiagnostics::record("companion_missed_disconnect_recovered");
    onHostDisconnected();
    scheduleAdvertisingRestart("missed_disconnect", COMPANION_DISCONNECT_ADVERTISING_RESTART_DELAY_MS);
    return;
  }

  if (hostConnected && responsiveUntilMs != 0 && static_cast<long>(now - responsiveUntilMs) >= 0) {
    responsiveUntilMs = 0;
    requestIdleConnectionParamsIfReady("responsive_window_elapsed");
  }

  if (lastStateLogAtMs == 0 || now - lastStateLogAtMs >= COMPANION_STATE_LOG_INTERVAL_MS) {
    logStateSnapshot("periodic");
  }

  if (!hostConnected && !hasPeers) {
    auto* advertising = NimBLEDevice::getAdvertising();
    if (!advertising || !advertising->isAdvertising()) {
      restartAdvertising("idle_maintenance");
    }
    return;
  }

  if (isConnectionHandshakeActive() && hostConnectedAtMs != 0 &&
      now - hostConnectedAtMs > COMPANION_HANDSHAKE_TIMEOUT_MS) {
    LOG_INF("COMP", "Disconnecting stale companion host handshake; buttonSub=%d stateReceived=%d ageMs=%lu",
            buttonEventSubscribed, hostStateReceived, now - hostConnectedAtMs);
    BluetoothDiagnostics::recordf("companion_stale_handshake_disconnect", "buttonSub=%d state=%d ageMs=%lu",
                                  buttonEventSubscribed, hostStateReceived, now - hostConnectedAtMs);
    disconnectConnectedHosts();
    resetSessionState();
    publishHostStateValues();
    statusChanged = true;
    notifyStatusChanged();
    restartAdvertising("stale_handshake");
  }
}

bool CompanionBleService::consumeStatusChanged() {
  const bool changed = statusChanged;
  statusChanged = false;
  return changed;
}

bool CompanionBleService::notifyToggleMuteReleased() {
  if (!running || !hostConnected || !buttonEventCharacteristic || !buttonEventSubscribed) {
    LOG_INF("COMP", "Mute button event not sent; running=%d connected=%d buttonChar=%p subscribed=%d", running,
            hostConnected, buttonEventCharacteristic, buttonEventSubscribed);
    return false;
  }

  publishButtonEvent(static_cast<uint8_t>(CompanionProtocol::ButtonId::ToggleMute),
                     static_cast<uint8_t>(CompanionProtocol::ButtonAction::Released));
  return true;
}

void CompanionBleService::setStatusChangedCallback(std::function<void()> callback) {
  statusChangedCallback = std::move(callback);
}

void CompanionBleService::onHostConnected() {
  onHostConnected(0xFFFF);
}

void CompanionBleService::onHostConnected(uint16_t connHandle) {
  advertisingRestartPending = false;
  pendingAdvertisingRestartAtMs = 0;
  pendingAdvertisingRestartReason = nullptr;
  resetSessionState();
  hostConnected = true;
  hostConnHandle = connHandle;
  hostConnectedAtMs = millis();
  statusChanged = true;
  BluetoothDiagnostics::record("companion_host_connected");
  LOG_INF("COMP", "Host connected to companion BLE GATT server");
  requestConnectionParams(ConnectionPowerProfile::Responsive, "connect");
  logStateSnapshot("connect");
  notifyStatusChanged();
}

void CompanionBleService::onHostDisconnected() {
  resetSessionState();
  publishHostStateValues();
  statusChanged = true;
  BluetoothDiagnostics::record("companion_host_disconnected");
  LOG_INF("COMP", "Host disconnected; host state reset to defaults");
  logStateSnapshot("disconnect");
  notifyStatusChanged();
}

void CompanionBleService::scheduleAdvertisingRestart(const char* reason, unsigned long delayMs) {
  if (!running) {
    return;
  }

  advertisingRestartPending = true;
  pendingAdvertisingRestartAtMs = millis() + delayMs;
  pendingAdvertisingRestartReason = reason;
  LOG_INF("COMP", "Advertising restart scheduled reason=%s delayMs=%lu", reason ? reason : "",
          static_cast<unsigned long>(delayMs));
}

void CompanionBleService::onConnParamsUpdated(uint16_t interval, uint16_t latency, uint16_t timeout) {
  if (interval >= BLE_CONN_INTERVAL_IDLE_MIN && interval <= BLE_CONN_INTERVAL_IDLE_MAX &&
      latency >= BLE_CONN_LATENCY_IDLE && timeout >= BLE_CONN_TIMEOUT_IDLE) {
    connectionProfile = ConnectionPowerProfile::Idle;
  } else if (interval <= BLE_CONN_INTERVAL_RESPONSIVE_MAX && latency == BLE_CONN_LATENCY_RESPONSIVE) {
    connectionProfile = ConnectionPowerProfile::Responsive;
  }
}

void CompanionBleService::onHostTeamsStateWritten(NimBLECharacteristic* characteristic) {
  if (!characteristic) {
    return;
  }

  const std::string value = characteristic->getValue();
  if (value.empty()) {
    LOG_INF("COMP", "Ignored empty teams state write");
    return;
  }

  const bool next = value[0] != 0;
  const bool changed = hostStatus.teamsDetected != next;
  const bool firstStateWrite = !hostStateReceived;
  hostStatus.teamsDetected = next;
  hostStateReceived = true;
  if (changed || firstStateWrite) {
    statusChanged = true;
    LOG_INF("COMP", "Host teams state=%d", hostStatus.teamsDetected);
    logStateSnapshot("teams_state");
    notifyStatusChanged();
  }
  requestIdleConnectionParamsIfReady("teams_state");
}

void CompanionBleService::onHostMicrophoneStateWritten(NimBLECharacteristic* characteristic) {
  if (!characteristic) {
    return;
  }

  const std::string value = characteristic->getValue();
  if (value.empty()) {
    LOG_INF("COMP", "Ignored empty microphone state write");
    return;
  }

  const uint8_t next = static_cast<uint8_t>(value[0]);
  const bool changed = hostStatus.microphone != next;
  const bool firstStateWrite = !hostStateReceived;
  hostStatus.microphone = next;
  hostStateReceived = true;
  if (changed || firstStateWrite) {
    statusChanged = true;
    LOG_INF("COMP", "Host microphone state=%u", static_cast<unsigned>(hostStatus.microphone));
    logStateSnapshot("microphone_state");
    notifyStatusChanged();
  }
  requestIdleConnectionParamsIfReady("microphone_state");
}

void CompanionBleService::onHostCameraStateWritten(NimBLECharacteristic* characteristic) {
  if (!characteristic) {
    return;
  }

  const std::string value = characteristic->getValue();
  if (value.empty()) {
    LOG_INF("COMP", "Ignored empty camera state write");
    return;
  }

  const uint8_t next = static_cast<uint8_t>(value[0]);
  const bool changed = hostStatus.camera != next;
  const bool firstStateWrite = !hostStateReceived;
  hostStatus.camera = next;
  hostStateReceived = true;
  if (changed || firstStateWrite) {
    statusChanged = true;
    LOG_INF("COMP", "Host camera state=%u", static_cast<unsigned>(hostStatus.camera));
    logStateSnapshot("camera_state");
    notifyStatusChanged();
  }
  requestIdleConnectionParamsIfReady("camera_state");
}

void CompanionBleService::onHostStatusMessageWritten(NimBLECharacteristic* characteristic) {
  if (!characteristic) {
    return;
  }

  std::string next = characteristic->getValue();
  if (next.size() > COMPANION_STATUS_MESSAGE_MAX_LEN) {
    next.resize(COMPANION_STATUS_MESSAGE_MAX_LEN);
  }

  const bool changed = hostStatus.message != next;
  const bool firstStateWrite = !hostStateReceived;
  hostStatus.message = next;
  hostStateReceived = true;
  if (changed || firstStateWrite) {
    statusChanged = true;
    LOG_INF("COMP", "Host message=%s", hostStatus.message.c_str());
    logStateSnapshot("message_state");
    notifyStatusChanged();
  }
  requestIdleConnectionParamsIfReady("message_state");
}

void CompanionBleService::onButtonEventSubscribed(bool subscribed) {
  buttonEventSubscribed = subscribed;
  statusChanged = true;
  LOG_INF("COMP", "Companion host button notifications subscribed=%d", subscribed);
  requestIdleConnectionParamsIfReady("subscribe");
  logStateSnapshot("button_subscribe");
  notifyStatusChanged();
}

void CompanionBleService::requestConnectionParams(ConnectionPowerProfile profile, const char* reason) {
  if (!server || !hostConnected || hostConnHandle == 0xFFFF) {
    return;
  }

  const unsigned long now = millis();
  if (connectionProfile == profile) {
    return;
  }
  if (lastConnParamRequestAtMs != 0 &&
      now - lastConnParamRequestAtMs < COMPANION_CONN_PARAM_REQUEST_MIN_INTERVAL_MS) {
    return;
  }

  uint16_t minInterval = BLE_CONN_INTERVAL_IDLE_MIN;
  uint16_t maxInterval = BLE_CONN_INTERVAL_IDLE_MAX;
  uint16_t latency = BLE_CONN_LATENCY_IDLE;
  uint16_t timeout = BLE_CONN_TIMEOUT_IDLE;
  if (profile == ConnectionPowerProfile::Responsive) {
    minInterval = BLE_CONN_INTERVAL_RESPONSIVE_MIN;
    maxInterval = BLE_CONN_INTERVAL_RESPONSIVE_MAX;
    latency = BLE_CONN_LATENCY_RESPONSIVE;
    timeout = BLE_CONN_TIMEOUT_RESPONSIVE;
  }

  lastConnParamRequestAtMs = now;
  connectionProfile = profile;
  server->updateConnParams(hostConnHandle, minInterval, maxInterval, latency, timeout);
  LOG_INF("COMP", "Requested %s connection params reason=%s min=%u max=%u latency=%u timeout=%u", profileName(profile),
          reason ? reason : "", static_cast<unsigned>(minInterval), static_cast<unsigned>(maxInterval),
          static_cast<unsigned>(latency), static_cast<unsigned>(timeout));
  BluetoothDiagnostics::recordf("companion_conn_params_requested", "profile=%s reason=%s", profileName(profile),
                                reason ? reason : "");
}

void CompanionBleService::requestIdleConnectionParamsIfReady(const char* reason) {
  if (!hostConnected || !hostStateReceived || !buttonEventSubscribed) {
    return;
  }
  if (responsiveUntilMs != 0) {
    return;
  }
  requestConnectionParams(ConnectionPowerProfile::Idle, reason);
}

bool CompanionBleService::restartAdvertising(const char* reason) {
  if (!running) {
    return false;
  }

  const bool hasPeers = hasConnectedHosts();
  if (hasPeers && hostConnected) {
    LOG_DBG("COMP", "Advertising restart skipped; host peer still connected reason=%s", reason ? reason : "");
    return false;
  }
  if (hasPeers) {
    LOG_INF("COMP", "Advertising restart continuing with stale peer list reason=%s", reason ? reason : "");
  }

  const unsigned long now = millis();
  auto* advertising = NimBLEDevice::getAdvertising();
  if (advertising && advertising->isAdvertising()) {
    return true;
  }

  if (lastAdvertisingRestartAtMs != 0 &&
      now - lastAdvertisingRestartAtMs < COMPANION_ADVERTISING_RESTART_INTERVAL_MS) {
    return false;
  }

  lastAdvertisingRestartAtMs = now;
  const bool ok = NimBLEDevice::startAdvertising();
  LOG_INF("COMP", "Advertising restart reason=%s ok=%d", reason ? reason : "", ok);
  BluetoothDiagnostics::recordf("companion_advertising_restart", "reason=%s ok=%d", reason ? reason : "", ok);
  return ok;
}

void CompanionBleService::publishHostStateValues() {
  const uint8_t teams = hostStatus.teamsDetected ? 1 : 0;
  if (hostTeamsStateCharacteristic) {
    hostTeamsStateCharacteristic->setValue(&teams, sizeof(teams));
  }
  if (hostMicrophoneStateCharacteristic) {
    hostMicrophoneStateCharacteristic->setValue(&hostStatus.microphone, sizeof(hostStatus.microphone));
  }
  if (hostCameraStateCharacteristic) {
    hostCameraStateCharacteristic->setValue(&hostStatus.camera, sizeof(hostStatus.camera));
  }
  if (hostStatusMessageCharacteristic) {
    hostStatusMessageCharacteristic->setValue(hostStatus.message);
  }
}

void CompanionBleService::notifyStatusChanged() {
  if (statusChangedCallback) {
    statusChangedCallback();
  }
}

void CompanionBleService::publishDeviceInfo() {
  if (!deviceInfoCharacteristic) {
    return;
  }

  uint8_t payload[] = {
      CompanionProtocol::PROTOCOL_VERSION,
      0x02,
  };
  deviceInfoCharacteristic->setValue(payload, sizeof(payload));
}

void CompanionBleService::publishButtonEvent(uint8_t buttonId, uint8_t action) {
  if (!buttonEventCharacteristic) {
    return;
  }

  responsiveUntilMs = millis() + COMPANION_BUTTON_RESPONSIVE_WINDOW_MS;
  requestConnectionParams(ConnectionPowerProfile::Responsive, "button_event");

  buttonEventSequence++;
  const uint32_t uptimeMs = millis();
  uint8_t payload[] = {
      CompanionProtocol::PROTOCOL_VERSION,
      buttonId,
      action,
      static_cast<uint8_t>(buttonEventSequence & 0xFF),
      static_cast<uint8_t>((buttonEventSequence >> 8) & 0xFF),
      static_cast<uint8_t>(uptimeMs & 0xFF),
      static_cast<uint8_t>((uptimeMs >> 8) & 0xFF),
      static_cast<uint8_t>((uptimeMs >> 16) & 0xFF),
      static_cast<uint8_t>((uptimeMs >> 24) & 0xFF),
  };
  buttonEventCharacteristic->setValue(payload, sizeof(payload));
  buttonEventCharacteristic->notify();
  LOG_INF("COMP", "Button event notified seq=%u button=%u action=%u uptimeMs=%lu",
          static_cast<unsigned>(buttonEventSequence), static_cast<unsigned>(buttonId), static_cast<unsigned>(action),
          static_cast<unsigned long>(uptimeMs));
  BluetoothDiagnostics::recordf("companion_button_event_notify", "seq=%u button=%u action=%u",
                                static_cast<unsigned>(buttonEventSequence), static_cast<unsigned>(buttonId),
                                static_cast<unsigned>(action));
}

void CompanionBleService::logStateSnapshot(const char* reason) {
  lastStateLogAtMs = millis();
  LOG_INF("COMP", "State reason=%s running=%d connected=%d buttonSub=%d stateRx=%d profile=%s teams=%d mic=%u cam=%u heap=%u",
          reason ? reason : "", running, hostConnected, buttonEventSubscribed, hostStateReceived,
          profileName(connectionProfile), hostStatus.teamsDetected, static_cast<unsigned>(hostStatus.microphone),
          static_cast<unsigned>(hostStatus.camera), static_cast<unsigned>(ESP.getFreeHeap()));
}
