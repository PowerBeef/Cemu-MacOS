#include "Cemu/Telemetry/Telemetry.h"

#include "Cemu/Logging/CemuLogging.h"
#include "Common/version.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>

namespace tlm
{
	uint32_t g_areaMask = 0;
	alignas(64) uint64_t g_counters[kSlots][kRowU64] = {};

	// Constant initializer -- g_counters[0] is a link-time address, so this compiles to
	// the cheap TLS sequence rather than a __tls_get_addr-style call. If you change this
	// to something needing dynamic init, disassemble TLM_INC before believing it is
	// still cheap. RelWithDebInfo keeps LTO off so `llvm-objdump -d` on the .o works.
	__thread uint64_t* t_slot = &g_counters[0][0];

	namespace
	{
		struct CounterMeta
		{
			const char* name;
			const char* unit;
			Area area;
		};

		constexpr CounterMeta kMeta[kCounterCount] = {
#define TLM_COUNTER(id, name, unit, area) {name, unit, Area::area},
#include "TelemetryCounters.def"
#undef TLM_COUNTER
		};

		// ---- output sink ------------------------------------------------------------
		// A writer thread draining a queue of ready-formatted lines. Records are written
		// with ::write() as they are produced: CemuApp::OnExit calls _Exit(), so anything
		// still buffered at shutdown is simply lost.
		class Sink
		{
		public:
			bool Open(const std::string& path)
			{
				m_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
				if (m_fd < 0)
					return false;
				m_running = true;
				m_thread = std::thread(&Sink::Run, this);
				return true;
			}

			void Push(std::string line)
			{
				if (m_fd < 0)
					return;
				{
					std::lock_guard lock(m_mutex);
					// Bound the queue so a stalled disk cannot grow it without limit.
					// Dropping is always preferable to stalling the GPU thread; the
					// count is reported in the summary so a drop is never silent.
					if (m_queue.size() >= kMaxQueued)
					{
						m_dropped++;
						return;
					}
					m_queue.emplace_back(std::move(line));
				}
				m_cv.notify_one();
			}

			void Close()
			{
				if (m_fd < 0)
					return;
				{
					std::lock_guard lock(m_mutex);
					m_running = false;
				}
				m_cv.notify_one();
				if (m_thread.joinable())
					m_thread.join();
				::close(m_fd);
				m_fd = -1;
			}

			uint64_t Dropped() const { return m_dropped; }
			bool IsOpen() const { return m_fd >= 0; }

		private:
			static constexpr size_t kMaxQueued = 4096;

			void Run()
			{
				std::deque<std::string> batch;
				while (true)
				{
					{
						std::unique_lock lock(m_mutex);
						m_cv.wait(lock, [&] { return !m_queue.empty() || !m_running; });
						if (m_queue.empty() && !m_running)
							return;
						batch.swap(m_queue);
					}
					for (auto& line : batch)
					{
						size_t off = 0;
						while (off < line.size())
						{
							ssize_t n = ::write(m_fd, line.data() + off, line.size() - off);
							if (n <= 0)
								break;
							off += (size_t)n;
						}
					}
					batch.clear();
				}
			}

			int m_fd = -1;
			std::thread m_thread;
			std::mutex m_mutex;
			std::condition_variable m_cv;
			std::deque<std::string> m_queue;
			bool m_running = false;
			std::atomic<uint64_t> m_dropped{0};
		};

		Sink s_sink;
		std::string s_label;
		std::atomic<uint32_t> s_nextSlot{1}; // slot 0 is the shared fallback row
		std::mutex s_threadNamesMutex;
		std::vector<std::pair<uint32_t, std::string>> s_threadNames;

		uint64_t s_prev[kCounterCount] = {};
		uint64_t s_totals[kCounterCount] = {};
		uint64_t s_frameIndex = 0;
		HRTick s_runStart = 0;
		HRTick s_lastFrame = 0;
		bool s_headerWritten = false;

		// Accuracy details are rare and unbounded in principle, so they are capped and
		// deduplicated. The point is to name what was hit, not to log every occurrence.
		std::mutex s_detailMutex;
		std::vector<std::pair<CounterId, std::string>> s_details;
		size_t s_detailsEmitted = 0;
		constexpr size_t kMaxDetails = 256;

		std::mutex s_metaMutex;
		uint64_t s_titleId = 0;
		std::string s_titleName;
		std::vector<std::pair<std::string, std::string>> s_settings;
		bool s_haveTitleMetadata = false;

		std::string JsonEscape(std::string_view in)
		{
			std::string out;
			out.reserve(in.size() + 8);
			for (char c : in)
			{
				switch (c)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if ((unsigned char)c < 0x20)
						out += fmt::format("\\u{:04x}", (unsigned)(unsigned char)c);
					else
						out += c;
				}
			}
			return out;
		}

		void SumInto(uint64_t* out)
		{
			const uint32_t used = std::min<uint32_t>(s_nextSlot.load(std::memory_order_relaxed), kSlots);
			for (size_t c = 0; c < kCounterCount; c++)
				out[c] = 0;
			for (uint32_t s = 0; s < used; s++)
			{
				for (size_t c = 0; c < kCounterCount; c++)
				{
					// Relaxed atomic read: producers write these without synchronisation,
					// so a plain read would be a data race. On arm64 this is the same
					// `ldr` either way.
					out[c] += std::atomic_ref<uint64_t>(g_counters[s][c]).load(std::memory_order_relaxed);
				}
			}
		}
	}

	const char* CounterName(CounterId id) { return kMeta[(size_t)id].name; }
	const char* CounterUnit(CounterId id) { return kMeta[(size_t)id].unit; }
	Area CounterArea(CounterId id) { return kMeta[(size_t)id].area; }

	void Init(const std::string& outputPath, const std::string& label, uint32_t areaMask)
	{
		if (outputPath.empty() || areaMask == 0)
			return;
		if (!s_sink.Open(outputPath))
		{
			// Never abort a run because telemetry could not be written.
			cemuLog_log(LogType::Force, "Telemetry: failed to open {} - telemetry disabled", outputPath);
			return;
		}
		s_label = label;
		s_runStart = HighResolutionTimer::now().getTick();
		s_lastFrame = s_runStart;
		g_areaMask = areaMask; // publish last: nothing counts until the sink is ready
		cemuLog_log(LogType::Force, "Telemetry: recording to {} (areas mask {:#x})", outputPath, areaMask);
	}

	void RegisterThread(const char* name)
	{
		if (!Enabled())
			return;
		uint32_t slot = s_nextSlot.fetch_add(1, std::memory_order_relaxed);
		if (slot >= kSlots)
		{
			cemuLog_log(LogType::Force, "Telemetry: out of counter slots, '{}' will share the fallback row", name);
			return;
		}
		t_slot = &g_counters[slot][0];
		std::lock_guard lock(s_threadNamesMutex);
		s_threadNames.emplace_back(slot, name ? name : "?");
	}

	void OnTitleLoaded(uint64_t titleId, const std::string& titleName,
					   const std::vector<std::pair<std::string, std::string>>& settings)
	{
		if (!Enabled())
			return;
		// Stash it rather than writing now. Threads register as they start, and
		// LatteThread and the OSSched cores all start *after* the title loads, so a
		// header written here would carry an empty thread list. The first frame boundary
		// emits it instead.
		std::lock_guard lock(s_metaMutex);
		s_titleId = titleId;
		s_titleName = titleName;
		s_settings = settings;
		s_haveTitleMetadata = true;
	}

	namespace
	{
		void WriteRunHeaderLocked()
		{
			std::string counters;
			for (size_t i = 0; i < kCounterCount; i++)
			{
				if (i)
					counters += ',';
				counters += fmt::format(R"({{"i":{},"n":"{}","u":"{}"}})", i, kMeta[i].name, kMeta[i].unit);
			}
			std::string threads;
			{
				std::lock_guard lock(s_threadNamesMutex);
				for (size_t i = 0; i < s_threadNames.size(); i++)
				{
					if (i)
						threads += ',';
					threads += fmt::format(R"({{"slot":{},"name":"{}"}})", s_threadNames[i].first,
										   JsonEscape(s_threadNames[i].second));
				}
			}
			std::string cfg;
			for (size_t i = 0; i < s_settings.size(); i++)
			{
				if (i)
					cfg += ',';
				cfg += fmt::format(R"("{}":"{}")", JsonEscape(s_settings[i].first),
								   JsonEscape(s_settings[i].second));
			}

			s_sink.Push(fmt::format(
				R"({{"type":"run","schema":1,"build":"{}","label":"{}","title":{{"id":"{:016x}","name":"{}"}},)"
				R"("config":{{{}}},"threads":[{}],"counters":[{}]}})"
				"\n",
				_XSTRINGFY(EMULATOR_HASH), JsonEscape(s_label), s_titleId, JsonEscape(s_titleName), cfg,
				threads, counters));
		}

		// Accuracy details are re-emitted periodically rather than only in the summary.
		// The summary is best-effort: CemuApp::OnExit calls _Exit(), and every scripted
		// benchmark in this repo terminates Cemu with a signal, so a run that only wrote
		// its findings at shutdown would in practice never write them at all.
		void MaybeEmitAccuracyDetails()
		{
			std::string details;
			{
				std::lock_guard lock(s_detailMutex);
				if (s_details.size() == s_detailsEmitted)
					return;
				s_detailsEmitted = s_details.size();
				for (size_t i = 0; i < s_details.size(); i++)
				{
					if (i)
						details += ',';
					details += fmt::format(R"({{"signal":"{}","detail":"{}"}})",
										   kMeta[(size_t)s_details[i].first].name,
										   JsonEscape(s_details[i].second));
				}
			}
			s_sink.Push(fmt::format(R"({{"t":"acc","details":[{}]}})"
									"\n",
									details));
		}
	}


	void OnFrameBoundary()
	{
		if (!Enabled())
			return;

		if (!s_headerWritten)
		{
			std::lock_guard lock(s_metaMutex);
			if (s_haveTitleMetadata)
			{
				WriteRunHeaderLocked();
				s_headerWritten = true;
			}
		}

		SumInto(s_totals);

		const HRTick now = HighResolutionTimer::now().getTick();
		const uint64_t frameNs = now - s_lastFrame;
		s_lastFrame = now;

		std::string values;
		values.reserve(kCounterCount * 8);
		for (size_t c = 0; c < kCounterCount; c++)
		{
			if (c)
				values += ',';
			values += fmt::format("{}", s_totals[c] - s_prev[c]);
			s_prev[c] = s_totals[c];
		}

		s_sink.Push(fmt::format(R"({{"t":"f","n":{},"ns":{},"v":[{}]}})"
								"\n",
								s_frameIndex, frameNs, values));
		s_frameIndex++;
		if ((s_frameIndex % 300) == 0)
			MaybeEmitAccuracyDetails();
	}

	void NoteAccuracyDetail(CounterId id, const std::string& detail)
	{
		if (!AreaEnabled(Area::Accuracy))
			return;
		std::lock_guard lock(s_detailMutex);
		if (s_details.size() >= kMaxDetails)
			return;
		for (const auto& [existingId, existingDetail] : s_details)
		{
			if (existingId == id && existingDetail == detail)
				return;
		}
		s_details.emplace_back(id, detail);
	}

	void Shutdown()
	{
		if (!Enabled())
			return;
		g_areaMask = 0; // stop producers before draining

		SumInto(s_totals);
		const uint64_t wallNs = HighResolutionTimer::now().getTick() - s_runStart;

		std::string totals;
		for (size_t c = 0; c < kCounterCount; c++)
		{
			if (s_totals[c] == 0)
				continue; // a run file listing every zero counter is noise
			if (!totals.empty())
				totals += ',';
			totals += fmt::format(R"("{}":{})", kMeta[c].name, s_totals[c]);
		}
		std::string details;
		{
			std::lock_guard lock(s_detailMutex);
			for (size_t i = 0; i < s_details.size(); i++)
			{
				if (i)
					details += ',';
				details += fmt::format(R"({{"signal":"{}","detail":"{}"}})",
									   kMeta[(size_t)s_details[i].first].name,
									   JsonEscape(s_details[i].second));
			}
		}

		s_sink.Push(fmt::format(R"({{"type":"summary","frames":{},"wall_ns":{},"dropped":{},)"
								R"("totals":{{{}}},"accuracy_details":[{}]}})"
								"\n",
								s_frameIndex, wallNs, s_sink.Dropped(), totals, details));
		s_sink.Close();
		cemuLog_log(LogType::Force, "Telemetry: wrote {} frame records", s_frameIndex);
	}
}
