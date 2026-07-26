#include "input/api/GameController/GCGamepadController.h"
#include "input/api/GameController/GCController_mac.h"

GCGamepadController::GCGamepadController(std::string_view uuid, std::string_view display_name)
	: base_type(uuid, display_name)
{
	GCBridge::GamepadState state;
	m_isConnected = GCBridge::ReadState(std::string{uuid}, state);
}

bool GCGamepadController::is_connected()
{
	return m_isConnected;
}

bool GCGamepadController::connect()
{
	GCBridge::GamepadState state;
	m_isConnected = GCBridge::ReadState(m_uuid, state);
	return m_isConnected;
}

bool GCGamepadController::has_battery()
{
	GCBridge::GamepadState state;
	if (!GCBridge::ReadState(m_uuid, state))
		return false;
	return state.hasBattery;
}

bool GCGamepadController::has_low_battery()
{
	GCBridge::GamepadState state;
	if (!GCBridge::ReadState(m_uuid, state))
		return false;
	return state.hasBattery && state.batteryLow;
}

bool GCGamepadController::has_motion()
{
	GCBridge::GamepadState state;
	if (!GCBridge::ReadState(m_uuid, state))
		return false;
	return state.hasMotion;
}

MotionSample GCGamepadController::get_motion_sample()
{
	GCBridge::GamepadState state;
	if (!GCBridge::ReadState(m_uuid, state) || !state.hasMotion)
		return {};

	// GCMotion reports rotationRate in radians/second and acceleration in g, which is
	// what WiiUMotionHandler expects. It does the Mahony fusion and derives the
	// orientation and quaternion the VPAD API needs.
	const auto now = std::chrono::steady_clock::now();
	float deltaTime = 1.0f / 60.0f;
	if (m_hasMotionSample)
	{
		deltaTime = std::chrono::duration<float>(now - m_lastMotionSample).count();
		// clamp: a stalled frame would otherwise integrate a huge step into the IMU
		deltaTime = std::clamp(deltaTime, 1.0f / 1000.0f, 1.0f / 10.0f);
	}
	m_lastMotionSample = now;
	m_hasMotionSample = true;

	m_motionHandler.processMotionSample(deltaTime,
		(float)state.gyroX, (float)state.gyroY, (float)state.gyroZ,
		(float)state.accelX, (float)state.accelY, (float)state.accelZ);
	return m_motionHandler.getMotionSample();
}

ControllerState GCGamepadController::raw_state()
{
	ControllerState result{};

	GCBridge::GamepadState s;
	if (!GCBridge::ReadState(m_uuid, s))
	{
		m_isConnected = false;
		return result;
	}
	m_isConnected = true;

	// Element order matches the SDL provider's button numbering so the existing
	// generic default mapping in VPADController::set_default_mapping applies
	// unchanged. That is the point of this provider: the OS hands us a standard
	// profile, so no per-device mapping table is needed.
	auto setButton = [&result](uint32 id, bool pressed) { result.buttons.SetButtonState(id, pressed); };

	setButton(kButton0, s.b);              // south -> matches SDL's index 0
	setButton(kButton1, s.a);
	setButton(kButton2, s.y);
	setButton(kButton3, s.x);

	setButton(kButton4, s.options);        // "minus" equivalent
	setButton(kButton5, s.home);
	setButton(kButton6, s.menu);           // "plus" equivalent

	setButton(kButton7, s.leftThumbstick);
	setButton(kButton8, s.rightThumbstick);
	setButton(kButton9, s.leftShoulder);
	setButton(kButton10, s.rightShoulder);

	setButton(kButton11, s.up);
	setButton(kButton12, s.down);
	setButton(kButton13, s.left);
	setButton(kButton14, s.right);

	result.axis.x = s.leftStickX;
	result.axis.y = s.leftStickY;
	result.rotation.x = s.rightStickX;
	result.rotation.y = s.rightStickY;
	result.trigger.x = s.leftTrigger;
	result.trigger.y = s.rightTrigger;

	return result;
}

std::string GCGamepadController::get_button_name(uint64 button) const
{
	// Names follow the standard extended-gamepad profile rather than a vendor
	// layout, since that is what the OS guarantees.
	switch (button)
	{
	case kButton0: return "B";
	case kButton1: return "A";
	case kButton2: return "Y";
	case kButton3: return "X";
	case kButton4: return "Options";
	case kButton5: return "Home";
	case kButton6: return "Menu";
	case kButton7: return "LS";
	case kButton8: return "RS";
	case kButton9: return "LB";
	case kButton10: return "RB";
	case kButton11: return "DPad Up";
	case kButton12: return "DPad Down";
	case kButton13: return "DPad Left";
	case kButton14: return "DPad Right";
	default: return base_type::get_button_name(button);
	}
}
