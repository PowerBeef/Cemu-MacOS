#pragma once

#include "IAudioAPI.h"

#include <cubeb/cubeb.h>

#include <memory>

class CubebAPI : public IAudioAPI
{
public:
	class CubebDeviceDescription : public DeviceDescription
	{
	public:
		CubebDeviceDescription(cubeb_devid devid, std::string device_id, const std::wstring& name)
			: DeviceDescription(name), m_devid(devid), m_device_id(std::move(device_id)) { }

		std::wstring GetIdentifier() const override { return  boost::nowide::widen(m_device_id); }
		cubeb_devid GetDeviceId() const { return m_devid; }

	private:
		cubeb_devid m_devid;
		std::string m_device_id;
	};

	using CubebDeviceDescriptionPtr = std::shared_ptr<CubebDeviceDescription>;

	CubebAPI(cubeb_devid devid, uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample);
	~CubebAPI();

	AudioAPI GetType() const override { return Cubeb; }
	bool NeedAdditionalBlocks() const override;
	bool FeedBlock(sint16* data) override;
	bool Play() override;
	bool Stop() override;
	void SetVolume(sint32 volume) override;

	static std::vector<DeviceDescriptionPtr> GetDevices();

	static bool InitializeStatic();
	static void Destroy();

private:
	inline static cubeb* s_context = nullptr;

	cubeb_stream* m_stream = nullptr;
	bool m_is_playing = false;

	// Single-producer / single-consumer ring buffer.
	//
	// The producer is the emulation thread (FeedBlock); the consumer is cubeb's
	// data_cb, which CoreAudio runs on a Mach time-constraint thread. That thread sits
	// above every QoS class, so a mutex shared with a normal-priority producer is an
	// unfixable priority inversion -- no QoS assignment can help, because there is no
	// class high enough to represent "real-time". The previous implementation also did
	// a std::vector::erase from the front inside the callback, i.e. an O(n) memmove of
	// up to 4 blocks on the realtime thread.
	//
	// Wait-free on both sides: each side owns one index and only ever publishes it with
	// a release store, reading the other with an acquire load.
	std::unique_ptr<uint8[]> m_ringBuffer;
	size_t m_ringSize = 0;                    // capacity in bytes, > any single block
	std::atomic<size_t> m_ringHead{0};        // next byte to read  (consumer owns)
	std::atomic<size_t> m_ringTail{0};        // next byte to write (producer owns)

	size_t RingBytesUsed() const
	{
		const size_t tail = m_ringTail.load(std::memory_order_acquire);
		const size_t head = m_ringHead.load(std::memory_order_acquire);
		return tail - head;
	}

	static long data_cb(cubeb_stream* stream, void* user, const void* inputbuffer, void* outputbuffer, long nframes);
};
