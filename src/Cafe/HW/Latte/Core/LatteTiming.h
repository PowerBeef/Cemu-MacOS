#pragma once

void LatteTiming_setCustomVsyncFrequency(sint32 frequency);
void LatteTiming_disableCustomVsyncFrequency();
bool LatteTiming_getCustomVsyncFrequency(sint32& customFrequency);

void LatteTiming_EnableHostDrivenVSync();

// Diagnostics for the IT_WAIT_REG_MEM fence stall. The stall lasts ~15.3 ms whether the
// title is drawing 113 objects or 3,516, which rules out any work dependency and points at
// a timer. Emulated vsync IS a timer, polled by the Latte thread from inside that very
// wait loop, so the test is whether the fence releases when a vsync fires or at an
// unrelated moment. Read on the Latte thread only, which is also the only writer.
uint64 LatteTiming_GetVsyncSignalCount();
uint64 LatteTiming_GetFlipSignalCount();
uint64 LatteTiming_GetLastVsyncTick();