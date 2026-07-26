#include "Cafe/HW/Espresso/Const.h"
#include "config/ActiveSettings.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "Common/cpu_features.h"

#include <atomic>

// Espresso timebase, derived from the ARM generic timer.
//
// This used to spend 3 real seconds at every launch measuring the counter frequency
// against a wall clock, then read the result under a global spinlock with a 128-bit
// multiply and a 128-bit divide per call. None of that is needed on Apple silicon:
// cntfrq_el0 reports the frequency exactly, in one instruction, and its ratio to the
// Espresso core clock is a small exact rational.
//
//   CORE_CLOCK / cntfrq = 1243125000 / 24000000 = 3315 / 64   (exact, not an approximation)
//
// The old code then scaled by <<3 and >>timerShiftFactor, so the whole conversion
// collapses to one multiply and one shift:
//
//   guestTicks = counterDelta * (3315/64) * 8 >> shift
//              = (counterDelta * 3315) >> (3 + shift)
//
// This sits on OSGetSystemTime, on guest `mftb`, and on every scheduler thread wake, so
// it is one of the hottest non-JIT paths in the emulator.

static uint64 sCounterFrequency = 0;

// The exact rational is only valid at the frequency it was derived for. Any other part
// falls back to a general path that is slower but still correct.
static constexpr uint64 EXPECTED_COUNTER_FREQ = 24000000;
static constexpr uint64 CORE_CLOCK_NUMERATOR = 3315; // CORE_CLOCK/cntfrq == 3315/64
static bool sUseExactPath = false;

static inline uint64 ReadCounter()
{
	uint64 t;
	asm volatile("mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

// Timebase origin. The returned tick count is a pure function of the hardware counter and
// this origin, which makes it inherently monotonic and lock-free -- there is no
// accumulator, so the read path mutates nothing and needs no lock to serialise it.
//
// The origin only moves when the user changes emulation speed, because the shift factor
// applies to time elapsed from that point on. Rescaling from the raw counter instead would
// retroactively reinterpret time already elapsed and jump the guest clock. A seqlock keeps
// that rare update consistent for readers without imposing a lock on them.
struct TimerOrigin
{
	uint64 baseCounter;
	uint64 baseTicks;
	uint8 shift;
};

static std::atomic<uint32> sOriginSeq{0};
static TimerOrigin sOrigin{};

static inline uint64 ScaleToTicks(uint64 counterDelta, uint8 shift)
{
	if (sUseExactPath) [[likely]]
	{
		// counterDelta * 3315 cannot overflow: that needs ~7 years of uptime at 24 MHz.
		return (counterDelta * CORE_CLOCK_NUMERATOR) >> (3 + shift);
	}
	// General path: (delta * CORE_CLOCK / freq) << 3 >> shift, widened against overflow.
	unsigned __int128 scaled = (unsigned __int128)counterDelta * Espresso::CORE_CLOCK;
	return (uint64)(scaled / sCounterFrequency) << 3 >> shift;
}

static void RebaseOrigin(uint64 nowCounter, uint64 nowTicks, uint8 newShift)
{
	uint32 seq = sOriginSeq.load(std::memory_order_relaxed);
	if (seq & 1)
		return; // another thread is mid-update; its result is equally valid
	if (!sOriginSeq.compare_exchange_strong(seq, seq + 1, std::memory_order_acquire))
		return;
	sOrigin.baseCounter = nowCounter;
	sOrigin.baseTicks = nowTicks;
	sOrigin.shift = newShift;
	sOriginSeq.store(seq + 2, std::memory_order_release);
}

void PPCTimer_init()
{
	asm volatile("mrs %0, cntfrq_el0" : "=r"(sCounterFrequency));
	if (sCounterFrequency == 0)
	{
		// Should be impossible on AArch64, but a zero would divide by zero below.
		sCounterFrequency = EXPECTED_COUNTER_FREQ;
	}
	sUseExactPath = (sCounterFrequency == EXPECTED_COUNTER_FREQ);
	if (!sUseExactPath)
	{
		cemuLog_log(LogType::Force,
			"PPCTimer: unexpected cntfrq_el0 of {} Hz (expected {}), using the general conversion path",
			sCounterFrequency, EXPECTED_COUNTER_FREQ);
	}

	sOrigin.baseCounter = ReadCounter();
	sOrigin.baseTicks = 0;
	sOrigin.shift = ActiveSettings::GetTimerShiftFactor();
	sOriginSeq.store(2, std::memory_order_release);
}

void PPCTimer_start()
{
	sOrigin.baseCounter = ReadCounter();
	sOrigin.baseTicks = 0;
	sOrigin.shift = ActiveSettings::GetTimerShiftFactor();
	sOriginSeq.store(sOriginSeq.load(std::memory_order_relaxed) + 2, std::memory_order_release);
}

uint64 PPCTimer_getRawTsc()
{
	return ReadCounter();
}

uint64 PPCTimer_microsecondsToTsc(uint64 us)
{
	return (us * sCounterFrequency) / 1000000ULL;
}

uint64 PPCTimer_tscToMicroseconds(uint64 tsc)
{
	return (uint64)((unsigned __int128)tsc * 1000000ULL / sCounterFrequency);
}

bool PPCTimer_isReady()
{
	return sCounterFrequency != 0;
}

void PPCTimer_waitForInit()
{
	// Initialisation is now two register reads rather than a 3 second calibration, so this
	// is already satisfied by the time anything can call it. Kept because callers
	// legitimately want to express the dependency.
	if (!PPCTimer_isReady())
		PPCTimer_init();
}

// thread safe
uint64 PPCTimer_getFromRDTSC()
{
	const uint8 shift = ActiveSettings::GetTimerShiftFactor();

	for (;;)
	{
		uint32 seq = sOriginSeq.load(std::memory_order_acquire);
		if (seq & 1)
			continue; // update in flight
		const TimerOrigin origin = sOrigin;
		if (sOriginSeq.load(std::memory_order_acquire) != seq)
			continue; // torn read, retry

		const uint64 nowCounter = ReadCounter();
		// The counter is monotonic, but a rebase racing with this read could leave
		// baseCounter marginally ahead; clamp instead of wrapping to a huge value.
		const uint64 delta = (nowCounter > origin.baseCounter) ? (nowCounter - origin.baseCounter) : 0;
		const uint64 ticks = origin.baseTicks + ScaleToTicks(delta, origin.shift);

		if (shift != origin.shift) [[unlikely]]
		{
			// Speed changed: freeze everything elapsed so far at the old rate, then apply
			// the new rate only going forward.
			RebaseOrigin(nowCounter, ticks, shift);
			continue;
		}
		return ticks;
	}
}
