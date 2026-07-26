#pragma once

#include "input/api/Controller.h"
#include "input/api/GameController/GCControllerProvider.h"
#include "input/motion/MotionHandler.h"

#include <chrono>

class GCGamepadController : public Controller<GCControllerProvider>
{
public:
	GCGamepadController(std::string_view uuid, std::string_view display_name);

	std::string_view api_name() const override
	{
		static_assert(to_string(InputAPI::GameController) == "GameController");
		return to_string(InputAPI::GameController);
	}
	InputAPI::Type api() const override { return InputAPI::GameController; }

	bool is_connected() override;
	bool connect() override;

	bool has_battery() override;
	bool has_low_battery() override;

	bool has_motion() override;
	MotionSample get_motion_sample() override;

	std::string get_button_name(uint64 button) const override;

protected:
	ControllerState raw_state() override;

private:
	bool m_isConnected = false;

	// GameController reports raw gyro/accelerometer; the VPAD API wants fused
	// orientation, so run it through the same handler the SDL and DSU paths use.
	WiiUMotionHandler m_motionHandler;
	std::chrono::steady_clock::time_point m_lastMotionSample{};
	bool m_hasMotionSample = false;
};
