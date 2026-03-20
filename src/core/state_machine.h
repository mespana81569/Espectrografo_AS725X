#pragma once

enum class SystemState {
    IDLE,
    CALIBRATION,
    WAIT_CONFIRMATION,
    MEASUREMENT,
    VALIDATION,
    SAVE_DECISION
};

class StateMachine {
public:
    StateMachine();
    void tick();
    SystemState getState() const;
    void requestTransition(SystemState next);
    const char* getStateName() const;

private:
    SystemState _state;
    SystemState _requested;
    bool _transitionPending;

    void enterState(SystemState s);
    void exitState(SystemState s);
};

extern StateMachine g_stateMachine;
