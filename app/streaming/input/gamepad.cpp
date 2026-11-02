#include "streaming/session.h"

#include <Limelight.h>
#include "SDL_compat.h"
#include "settings/mappingmanager.h"
#include "utils.h"

#include <QtMath>

// How long the Start button must be pressed to toggle mouse emulation
#define MOUSE_EMULATION_LONG_PRESS_TIME 750

// How long between polling the gamepad to send virtual mouse input
#define MOUSE_EMULATION_POLLING_INTERVAL 50

// Determines how fast the mouse will move each interval
#define MOUSE_EMULATION_MOTION_MULTIPLIER 4

// Determines the maximum motion amount before allowing movement
#define MOUSE_EMULATION_DEADZONE 2

// Haptic capabilities (in addition to those from SDL_HapticQuery())
#define ML_HAPTIC_GC_RUMBLE         (1U << 16)
#define ML_HAPTIC_SIMPLE_RUMBLE     (1U << 17)
#define ML_HAPTIC_GC_TRIGGER_RUMBLE (1U << 18)

const int SdlInputHandler::k_ButtonMap[] = {
    A_FLAG, B_FLAG, X_FLAG, Y_FLAG,
    BACK_FLAG, SPECIAL_FLAG, PLAY_FLAG,
    LS_CLK_FLAG, RS_CLK_FLAG,
    LB_FLAG, RB_FLAG,
    UP_FLAG, DOWN_FLAG, LEFT_FLAG, RIGHT_FLAG,
    MISC_FLAG,
    PADDLE1_FLAG, PADDLE2_FLAG, PADDLE3_FLAG, PADDLE4_FLAG,
    TOUCHPAD_FLAG,
};

GamepadState*
SdlInputHandler::findStateForGamepad(SDL_JoystickID id)
{
    int i;

    for (i = 0; i < MAX_GAMEPADS; i++) {
        if (m_GamepadState[i].jsId == id) {
            SDL_assert(!m_MultiController || m_GamepadState[i].index == i);
            return &m_GamepadState[i];
        }
    }

    // We can get a spurious removal event if the device is removed
    // before or during SDL_GameControllerOpen(). This is fine to ignore.
    return nullptr;
}

void SdlInputHandler::sendGamepadState(GamepadState* state)
{
    SDL_assert(m_GamepadMask == 0x1 || m_MultiController);

    // Handle Select+PS as the clickpad button on PS4/5 controllers without a clickpad mapping
    int buttons = state->buttons;
    if (state->clickpadButtonEmulationEnabled) {
        if (state->buttons == (BACK_FLAG | SPECIAL_FLAG)) {
            buttons = MISC_FLAG;
            state->emulatedClickpadButtonDown = true;
        }
        else if (state->emulatedClickpadButtonDown) {
            buttons &= ~MISC_FLAG;
            state->emulatedClickpadButtonDown = false;
        }
    }

    unsigned char lt = state->lt;
    unsigned char rt = state->rt;
    short lsX = state->lsX;
    short lsY = state->lsY;
    short rsX = state->rsX;
    short rsY = state->rsY;

    // When in single controller mode, merge all gamepad state together
    if (!m_MultiController) {
        for (int i = 0; i < MAX_GAMEPADS; i++) {
            if (m_GamepadState[i].index == state->index) {
                buttons |= m_GamepadState[i].buttons;
                if (lt < m_GamepadState[i].lt) {
                    lt = m_GamepadState[i].lt;
                }
                if (rt < m_GamepadState[i].rt) {
                    rt = m_GamepadState[i].rt;
                }

                // We use abs() here instead of qAbs() for get proper integer promotion to
                // correctly handle abs(-32768), which is not representable in a short.
                if (abs(lsX) < abs(m_GamepadState[i].lsX) || abs(lsY) < abs(m_GamepadState[i].lsY)) {
                    lsX = m_GamepadState[i].lsX;
                    lsY = m_GamepadState[i].lsY;
                }
                if (abs(rsX) < abs(m_GamepadState[i].rsX) || abs(rsY) < abs(m_GamepadState[i].rsY)) {
                    rsX = m_GamepadState[i].rsX;
                    rsY = m_GamepadState[i].rsY;
                }
            }
        }
    }

    LiSendMultiControllerEvent(state->index,
                               m_GamepadMask,
                               buttons,
                               lt,
                               rt,
                               lsX,
                               lsY,
                               rsX,
                               rsY);
}

void SdlInputHandler::sendGamepadBatteryState(GamepadState* state, SDL_JoystickPowerLevel level)
{
    uint8_t batteryPercentage;
    uint8_t batteryState;

    // SDL's battery reporting capabilities are quite limited. Notably, we cannot
    // tell the battery level while charging (or even if a battery is present).
    // We also cannot tell the percentage of charge exactly in any case.
    switch (level)
    {
    case SDL_JOYSTICK_POWER_UNKNOWN:
        batteryState = LI_BATTERY_STATE_UNKNOWN;
        batteryPercentage = LI_BATTERY_PERCENTAGE_UNKNOWN;
        break;
    case SDL_JOYSTICK_POWER_WIRED:
        batteryState = LI_BATTERY_STATE_CHARGING;
        batteryPercentage = LI_BATTERY_PERCENTAGE_UNKNOWN;
        break;
    case SDL_JOYSTICK_POWER_EMPTY:
        batteryState = LI_BATTERY_STATE_DISCHARGING;
        batteryPercentage = 5;
        break;
    case SDL_JOYSTICK_POWER_LOW:
        batteryState = LI_BATTERY_STATE_DISCHARGING;
        batteryPercentage = 20;
        break;
    case SDL_JOYSTICK_POWER_MEDIUM:
        batteryState = LI_BATTERY_STATE_DISCHARGING;
        batteryPercentage = 50;
        break;
    case SDL_JOYSTICK_POWER_FULL:
        batteryState = LI_BATTERY_STATE_DISCHARGING;
        batteryPercentage = 90;
        break;
    default:
        return;
    }

    LiSendControllerBatteryEvent(state->index, batteryState, batteryPercentage);
}

Uint32 SdlInputHandler::mouseEmulationTimerCallback(Uint32 interval, void *param)
{
    auto gamepad = reinterpret_cast<GamepadState*>(param);

    int rawX;
    int rawY;

    // Determine which analog stick is currently receiving the strongest input
    if (abs(gamepad->lsX) + abs(gamepad->lsY) > abs(gamepad->rsX) + abs(gamepad->rsY)) {
        rawX = gamepad->lsX;
        rawY = -gamepad->lsY;
    }
    else {
        rawX = gamepad->rsX;
        rawY = -gamepad->rsY;
    }

    float deltaX;
    float deltaY;

    // Produce a base vector for mouse movement with increased speed as we deviate further from center
    deltaX = qPow(rawX / 32766.0f * MOUSE_EMULATION_MOTION_MULTIPLIER, 3);
    deltaY = qPow(rawY / 32766.0f * MOUSE_EMULATION_MOTION_MULTIPLIER, 3);

    // Enforce deadzones
    deltaX = qAbs(deltaX) > MOUSE_EMULATION_DEADZONE ? deltaX - MOUSE_EMULATION_DEADZONE : 0;
    deltaY = qAbs(deltaY) > MOUSE_EMULATION_DEADZONE ? deltaY - MOUSE_EMULATION_DEADZONE : 0;

    if (deltaX != 0 || deltaY != 0) {
        LiSendMouseMoveEvent((short)deltaX, (short)deltaY);
    }

    return interval;
}

static void updateAnalogStickAxis(short& value, short newValue, bool& dirty)
{
#ifdef STEAM_LINK
    // Use a deadzone on Steam Link to reduce CPU usage from idle joysticks
    newValue = abs(newValue) < 1500 ? 0 : newValue;
#endif
    dirty |= (newValue != value);
    value = newValue;
}

static void updateTriggerAxis(unsigned char& value, unsigned char newValue, bool& dirty)
{
#ifdef STEAM_LINK
    // Use a deadzone on Steam Link to reduce CPU usage from idle joysticks
    newValue = newValue < 10 ? 0 : newValue;
#endif
    dirty |= (newValue != value);
    value = newValue;
}

void SdlInputHandler::handleControllerAxisEvent(SDL_ControllerAxisEvent* event)
{
    SDL_JoystickID gameControllerId = event->which;
    GamepadState* state = findStateForGamepad(gameControllerId);
    if (state == NULL) {
        return;
    }

    // Batch all pending axis motion events for this gamepad to save CPU time
    SDL_Event nextEvent;
    bool dirty = false;
    for (;;) {
        switch (event->axis)
        {
            case SDL_CONTROLLER_AXIS_LEFTX:
                updateAnalogStickAxis(state->lsX, event->value, dirty);
                break;
            case SDL_CONTROLLER_AXIS_LEFTY:
                // Signed values have one more negative value than
                // positive value, so inverting the sign on -32768
                // could actually cause the value to overflow and
                // wrap around to be negative again. Avoid that by
                // capping the value at 32767.
                updateAnalogStickAxis(state->lsY, -qMax(event->value, (short)-32767), dirty);
                break;
            case SDL_CONTROLLER_AXIS_RIGHTX:
                updateAnalogStickAxis(state->rsX, event->value, dirty);
                break;
            case SDL_CONTROLLER_AXIS_RIGHTY:
                updateAnalogStickAxis(state->rsY, -qMax(event->value, (short)-32767), dirty);
                break;
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                updateTriggerAxis(state->lt, (unsigned char)(event->value * 255UL / 32767), dirty);
                break;
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                updateTriggerAxis(state->rt, (unsigned char)(event->value * 255UL / 32767), dirty);
                break;
            default:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Unhandled controller axis: %d",
                            event->axis);
                return;
        }

        // Check for another event to batch with
        if (SDL_PeepEvents(&nextEvent, 1, SDL_PEEKEVENT, SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERAXISMOTION) <= 0) {
            break;
        }

        event = &nextEvent.caxis;
        if (event->which != gameControllerId) {
            // Stop batching if a different gamepad interrupts us
            break;
        }

        // Remove the next event to batch
        SDL_PeepEvents(&nextEvent, 1, SDL_GETEVENT, SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERAXISMOTION);
    }

    // Only send the gamepad state to the host if it's not in mouse emulation mode
    if (state->mouseEmulationTimer == 0 && dirty) {
        sendGamepadState(state);
    }
}

void SdlInputHandler::handleControllerButtonEvent(SDL_ControllerButtonEvent* event)
{
    if (event->button >= SDL_arraysize(k_ButtonMap)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "No mapping for gamepad button: %u",
                    event->button);
        return;
    }

    GamepadState* state = findStateForGamepad(event->which);
    if (state == NULL) {
        return;
    }

    if (m_SwapFaceButtons) {
        switch (event->button) {
        case SDL_CONTROLLER_BUTTON_A:
            event->button = SDL_CONTROLLER_BUTTON_B;
            break;
        case SDL_CONTROLLER_BUTTON_B:
            event->button = SDL_CONTROLLER_BUTTON_A;
            break;
        case SDL_CONTROLLER_BUTTON_X:
            event->button = SDL_CONTROLLER_BUTTON_Y;
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            event->button = SDL_CONTROLLER_BUTTON_X;
            break;
        }
    }

    if (event->state == SDL_PRESSED) {
        state->buttons |= k_ButtonMap[event->button];

        if (event->button == SDL_CONTROLLER_BUTTON_START) {
            state->lastStartDownTime = SDL_GetTicks();
        }
        else if (state->mouseEmulationTimer != 0) {
            if (event->button == SDL_CONTROLLER_BUTTON_A) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_B) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_X) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_MIDDLE);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_X1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_X2);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                LiSendScrollEvent(1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                LiSendScrollEvent(-1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                LiSendHScrollEvent(1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
                LiSendHScrollEvent(-1);
            }
        }
    }
    else {
        state->buttons &= ~k_ButtonMap[event->button];

        if (event->button == SDL_CONTROLLER_BUTTON_START) {
            if (SDL_GetTicks() - state->lastStartDownTime > MOUSE_EMULATION_LONG_PRESS_TIME) {
                if (state->mouseEmulationTimer != 0) {
                    SDL_RemoveTimer(state->mouseEmulationTimer);
                    state->mouseEmulationTimer = 0;

                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Mouse emulation deactivated");
                    Session::get()->notifyMouseEmulationMode(false);
                }
                else if (m_GamepadMouse) {
                    // Send the start button up event to the host, since we won't do it below
                    sendGamepadState(state);

                    state->mouseEmulationTimer = SDL_AddTimer(MOUSE_EMULATION_POLLING_INTERVAL, SdlInputHandler::mouseEmulationTimerCallback, state);

                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Mouse emulation active");
                    Session::get()->notifyMouseEmulationMode(true);
                }
            }
        }
        else if (state->mouseEmulationTimer != 0) {
            if (event->button == SDL_CONTROLLER_BUTTON_A) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_B) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_X) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_X1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_X2);
            }
        }
    }

    // Handle Start+Select+L1+R1 as a gamepad quit combo
    if (state->buttons == (PLAY_FLAG | BACK_FLAG | LB_FLAG | RB_FLAG) && qgetenv("NO_GAMEPAD_QUIT") != "1") {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected quit gamepad button combo");

        // Push a quit event to the main loop
        SDL_Event event;
        event.type = SDL_QUIT;
        event.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&event);

        // Clear buttons down on this gamepad
        LiSendMultiControllerEvent(state->index, m_GamepadMask,
                                   0, 0, 0, 0, 0, 0, 0);
        return;
    }

    // Handle Select+L1+R1+X as a gamepad overlay combo
    if (state->buttons == (BACK_FLAG | LB_FLAG | RB_FLAG | X_FLAG)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected stats toggle gamepad combo");

        // Toggle the stats overlay
        Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayDebug,
                                                            !Session::get()->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug));

        // Clear buttons down on this gamepad
        LiSendMultiControllerEvent(state->index, m_GamepadMask,
                                   0, 0, 0, 0, 0, 0, 0);
        return;
    }

    // Only send the gamepad state to the host if it's not in mouse emulation mode
    if (state->mouseEmulationTimer == 0) {
        sendGamepadState(state);
    }
}

#if SDL_VERSION_ATLEAST(2, 0, 14)

void SdlInputHandler::handleControllerSensorEvent(SDL_ControllerSensorEvent* event)
{
    GamepadState* state = findStateForGamepad(event->which);
    if (state == NULL) {
        return;
    }

    switch (event->sensor) {
    case SDL_SENSOR_ACCEL:
        if (state->accelReportPeriodMs &&
                SDL_TICKS_PASSED(event->timestamp, state->lastAccelEventTime + state->accelReportPeriodMs) &&
                memcmp(event->data, state->lastAccelEventData, sizeof(event->data)) != 0) {
            memcpy(state->lastAccelEventData, event->data, sizeof(event->data));
            state->lastAccelEventTime = event->timestamp;

            LiSendControllerMotionEvent((uint8_t)state->index, LI_MOTION_TYPE_ACCEL, event->data[0], event->data[1], event->data[2]);
        }
        break;
    case SDL_SENSOR_GYRO:
        if (state->gyroReportPeriodMs &&
                SDL_TICKS_PASSED(event->timestamp, state->lastGyroEventTime + state->gyroReportPeriodMs) &&
                memcmp(event->data, state->lastGyroEventData, sizeof(event->data)) != 0) {
            memcpy(state->lastGyroEventData, event->data, sizeof(event->data));
            state->lastGyroEventTime = event->timestamp;

            // Convert rad/s to deg/s
            LiSendControllerMotionEvent((uint8_t)state->index, LI_MOTION_TYPE_GYRO,
                                        event->data[0] * 57.2957795f,
                                        event->data[1] * 57.2957795f,
                                        event->data[2] * 57.2957795f);
        }
        break;
    }
}

void SdlInputHandler::handleControllerTouchpadEvent(SDL_ControllerTouchpadEvent* event)
{
    GamepadState* state = findStateForGamepad(event->which);
    if (state == NULL) {
        return;
    }

    uint8_t eventType;
    switch (event->type) {
    case SDL_CONTROLLERTOUCHPADDOWN:
        eventType = LI_TOUCH_EVENT_DOWN;
        break;
    case SDL_CONTROLLERTOUCHPADUP:
        eventType = LI_TOUCH_EVENT_UP;
        break;
    case SDL_CONTROLLERTOUCHPADMOTION:
        eventType = LI_TOUCH_EVENT_MOVE;
        break;
    default:
        return;
    }

    LiSendControllerTouchEvent2((uint8_t)state->index, eventType,
                                (uint8_t)event->touchpad, event->finger,
                                event->x, event->y, event->pressure);
}

#endif

#if SDL_VERSION_ATLEAST(2, 24, 0)

void SdlInputHandler::handleJoystickBatteryEvent(SDL_JoyBatteryEvent* event)
{
    GamepadState* state = findStateForGamepad(event->which);
    if (state == NULL) {
        return;
    }

    sendGamepadBatteryState(state, event->level);
}

#endif

void SdlInputHandler::handleControllerDeviceEvent(SDL_ControllerDeviceEvent* event)
{
    GamepadState* state;

    if (event->type == SDL_CONTROLLERDEVICEADDED) {
        int i;
        const char* name;
        SDL_GameController* controller;
        const char* mapping;
        char guidStr[33];
        uint32_t hapticCaps;

        controller = SDL_GameControllerOpen(event->which);
        if (controller == NULL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to open gamepad: %s",
                         SDL_GetError());
            return;
        }

        // SDL_CONTROLLERDEVICEADDED can be reported multiple times for the same
        // gamepad in rare cases, because SDL doesn't fixup the device index in
        // the SDL_CONTROLLERDEVICEADDED event if an unopened gamepad disappears
        // before we've processed the add event.
        for (int i = 0; i < MAX_GAMEPADS; i++) {
            if (m_GamepadState[i].controller == controller) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Received duplicate add event for controller index: %d",
                            event->which);
                SDL_GameControllerClose(controller);
                return;
            }
        }

        // We used to use SDL_GameControllerGetPlayerIndex() here but that
        // can lead to strange issues due to bugs in Windows where an Xbox
        // controller will join as player 2, even though no player 1 controller
        // is connected at all. This pretty much screws any attempt to use
        // the gamepad in single player games, so just assign them in order from 0.
        i = 0;

        for (; i < MAX_GAMEPADS; i++) {
            SDL_assert(m_GamepadState[i].controller != controller);
            if (m_GamepadState[i].controller == NULL) {
                // Found an empty slot
                break;
            }
        }

        if (i == MAX_GAMEPADS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "No open gamepad slots found!");
            SDL_GameControllerClose(controller);
            return;
        }

        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(controller)),
                                  guidStr, sizeof(guidStr));
        if (m_IgnoreDeviceGuids.contains(guidStr, Qt::CaseInsensitive))
        {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Skipping ignored device with GUID: %s",
                        guidStr);
            SDL_GameControllerClose(controller);
            return;
        }

        state = &m_GamepadState[i];
        if (m_MultiController) {
            state->index = i;

#if SDL_VERSION_ATLEAST(2, 0, 12)
            // This will change indicators on the controller to show the assigned
            // player index. For Xbox 360 controllers, that means updating the LED
            // ring to light up the corresponding quadrant for this player.
            SDL_GameControllerSetPlayerIndex(controller, state->index);
#endif
        }
        else {
            // Always player 1 in single controller mode
            state->index = 0;
        }

        state->controller = controller;
        state->jsId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(state->controller));

        hapticCaps = 0;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        hapticCaps |= SDL_GameControllerHasRumble(controller) ? ML_HAPTIC_GC_RUMBLE : 0;
        hapticCaps |= SDL_GameControllerHasRumbleTriggers(controller) ? ML_HAPTIC_GC_TRIGGER_RUMBLE : 0;
#elif SDL_VERSION_ATLEAST(2, 0, 9)
        // Perform a tiny rumbles to see if haptics are supported.
        // NB: We cannot use zeros for rumble intensity or SDL will not actually call the JS driver
        // and we'll get a (potentially false) success value returned.
        hapticCaps |= SDL_GameControllerRumble(controller, 1, 1, 1) == 0 ? ML_HAPTIC_GC_RUMBLE : 0;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        hapticCaps |= SDL_GameControllerRumbleTriggers(controller, 1, 1, 1) == 0 ? ML_HAPTIC_GC_TRIGGER_RUMBLE : 0;
#endif
#else
        state->haptic = SDL_HapticOpenFromJoystick(SDL_GameControllerGetJoystick(state->controller));
        state->hapticEffectId = -1;
        state->hapticMethod = GAMEPAD_HAPTIC_METHOD_NONE;
        if (state->haptic != nullptr) {
            // Query for supported haptic effects
            hapticCaps = SDL_HapticQuery(state->haptic);
            hapticCaps |= SDL_HapticRumbleSupported(state->haptic) ?
                            ML_HAPTIC_SIMPLE_RUMBLE : 0;

            if ((SDL_HapticQuery(state->haptic) & SDL_HAPTIC_LEFTRIGHT) == 0) {
                if (SDL_HapticRumbleSupported(state->haptic)) {
                    if (SDL_HapticRumbleInit(state->haptic) == 0) {
                        state->hapticMethod = GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE;
                    }
                }
                if (state->hapticMethod == GAMEPAD_HAPTIC_METHOD_NONE) {
                    SDL_HapticClose(state->haptic);
                    state->haptic = nullptr;
                }
            } else {
                state->hapticMethod = GAMEPAD_HAPTIC_METHOD_LEFTRIGHT;
            }
        }
        else {
            hapticCaps = 0;
        }
#endif

        mapping = SDL_GameControllerMapping(state->controller);
        name = SDL_GameControllerName(state->controller);

        uint16_t vendorId = SDL_GameControllerGetVendor(state->controller);
        uint16_t productId = SDL_GameControllerGetProduct(state->controller);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Gamepad %d (player %d) is: %s (VID/PID: 0x%.4x/0x%.4x) (haptic capabilities: 0x%x) (mapping: %s -> %s)",
                    i,
                    state->index,
                    name != nullptr ? name : "<null>",
                    vendorId,
                    productId,
                    hapticCaps,
                    guidStr,
                    mapping != nullptr ? mapping : "<null>");
        if (mapping != nullptr) {
            SDL_free((void*)mapping);
        }

        // Add this gamepad to the gamepad mask
        if (m_MultiController) {
            // NB: Don't assert that it's unset here because we will already
            // have the mask set for initially attached gamepads to avoid confusing
            // apps running on the host.
            m_GamepadMask |= (1 << state->index);
        }
        else {
            SDL_assert(m_GamepadMask == 0x1);
        }

        SDL_JoystickPowerLevel powerLevel = SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(state->controller));

#if SDL_VERSION_ATLEAST(2, 0, 14)
        // On SDL 2.0.14 and later, we can provide enhanced controller information to the host PC
        // for it to use as a hint for the type of controller to emulate.
        uint32_t supportedButtonFlags = 0;
        for (int i = 0; i < (int)SDL_arraysize(k_ButtonMap); i++) {
            if (SDL_GameControllerHasButton(state->controller, (SDL_GameControllerButton)i)) {
                supportedButtonFlags |= k_ButtonMap[i];
            }
        }

        uint32_t capabilities = 0;
        if (SDL_GameControllerGetBindForAxis(state->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT).bindType == SDL_CONTROLLER_BINDTYPE_AXIS ||
            SDL_GameControllerGetBindForAxis(state->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT).bindType == SDL_CONTROLLER_BINDTYPE_AXIS) {
            // We assume these are analog triggers if the binding is to an axis rather than a button
            capabilities |= LI_CCAP_ANALOG_TRIGGERS;
        }
        if (hapticCaps & ML_HAPTIC_GC_RUMBLE) {
            capabilities |= LI_CCAP_RUMBLE;
        }
        if (hapticCaps & ML_HAPTIC_GC_TRIGGER_RUMBLE) {
            capabilities |= LI_CCAP_TRIGGER_RUMBLE;
        }
        if (SDL_GameControllerGetNumTouchpads(state->controller) > 0) {
            capabilities |= LI_CCAP_TOUCHPAD;
            if (SDL_GameControllerGetNumTouchpads(state->controller) > 1) {
                capabilities |= LI_CCAP_DUAL_TOUCHPAD;
            }
        }
        if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_ACCEL)) {
            capabilities |= LI_CCAP_ACCEL;
        }
        if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_GYRO)) {
            capabilities |= LI_CCAP_GYRO;
        }
        if (powerLevel != SDL_JOYSTICK_POWER_UNKNOWN || SDL_VERSION_ATLEAST(2, 24, 0)) {
            capabilities |= LI_CCAP_BATTERY_STATE;
        }
        if (SDL_GameControllerHasLED(state->controller)) {
            capabilities |= LI_CCAP_RGB_LED;
        }

        uint8_t type;
        switch (SDL_GameControllerGetType(state->controller)) {
        case SDL_CONTROLLER_TYPE_XBOX360:
        case SDL_CONTROLLER_TYPE_XBOXONE:
            type = LI_CTYPE_XBOX;
            break;
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5:
            type = LI_CTYPE_PS;
            break;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
            type = LI_CTYPE_NINTENDO;
            break;
        default:
            // These Steam Controller VID/PID combos come from SDL's controller_list.h
            // TODO: Use SDL_GAMEPAD_TYPE_STEAM on SDL 3.6+
            if (vendorId == 0x28de) {
                switch (productId) {
                case 0x1101:
                case 0x1102:
                case 0x1105:
                case 0x1106:
                case 0x1142:
                case 0x1201:
                case 0x1202:
                case 0x1205:
                case 0x1302:
                case 0x1303:
                case 0x1304:
                case 0x1305:
                    type = LI_CTYPE_STEAM;
                    break;
                default:
                    type = LI_CTYPE_UNKNOWN;
                    break;
                }
            }
            else {
                type = LI_CTYPE_UNKNOWN;
            }
            break;
        }

        // If this is a PlayStation controller that doesn't have a touchpad button mapped,
        // we'll allow the Select+PS button combo to act as the touchpad.
        state->clickpadButtonEmulationEnabled =
#if SDL_VERSION_ATLEAST(2, 0, 14)
            SDL_GameControllerGetBindForButton(state->controller, SDL_CONTROLLER_BUTTON_TOUCHPAD).bindType == SDL_CONTROLLER_BINDTYPE_NONE &&
#endif
            type == LI_CTYPE_PS;

        LiSendControllerArrivalEvent(state->index, m_GamepadMask, type, supportedButtonFlags, capabilities);
#else

        // Send an empty event to tell the PC we've arrived
        sendGamepadState(state);
#endif

        // Send a power level if it's known at this time
        if (powerLevel != SDL_JOYSTICK_POWER_UNKNOWN) {
            sendGamepadBatteryState(state, powerLevel);
        }
    }
    else if (event->type == SDL_CONTROLLERDEVICEREMOVED) {
        state = findStateForGamepad(event->which);
        if (state != NULL) {
            if (state->mouseEmulationTimer != 0) {
                Session::get()->notifyMouseEmulationMode(false);
                SDL_RemoveTimer(state->mouseEmulationTimer);
            }

            stopDualSenseHapticsWriter();
            m_DualSenseHapticsDisabled = false;
            m_DualSenseHapticsUsbAudio = false;
            if (m_DualSenseHapticsAudio != 0) {
                SDL_CloseAudioDevice(m_DualSenseHapticsAudio);
                m_DualSenseHapticsAudio = 0;
            }

            SDL_GameControllerClose(state->controller);

#if !SDL_VERSION_ATLEAST(2, 0, 9)
            if (state->haptic != nullptr) {
                SDL_HapticClose(state->haptic);
            }
#endif

            // Remove this from the gamepad mask in MC-mode
            if (m_MultiController) {
                SDL_assert(m_GamepadMask & (1 << state->index));
                m_GamepadMask &= ~(1 << state->index);
            }
            else {
                SDL_assert(m_GamepadMask == 0x1);
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Gamepad %d is gone",
                        state->index);

            // Send a final event to let the PC know this gamepad is gone
            LiSendMultiControllerEvent(state->index, m_GamepadMask,
                                       0, 0, 0, 0, 0, 0, 0);

            // Clear all remaining state from this slot
            SDL_memset(state, 0, sizeof(*state));
        }
    }
}

void SdlInputHandler::handleJoystickArrivalEvent(SDL_JoyDeviceEvent* event)
{
    SDL_assert(event->type == SDL_JOYDEVICEADDED);

    if (!SDL_IsGameController(event->which)) {
        char guidStr[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(event->which),
                                  guidStr, sizeof(guidStr));
        const char* name = SDL_JoystickNameForIndex(event->which);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Joystick discovered with no mapping: %s %s",
                    name ? name : "<UNKNOWN>",
                    guidStr);
        SDL_Joystick* joy = SDL_JoystickOpen(event->which);
        if (joy != nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Number of axes: %d | Number of buttons: %d | Number of hats: %d",
                        SDL_JoystickNumAxes(joy), SDL_JoystickNumButtons(joy),
                        SDL_JoystickNumHats(joy));
            SDL_JoystickClose(joy);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to open joystick for query: %s",
                        SDL_GetError());
        }
    }
}

void SdlInputHandler::rumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor)
{
    // Make sure the controller number is within our supported count
    if (controllerNumber >= MAX_GAMEPADS) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 9)
    if (m_GamepadState[controllerNumber].controller != nullptr) {
        SDL_GameControllerRumble(m_GamepadState[controllerNumber].controller, lowFreqMotor, highFreqMotor, 30000);
    }
#else
    // Check if the controller supports haptics (and if the controller exists at all)
    SDL_Haptic* haptic = m_GamepadState[controllerNumber].haptic;
    if (haptic == nullptr) {
        return;
    }

    // Stop the last effect we played
    if (m_GamepadState[controllerNumber].hapticMethod == GAMEPAD_HAPTIC_METHOD_LEFTRIGHT) {
        if (m_GamepadState[controllerNumber].hapticEffectId >= 0) {
            SDL_HapticDestroyEffect(haptic, m_GamepadState[controllerNumber].hapticEffectId);
        }
    } else if (m_GamepadState[controllerNumber].hapticMethod == GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE) {
        SDL_HapticRumbleStop(haptic);
    }

    // If this callback is telling us to stop both motors, don't bother queuing a new effect
    if (lowFreqMotor == 0 && highFreqMotor == 0) {
        return;
    }

    if (m_GamepadState[controllerNumber].hapticMethod == GAMEPAD_HAPTIC_METHOD_LEFTRIGHT) {
        SDL_HapticEffect effect;
        SDL_memset(&effect, 0, sizeof(effect));
        effect.type = SDL_HAPTIC_LEFTRIGHT;

        // The effect should last until we are instructed to stop or change it
        effect.leftright.length = SDL_HAPTIC_INFINITY;

        // SDL haptics range from 0-32767 but XInput uses 0-65535, so divide by 2 to correct for SDL's scaling
        effect.leftright.large_magnitude = lowFreqMotor / 2;
        effect.leftright.small_magnitude = highFreqMotor / 2;

        // Play the new effect
        m_GamepadState[controllerNumber].hapticEffectId = SDL_HapticNewEffect(haptic, &effect);
        if (m_GamepadState[controllerNumber].hapticEffectId >= 0) {
            SDL_HapticRunEffect(haptic, m_GamepadState[controllerNumber].hapticEffectId, 1);
        }
    } else if (m_GamepadState[controllerNumber].hapticMethod == GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE) {
        SDL_HapticRumblePlay(haptic,
                             std::min(1.0, (GAMEPAD_HAPTIC_SIMPLE_HIFREQ_MOTOR_WEIGHT*highFreqMotor +
                                            GAMEPAD_HAPTIC_SIMPLE_LOWFREQ_MOTOR_WEIGHT*lowFreqMotor) / 65535.0),
                             SDL_HAPTIC_INFINITY);
    }
#endif
}

void SdlInputHandler::rumbleTriggers(uint16_t controllerNumber, uint16_t leftTrigger, uint16_t rightTrigger)
{
    // Make sure the controller number is within our supported count
    if (controllerNumber >= MAX_GAMEPADS) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (m_GamepadState[controllerNumber].controller != nullptr) {
        SDL_GameControllerRumbleTriggers(m_GamepadState[controllerNumber].controller, leftTrigger, rightTrigger, 30000);
    }
#endif
}

void SdlInputHandler::setMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz)
{
    // Make sure the controller number is within our supported count
    if (controllerNumber >= MAX_GAMEPADS) {
        return;
    }

    uint16_t reportRateHzLimit;
    if (Utils::getEnvironmentVariableOverride("SENSOR_REPORT_RATE_LIMIT_HZ", &reportRateHzLimit) &&
        reportRateHz > reportRateHzLimit) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Sensor report rate limited to %u Hz by environment variable",
                    reportRateHzLimit);
        reportRateHz = reportRateHzLimit;
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (m_GamepadState[controllerNumber].controller != nullptr) {
        uint8_t reportPeriodMs = reportRateHz ? (1000 / reportRateHz) : 0;

        switch (motionType) {
        case LI_MOTION_TYPE_ACCEL:
            m_GamepadState[controllerNumber].accelReportPeriodMs = reportPeriodMs;
            SDL_GameControllerSetSensorEnabled(m_GamepadState[controllerNumber].controller, SDL_SENSOR_ACCEL, reportRateHz ? SDL_TRUE : SDL_FALSE);
            break;

        case LI_MOTION_TYPE_GYRO:
            m_GamepadState[controllerNumber].gyroReportPeriodMs = reportPeriodMs;
            SDL_GameControllerSetSensorEnabled(m_GamepadState[controllerNumber].controller, SDL_SENSOR_GYRO, reportRateHz ? SDL_TRUE : SDL_FALSE);
            break;
        }
    }
#endif
}

void SdlInputHandler::setControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b)
{
    // Make sure the controller number is within our supported count
    if (controllerNumber >= MAX_GAMEPADS) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (m_GamepadState[controllerNumber].controller != nullptr) {
        SDL_GameControllerSetLED(m_GamepadState[controllerNumber].controller, r, g, b);
    }
#endif
}

void SdlInputHandler::setAdaptiveTriggers(uint16_t controllerNumber, DualSenseOutputReport *report){

#if SDL_VERSION_ATLEAST(2, 0, 16)
        // Make sure the controller number is within our supported count
    if (controllerNumber <= MAX_GAMEPADS &&
        // and we have a valid controller
        m_GamepadState[controllerNumber].controller != nullptr &&
        // and it's a PS5 controller
        SDL_GameControllerGetType(m_GamepadState[controllerNumber].controller) == SDL_CONTROLLER_TYPE_PS5) {
        SDL_GameControllerSendEffect(m_GamepadState[controllerNumber].controller, report, sizeof(*report));
    }
#endif

    SDL_free(report);
}

static bool dualSenseSerialMatches(const char *a, const char *b)
{
    if (a == nullptr || b == nullptr) {
        return true;
    }

    // SDL reports the Bluetooth MAC with dashes, while HIDAPI uses colons.
    // Ignore separators and case when comparing.
    while (*a != '\0' && *b != '\0') {
        if (*a == ':' || *a == '-') {
            a++;
            continue;
        }
        if (*b == ':' || *b == '-') {
            b++;
            continue;
        }
        if (SDL_tolower((unsigned char)*a) != SDL_tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    while (*a == ':' || *a == '-') {
        a++;
    }
    while (*b == ':' || *b == '-') {
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static uint32_t dualSenseBluetoothCrc32(const uint8_t *data, size_t length)
{
    // Sony's seeded CRC32 (seed 0xEADA2D49, equivalent to crc32([0xA2]))
    // is required for Bluetooth output reports; the controller rejects a
    // report with a bad checksum.
    uint32_t crc = 0xEADA2D49u ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

struct SdlInputHandler::DualSenseHapticsWriter {
    SDL_Thread* thread = nullptr;
    SDL_mutex* mutex = nullptr;
    uint8_t pending[64] = {};
    bool hasPending = false;
    bool stop = false;
    bool sendPrime = false;
    uint8_t sequence = 0;
    uint8_t packetCounter = 0;
};

static void appendDualSenseCrc(uint8_t *report, std::size_t bodySize)
{
    uint32_t crc = dualSenseBluetoothCrc32(report, bodySize);
    report[bodySize] = static_cast<uint8_t>(crc);
    report[bodySize + 1] = static_cast<uint8_t>(crc >> 8);
    report[bodySize + 2] = static_cast<uint8_t>(crc >> 16);
    report[bodySize + 3] = static_cast<uint8_t>(crc >> 24);
}

static bool extractDualSenseHapticsSamples(const uint8_t *combinedReport, int gain,
                                           uint8_t *samples)
{
    if (combinedReport[0] != 0x36) {
        return false;
    }

    // Modern 0x36 layout: session block 0x11 at 2, haptics 0x12 at 11, samples at 13.
    // Legacy combined layout: a 0x10 state block sits before the haptics block,
    // moving the 0x12 sub-packet to 76 and its samples to 78.
    const std::size_t samplesOffset =
        (combinedReport[11] & 0x7F) == 0x10 ? 78 : 13;
    if ((combinedReport[samplesOffset - 2] & 0x7F) != 0x12 ||
        combinedReport[samplesOffset - 1] != 0x40) {
        return false;
    }

    for (std::size_t i = 0; i < 64; ++i) {
        int value = static_cast<int>(static_cast<int8_t>(combinedReport[samplesOffset + i])) * gain;
        if (value > 127) {
            value = 127;
        }
        else if (value < -128) {
            value = -128;
        }
        samples[i] = static_cast<uint8_t>(static_cast<int8_t>(value));
    }

    return true;
}

static void buildDualSenseHapticsReport(const uint8_t *samples, uint8_t sequence,
                                        uint8_t packetCounter, uint8_t *report)
{
    // Build a 0x32 haptics-only report (142 bytes) with the vDS/DS5Dongle
    // header: sequence tag, 0x11 session block, 0x12 haptics block, seeded CRC.
    // This avoids the slow 398-byte 0x36 write on macOS.
    SDL_memset(report, 0, 142);
    report[0] = 0x32;
    report[1] = (uint8_t)((sequence & 0x0F) << 4);
    report[2] = 0x91;
    report[3] = 0x07;
    report[4] = 0xFE;
    report[5] = 0x40;
    report[6] = 0x40;
    report[7] = 0x40;
    report[8] = 0x40;
    report[9] = 0x40;
    report[10] = packetCounter;
    report[11] = 0x92;
    report[12] = 0x40;
    SDL_memcpy(report + 13, samples, 64);

    appendDualSenseCrc(report, 138);
}

static void buildDualSenseHapticsPrime(uint8_t *prime)
{
    // The Bluetooth audio/haptics lane stays closed until the controller
    // receives this 0x32 init-prime. It embeds a 63-byte state sub-packet
    // (47-byte output state + zeroed tail) that mirrors the routing/volume
    // state used by the reference implementation for haptics-only playback.
    SDL_memset(prime, 0, 142);
    prime[0] = 0x32;
    prime[1] = 0x10;
    prime[2] = 0x90;
    prime[3] = 0x3F;

    uint8_t *state = prime + 4;
    state[0] = 0xB0;  // AllowHeadphoneVolume | AllowSpeakerVolume | AllowAudioControl
    state[1] = 0x80;  // AllowAudioControl2
    state[4] = 0x3F;  // Headphone volume
    state[5] = 0x50;  // Speaker volume
    state[7] = 0x00;  // Output to headphones (haptics are unaffected)
    state[37] = 0x02; // Audio control 2

    appendDualSenseCrc(prime, 138);
}

int SdlInputHandler::dualSenseHapticsWriterThread(void* param)
{
    SdlInputHandler* self = static_cast<SdlInputHandler*>(param);
    DualSenseHapticsWriter* writer = self->m_DualSenseHapticsWriter;
    Uint32 lastFailureLog = 0;
    bool primed = false;
    bool loggedWrite = false;

    for (;;) {
        uint8_t samples[64];
        uint8_t sequence;
        uint8_t packetCounter;
        bool sendPrime;

        SDL_LockMutex(writer->mutex);
        if (writer->stop) {
            SDL_UnlockMutex(writer->mutex);
            break;
        }
        if (!writer->hasPending) {
            SDL_UnlockMutex(writer->mutex);
            SDL_Delay(1);
            continue;
        }
        SDL_memcpy(samples, writer->pending, sizeof(samples));
        writer->hasPending = false;
        sendPrime = writer->sendPrime;
        writer->sendPrime = false;
        sequence = writer->sequence;
        packetCounter = writer->packetCounter;
        writer->sequence = (writer->sequence + 1) & 0x0F;
        writer->packetCounter = writer->packetCounter + 1;
        SDL_UnlockMutex(writer->mutex);

        const Uint32 now = SDL_GetTicks();
        if (sendPrime || !primed) {
            uint8_t prime[142];
            buildDualSenseHapticsPrime(prime);
            if (SDL_hid_write(self->m_DualSenseHapticsDevice, prime, sizeof(prime)) >= 0) {
                if (!primed) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "DualSense Bluetooth HD haptics stream primed");
                }
                primed = true;
            }
            else if ((Uint32)(now - lastFailureLog) > 1000) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "DualSense Bluetooth init-prime failed: %s", SDL_GetError());
                lastFailureLog = now;
            }
        }

        uint8_t report[142];
        buildDualSenseHapticsReport(samples, sequence, packetCounter, report);

        const int written = SDL_hid_write(self->m_DualSenseHapticsDevice, report, sizeof(report));
        if (written < 0) {
            const Uint32 failureNow = SDL_GetTicks();
            if ((Uint32)(failureNow - lastFailureLog) > 1000) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "DualSense HD haptics write failed: %s", SDL_GetError());
                lastFailureLog = failureNow;
            }
        }
        else if (!loggedWrite) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "DualSense HD haptics write OK (%d bytes)", written);
            loggedWrite = true;
        }
    }

    return 0;
}

void SdlInputHandler::stopDualSenseHapticsWriter()
{
    DualSenseHapticsWriter* writer = m_DualSenseHapticsWriter;
    if (writer != nullptr) {
        if (writer->mutex != nullptr) {
            SDL_LockMutex(writer->mutex);
            writer->stop = true;
            SDL_UnlockMutex(writer->mutex);
        }
        if (writer->thread != nullptr) {
            SDL_WaitThread(writer->thread, nullptr);
        }
        if (writer->mutex != nullptr) {
            SDL_DestroyMutex(writer->mutex);
        }
        delete writer;
        m_DualSenseHapticsWriter = nullptr;
    }

    if (m_DualSenseHapticsDevice != nullptr) {
        SDL_hid_close(m_DualSenseHapticsDevice);
        m_DualSenseHapticsDevice = nullptr;
    }

    m_DualSenseHapticsLastPacket = 0;
}

static bool hasDualSenseAudioOutput()
{
    const int count = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < count; ++i) {
        const char *name = SDL_GetAudioDeviceName(i, 0);
        if (name == nullptr ||
            (SDL_strcasestr(name, "DualSense") == nullptr &&
             SDL_strcasestr(name, "Wireless Controller") == nullptr)) {
            continue;
        }

        // The USB haptics endpoint exposes 4 channels. Bluetooth audio (if any)
        // is stereo, so require 4 channels to distinguish the USB haptics path.
        SDL_AudioSpec spec = {};
        if (SDL_GetAudioDeviceSpec(i, 0, &spec) == 0 && spec.channels == 4) {
            return true;
        }
    }
    return false;
}

static SDL_hid_device *openDualSenseHapticsDevice(SDL_GameController *controller)
{
    SDL_hid_device_info *devices = SDL_hid_enumerate(0x054C, 0x0CE6);
    if (devices == nullptr) {
        devices = SDL_hid_enumerate(0x054C, 0x0DF2);
    }
    if (devices == nullptr) {
        return nullptr;
    }

    const char *serial = SDL_JoystickGetSerial(SDL_GameControllerGetJoystick(controller));
    SDL_hid_device *result = nullptr;
    for (SDL_hid_device_info *info = devices; info != nullptr; info = info->next) {
        if (info->vendor_id != 0x054C || (info->product_id != 0x0CE6 && info->product_id != 0x0DF2)) {
            continue;
        }
#if SDL_VERSION_ATLEAST(3, 0, 0)
        if (!dualSenseSerialMatches(serial, info->serial_number)) {
            continue;
        }
#else
        if (serial != nullptr && info->serial_number != nullptr) {
            // SDL2 exposes HID serial numbers as wchar_t, while
            // SDL_JoystickGetSerial() returns UTF-8. Convert before comparing.
            // The USB DualSense exposes an empty HID serial, so treat that as
            // a match rather than rejecting the only connected controller.
            char *infoSerial = SDL_iconv_wchar_utf8(info->serial_number);
            bool mismatch = infoSerial != nullptr && infoSerial[0] != '\0' &&
                            !dualSenseSerialMatches(serial, infoSerial);
            SDL_free(infoSerial);
            if (mismatch) {
                continue;
            }
        }
#endif
        result = SDL_hid_open_path(info->path, 0);
        if (result != nullptr) {
            SDL_hid_set_nonblocking(result, 1);
            break;
        }
    }

    SDL_hid_free_enumeration(devices);
    return result;
}

void SdlInputHandler::setDualSenseHaptics(uint16_t controllerNumber, uint8_t *report)
{
    if (SDL_getenv("MOONLIGHT_DISABLE_DS_HAPTICS") != nullptr || report == nullptr) {
        SDL_free(report);
        return;
    }

    static int packetLogCount = 0;
    if (packetLogCount < 10) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "DualSense HD haptics packet #%d: id=0x%02X size=%d first=[%02X %02X %02X %02X] last=[%02X %02X %02X %02X]",
                    packetLogCount, report[0], SS_DS_HAPTICS_REPORT_SIZE,
                    report[0], report[1], report[2], report[3],
                    report[SS_DS_HAPTICS_REPORT_SIZE - 4], report[SS_DS_HAPTICS_REPORT_SIZE - 3],
                    report[SS_DS_HAPTICS_REPORT_SIZE - 2], report[SS_DS_HAPTICS_REPORT_SIZE - 1]);
        packetLogCount++;
    }

#if SDL_VERSION_ATLEAST(2, 0, 16)
    // Make sure the controller number is within our supported count
    if (controllerNumber < MAX_GAMEPADS &&
        // and we have a valid controller
        m_GamepadState[controllerNumber].controller != nullptr &&
        SDL_GameControllerGetAttached(m_GamepadState[controllerNumber].controller) &&
        // and it's a PS5 controller
        SDL_GameControllerGetType(m_GamepadState[controllerNumber].controller) == SDL_CONTROLLER_TYPE_PS5) {
        if (m_DualSenseHapticsDevice == nullptr && !m_DualSenseHapticsDisabled) {
            m_DualSenseHapticsDevice = openDualSenseHapticsDevice(m_GamepadState[controllerNumber].controller);
            if (m_DualSenseHapticsDevice == nullptr) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unable to open DualSense for HD haptics");
                m_DualSenseHapticsDisabled = true;
            }
            else {
                m_DualSenseHapticsUsbAudio = hasDualSenseAudioOutput();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DualSense HD haptics device opened (%s)",
                            m_DualSenseHapticsUsbAudio ? "USB audio" : "Bluetooth HID");

                if (!m_DualSenseHapticsUsbAudio) {
                    DualSenseHapticsWriter* writer = new DualSenseHapticsWriter;
                    writer->mutex = SDL_CreateMutex();
                    if (writer->mutex != nullptr) {
                        m_DualSenseHapticsWriter = writer;
                        writer->thread = SDL_CreateThread(dualSenseHapticsWriterThread, "DSHaptics", this);
                        if (writer->thread == nullptr) {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                        "Unable to start DualSense HD haptics writer: %s", SDL_GetError());
                            SDL_DestroyMutex(writer->mutex);
                            delete writer;
                            m_DualSenseHapticsWriter = nullptr;
                            m_DualSenseHapticsDisabled = true;
                        }
                    }
                    else {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                    "Unable to create DualSense HD haptics mutex: %s", SDL_GetError());
                        delete writer;
                        m_DualSenseHapticsDisabled = true;
                    }
                }
            }
        }

        // The host builds a 398-byte Bluetooth report 0x36. Repackage its
        // haptics block into a 142-byte 0x32 report so the blocking HID write
        // happens on a worker thread and is fast enough on macOS.
        if (m_DualSenseHapticsWriter != nullptr && !m_DualSenseHapticsUsbAudio) {
            static const int btGain = [] {
                int value = 2;
                const char *env = SDL_getenv("MOONLIGHT_DS_HAPTICS_BT_GAIN");
                if (env != nullptr) {
                    int parsed = SDL_atoi(env);
                    if (parsed > 0 && parsed <= 8) {
                        value = parsed;
                    }
                }
                return value;
            }();

            uint8_t samples[64];
            if (extractDualSenseHapticsSamples(report, btGain, samples)) {
                const Uint32 now = SDL_GetTicks();
                const bool gap = m_DualSenseHapticsLastPacket == 0 ||
                                 (Uint32)(now - m_DualSenseHapticsLastPacket) > 250;

                DualSenseHapticsWriter* writer = m_DualSenseHapticsWriter;
                SDL_LockMutex(writer->mutex);
                SDL_memcpy(writer->pending, samples, sizeof(samples));
                writer->hasPending = true;
                if (gap) {
                    writer->sendPrime = true;
                }
                SDL_UnlockMutex(writer->mutex);

                m_DualSenseHapticsLastPacket = now;
            }
            else {
                static Uint32 lastLayoutLog = 0;
                const Uint32 now = SDL_GetTicks();
                if ((Uint32)(now - lastLayoutLog) > 1000) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Unexpected DualSense haptics report layout: id=0x%02X block=0x%02X/%u",
                                report[0], report[11], report[12]);
                    lastLayoutLog = now;
                }
            }
        }
    }
#endif

    SDL_free(report);
}

static SDL_AudioDeviceID openDualSenseHapticsAudio()
{
    const int count = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < count; ++i) {
        const char *name = SDL_GetAudioDeviceName(i, 0);
        if (name == nullptr) {
            continue;
        }
        if (SDL_strcasestr(name, "DualSense") == nullptr &&
            SDL_strcasestr(name, "Wireless Controller") == nullptr) {
            continue;
        }

        SDL_AudioSpec want = {};
        want.freq = 48000;
        want.format = AUDIO_S16LSB;
        want.channels = 4;
        want.samples = 480;

        SDL_AudioDeviceID id = SDL_OpenAudioDevice(name, 0, &want, nullptr, 0);
        if (id != 0) {
            SDL_PauseAudioDevice(id, 0);
            return id;
        }
    }
    return 0;
}

void SdlInputHandler::setDualSenseRearHaptics(uint16_t controllerNumber, uint8_t *pcm)
{
    if (SDL_getenv("MOONLIGHT_DISABLE_DS_HAPTICS") != nullptr) {
        SDL_free(pcm);
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 16)
    if (controllerNumber < MAX_GAMEPADS &&
        m_GamepadState[controllerNumber].controller != nullptr &&
        SDL_GameControllerGetType(m_GamepadState[controllerNumber].controller) == SDL_CONTROLLER_TYPE_PS5) {
        if (m_DualSenseHapticsAudio == 0 && !m_DualSenseHapticsAudioDisabled) {
            m_DualSenseHapticsAudio = openDualSenseHapticsAudio();
            if (m_DualSenseHapticsAudio == 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unable to open DualSense USB haptics audio");
                m_DualSenseHapticsAudioDisabled = true;
            }
        }

        if (m_DualSenseHapticsAudio != 0) {
            constexpr int frames = 480;
            if (SDL_GetQueuedAudioSize(m_DualSenseHapticsAudio) <
                frames * 4 * static_cast<int>(sizeof(Sint16)) * 4) {
                static const int gain = [] {
                    int value = 3;
                    const char *env = SDL_getenv("MOONLIGHT_DS_HAPTICS_GAIN");
                    if (env != nullptr) {
                        int parsed = SDL_atoi(env);
                        if (parsed > 0 && parsed <= 16) {
                            value = parsed;
                        }
                    }
                    return value;
                }();
                auto clamp = [](int32_t value) -> Sint16 {
                    if (value > 32767) return 32767;
                    if (value < -32768) return -32768;
                    return static_cast<Sint16>(value);
                };
                Sint16 interleaved[frames * 4] = {};
                const Sint16 *rear = reinterpret_cast<const Sint16 *>(pcm);
                for (int i = 0; i < frames; ++i) {
                    interleaved[i * 4 + 2] = clamp(static_cast<int32_t>(rear[i * 2]) * gain);
                    interleaved[i * 4 + 3] = clamp(static_cast<int32_t>(rear[i * 2 + 1]) * gain);
                }
                SDL_QueueAudio(m_DualSenseHapticsAudio, interleaved, sizeof(interleaved));
            }
        }
    }
#endif

    SDL_free(pcm);
}

QString SdlInputHandler::getUnmappedGamepads()
{
    QString ret;

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) failed: %s",
                     SDL_GetError());
    }

    MappingManager mappingManager;
    mappingManager.applyMappings();

    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (!SDL_IsGameController(i)) {
            char guidStr[33];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),
                                      guidStr, sizeof(guidStr));
            const char* name = SDL_JoystickNameForIndex(i);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Unmapped joystick: %s %s",
                        name ? name : "<UNKNOWN>",
                        guidStr);
            SDL_Joystick* joy = SDL_JoystickOpen(i);
            if (joy != nullptr) {
                int numButtons = SDL_JoystickNumButtons(joy);
                int numHats = SDL_JoystickNumHats(joy);
                int numAxes = SDL_JoystickNumAxes(joy);

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Number of axes: %d | Number of buttons: %d | Number of hats: %d",
                            numAxes, numButtons, numHats);

                if ((numAxes >= 4 && numAxes <= 8) && numButtons >= 8 && numHats <= 1) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Joystick likely to be an unmapped game controller");
                    if (!ret.isEmpty()) {
                        ret += ", ";
                    }

                    ret += name;
                }

                SDL_JoystickClose(joy);
            }
            else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Unable to open joystick for query: %s",
                            SDL_GetError());
            }
        }
    }

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);

    // Flush stale events so they aren't processed by the main session event loop
    SDL_FlushEvents(SDL_JOYDEVICEADDED, SDL_JOYDEVICEREMOVED);
    SDL_FlushEvents(SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEREMAPPED);

    return ret;
}

int SdlInputHandler::getAttachedGamepadMask()
{
    int count;
    int mask;

    if (!m_MultiController) {
        // Player 1 is always present in non-MC mode
        return 0x1;
    }

    count = mask = 0;
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            char guidStr[33];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),
                                      guidStr, sizeof(guidStr));

            if (!m_IgnoreDeviceGuids.contains(guidStr, Qt::CaseInsensitive))
            {
                mask |= (1 << count++);
            }
        }
    }

    return mask;
}
