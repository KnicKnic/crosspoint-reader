#include "CompanionBleService.h"

#include <Arduino.h>
#include <BluetoothDiagnostics.h>
#include <HalBluetoothManager.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <PmLockTrace.h>

#include <algorithm>
#include <cstdio>
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

constexpr uint16_t BLE_CONN_INTERVAL_IDLE_MIN = 80;        // 100 ms
constexpr uint16_t BLE_CONN_INTERVAL_IDLE_MAX = 96;        // 120 ms
constexpr uint16_t BLE_CONN_LATENCY_IDLE = 39;             // Up to 4-4.8 s between idle radio events.
constexpr uint16_t BLE_CONN_TIMEOUT_IDLE = 1500;           // 15 s
constexpr uint16_t BLE_ADV_INTERVAL_LOW_POWER_MIN = 800;   // 500 ms
constexpr uint16_t BLE_ADV_INTERVAL_LOW_POWER_MAX = 1600;  // 1000 ms

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

uint32_t connIntervalTenthsMs(uint16_t interval) {
  return (static_cast<uint32_t>(interval) * 125U + 5U) / 10U;
}

uint32_t advIntervalTenthsMs(uint16_t interval) {
  return (static_cast<uint32_t>(interval) * 625U + 50U) / 100U;
}

void formatTenthsMs(uint32_t tenthsMs, char* buffer, size_t bufferSize) {
  if (tenthsMs % 10U == 0) {
    snprintf(buffer, bufferSize, "%lums", static_cast<unsigned long>(tenthsMs / 10U));
    return;
  }
  snprintf(buffer, bufferSize, "%lu.%lums", static_cast<unsigned long>(tenthsMs / 10U),
           static_cast<unsigned long>(tenthsMs % 10U));
}

CompanionBleService* g_service = nullptr;

uint32_t deltaCounter(uint32_t current, uint32_t previous) {
  return current >= previous ? current - previous : 0;
}

unsigned long minDelayMs(unsigned long current, unsigned long candidate) {
  return candidate < current ? candidate : current;
}

unsigned long delayUntilMs(unsigned long now, unsigned long target) {
  return static_cast<long>(now - target) >= 0 ? 0 : target - now;
}

void clearCallbackTarget(CompanionBleService* service) {
  if (g_service == service) {
    g_service = nullptr;
  }
}

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
      g_service->onConnParamsUpdated(connInfo.getConnInterval(), connInfo.getConnLatency(), connInfo.getConnTimeout());
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

  void onMTUChange(uint16_t mtu, NimBLEConnInfo& connInfo) override {
    LOG_INF("COMP", "Host MTU changed address=%s handle=%u mtu=%u interval=%.1f ms latency=%u timeout=%u ms",
            connInfo.getAddress().toString().c_str(), static_cast<unsigned>(connInfo.getConnHandle()),
            static_cast<unsigned>(mtu), connInfo.getConnInterval() * 1.25f,
            static_cast<unsigned>(connInfo.getConnLatency()), static_cast<unsigned>(connInfo.getConnTimeout() * 10));
    BluetoothDiagnostics::recordf("companion_host_mtu_changed", "handle=%u mtu=%u",
                                  static_cast<unsigned>(connInfo.getConnHandle()), static_cast<unsigned>(mtu));
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    LOG_INF("COMP", "Host auth complete handle=%u bonded=%d enc=%d auth=%d key=%u",
            static_cast<unsigned>(connInfo.getConnHandle()), connInfo.isBonded(), connInfo.isEncrypted(),
            connInfo.isAuthenticated(), static_cast<unsigned>(connInfo.getSecKeySize()));
    BluetoothDiagnostics::recordf("companion_host_auth_complete", "handle=%u bonded=%d enc=%d auth=%d",
                                  static_cast<unsigned>(connInfo.getConnHandle()), connInfo.isBonded(),
                                  connInfo.isEncrypted(), connInfo.isAuthenticated());
  }

  void onIdentity(NimBLEConnInfo& connInfo) override {
    LOG_INF("COMP", "Host identity resolved handle=%u addr=%s id=%s",
            static_cast<unsigned>(connInfo.getConnHandle()), connInfo.getAddress().toString().c_str(),
            connInfo.getIdAddress().toString().c_str());
    BluetoothDiagnostics::recordf("companion_host_identity", "handle=%u",
                                  static_cast<unsigned>(connInfo.getConnHandle()));
  }

  void onPhyUpdate(NimBLEConnInfo& connInfo, uint8_t txPhy, uint8_t rxPhy) override {
    LOG_INF("COMP", "Host PHY update handle=%u tx=%u rx=%u", static_cast<unsigned>(connInfo.getConnHandle()),
            static_cast<unsigned>(txPhy), static_cast<unsigned>(rxPhy));
    BluetoothDiagnostics::recordf("companion_host_phy_update", "handle=%u tx=%u rx=%u",
                                  static_cast<unsigned>(connInfo.getConnHandle()), static_cast<unsigned>(txPhy),
                                  static_cast<unsigned>(rxPhy));
  }
};

class HostStateCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit HostStateCallbacks(HostStateField field) : field(field) {}

 private:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    const size_t len = characteristic ? characteristic->getValue().size() : 0;
    LOG_INF("COMP", "Host write field=%s handle=%u len=%u interval=%.1f ms latency=%u timeout=%u ms",
            hostStateFieldName(field), static_cast<unsigned>(connInfo.getConnHandle()), static_cast<unsigned>(len),
            connInfo.getConnInterval() * 1.25f, static_cast<unsigned>(connInfo.getConnLatency()),
            static_cast<unsigned>(connInfo.getConnTimeout() * 10));
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
  void onStatus(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo, int code) override {
    (void)characteristic;
    LOG_INF("COMP", "Button notify tx status handle=%u code=%d %s", static_cast<unsigned>(connInfo.getConnHandle()),
            code, NimBLEUtils::returnCodeToString(code));
    BluetoothDiagnostics::recordf("companion_button_event_notify_status", "handle=%u code=%d",
                                  static_cast<unsigned>(connInfo.getConnHandle()), code);
  }

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
      clearCallbackTarget(this);
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
      clearCallbackTarget(this);
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
      clearCallbackTarget(this);
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
      clearCallbackTarget(this);
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
  advertising->setMinInterval(BLE_ADV_INTERVAL_LOW_POWER_MIN);
  advertising->setMaxInterval(BLE_ADV_INTERVAL_LOW_POWER_MAX);
  advertising->setPreferredParams(BLE_CONN_INTERVAL_IDLE_MIN, BLE_CONN_INTERVAL_IDLE_MAX);
  advertising->setName("X3 Companion");
  if (!advertising->start()) {
    LOG_ERR("COMP", "Failed to start companion advertising");
    BluetoothDiagnostics::record("companion_advertising_start_failed");
    clearCallbackTarget(this);
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
  clearCallbackTarget(this);
  statusChangedCallback = nullptr;

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

bool CompanionBleService::isAdvertising() const {
  auto* advertising = NimBLEDevice::getAdvertising();
  return running && !hostConnected && advertising && advertising->isAdvertising();
}

void CompanionBleService::resetSessionState() {
  hostConnected = false;
  hostConnHandle = 0xFFFF;
  hostConnectedAtMs = 0;
  hostStateReceived = false;
  buttonEventSubscribed = false;
  connectionProfile = ConnectionPowerProfile::Unknown;
  requestedConnectionProfile = ConnectionPowerProfile::Unknown;
  requestedConnIntervalMin = 0;
  requestedConnIntervalMax = 0;
  requestedConnLatency = 0;
  requestedConnTimeout = 0;
  negotiatedConnInterval = 0;
  negotiatedConnLatency = 0;
  negotiatedConnTimeout = 0;
  hasNegotiatedConnParams = false;
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

unsigned long CompanionBleService::getNextUpdateDelayMs(unsigned long now) const {
  if (!running || !server) {
    return COMPANION_MAINTENANCE_INTERVAL_MS;
  }

  unsigned long delayMs = delayUntilMs(now, lastMaintenanceAtMs + COMPANION_MAINTENANCE_INTERVAL_MS);
  if (advertisingRestartPending) {
    delayMs = minDelayMs(delayMs, delayUntilMs(now, pendingAdvertisingRestartAtMs));
  }
  if (hostConnected && responsiveUntilMs != 0) {
    delayMs = minDelayMs(delayMs, delayUntilMs(now, responsiveUntilMs));
  }
  if (isConnectionHandshakeActive() && hostConnectedAtMs != 0) {
    delayMs = minDelayMs(delayMs, delayUntilMs(now, hostConnectedAtMs + COMPANION_HANDSHAKE_TIMEOUT_MS));
  }
  if (lastStateLogAtMs != 0) {
    delayMs = minDelayMs(delayMs, delayUntilMs(now, lastStateLogAtMs + COMPANION_STATE_LOG_INTERVAL_MS));
  }
  return delayMs;
}

void CompanionBleService::update() {
  if (!running || !server) {
    return;
  }

  activityStats.updateCalls++;
  const unsigned long now = millis();
  const bool pendingRestartDue =
      advertisingRestartPending && static_cast<long>(now - pendingAdvertisingRestartAtMs) >= 0;
  const bool responsiveDue = hostConnected && responsiveUntilMs != 0 && static_cast<long>(now - responsiveUntilMs) >= 0;
  const bool stateLogDue = lastStateLogAtMs == 0 || now - lastStateLogAtMs >= COMPANION_STATE_LOG_INTERVAL_MS;
  const bool handshakeDue = isConnectionHandshakeActive() && hostConnectedAtMs != 0 &&
                            now - hostConnectedAtMs > COMPANION_HANDSHAKE_TIMEOUT_MS;
  const bool maintenanceDue = now - lastMaintenanceAtMs >= COMPANION_MAINTENANCE_INTERVAL_MS;
  if (!pendingRestartDue && !responsiveDue && !stateLogDue && !handshakeDue && !maintenanceDue) {
    return;
  }
  activityStats.maintenanceRuns++;
  if (maintenanceDue || pendingRestartDue) {
    lastMaintenanceAtMs = now;
  }

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

  if (responsiveDue) {
    responsiveUntilMs = 0;
    requestIdleConnectionParamsIfReady("responsive_window_elapsed");
  }

  if (stateLogDue) {
    logStateSnapshot("periodic");
  }

  if (!hostConnected && !hasPeers) {
    auto* advertising = NimBLEDevice::getAdvertising();
    if (!advertising || !advertising->isAdvertising()) {
      restartAdvertising("idle_maintenance");
    }
    return;
  }

  if (handshakeDue) {
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

std::string CompanionBleService::formatTimingDiagnostics() const {
  char advMin[12];
  char advMax[12];
  char prefMin[12];
  char prefMax[12];
  formatTenthsMs(advIntervalTenthsMs(BLE_ADV_INTERVAL_LOW_POWER_MIN), advMin, sizeof(advMin));
  formatTenthsMs(advIntervalTenthsMs(BLE_ADV_INTERVAL_LOW_POWER_MAX), advMax, sizeof(advMax));
  formatTenthsMs(connIntervalTenthsMs(BLE_CONN_INTERVAL_IDLE_MIN), prefMin, sizeof(prefMin));
  formatTenthsMs(connIntervalTenthsMs(BLE_CONN_INTERVAL_IDLE_MAX), prefMax, sizeof(prefMax));

  char line[144];
  const bool advertising = running && !hostConnected && NimBLEDevice::getAdvertising() &&
                           NimBLEDevice::getAdvertising()->isAdvertising();
  snprintf(line, sizeof(line), "BLEA: adv %s-%s pref %s-%s %s", advMin, advMax, prefMin, prefMax,
           advertising ? "on" : "off");

  std::string result(line);
  result += "\n";

  char reqMin[12] = "?";
  char reqMax[12] = "?";
  if (requestedConnIntervalMin != 0 && requestedConnIntervalMax != 0) {
    formatTenthsMs(connIntervalTenthsMs(requestedConnIntervalMin), reqMin, sizeof(reqMin));
    formatTenthsMs(connIntervalTenthsMs(requestedConnIntervalMax), reqMax, sizeof(reqMax));
  }

  if (hostConnected && hasNegotiatedConnParams) {
    char got[12];
    formatTenthsMs(connIntervalTenthsMs(negotiatedConnInterval), got, sizeof(got));
    snprintf(line, sizeof(line), "BLEC: got %s L%u T%us req %s %s-%s L%u T%us", got,
             static_cast<unsigned>(negotiatedConnLatency), static_cast<unsigned>(negotiatedConnTimeout / 100U),
             profileName(requestedConnectionProfile), reqMin, reqMax, static_cast<unsigned>(requestedConnLatency),
             static_cast<unsigned>(requestedConnTimeout / 100U));
  } else if (hostConnected) {
    snprintf(line, sizeof(line), "BLEC: pending req %s %s-%s L%u T%us", profileName(requestedConnectionProfile),
             reqMin, reqMax, static_cast<unsigned>(requestedConnLatency),
             static_cast<unsigned>(requestedConnTimeout / 100U));
  } else {
    snprintf(line, sizeof(line), "BLEC: disc req %s %s-%s L%u T%us", profileName(requestedConnectionProfile), reqMin,
             reqMax, static_cast<unsigned>(requestedConnLatency),
             static_cast<unsigned>(requestedConnTimeout / 100U));
  }
  result += line;
  return result;
}

std::string CompanionBleService::formatActivityDeltaDiagnostics() {
  if (!hasPreviousActivityStats) {
    previousActivityStats = activityStats;
    hasPreviousActivityStats = true;
    return "BLED: baseline";
  }

  const ActivityStats current = activityStats;
  char line[176];
  snprintf(line, sizeof(line),
           "BLED: upd+%lu m+%lu wr+%lu chg+%lu sub+%lu ntf+%lu req+%lu got+%lu adv+%lu gap+%lu/%lu st+%lu",
           static_cast<unsigned long>(deltaCounter(current.updateCalls, previousActivityStats.updateCalls)),
           static_cast<unsigned long>(deltaCounter(current.maintenanceRuns, previousActivityStats.maintenanceRuns)),
           static_cast<unsigned long>(deltaCounter(current.hostWrites, previousActivityStats.hostWrites)),
           static_cast<unsigned long>(deltaCounter(current.hostStateChanges, previousActivityStats.hostStateChanges)),
           static_cast<unsigned long>(deltaCounter(current.buttonSubscribes, previousActivityStats.buttonSubscribes)),
           static_cast<unsigned long>(deltaCounter(current.buttonNotifications,
                                                   previousActivityStats.buttonNotifications)),
           static_cast<unsigned long>(deltaCounter(current.connParamRequests,
                                                   previousActivityStats.connParamRequests)),
           static_cast<unsigned long>(deltaCounter(current.connParamUpdates, previousActivityStats.connParamUpdates)),
           static_cast<unsigned long>(deltaCounter(current.advertisingRestarts,
                                                   previousActivityStats.advertisingRestarts)),
           static_cast<unsigned long>(deltaCounter(current.gapConnects, previousActivityStats.gapConnects)),
           static_cast<unsigned long>(deltaCounter(current.gapDisconnects, previousActivityStats.gapDisconnects)),
           static_cast<unsigned long>(deltaCounter(current.statusNotifications,
                                                   previousActivityStats.statusNotifications)));
  previousActivityStats = current;
  return line;
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
  activityStats.gapConnects++;
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
  activityStats.gapDisconnects++;
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
  activityStats.connParamUpdates++;
  negotiatedConnInterval = interval;
  negotiatedConnLatency = latency;
  negotiatedConnTimeout = timeout;
  setBtLockTraceConnectionParams(interval, latency);
  hasNegotiatedConnParams = true;
  if (interval >= BLE_CONN_INTERVAL_IDLE_MIN && interval <= BLE_CONN_INTERVAL_IDLE_MAX &&
      latency >= BLE_CONN_LATENCY_IDLE && timeout >= BLE_CONN_TIMEOUT_IDLE) {
    connectionProfile = ConnectionPowerProfile::Idle;
  } else if (interval <= BLE_CONN_INTERVAL_RESPONSIVE_MAX && latency == BLE_CONN_LATENCY_RESPONSIVE) {
    connectionProfile = ConnectionPowerProfile::Responsive;
  }
}

void CompanionBleService::onHostTeamsStateWritten(NimBLECharacteristic* characteristic) {
  activityStats.hostWrites++;
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
    activityStats.hostStateChanges++;
    statusChanged = true;
    LOG_INF("COMP", "Host teams state=%d", hostStatus.teamsDetected);
    logStateSnapshot("teams_state");
    notifyStatusChanged();
  }
  requestIdleConnectionParamsIfReady("teams_state");
}

void CompanionBleService::onHostMicrophoneStateWritten(NimBLECharacteristic* characteristic) {
  activityStats.hostWrites++;
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
    activityStats.hostStateChanges++;
    statusChanged = true;
    LOG_INF("COMP", "Host microphone state=%u", static_cast<unsigned>(hostStatus.microphone));
    logStateSnapshot("microphone_state");
    notifyStatusChanged();
  }
  requestIdleConnectionParamsIfReady("microphone_state");
}

void CompanionBleService::onHostCameraStateWritten(NimBLECharacteristic* characteristic) {
  activityStats.hostWrites++;
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
    activityStats.hostStateChanges++;
    statusChanged = true;
    LOG_INF("COMP", "Host camera state=%u", static_cast<unsigned>(hostStatus.camera));
    logStateSnapshot("camera_state");
    notifyStatusChanged();
  }
  requestIdleConnectionParamsIfReady("camera_state");
}

void CompanionBleService::onHostStatusMessageWritten(NimBLECharacteristic* characteristic) {
  activityStats.hostWrites++;
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
    activityStats.hostStateChanges++;
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
  activityStats.buttonSubscribes++;
  LOG_INF("COMP", "Companion host button notifications subscribed=%d", subscribed);
  requestIdleConnectionParamsIfReady("subscribe");
  logStateSnapshot("button_subscribe");
  notifyStatusChanged();
}

void CompanionBleService::requestConnectionParams(ConnectionPowerProfile profile, const char* reason) {
  if (!server || !hostConnected || hostConnHandle == 0xFFFF) {
    LOG_DBG("COMP", "Conn param request skipped reason=%s server=%p connected=%d handle=%u", reason ? reason : "",
            server, hostConnected, static_cast<unsigned>(hostConnHandle));
    return;
  }

  const unsigned long now = millis();
  if (connectionProfile == profile) {
    LOG_DBG("COMP", "Conn param request skipped reason=%s already profile=%s", reason ? reason : "",
            profileName(profile));
    return;
  }
  if (lastConnParamRequestAtMs != 0 &&
      now - lastConnParamRequestAtMs < COMPANION_CONN_PARAM_REQUEST_MIN_INTERVAL_MS) {
    LOG_INF("COMP", "Conn param request throttled reason=%s profile=%s ageMs=%lu minAgeMs=%lu", reason ? reason : "",
            profileName(profile), static_cast<unsigned long>(now - lastConnParamRequestAtMs),
            static_cast<unsigned long>(COMPANION_CONN_PARAM_REQUEST_MIN_INTERVAL_MS));
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
  requestedConnectionProfile = profile;
  requestedConnIntervalMin = minInterval;
  requestedConnIntervalMax = maxInterval;
  requestedConnLatency = latency;
  requestedConnTimeout = timeout;
  activityStats.connParamRequests++;
  server->updateConnParams(hostConnHandle, minInterval, maxInterval, latency, timeout);
  LOG_INF("COMP", "Requested %s connection params reason=%s min=%u max=%u latency=%u timeout=%u", profileName(profile),
          reason ? reason : "", static_cast<unsigned>(minInterval), static_cast<unsigned>(maxInterval),
          static_cast<unsigned>(latency), static_cast<unsigned>(timeout));
  BluetoothDiagnostics::recordf("companion_conn_params_requested", "profile=%s reason=%s", profileName(profile),
                                reason ? reason : "");
}

void CompanionBleService::requestIdleConnectionParamsIfReady(const char* reason) {
  if (!hostConnected || !hostStateReceived || !buttonEventSubscribed) {
    LOG_DBG("COMP", "Idle conn params not ready reason=%s connected=%d stateRx=%d buttonSub=%d", reason ? reason : "",
            hostConnected, hostStateReceived, buttonEventSubscribed);
    return;
  }
  if (responsiveUntilMs != 0) {
    LOG_DBG("COMP", "Idle conn params deferred reason=%s responsiveUntilMs=%lu nowMs=%lu", reason ? reason : "",
            static_cast<unsigned long>(responsiveUntilMs), static_cast<unsigned long>(millis()));
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
    LOG_DBG("COMP", "Advertising restart skipped; already advertising reason=%s", reason ? reason : "");
    return true;
  }

  if (lastAdvertisingRestartAtMs != 0 &&
      now - lastAdvertisingRestartAtMs < COMPANION_ADVERTISING_RESTART_INTERVAL_MS) {
    LOG_INF("COMP", "Advertising restart throttled reason=%s ageMs=%lu minAgeMs=%lu", reason ? reason : "",
            static_cast<unsigned long>(now - lastAdvertisingRestartAtMs),
            static_cast<unsigned long>(COMPANION_ADVERTISING_RESTART_INTERVAL_MS));
    return false;
  }

  lastAdvertisingRestartAtMs = now;
  const bool ok = NimBLEDevice::startAdvertising();
  activityStats.advertisingRestarts++;
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
  activityStats.statusNotifications++;
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
  activityStats.buttonNotifications++;
  LOG_INF("COMP", "Button event notified seq=%u button=%u action=%u uptimeMs=%lu",
          static_cast<unsigned>(buttonEventSequence), static_cast<unsigned>(buttonId), static_cast<unsigned>(action),
          static_cast<unsigned long>(uptimeMs));
  BluetoothDiagnostics::recordf("companion_button_event_notify", "seq=%u button=%u action=%u",
                                static_cast<unsigned>(buttonEventSequence), static_cast<unsigned>(buttonId),
                                static_cast<unsigned>(action));
}

void CompanionBleService::logStateSnapshot(const char* reason) {
  lastStateLogAtMs = millis();
  activityStats.stateLogs++;
  LOG_INF("COMP", "State reason=%s running=%d connected=%d buttonSub=%d stateRx=%d profile=%s teams=%d mic=%u cam=%u heap=%u",
          reason ? reason : "", running, hostConnected, buttonEventSubscribed, hostStateReceived,
          profileName(connectionProfile), hostStatus.teamsDetected, static_cast<unsigned>(hostStatus.microphone),
          static_cast<unsigned>(hostStatus.camera), static_cast<unsigned>(ESP.getFreeHeap()));
}
