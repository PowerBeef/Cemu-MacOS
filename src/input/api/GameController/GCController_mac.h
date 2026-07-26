#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Plain-C++ view of a GameController.framework device, so the rest of the input
// layer never has to include Objective-C headers. Implemented in GCController_mac.mm.
namespace GCBridge
{
	struct DeviceInfo
	{
		std::string uuid;
		std::string displayName;
	};

	// Snapshot of the standard extended-gamepad profile. Every supported controller
	// reports this same layout regardless of vendor -- that is the whole point of
	// GameController.framework and why no per-button mapping is needed.
	struct GamepadState
	{
		bool connected = false;

		// face buttons
		bool a = false, b = false, x = false, y = false;
		// shoulders
		bool leftShoulder = false, rightShoulder = false;
		// dpad
		bool up = false, down = false, left = false, right = false;
		// menu / options / home
		bool menu = false, options = false, home = false;
		// thumbstick clicks (optional on some pads)
		bool leftThumbstick = false, rightThumbstick = false;

		// analog, [-1, 1] for sticks and [0, 1] for triggers
		float leftStickX = 0.0f, leftStickY = 0.0f;
		float rightStickX = 0.0f, rightStickY = 0.0f;
		float leftTrigger = 0.0f, rightTrigger = 0.0f;

		bool hasBattery = false;
		float batteryLevel = 1.0f;   // [0, 1]
		bool batteryLow = false;

		bool hasMotion = false;
		double gyroX = 0.0, gyroY = 0.0, gyroZ = 0.0;
		double accelX = 0.0, accelY = 0.0, accelZ = 0.0;

		bool hasHaptics = false;
	};

	// Starts observing connect/disconnect notifications. Idempotent.
	void Initialize();
	void Shutdown();

	std::vector<DeviceInfo> EnumerateDevices();
	bool ReadState(const std::string& uuid, GamepadState& out);

	bool SetRumble(const std::string& uuid, float intensity);
	bool HasHaptics(const std::string& uuid);
}
