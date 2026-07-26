#include "util/ScreenSaver/ScreenSaver.h"
#include "Cemu/Logging/CemuLogging.h"

#import <Foundation/Foundation.h>

namespace
{
	// NSProcessInfo activity token. Held for as long as emulation is running.
	id<NSObject> s_activity = nil;
}

void ScreenSaver::SetInhibit(bool inhibit)
{
	// -beginActivityWithOptions: rather than IOPMAssertionCreateWithName: the activity
	// is scoped to the process and released automatically if we die, whereas an orphaned
	// IOPMAssertion keeps the user's display awake until reboot. It also disables App Nap
	// and timer coalescing via NSActivityUserInitiated, which is exactly what we want
	// while a game is running.
	//
	// Safe to call from any thread.
	if (inhibit)
	{
		if (s_activity)
			return;
		s_activity = [[[NSProcessInfo processInfo]
			beginActivityWithOptions:(NSActivityIdleDisplaySleepDisabled |
			                          NSActivityIdleSystemSleepDisabled |
			                          NSActivityUserInitiated)
			                  reason:@"Emulating a Wii U title"] retain];
	}
	else
	{
		if (!s_activity)
			return;
		[[NSProcessInfo processInfo] endActivity:s_activity];
		[s_activity release];
		s_activity = nil;
	}
}
