#include "as7265x_driver.h"
#include <Wire.h>
#include <Arduino.h>

AS7265xDriver g_sensorDriver;

// AS7265X 18 channel wavelengths in nm (A through R order)
const uint16_t AS7265xDriver::WAVELENGTHS[18] = {
    410, 435, 460, 485, 510, 535,   // UV channels  A-F  (AS72651)
    560, 585, 610, 645, 680, 705,   // VIS channels G-L  (AS72652)
    730, 760, 810, 860, 900, 940    // NIR channels R-W  (AS72653) — mapped indices 12-17
};

AS7265xDriver::AS7265xDriver() : _initialized(false) {}

bool AS7265xDriver::begin() {
    Wire.begin();
    Wire.setClock(400000);
    if (!_sensor.begin()) {
        Serial.println("[Sensor] AS7265X not found");
        _initialized = false;
        return false;
    }
    _initialized = true;
    _sensor.disableIndicator();  // turn off the onboard status LED
    applyConfig(_cfg);
    Serial.println("[Sensor] AS7265X ready");
    return true;
}

bool AS7265xDriver::isReady() const {
    return _initialized;
}

static uint8_t maToConst(uint8_t ma) {
    if      (ma <= 12)  return AS7265X_LED_CURRENT_LIMIT_12_5MA;
    else if (ma <= 25)  return AS7265X_LED_CURRENT_LIMIT_25MA;
    else if (ma <= 50)  return AS7265X_LED_CURRENT_LIMIT_50MA;
    else                return AS7265X_LED_CURRENT_LIMIT_100MA;
}

void AS7265xDriver::applyConfig(const SensorConfig& cfg) {
    _cfg = cfg;
    if (!_initialized) return;

    _sensor.setGain(static_cast<uint8_t>(cfg.gain));
    _sensor.setIntegrationCycles(cfg.integrationCycles);
    _sensor.setMeasurementMode(static_cast<uint8_t>(cfg.mode));

    _sensor.setBulbCurrent(maToConst(cfg.ledWhiteCurrent), AS7265x_LED_WHITE);
    _sensor.setBulbCurrent(maToConst(cfg.ledIrCurrent),    AS7265x_LED_IR);
    _sensor.setBulbCurrent(maToConst(cfg.ledUvCurrent),    AS7265x_LED_UV);
}

SensorConfig AS7265xDriver::getConfig() const {
    return _cfg;
}

bool AS7265xDriver::takeMeasurement(float* out18) {
    if (!_initialized) return false;

    if (_cfg.ledWhiteEnabled) _sensor.enableBulb(AS7265x_LED_WHITE);
    if (_cfg.ledIrEnabled)    _sensor.enableBulb(AS7265x_LED_IR);
    if (_cfg.ledUvEnabled)    _sensor.enableBulb(AS7265x_LED_UV);

    _sensor.takeMeasurementsWithBulb();

    _sensor.disableBulb(AS7265x_LED_WHITE);
    _sensor.disableBulb(AS7265x_LED_IR);
    _sensor.disableBulb(AS7265x_LED_UV);

    // Channels in order: A B C D E F (UV) | G H I J K L (VIS) | R S T U V W (NIR)
    out18[0]  = _sensor.getCalibratedA();
    out18[1]  = _sensor.getCalibratedB();
    out18[2]  = _sensor.getCalibratedC();
    out18[3]  = _sensor.getCalibratedD();
    out18[4]  = _sensor.getCalibratedE();
    out18[5]  = _sensor.getCalibratedF();
    out18[6]  = _sensor.getCalibratedG();
    out18[7]  = _sensor.getCalibratedH();
    out18[8]  = _sensor.getCalibratedI();
    out18[9]  = _sensor.getCalibratedJ();
    out18[10] = _sensor.getCalibratedK();
    out18[11] = _sensor.getCalibratedL();
    out18[12] = _sensor.getCalibratedR();
    out18[13] = _sensor.getCalibratedS();
    out18[14] = _sensor.getCalibratedT();
    out18[15] = _sensor.getCalibratedU();
    out18[16] = _sensor.getCalibratedV();
    out18[17] = _sensor.getCalibratedW();

    return true;
}

void AS7265xDriver::setSleepMode(bool sleep) {
    if (!_initialized) return;
    // The AS7265X has no true standby register. Disabling the bulbs is the
    // only safe idle action — do NOT touch the measurement-mode register here
    // because applyConfig() already set it correctly and overwriting it would
    // corrupt subsequent measurements.
    if (sleep) {
        _sensor.disableBulb(AS7265x_LED_WHITE);
        _sensor.disableBulb(AS7265x_LED_IR);
        _sensor.disableBulb(AS7265x_LED_UV);
    }
    // On wake-up, applyConfig() was already called during begin(); no action needed.
}
