#include "input/api/GameController/GCController_mac.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <mutex>
#include <unordered_map>

namespace
{
	std::mutex g_mutex;
	bool g_initialized = false;
	id g_connectObserver = nil;
	id g_disconnectObserver = nil;

	// GCController objects are owned by the framework and come and go with the
	// hardware, so we key our own map by a stable identity string rather than
	// holding pointers across frames.
	std::unordered_map<std::string, GCController*> g_controllers;

	// GameController exposes no persistent per-device UUID. productCategory plus
	// the player index is not stable either. vendorName plus the object's address
	// would change across reconnects, so we synthesise a stable-enough identity
	// from the vendor name and a per-name occurrence counter -- matching how the
	// SDL provider disambiguates identical devices by index.
	std::string MakeIdentity(GCController* c, int occurrence)
	{
		const char* name = c.vendorName ? c.vendorName.UTF8String : "Game Controller";
		return std::string(name) + "_" + std::to_string(occurrence);
	}

	void RefreshControllerMapLocked()
	{
		g_controllers.clear();
		std::unordered_map<std::string, int> nameCount;
		for (GCController* c in GCController.controllers)
		{
			const char* name = c.vendorName ? c.vendorName.UTF8String : "Game Controller";
			const int occurrence = nameCount[name]++;
			g_controllers[MakeIdentity(c, occurrence)] = c;
		}
	}

	GCExtendedGamepad* ExtendedProfile(GCController* c)
	{
		return c ? c.extendedGamepad : nil;
	}
}

namespace GCBridge
{
	void Initialize()
	{
		std::lock_guard lock(g_mutex);
		if (g_initialized)
			return;
		g_initialized = true;

		// Hot-plug. Without these the controller list is only correct at startup,
		// which is precisely the case that fails when a pad is switched on after launch.
		g_connectObserver = [[NSNotificationCenter defaultCenter]
			addObserverForName:GCControllerDidConnectNotification
			            object:nil
			             queue:nil
			        usingBlock:^(NSNotification*) {
				std::lock_guard l(g_mutex);
				RefreshControllerMapLocked();
			}];
		g_disconnectObserver = [[NSNotificationCenter defaultCenter]
			addObserverForName:GCControllerDidDisconnectNotification
			            object:nil
			             queue:nil
			        usingBlock:^(NSNotification*) {
				std::lock_guard l(g_mutex);
				RefreshControllerMapLocked();
			}];

		RefreshControllerMapLocked();
	}

	void Shutdown()
	{
		std::lock_guard lock(g_mutex);
		if (!g_initialized)
			return;
		if (g_connectObserver)
			[[NSNotificationCenter defaultCenter] removeObserver:g_connectObserver];
		if (g_disconnectObserver)
			[[NSNotificationCenter defaultCenter] removeObserver:g_disconnectObserver];
		g_connectObserver = nil;
		g_disconnectObserver = nil;
		g_controllers.clear();
		g_initialized = false;
	}

	std::vector<DeviceInfo> EnumerateDevices()
	{
		std::lock_guard lock(g_mutex);
		if (!g_initialized)
			return {};
		// The notification blocks keep the map fresh, but re-scan anyway: a controller
		// connected before Initialize() ran would otherwise never appear.
		RefreshControllerMapLocked();

		std::vector<DeviceInfo> result;
		result.reserve(g_controllers.size());
		for (const auto& [uuid, c] : g_controllers)
		{
			// Only adopt devices that expose the standard extended profile. Anything
			// else (Siri Remote and similar) has too few inputs to drive a Wii U pad.
			if (!ExtendedProfile(c))
				continue;
			DeviceInfo info;
			info.uuid = uuid;
			info.displayName = c.vendorName ? c.vendorName.UTF8String : "Game Controller";
			result.push_back(std::move(info));
		}
		return result;
	}

	bool ReadState(const std::string& uuid, GamepadState& out)
	{
		std::lock_guard lock(g_mutex);
		auto it = g_controllers.find(uuid);
		if (it == g_controllers.end())
			return false;
		GCController* c = it->second;
		GCExtendedGamepad* gp = ExtendedProfile(c);
		if (!gp)
			return false;

		out.connected = true;

		out.a = gp.buttonA.pressed;
		out.b = gp.buttonB.pressed;
		out.x = gp.buttonX.pressed;
		out.y = gp.buttonY.pressed;

		out.leftShoulder = gp.leftShoulder.pressed;
		out.rightShoulder = gp.rightShoulder.pressed;

		out.up = gp.dpad.up.pressed;
		out.down = gp.dpad.down.pressed;
		out.left = gp.dpad.left.pressed;
		out.right = gp.dpad.right.pressed;

		out.menu = gp.buttonMenu.pressed;
		out.options = gp.buttonOptions ? gp.buttonOptions.pressed : false;
		out.home = gp.buttonHome ? gp.buttonHome.pressed : false;

		out.leftThumbstick = gp.leftThumbstickButton ? gp.leftThumbstickButton.pressed : false;
		out.rightThumbstick = gp.rightThumbstickButton ? gp.rightThumbstickButton.pressed : false;

		out.leftStickX = gp.leftThumbstick.xAxis.value;
		out.leftStickY = gp.leftThumbstick.yAxis.value;
		out.rightStickX = gp.rightThumbstick.xAxis.value;
		out.rightStickY = gp.rightThumbstick.yAxis.value;
		out.leftTrigger = gp.leftTrigger.value;
		out.rightTrigger = gp.rightTrigger.value;

		if (c.battery)
		{
			out.hasBattery = true;
			out.batteryLevel = c.battery.batteryLevel;
			// GCDeviceBatteryState has no "critical" case (only Unknown/Discharging/
			// Charging/Full), so low battery is derived from the level, and only while
			// actually discharging -- a pad at 15% on the cable is not low.
			out.batteryLow = (c.battery.batteryState == GCDeviceBatteryStateDischarging) &&
			                 (out.batteryLevel > 0.0f && out.batteryLevel < 0.2f);
		}

		if (c.motion)
		{
			out.hasMotion = true;
			out.gyroX = c.motion.rotationRate.x;
			out.gyroY = c.motion.rotationRate.y;
			out.gyroZ = c.motion.rotationRate.z;
			out.accelX = c.motion.acceleration.x;
			out.accelY = c.motion.acceleration.y;
			out.accelZ = c.motion.acceleration.z;
		}

		out.hasHaptics = (c.haptics != nil);
		return true;
	}

	bool HasHaptics(const std::string& uuid)
	{
		std::lock_guard lock(g_mutex);
		auto it = g_controllers.find(uuid);
		if (it == g_controllers.end())
			return false;
		return it->second.haptics != nil;
	}

	bool SetRumble(const std::string& uuid, float intensity)
	{
		// Rumble through GCHaptics needs a CHHapticEngine and a running pattern
		// player. Not wired up yet -- reporting false keeps Cemu from advertising
		// rumble it cannot deliver, rather than silently doing nothing.
		(void)uuid;
		(void)intensity;
		return false;
	}
}
