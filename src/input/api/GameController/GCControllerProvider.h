#pragma once

#include "input/api/ControllerProvider.h"

#include <memory>
#include <string>
#include <vector>

// Input provider backed by Apple's GameController.framework.
//
// Why this exists alongside the SDL provider: SDL is already built on
// GameController.framework for supported pads, so basic buttons and axes are
// equivalent. What SDL does not surface is the platform-specific layer --
// per-controller battery level, DualSense adaptive triggers and haptics, the
// controller light, and the user's own remapping profiles configured in
// System Settings > Game Controllers. Those profiles are applied by the OS
// before input reaches us, so a controller the user has remapped there behaves
// as they expect without Cemu knowing anything about it.
//
// SDL remains the provider for everything GameController does not adopt --
// notably Wiimotes, whose HID transport runs through SDL.
struct GCDeviceInfo
{
	std::string uuid;         // stable identity used in controller profiles
	std::string displayName;
	bool isConnected = false;
};

class GCControllerProvider : public ControllerProviderBase
{
public:
	GCControllerProvider();
	~GCControllerProvider() override;

	inline static InputAPI::Type kAPIType = InputAPI::GameController;
	InputAPI::Type api() const override { return kAPIType; }

	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

	// Implemented in the Objective-C++ translation unit.
	static std::vector<GCDeviceInfo> EnumerateDevices();
};
