#pragma once

// Telemetry: permanent, structured, per-frame instrumentation for the emulator core.
//
// Why this exists: every performance conclusion in this project so far was reached with
// throwaway instrumentation that was then deleted, and three of them were wrong on the
// first pass because of it. GPU frame timing in particular has been hand-added and
// discarded twice. This is the same measurements, kept.
//
// Design constraints, in priority order:
//
//  1. ~Zero cost when disabled. One predictable branch on a shared read-only global.
//     This matters more than it sounds: A/B comparisons must run the SAME BINARY in
//     both arms, because rebuilding changes codegen and invalidates the comparison.
//     So the counters ship compiled in, and are off unless asked for.
//
//  2. No atomics on hot paths. Each producer thread owns a cache-line-aligned row and
//     increments it with a plain load/add/store. Rows are summed at the frame boundary,
//     which is the only place any cross-thread reading happens.
//
//  3. Output is structured and lands on disk incrementally. CemuApp::OnExit calls
//     _Exit(), so anything buffered at shutdown is lost -- records are written as they
//     are produced, not at the end.
//
// Usage:
//     TLM_INC(Gpu, GpuDrawCalls);
//     TLM_ADD(Mem, MemBufCacheUploadBytes, size);
//
// Declare the counter in TelemetryCounters.def first; that file is the single source of
// truth and also generates the id->name table written into each run header.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tlm
{
	enum class Area : uint32_t
	{
		Cpu = 0,
		Gpu = 1,
		Mem = 2,
		Accuracy = 3,
		_Count
	};

	enum class CounterId : uint16_t
	{
#define TLM_COUNTER(id, name, unit, area) id,
#include "TelemetryCounters.def"
#undef TLM_COUNTER
		_Count
	};

	inline constexpr size_t kCounterCount = (size_t)CounterId::_Count;
	// Pad each row out to a whole number of 64-byte cache lines so two threads never
	// share one. Rows are indexed by slot, so a false-sharing stall here would show up
	// as a mysterious slowdown proportional to thread count.
	inline constexpr size_t kRowU64 = (kCounterCount + 7) & ~(size_t)7;
	inline constexpr size_t kSlots = 64;

	// 0 means fully disabled. Read on every instrumented site, written once at startup.
	extern uint32_t g_areaMask;

	alignas(64) extern uint64_t g_counters[kSlots][kRowU64];

	// Deliberately __thread and not thread_local: a thread_local with a non-constant
	// initializer or a destructor goes through a TLS wrapper *function call* on
	// AppleClang. This has a constant (link-time) initializer, so clang emits the cheap
	// TPIDRRO_EL0 sequence instead. Verified by disassembly -- see the note in
	// Telemetry.cpp. Threads that never register share row 0; their counts are racy and
	// lossy, which is the correct behaviour for threads we do not measure.
	extern __thread uint64_t* t_slot;

	inline bool Enabled() { return g_areaMask != 0; }
	inline bool AreaEnabled(Area a) { return (g_areaMask & (1u << (uint32_t)a)) != 0; }

	// Called once from main(), before any guest code runs.
	void Init(const std::string& outputPath, const std::string& label, uint32_t areaMask);
	// Called once the title is known, to write the run header. Metadata is passed in
	// rather than looked up, because Telemetry lives in CemuComponents and must not
	// depend on Cafe -- the dependency runs the other way.
	//
	// Without this record a run file is uninterpretable weeks later: buffer cache mode
	// alone changes GPU time by 16%, so "which settings produced these numbers" is not
	// optional metadata.
	void OnTitleLoaded(uint64_t titleId, const std::string& titleName,
					   const std::vector<std::pair<std::string, std::string>>& settings);
	// Assigns this thread its own counter row. Call from thread entry points that matter.
	void RegisterThread(const char* name);
	// Called at the one authoritative frame boundary.
	void OnFrameBoundary();
	// Best-effort: writes the run summary. Safe to call when disabled or already shut down.
	void Shutdown();

	// Records a named accuracy signal the first time it fires, so the summary can list
	// *what* was hit rather than only how often. Cheap: rare paths only.
	void NoteAccuracyDetail(CounterId id, const std::string& detail);

	const char* CounterName(CounterId id);
	const char* CounterUnit(CounterId id);
	Area CounterArea(CounterId id);
}

// The whole hot path. Disabled cost is one L1-resident load of a shared read-only global
// plus a perfectly-predicted branch; the load has no coherence traffic because nothing
// writes g_areaMask after startup. Area masking is free -- it folds into the same test
// that the enable check needed anyway.
#define TLM_ADD(area_, id_, n_)                                                          \
	do {                                                                                 \
		if (::tlm::g_areaMask & (1u << (uint32_t)::tlm::Area::area_)) [[unlikely]]        \
			::tlm::t_slot[(size_t)::tlm::CounterId::id_] += (uint64_t)(n_);              \
	} while (0)

#define TLM_INC(area_, id_) TLM_ADD(area_, id_, 1)
