#include "CubebAPI.h"

#if BOOST_OS_WINDOWS
#include <combaseapi.h>
#include <mmreg.h>
#include <mmsystem.h>
#pragma comment(lib, "Avrt.lib")
#pragma comment(lib, "ksuser.lib")
#endif


static void state_cb(cubeb_stream* stream, void* user, cubeb_state state)
{
	if (!stream)
		return;

	/*switch (state)
	{
	case CUBEB_STATE_STARTED:
		fprintf(stderr, "stream started\n");
		break;
	case CUBEB_STATE_STOPPED:
		fprintf(stderr, "stream stopped\n");
		break;
	case CUBEB_STATE_DRAINED:
		fprintf(stderr, "stream drained\n");
		break;
	default:
		fprintf(stderr, "unknown stream state %d\n", state);
	}*/
}

long CubebAPI::data_cb(cubeb_stream* stream, void* user, const void* inputbuffer, void* outputbuffer, long nframes)
{
	auto* thisptr = (CubebAPI*)user;
	//const auto size = (size_t)thisptr->m_bytesPerBlock; // (size_t)nframes* thisptr->m_channels;

	// m_bytesPerBlock = samples_per_block * channels * (bits_per_sample / 8);
	const auto size = (size_t)nframes * thisptr->m_channels * (thisptr->m_bitsPerSample/8);

	// Runs on CoreAudio's realtime thread. Nothing here may block, allocate, or take a
	// lock.
	const size_t head = thisptr->m_ringHead.load(std::memory_order_relaxed);
	const size_t tail = thisptr->m_ringTail.load(std::memory_order_acquire);
	const size_t available = tail - head;
	const size_t copied = std::min(available, size);

	if (copied)
	{
		const size_t cap = thisptr->m_ringSize;
		const size_t offset = head % cap;
		const size_t firstChunk = std::min(copied, cap - offset);
		memcpy(outputbuffer, thisptr->m_ringBuffer.get() + offset, firstChunk);
		if (copied > firstChunk)
			memcpy((uint8*)outputbuffer + firstChunk, thisptr->m_ringBuffer.get(), copied - firstChunk);
		thisptr->m_ringHead.store(head + copied, std::memory_order_release);
	}
	// underrun (or partial): pad with silence rather than repeating stale audio
	if (copied != size)
		memset((uint8*)outputbuffer + copied, 0x00, size - copied);

	return nframes;
}

CubebAPI::CubebAPI(cubeb_devid devid, uint32 samplerate, uint32 channels, uint32 samples_per_block,
                   uint32 bits_per_sample)
	: IAudioAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
	cubeb_stream_params output_params;

	output_params.format = CUBEB_SAMPLE_S16LE;
	output_params.rate = samplerate;
	output_params.channels = channels;
	output_params.prefs = CUBEB_STREAM_PREF_NONE;

	switch (channels)
	{
	case 8:
		output_params.layout = CUBEB_LAYOUT_3F4_LFE;
		break;
	case 6:
		output_params.layout = CUBEB_LAYOUT_3F2_LFE_BACK;
		break;
	case 4:
		output_params.layout = CUBEB_LAYOUT_QUAD;
		break;
	case 2:
		output_params.layout = CUBEB_LAYOUT_STEREO;
		break;
	default:
		output_params.layout = CUBEB_LAYOUT_MONO;
		break;
	}

	// cubeb_get_min_latency() reports the smallest buffer the device will accept. It is
	// a floor, not a target: requesting less makes cubeb_stream_init fail outright.
	//
	// The Wii U's AI block delivers audio on a coarse cadence tied to guest scheduling,
	// so running at the device floor invites underruns. Ask for a little more headroom
	// where the device allows it (~20ms at 48kHz), but never less than the floor.
	uint32 deviceMinLatency = 1;
	if (cubeb_get_min_latency(s_context, &output_params, &deviceMinLatency) != CUBEB_OK)
		deviceMinLatency = 1;
	constexpr uint32 kDesiredLatencyFrames = 960; // ~20ms at 48kHz
	const uint32 latency = std::max(deviceMinLatency, kDesiredLatencyFrames);
	cemuLog_logDebug(LogType::Force, "Cubeb: device min latency {} frames, requesting {}", deviceMinLatency, latency);

	m_ringSize = (size_t)m_bytesPerBlock * kBlockCount;
	m_ringBuffer = std::make_unique<uint8[]>(m_ringSize);

	if (cubeb_stream_init(s_context, &m_stream, "Cemu Cubeb output",
	                      nullptr, nullptr,
	                      devid, &output_params,
	                      latency, data_cb, state_cb, this) != CUBEB_OK)
	{
		cemuLog_log(LogType::Force, "Cubeb: cubeb_stream_init failed (latency {} frames, {} ch, {} Hz)", latency, channels, samplerate);
		throw std::runtime_error("can't initialize cubeb device");
	}
}

CubebAPI::~CubebAPI()
{
	if (m_stream)
	{
		Stop();
		cubeb_stream_destroy(m_stream);
	}
}

bool CubebAPI::NeedAdditionalBlocks() const
{
	return RingBytesUsed() < (size_t)GetAudioDelay() * m_bytesPerBlock;
}

bool CubebAPI::FeedBlock(sint16* data)
{
	const size_t tail = m_ringTail.load(std::memory_order_relaxed);
	const size_t head = m_ringHead.load(std::memory_order_acquire);
	if (m_ringSize - (tail - head) < m_bytesPerBlock)
	{
		cemuLog_logDebug(LogType::Force, "dropped audio block since too many buffers are queued");
		return false;
	}

	const size_t offset = tail % m_ringSize;
	const size_t firstChunk = std::min<size_t>(m_bytesPerBlock, m_ringSize - offset);
	memcpy(m_ringBuffer.get() + offset, data, firstChunk);
	if (m_bytesPerBlock > firstChunk)
		memcpy(m_ringBuffer.get(), (uint8*)data + firstChunk, m_bytesPerBlock - firstChunk);
	m_ringTail.store(tail + m_bytesPerBlock, std::memory_order_release);
	return true;
}

bool CubebAPI::Play()
{
	if (m_is_playing)
		return true;

	if (cubeb_stream_start(m_stream) == CUBEB_OK)
	{
		m_is_playing = true;
		return true;
	}

	return false;
}

bool CubebAPI::Stop()
{
	if (!m_is_playing)
		return true;

	if (cubeb_stream_stop(m_stream) == CUBEB_OK)
	{
		m_is_playing = false;
		return true;
	}

	return false;
}

void CubebAPI::SetVolume(sint32 volume)
{
	IAudioAPI::SetVolume(volume);
	cubeb_stream_set_volume(m_stream, (float)volume / 100.0f);
}


bool CubebAPI::InitializeStatic()
{
	if (cubeb_init(&s_context, "Cemu Cubeb", nullptr))
	{
		cemuLog_log(LogType::Force, "can't create cubeb audio api");
		return false;
	}
	return true;
}

void CubebAPI::Destroy()
{
	if (s_context)
		cubeb_destroy(s_context);
}

std::vector<IAudioAPI::DeviceDescriptionPtr> CubebAPI::GetDevices()
{
	std::vector<DeviceDescriptionPtr> result;
	// Add the default device to the list
	auto defaultDevice = std::make_shared<CubebDeviceDescription>(nullptr, "default", L"Default Device");
	result.emplace_back(defaultDevice);

	cubeb_device_collection devices;
	if (cubeb_enumerate_devices(s_context, CUBEB_DEVICE_TYPE_OUTPUT, &devices) != CUBEB_OK)
		return result;

	result.reserve(devices.count + 1); // The default device already occupies one element

	for (size_t i = 0; i < devices.count; ++i)
	{
		// const auto& device = devices.device[i];
		if (devices.device[i].state == CUBEB_DEVICE_STATE_ENABLED)
		{
			auto device = std::make_shared<CubebDeviceDescription>(devices.device[i].devid, devices.device[i].device_id,
																   boost::nowide::widen(
																	   devices.device[i].friendly_name));
			result.emplace_back(device);
		}
	}

	cubeb_device_collection_destroy(s_context, &devices);

	return result;
}
