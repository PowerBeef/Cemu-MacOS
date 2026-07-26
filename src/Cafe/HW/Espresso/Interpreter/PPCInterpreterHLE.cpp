#include "../PPCState.h"
#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"
#include "Cemu/Telemetry/Telemetry.h"

#include <shared_mutex>
#include <unordered_map>
#include <memory>

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

std::mutex s_hleLogMutex;

void PPCInterpreter_virtualHLE(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	uint32 hleFuncId = opcode & 0xFFFF;
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