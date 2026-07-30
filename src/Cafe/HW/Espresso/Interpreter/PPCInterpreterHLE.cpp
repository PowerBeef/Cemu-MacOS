#include "../PPCState.h"
#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"
#include "Cemu/Telemetry/Telemetry.h"

#include <shared_mutex>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <vector>

// Per-import call counts for unresolved imports, keyed by the address of the trampoline
// the RPL loader synthesised. That address is unique and stable per import, so the hot
// path is a uint32 hash lookup with no string work at all.
//
// The previous version built `fmt::format("Unsupported lib call: {}", name)` -- a heap
// allocation -- on *every* call, then hashed and looked up that string. That was tolerable
// while only the interpreter reached here; routing the recompiler through it too would
// have put ~17,000 allocations/second on the default path in BotW, which calls unresolved
// imports 557 times per frame.
struct UnsupportedHLECall
{
	std::string name;
	std::atomic<uint64> count;
};
static std::shared_mutex s_unsupportedHLEMutex;
static std::unordered_map<uint32, std::unique_ptr<UnsupportedHLECall>> s_unsupportedHLECalls;

void PPCInterpreter_handleUnsupportedHLECall(PPCInterpreter_t* hCPU)
{
	const uint32 trampolineIP = hCPU->instructionPointer;
	// Reached from up to three guest core threads concurrently, so this must be locked --
	// the interpreter caller used to hold a mutex around it and the recompiler caller
	// does not.
	{
		std::shared_lock readLock(s_unsupportedHLEMutex);
		auto it = s_unsupportedHLECalls.find(trampolineIP);
		if (it != s_unsupportedHLECalls.end()) [[likely]]
		{
			it->second->count.fetch_add(1, std::memory_order_relaxed);
			readLock.unlock();
			TLM_INC(Accuracy, AccUnsupportedHleCalls);
			hCPU->gpr[3] = 0;
			PPCInterpreter_nextInstruction(hCPU);
			return;
		}
	}
	// First sighting of this import: the RPL loader wrote the "lib.func" string inline
	// after the trap and the blr.
	const char* libFuncName = (char*)memory_getPointerFromVirtualOffset(trampolineIP + 8);
	{
		std::unique_lock writeLock(s_unsupportedHLEMutex);
		auto [it, inserted] = s_unsupportedHLECalls.try_emplace(trampolineIP);
		if (inserted)
		{
			it->second = std::make_unique<UnsupportedHLECall>();
			it->second->name = libFuncName;
			it->second->count.store(1, std::memory_order_relaxed);
			cemuLog_log(LogType::UnsupportedAPI, "Unsupported lib call: {}", libFuncName);
		}
		else
		{
			it->second->count.fetch_add(1, std::memory_order_relaxed);
		}
	}
	TLM_INC(Accuracy, AccUnsupportedHleCalls);
	hCPU->gpr[3] = 0;
	PPCInterpreter_nextInstruction(hCPU);
}

// Pushes the histogram into telemetry. Registered as a flush callback so it runs only
// when details are about to be written, not on the hot path.
void PPCInterpreter_flushUnsupportedHLEStats()
{
	std::shared_lock readLock(s_unsupportedHLEMutex);
	for (const auto& [ip, info] : s_unsupportedHLECalls)
		tlm::NoteAccuracyDetail(tlm::CounterId::AccUnsupportedHleCalls, info->name,
								info->count.load(std::memory_order_relaxed));
}

static constexpr size_t HLE_TABLE_CAPACITY = 0x4000;
HLECALL s_ppcHleTable[HLE_TABLE_CAPACITY]{};
std::string s_ppcHleNames[HLE_TABLE_CAPACITY];
sint32 s_ppcHleTableWriteIndex = 0;
std::mutex s_ppcHleTableMutex;

HLEIDX PPCInterpreter_registerHLECall(HLECALL hleCall, std::string hleName)
{
	std::unique_lock _l(s_ppcHleTableMutex);
	if (s_ppcHleTableWriteIndex >= HLE_TABLE_CAPACITY)
	{
		cemuLog_log(LogType::Force, "HLE table is full");
		cemu_assert(false);
	}
	for (sint32 i = 0; i < s_ppcHleTableWriteIndex; i++)
	{
		if (s_ppcHleTable[i] == hleCall)
		{
			return i;
		}
	}
	cemu_assert(s_ppcHleTableWriteIndex < HLE_TABLE_CAPACITY);
	s_ppcHleTable[s_ppcHleTableWriteIndex] = hleCall;
	// hleName arrives fully qualified as "lib.func" and used to be discarded here.
	// Keeping it is what makes a *named* HLE report possible; it costs one startup-time
	// string per export and nothing at runtime.
	s_ppcHleNames[s_ppcHleTableWriteIndex] = std::move(hleName);
	HLEIDX funcIndex = s_ppcHleTableWriteIndex;
	s_ppcHleTableWriteIndex++;
	return funcIndex;
}

HLECALL PPCInterpreter_getHLECall(HLEIDX funcIndex)
{
	if (funcIndex < 0 || funcIndex >= HLE_TABLE_CAPACITY)
		return nullptr;
	return s_ppcHleTable[funcIndex];
}

const std::string& PPCInterpreter_getHLEName(HLEIDX funcIndex)
{
	static const std::string kUnknown = "?";
	if (funcIndex < 0 || funcIndex >= (sint32)HLE_TABLE_CAPACITY)
		return kUnknown;
	return s_ppcHleNames[funcIndex];
}

// Per-function call counts, indexed by the same HLE table index the trap opcode carries.
// 16384 entries of 8 bytes is 128 KB of BSS, which buys a lock-free relaxed increment on a
// path taken ~1M times a second from three guest core threads.
static std::atomic<uint64> s_hleCallCounts[HLE_TABLE_CAPACITY];

// What a per-call charge *would* cost, if one existed. 500 is not a measured figure -- it is
// the number in the `remainingCycles -= 500` at BackendAArch64.cpp, whose trailing comment
// claims it applies "for each HLE call" when in fact it only ever runs on the 0xFFD0
// unresolved-import branch (1.1% of calls). Summing it here for *every* call is what makes
// the claim checkable: against ppcThreadQuantum = 45000, cpu.hle_would_charge_cycles says
// directly whether charging HLE calls could reach a reschedule or is lost in the noise.
// Nothing below writes remainingCycles.
static constexpr uint64 kNominalHleCycleCost = 500;

// Called from both CPU modes -- the interpreter's PPCInterpreter_virtualHLE and the
// recompiler's PPCRecompiler_virtualHLE. It lives here rather than being duplicated because
// that duplication is exactly what produced the last bug in this area: TLM_INC(CpuHleCalls)
// existed only on the recompiler side, so cpu.hle_calls silently read zero under
// --force-interpreter and the 52,330/frame figure excluded interpreter-mode calls entirely.
void PPCInterpreter_accountHLECall(uint32 hleFuncId)
{
	TLM_INC(Cpu, CpuHleCalls);
	TLM_ADD(Cpu, CpuHleWouldChargeCycles, kNominalHleCycleCost);
	if (hleFuncId < HLE_TABLE_CAPACITY && tlm::AreaEnabled(tlm::Area::Cpu)) [[unlikely]]
		s_hleCallCounts[hleFuncId].fetch_add(1, std::memory_order_relaxed);
}

// The resolved-import histogram. Unresolved imports are reported separately by
// PPCInterpreter_flushUnsupportedHLEStats above, keyed by trampoline address instead.
void PPCInterpreter_flushHLECallStats()
{
	// tlm::kMaxDetails caps *all* detail sources at 256 combined, and the unresolved-import
	// histogram already claims ~31 of those. Emitting the whole table would silently drop
	// entries past the cap with no indication which, so take the top N deliberately and say
	// how much was left out rather than truncating by accident.
	constexpr size_t kReportTopN = 64;
	std::vector<std::pair<uint64, HLEIDX>> byCount;
	byCount.reserve(256);
	uint64 total = 0;
	for (HLEIDX i = 0; i < (HLEIDX)HLE_TABLE_CAPACITY; i++)
	{
		uint64 c = s_hleCallCounts[i].load(std::memory_order_relaxed);
		if (c == 0)
			continue;
		byCount.emplace_back(c, i);
		total += c;
	}
	std::sort(byCount.begin(), byCount.end(), std::greater<>());

	uint64 reported = 0;
	for (size_t i = 0; i < byCount.size() && i < kReportTopN; i++)
	{
		tlm::NoteAccuracyDetail(tlm::CounterId::CpuHleCalls, PPCInterpreter_getHLEName(byCount[i].second), byCount[i].first);
		reported += byCount[i].first;
	}
	if (byCount.size() > kReportTopN)
	{
		// Two entries with FIXED keys, not one key containing the count. The detail string is
		// the dedup key, so "(N further functions)" mints a fresh entry every time N grows and
		// the tail shows up as a dozen near-duplicate rows -- the exact trap already recorded
		// for the fence stats. Varying data belongs in the count.
		tlm::NoteAccuracyDetail(tlm::CounterId::CpuHleCalls, "(calls beyond the top 64)", total - reported);
		tlm::NoteAccuracyDetail(tlm::CounterId::CpuHleCalls, "(distinct functions called)", byCount.size());
	}
}

std::mutex s_hleLogMutex;

void PPCInterpreter_virtualHLE(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	uint32 hleFuncId = opcode & 0xFFFF;
	PPCInterpreter_accountHLECall(hleFuncId);
	if (hleFuncId == 0xFFD0) [[unlikely]]
	{
		s_hleLogMutex.lock();
		PPCInterpreter_handleUnsupportedHLECall(hCPU);
		s_hleLogMutex.unlock();
	}
	else
	{
		// os lib function
		auto hleCall = PPCInterpreter_getHLECall(hleFuncId);
		cemu_assert(hleCall);
		hleCall(hCPU);
	}
}