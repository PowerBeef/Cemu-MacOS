#pragma once

using HRTick = uint64;

namespace HighResolutionTimerInternal
{
	// Mach timebase, resolved once at static-init time. It converts the ARM generic
	// timer count into nanoseconds. On every Apple silicon Mac shipped so far this is
	// exactly 125/3 (a 24 MHz counter), which the fast path below constant-folds into
	// a multiply and a multiply-shift. The general path exists only so a future part
	// with a different counter frequency stays correct rather than silently wrong.
	extern uint64 s_timebaseNumer;
	extern uint64 s_timebaseDenom;
}

class HighResolutionTimer
{
public:
	HighResolutionTimer()
	{
		m_timePoint = 0;
	}

	HRTick getTick() const
	{
		return m_timePoint;
	}

	uint64 getTickInSeconds() const
	{
		return m_timePoint / m_freq;
	}

	// return time difference in seconds, this is an utility function mainly intended for debugging/benchmarking purposes. Avoid using doubles for precise timing
	static double getTimeDiff(HRTick startTime, HRTick endTime)
	{
		return (double)(endTime - startTime) / (double)m_freq;
	}

	// returns tick difference and frequency
	static uint64 getTimeDiffEx(HRTick startTime, HRTick endTime, uint64& freq)
	{
		freq = m_freq;
		return endTime - startTime;
	}

	// Reads the ARM generic timer directly instead of going through
	// clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW). That libc call is backed by the very
	// same counter -- measured here, the two agree bit-for-bit -- but costs 18.4 ns per
	// call against 0.44 ns for the bare `mrs`, a 41x difference.
	//
	// That matters because the Latte command processor polls this clock from three spin
	// loops (the ring-buffer idle wait, the fence wait, and wait-for-flip), so on a
	// GPU-idle frame it is called at whatever rate the spin loop turns. Profiling put
	// mach_continuous_time at 47% self time for exactly this reason.
	//
	// Kept inline deliberately: at 0.44 ns the call overhead would otherwise dominate.
	static HighResolutionTimer now()
	{
		uint64 counter;
		asm volatile("mrs %0, cntvct_el0" : "=r"(counter));
		// t * 125 cannot overflow: it would take ~195 years of uptime at 24 MHz.
		if (HighResolutionTimerInternal::s_timebaseNumer == 125 && HighResolutionTimerInternal::s_timebaseDenom == 3) [[likely]]
			return HighResolutionTimer(counter * 125ull / 3ull);
		// widened, because an unknown numerator has no such overflow guarantee
		return HighResolutionTimer((uint64)((unsigned __int128)counter * HighResolutionTimerInternal::s_timebaseNumer / HighResolutionTimerInternal::s_timebaseDenom));
	}

	static HRTick getFrequency();

	static HRTick microsecondsToTicks(uint64 microseconds)
	{
		return microseconds * m_freq / 1000000;
	}

	static uint64 ticksToMicroseconds(HRTick ticks)
	{
		return ticks * 1000000 / m_freq;
	}

private:
	HighResolutionTimer(uint64 timePoint) : m_timePoint(timePoint) {};

	uint64 m_timePoint;
	static uint64 m_freq;
};

// benchmark helper utility
// measures time between Start() and Stop() call
class BenchmarkTimer
{
public:
	void Start()
	{
		m_startTime = HighResolutionTimer::now().getTick();
	}

	void Stop()
	{
		m_stopTime = HighResolutionTimer::now().getTick();
	}

	double GetElapsedMilliseconds() const
	{
		cemu_assert_debug(m_startTime != 0 && m_stopTime != 0);
		cemu_assert_debug(m_startTime <= m_stopTime);
		uint64 tickDif = m_stopTime - m_startTime;
		double freq = (double)HighResolutionTimer::now().getFrequency();
		double elapsedMS = (double)tickDif * 1000.0 / freq;
		return elapsedMS;
	}

private:
	HRTick m_startTime{};
	HRTick m_stopTime{};
};

