#pragma once

#include <string>
#include <cstdint>

// CPU feature reporting for Apple silicon.
//
// These flags are informational. Clang already targets apple-m1 by default for
// arm64-apple-macos (+lse +aes +sha2 +dotprod +fullfp16), so those features are
// baseline and compiled code paths must NOT be gated on them -- the compiler has
// already emitted LSE atomics unconditionally. They exist for the startup log
// line and for the one place a runtime check is free insurance (AES dispatch).
class CPUFeaturesImpl
{
public:
	CPUFeaturesImpl();

	std::string GetCPUName(); // empty if not available
	std::string GetCommaSeparatedExtensionList();

	struct
	{
		bool lse{ false };      // FEAT_LSE     - atomic CAS/SWP, used by the recompiler
		bool lse2{ false };     // FEAT_LSE2    - relaxed alignment rules for atomics
		bool aes{ false };      // FEAT_AES     - AESE/AESD, used by util/crypto/aes128
		bool sha256{ false };   // FEAT_SHA256
		bool sha512{ false };   // FEAT_SHA512
		bool crc32{ false };    // FEAT_CRC32
		bool dotprod{ false };  // FEAT_DotProd
		bool fp16{ false };     // FEAT_FP16    - half-precision arithmetic
		bool i8mm{ false };     // FEAT_I8MM
		bool bf16{ false };     // FEAT_BF16
	}arm;

	// Core topology. Worker pools should be sized against the efficiency-core
	// count so they do not contend with the guest cores and the render thread
	// for the performance cluster.
	uint32_t physicalCores{ 0 };
	uint32_t performanceCores{ 0 };
	uint32_t efficiencyCores{ 0 };
	uint32_t pageSize{ 0 };
	uint32_t cacheLineSize{ 0 };

private:
	char m_cpuBrandName[0x40]{ 0 };
};

extern CPUFeaturesImpl g_CPUFeatures;
