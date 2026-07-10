#include "HalTiltSensor.h"

#include <HalPowerManager.h>
#include <Logging.h>

HalTiltSensor halTiltSensor;  // Singleton instance

bool HalTiltSensor::writeReg(uint8_t reg, uint8_t val) const {
  HalPowerManager::PeripheralLock peripheralLock;
  Wire.beginTransmission(_i2cAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool HalTiltSensor::readReg(uint8_t reg, uint8_t* val) const {
  HalPowerManager::PeripheralLock peripheralLock;
  Wire.beginTransmission(_i2cAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  Wire.requestFrom(_i2cAddr, (uint8_t)1);
  if (Wire.available() < 1) {
    return false;
  }
  *val = Wire.read();
  return true;
}

bool HalTiltSensor::readGyro(float& gx, float& gy, float& gz) const {
  HalPowerManager::PeripheralLock peripheralLock;
  Wire.beginTransmission(_i2cAddr);
  Wire.write(REG_GX_L);  // Start reading at Gyro X Low
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  Wire.requestFrom(_i2cAddr, (uint8_t)6);
  if (Wire.available() < 6) {
    return false;
  }

  auto readInt16 = [&]() -> int16_t {
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    return static_cast<int16_t>((hi << 8) | lo);
  };

  // If Full Scale is ±512 dps, the scale factor is 32768 / 512 = 64 LSB/dps
  constexpr float SCALE = 1.0f / 64.0f;
  gx = readInt16() * SCALE;
  gy = readInt16() * SCALE;
  gz = readInt16() * SCALE;
  return true;
}

bool HalTiltSensor::runCtrl9Command(uint8_t command) const {
  if (!writeReg(REG_CTRL9, command)) {
    return false;
  }

  const unsigned long start = millis();
  uint8_t statusInt = 0;
  do {
    delay(1);
    if (readReg(REG_STATUSINT, &statusInt) && (statusInt & STATUSINT_CMD_DONE)) {
      return writeReg(REG_CTRL9, CTRL9_CMD_ACK);
    }
  } while (millis() - start < 100);

  LOG_ERR("GYR", "CTRL9 command 0x%02X timed out", command);
  return false;
}

void HalTiltSensor::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  // Try primary address, then alternate
  uint8_t whoami = 0;
  _i2cAddr = I2C_ADDR_QMI8658;
  if (!readReg(QMI8658_WHO_AM_I_REG, &whoami) || whoami != QMI8658_WHO_AM_I_VALUE) {
    _i2cAddr = I2C_ADDR_QMI8658_ALT;
    if (!readReg(QMI8658_WHO_AM_I_REG, &whoami) || whoami != QMI8658_WHO_AM_I_VALUE) {
      LOG_ERR("GYR", "QMI8658 IMU not found");
      _available = false;
      return;
    }
  }

  LOG_INF("GYR", "QMI8658 IMU found at 0x%02X", _i2cAddr);

  if (!writeReg(REG_CTRL7, CTRL7_DISABLE_ALL) || !writeReg(REG_CTRL3, CTRL3_FS_512DPS | CTRL3_ODR_28HZ) ||
      !writeReg(REG_CTRL1, CTRL1_BASE | CTRL1_SENSOR_DISABLE)) {
    LOG_ERR("GYR", "QMI8658 register configuration failed");
    _available = false;
    return;
  }

  _available = true;
  _initMs = millis();
  _lastPollMs = millis();
  LOG_INF("GYR", "QMI8658 gyro initialized and put to sleep");
}

bool HalTiltSensor::wake() {
  if (!_available) {
    return false;
  }

  // Wait for init to complete before waking
  if ((millis() - _initMs) < SLEEP_STABILIZE_MS) {
    return false;
  }

  if (writeReg(REG_CTRL1, CTRL1_BASE) && writeReg(REG_CTRL7, CTRL7_GYRO_ENABLE)) {
    _lastPollMs = millis();
    _lastTiltMs = millis();
    _wakeMs = millis();
    LOG_INF("GYR", "QMI8658 woke up");
    return true;
  } else {
    LOG_ERR("GYR", "Failed to wake QMI8658");
    return false;
  }
}

bool HalTiltSensor::deepSleep() {
  if (!_available) {
    return false;
  }

  if ((millis() - _wakeMs) < SLEEP_STABILIZE_MS) {
    return false;
  }

  if (writeReg(REG_CTRL7, CTRL7_DISABLE_ALL) && writeReg(REG_CTRL1, CTRL1_BASE | CTRL1_SENSOR_DISABLE)) {
    // Clear any residual state so it doesn't immediately trigger upon waking
    clearPendingEvents();
    _inTilt = false;
    LOG_INF("GYR", "QMI8658 entered sleep mode");
    return true;
  } else {
    LOG_ERR("GYR", "Failed to put QMI8658 to sleep");
    return false;
  }
}

bool HalTiltSensor::readGyroDps(float& gx, float& gy, float& gz) const {
  if (!_available) {
    return false;
  }
  return readGyro(gx, gy, gz);
}

bool HalTiltSensor::readStatus(uint8_t& statusInt, uint8_t& status0, uint8_t& status1) const {
  if (!_available) {
    statusInt = 0;
    status0 = 0;
    status1 = 0;
    return false;
  }

  return readReg(REG_STATUSINT, &statusInt) && readReg(REG_STATUS0, &status0) && readReg(REG_STATUS1, &status1);
}

bool HalTiltSensor::enableWakeOnMotionInterrupt(bool useInt1, uint8_t thresholdMg, uint8_t blankingSamples,
                                                bool initialHigh) {
  if (!_available || thresholdMg == 0) {
    return false;
  }

  if (blankingSamples > 0x3F) {
    blankingSamples = 0x3F;
  }
  const uint8_t interruptSelect = useInt1 ? (initialHigh ? WOM_INT1_INITIAL_HIGH : WOM_INT1_INITIAL_LOW)
                                          : (initialHigh ? WOM_INT2_INITIAL_HIGH : WOM_INT2_INITIAL_LOW);
  const uint8_t cal1H = interruptSelect | blankingSamples;
  const uint8_t intEnable = useInt1 ? CTRL1_INT1_ENABLE : CTRL1_INT2_ENABLE;

  if (!writeReg(REG_CTRL7, CTRL7_DISABLE_ALL) || !writeReg(REG_CTRL1, CTRL1_BASE | intEnable) ||
      !writeReg(REG_CTRL2, CTRL2_FS_2G | CTRL2_ODR_21HZ_LOW_POWER) ||
      !writeReg(REG_CTRL8, CTRL8_CTRL9_STATUSINT_HANDSHAKE) || !writeReg(REG_CAL1_L, thresholdMg) ||
      !writeReg(REG_CAL1_H, cal1H) || !runCtrl9Command(CTRL9_CMD_WRITE_WOM_SETTING) ||
      !writeReg(REG_CTRL7, CTRL7_ACCEL_ENABLE)) {
    LOG_ERR("GYR", "Failed to enable QMI8658 WoM on %s", useInt1 ? "INT1" : "INT2");
    return false;
  }

  _isAwake = true;
  clearPendingEvents();
  _inTilt = false;
  LOG_INF("GYR", "QMI8658 WoM enabled on %s threshold=%umg blanking=%u initial=%s", useInt1 ? "INT1" : "INT2",
          thresholdMg, blankingSamples, initialHigh ? "high" : "low");
  return true;
}

bool HalTiltSensor::enableWakeOnMotionInt1(uint8_t thresholdMg, uint8_t blankingSamples, bool initialHigh) {
  return enableWakeOnMotionInterrupt(true, thresholdMg, blankingSamples, initialHigh);
}

bool HalTiltSensor::disableWakeOnMotion() {
  if (!_available) {
    return false;
  }

  uint8_t ignored = 0;
  readReg(REG_STATUS1, &ignored);

  const bool ok = writeReg(REG_CTRL7, CTRL7_DISABLE_ALL) && writeReg(REG_CAL1_L, 0x00) && writeReg(REG_CAL1_H, 0x00) &&
                  writeReg(REG_CTRL8, CTRL8_CTRL9_STATUSINT_HANDSHAKE) &&
                  runCtrl9Command(CTRL9_CMD_WRITE_WOM_SETTING) &&
                  writeReg(REG_CTRL3, CTRL3_FS_512DPS | CTRL3_ODR_28HZ) &&
                  writeReg(REG_CTRL1, CTRL1_BASE | CTRL1_SENSOR_DISABLE) &&
                  writeReg(REG_CTRL7, CTRL7_DISABLE_ALL);
  if (!ok) {
    LOG_ERR("GYR", "Failed to disable QMI8658 WoM");
    return false;
  }

  _isAwake = false;
  clearPendingEvents();
  _inTilt = false;
  LOG_INF("GYR", "QMI8658 WoM disabled");
  return true;
}

void HalTiltSensor::update(const uint8_t mode, const uint8_t orientation, const bool inReader) {
  if (!_available) {
    return;
  }

  // State machine: wake up or sleep based on the enabled flag
  if ((mode != CrossPointTiltPageTurn::TILT_OFF) && !_isAwake) {
    _isAwake = wake();
    return;
  } else if ((mode == CrossPointTiltPageTurn::TILT_OFF) && _isAwake) {
    _isAwake = !deepSleep();
    return;
  }

  // If disabled, skip the rest of the polling logic and avoid unnecessary I2C traffic in non-reader activities
  if ((mode == CrossPointTiltPageTurn::TILT_OFF) || !inReader) {
    return;
  }

  const unsigned long now = millis();
  // Stabilization: discard readings during gyro startup transient
  if ((now - _wakeMs) < WAKE_STABILIZE_MS) {
    return;
  }

  if ((now - _lastPollMs) < POLL_INTERVAL_MS) {
    return;
  }
  _lastPollMs = now;

  float gx, gy, gz;
  if (!readGyro(gx, gy, gz)) {
    return;
  }

  // Map the gyro axis to left/right tilt based on reader orientation.
  // On the X3 PCB: X axis = left/right in portrait, Y axis = left/right in landscape.
  float tiltAxis;
  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? -gx : gx;
      break;
    case CrossPointOrientation::INVERTED:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? gx : -gx;
      break;
    case CrossPointOrientation::LANDSCAPE_CW:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? gy : -gy;
      break;
    case CrossPointOrientation::LANDSCAPE_CCW:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? -gy : gy;
      break;
    default:
      tiltAxis = gx;
      break;
  }

  if (_inTilt) {
    // Wait for device to return to neutral before allowing next trigger
    if (fabsf(tiltAxis) < NEUTRAL_RATE_DPS) {
      _inTilt = false;
    }
  } else {
    // Check for new tilt gesture (with cooldown)
    if ((now - _lastTiltMs) >= COOLDOWN_MS) {
      if (tiltAxis > RATE_THRESHOLD_DPS) {
        _tiltForwardEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Forward Trigger=(%.1f) dps", tiltAxis);
      } else if (tiltAxis < -RATE_THRESHOLD_DPS) {
        _tiltBackEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Backward Trigger=(%.1f) dps", tiltAxis);
      }
    }
  }
}

bool HalTiltSensor::wasTiltedForward() {
  const bool val = _tiltForwardEvent;
  _tiltForwardEvent = false;
  return val;
}

bool HalTiltSensor::wasTiltedBack() {
  const bool val = _tiltBackEvent;
  _tiltBackEvent = false;
  return val;
}

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}

void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _hadActivity = false;
  // Intentionally preserve _inTilt so a held tilt doesn't retrigger on next poll
}
