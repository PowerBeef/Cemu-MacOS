#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "Common/precompiled.h"

#include <mach/mach_time.h>

namespace HighResolutionTimerInternal
{
	uint64 s_timebaseNumer = 125;
	uint64 s_timebaseDenom = 3;

	// The defaults above are the Apple silicon values, so a clock read that happens during
	// static init -- before this initializer runs -- still returns the right answer on
	// every machine we support, rather than dividing by zero.
	static const int s_timebaseInit = []() -> int {
		mach_timebase_info_data_t tb{};
		if (mach_timebase_info(&tb) == KERN_SUCCESS && tb.numer != 0 && tb.denom != 0)
		{
			s_timebaseNumer = tb.numer;
			s_timebaseDenom = tb.denom;
		}
		return 0;
	}();
}

HRTick HighResolutionTimer::getFrequency()
{
	return m_freq;
}

// Ticks remain nanoseconds, exactly as before, so every existing HRTick comparison and
// conversion keeps its meaning. Only how a tick is obtained changed -- see now().
uint64 HighResolutionTimer::m_freq = 1000000000;
