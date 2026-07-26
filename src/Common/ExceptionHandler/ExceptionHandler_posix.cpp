#include <signal.h>
#include <execinfo.h>
#include <string.h>
#include <string>
#include <charconv>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include "config/CemuConfig.h"
#include "util/helpers/StringHelpers.h"
#include "ExceptionHandler.h"

#include "Cafe/HW/Espresso/Debugger/GDBStub.h"
#include "Cafe/HW/Espresso/Debugger/GDBBreakpoints.h"

namespace
{
	// A stack-overflow SIGSEGV cannot be reported without an alternate signal stack:
	// the handler needs stack space that, by definition, is not available. SIGSTKSZ is
	// sized for trivial handlers; ours formats strings and walks the stack, and arm64
	// frames are large, so allocate considerably more.
	constexpr size_t kAltStackSize = SIGSTKSZ * 8;

	thread_local char* t_altStack = nullptr;

	// Logs image UUID + load address for every loaded image. Combined with the raw
	// frame addresses this is what makes offline symbolication possible:
	//   atos -o Cemu.app.dSYM/Contents/Resources/DWARF/Cemu -l <load address> <addr>
	// It stays correct for a stripped release binary, which in-process symbol lookup
	// does not - especially now that everything is built -fvisibility=hidden.
	void WriteLoadedImageInfo()
	{
		CrashLog_WriteLine("Loaded images (uuid, load address, path):");
		const uint32_t imageCount = _dyld_image_count();
		for (uint32_t i = 0; i < imageCount; i++)
		{
			const struct mach_header* header = _dyld_get_image_header(i);
			const char* name = _dyld_get_image_name(i);
			if (!header || !name)
				continue;
			// only the main executable and our own dylibs are worth listing
			const bool isMain = header->filetype == MH_EXECUTE;
			if (!isMain && !strstr(name, "Cemu"))
				continue;

			std::string uuidStr = "<none>";
			const auto* cmd = (const struct load_command*)((const uint8_t*)header + sizeof(struct mach_header_64));
			for (uint32_t c = 0; c < header->ncmds; c++)
			{
				if (cmd->cmd == LC_UUID)
				{
					const auto* uc = (const struct uuid_command*)cmd;
					char buf[40];
					snprintf(buf, sizeof(buf),
						"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
						uc->uuid[0], uc->uuid[1], uc->uuid[2], uc->uuid[3], uc->uuid[4], uc->uuid[5],
						uc->uuid[6], uc->uuid[7], uc->uuid[8], uc->uuid[9], uc->uuid[10], uc->uuid[11],
						uc->uuid[12], uc->uuid[13], uc->uuid[14], uc->uuid[15]);
					uuidStr = buf;
					break;
				}
				cmd = (const struct load_command*)((const uint8_t*)cmd + cmd->cmdsize);
			}
			CrashLog_WriteLine(fmt::format("  {} 0x{:016x} {}", uuidStr, (uintptr_t)header, name));
		}
	}

	// macOS backtrace_symbols() produces
	//   "3   Cemu   0x0000000104a3f1c8 _ZN10LatteShader7CompileEv + 72"
	// which is a different shape from the glibc "image(symbol+0xoff) [addr]" the old
	// Linux parser expected. Split on whitespace and demangle field 3.
	void WriteDemangledBacktrace(void* const* addresses, size_t count)
	{
		char** symbols = backtrace_symbols(addresses, (int)count);
		if (!symbols)
		{
			CrashLog_WriteLine("Failed to read backtrace, raw addresses follow:");
			for (size_t i = 0; i < count; i++)
				CrashLog_WriteLine(fmt::format("  {:2} 0x{:016x}", i, (uintptr_t)addresses[i]));
			return;
		}
		for (size_t i = 0; i < count; i++)
		{
			std::string_view line{symbols[i]};
			// fields: index, image, address, symbol, "+", offset
			std::vector<std::string_view> fields;
			size_t pos = 0;
			while (pos < line.size() && fields.size() < 6)
			{
				while (pos < line.size() && line[pos] == ' ')
					pos++;
				const size_t start = pos;
				while (pos < line.size() && line[pos] != ' ')
					pos++;
				if (pos > start)
					fields.push_back(line.substr(start, pos - start));
			}
			if (fields.size() >= 4)
			{
				const std::string mangled{fields[3]};
				const std::string demangled = boost::core::demangle(mangled.c_str());
				const std::string_view tail = fields.size() >= 6 ? fields[5] : std::string_view{};
				CrashLog_WriteLine(fmt::format("  {:2} {:<28} {} {}{}",
					i, fields[1], fields[2], demangled,
					tail.empty() ? std::string{} : fmt::format(" + {}", tail)));
			}
			else
			{
				CrashLog_WriteLine(fmt::format("  {}", line));
			}
		}
		free(symbols);
	}
}

void ExceptionHandler_RegisterAltStackForThisThread()
{
	if (t_altStack)
		return;
	t_altStack = (char*)malloc(kAltStackSize);
	if (!t_altStack)
		return;
	stack_t ss{};
	ss.ss_sp = t_altStack;
	ss.ss_size = kAltStackSize;
	ss.ss_flags = 0;
	sigaltstack(&ss, nullptr);
}

// handle signals that would dump core, print stacktrace and then dump depending on config
void handlerDumpingSignal(int sig, siginfo_t *info, void *context)
{
    if(!CrashLog_Create())
        return; // give up if crashlog was already created

    char* sigName = strsignal(sig);
	if (sigName)
	{
		printf("%s!\n", sigName);
	}
	else
	{
		// should never be the case
		printf("Unknown core dumping signal!\n");
	}

	void* backtraceArray[128];
	size_t size;

	// get void*'s for all entries on the stack
	size = backtrace(backtraceArray, 128);
	// Replace the deepest entry with the actual faulting address. Use the accessor
	// rather than reading __pc directly: under pointer authentication the raw field
	// carries a signature and dereferencing it yields nonsense.
	if (context && size > 0)
	{
		auto* uc = (ucontext_t*)context;
		if (uc->uc_mcontext)
			backtraceArray[0] = (void*)arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
	}

    CrashLog_WriteLine(fmt::format("Error: signal {}:", sig));
	if (info)
		CrashLog_WriteLine(fmt::format("  code {} faulting address 0x{:016x}", info->si_code, (uintptr_t)info->si_addr));

	WriteDemangledBacktrace(backtraceArray, size);
	CrashLog_WriteLine("");
	WriteLoadedImageInfo();

    std::cerr << fmt::format("\nStacktrace and additional info written to:") << std::endl;
    std::cerr << cemuLog_GetLogFilePath().generic_string() << std::endl;

    CrashLog_SetOutputChannels(false, true);
    ExceptionHandler_LogGeneralInfo();
    CrashLog_SetOutputChannels(true, true);

	if (GetConfig().crash_dump == CrashDump::Enabled)
	{
		// reset signal handler to default and re-raise signal to dump core
		signal(sig, SIG_DFL);
		raise(sig);
		return;
	}
	// exit process ignoring all issues
	_Exit(1);
}

void handler_SIGINT(int sig)
{
	/*
	 * Received when pressing CTRL + C in a console
	 * Ideally should be exiting cleanly after saving settings but currently
	 * there's no clean exit pathway (at least on linux) and exiting the app
	 * by any mean ends up with a SIGABRT from the standard library destroying
	 * threads.
	 */
	_Exit(0);
}

void ExceptionHandler_Init()
{
	ExceptionHandler_RegisterAltStackForThisThread();

	struct sigaction action{};
	action.sa_flags = 0;
	sigfillset(&action.sa_mask); // don't allow signals to be interrupted

	action.sa_handler = handler_SIGINT;
	sigaction(SIGINT, &action, nullptr);
	sigaction(SIGTERM, &action, nullptr);

    // SA_ONSTACK runs the handler on the alternate stack registered above, which is
    // what allows a stack-overflow SIGSEGV to be reported at all.
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    action.sa_handler = nullptr;
	action.sa_sigaction = handlerDumpingSignal;
	sigaction(SIGABRT, &action, nullptr);
	sigaction(SIGBUS, &action, nullptr);
	sigaction(SIGFPE, &action, nullptr);
	sigaction(SIGILL, &action, nullptr);
	sigaction(SIGIOT, &action, nullptr);
	sigaction(SIGQUIT, &action, nullptr);
	sigaction(SIGSEGV, &action, nullptr);
	sigaction(SIGSYS, &action, nullptr);
	sigaction(SIGTRAP, &action, nullptr);
}
