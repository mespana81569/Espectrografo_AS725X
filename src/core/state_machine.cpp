#include "state_machine.h"
#include "../sensors/as7265x_driver.h"
#include "../acquisition/measurement_engine.h"
#include "../acquisition/calibration.h"
#include "../storage/sd_logger.h"
#include <Arduino.h>

StateMachine g_stateMachine;

StateMachine::StateMachine()
    : _state(SystemState::IDLE),
      _requested(SystemState::IDLE),
      _transitionPending(false) {}

void StateMachine::requestTransition(SystemState next) {
    _requested = next;
    _transitionPending = true;
}

SystemState StateMachine::getState() const {
    return _state;
}

const char* StateMachine::getStateName() const {
    switch (_state) {
        case SystemState::IDLE:              return "IDLE";
        case SystemState::CALIBRATION:       return "CALIBRATION";
        case SystemState::WAIT_CONFIRMATION: return "WAIT_CONFIRMATION";
        case SystemState::MEASUREMENT:       return "MEASUREMENT";
        case SystemState::VALIDATION:        return "VALIDATION";
        case SystemState::SAVE_DECISION:     return "SAVE_DECISION";
        default:                             return "UNKNOWN";
    }
}

void StateMachine::enterState(SystemState s) {
    switch (s) {
        case SystemState::IDLE:
            g_sensorDriver.setSleepMode(true);
            break;
        case SystemState::CALIBRATION:
            g_sensorDriver.setSleepMode(false);
            g_calibration.start();
            break;
        case SystemState::WAIT_CONFIRMATION:
            g_sensorDriver.setSleepMode(true);
            break;
        case SystemState::MEASUREMENT:
            g_sensorDriver.setSleepMode(false);
            g_measurementEngine.start();
            break;
        case SystemState::VALIDATION:
            // Data already in buffer, just signal UI
            break;
        case SystemState::SAVE_DECISION:
            // Await user YES/NO via web
            break;
    }
}

void StateMachine::exitState(SystemState s) {
    switch (s) {
        case SystemState::CALIBRATION:
            g_sdLogger.saveCalibration(g_calibration.getData());
            g_calibration.clearDoneFlag();
            break;
        default:
            break;
    }
}

void StateMachine::tick() {
    // Auto-transitions: only fire once, never overwrite an already-pending request
    if (!_transitionPending) {
        switch (_state) {
            case SystemState::CALIBRATION:
                if (g_calibration.isDone()) {
                    requestTransition(SystemState::WAIT_CONFIRMATION);
                }
                break;
            case SystemState::MEASUREMENT:
                if (g_measurementEngine.isDone()) {
                    requestTransition(SystemState::VALIDATION);
                }
                break;
            default:
                break;
        }
    }

    // Apply pending transition
    if (_transitionPending) {
        _transitionPending = false;
        exitState(_state);
        _state = _requested;
        enterState(_state);
        Serial.printf("[SM] → %s\n", getStateName());
    }
}
