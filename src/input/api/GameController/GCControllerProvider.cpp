#include "input/api/GameController/GCControllerProvider.h"
#include "input/api/GameController/GCController_mac.h"
#include "input/api/GameController/GCGamepadController.h"
#include "Cemu/Logging/CemuLogging.h"

GCControllerProvider::GCControllerProvider()
{
	GCBridge::Initialize();
}

GCControllerProvider::~GCControllerProvider()
{
	GCBridge::Shutdown();
}

std::vector<GCDeviceInfo> GCControllerProvider::EnumerateDevices()
{
	std::vector<GCDeviceInfo> result;
	for (const auto& d : GCBridge::EnumerateDevices())
		result.push_back({d.uuid, d.displayName, true});
	return result;
}

std::vector<std::shared_ptr<ControllerBase>> GCControllerProvider::get_controllers()
{
	const auto devices = GCBridge::EnumerateDevices();

	// GCController.controllers is empty until the run loop has spun at least once --
	// the framework delivers devices via notifications, not synchronously. Log the
	// first non-empty enumeration so it is obvious whether the OS handed us anything,
	// which is otherwise indistinguishable from "no controller connected".
	static bool s_loggedDevices = false;
	if (!s_loggedDevices && !devices.empty())
	{
		s_loggedDevices = true;
		for (const auto& d : devices)
			cemuLog_log(LogType::Force, "GameController: detected \"{}\" (id {})", d.displayName, d.uuid);
	}

	std::vector<std::shared_ptr<ControllerBase>> result;
	result.reserve(devices.size());
	for (const auto& d : devices)
		result.emplace_back(std::make_shared<GCGamepadController>(d.uuid, d.displayName));
	return result;
}
