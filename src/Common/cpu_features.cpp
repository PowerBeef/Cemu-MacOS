#include "cpu_features.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <cstring>
#include <vector>

namespace
{
	bool ReadSysctlBool(const char* name)
	{
		int32_t value = 0;
		size_t size = sizeof(value);
		if (sysctlbyname(name, &value, &size, nullptr, 0) != 0)
			return false;
		return value != 0;
	}

	uint32_t ReadSysctlU32(const char* name)
	{
		uint64_t value = 0;
		size_t size = sizeof(value);
		if (sysctlbyname(name, &value, &size, nullptr, 0) != 0)
			return 0;
		return (uint32_t)value;
	}
}

CPUFeaturesImpl::CPUFeaturesImpl()
{
	// brand name, e.g. "Apple M2"
	size_t size = 0;
	if (sysctlbyname("machdep.cpu.brand_string", nullptr, &size, nullptr, 0) == 0 && size > 0)
	{
		std::vector<char> buffer(size);
		if (sysctlbyname("machdep.cpu.brand_string", buffer.data(), &size, nullptr, 0) == 0 && size > 0)
		{
			strncpy(m_cpuBrandName, buffer.data(), sizeof(m_cpuBrandName) - 1);
			m_cpuBrandName[sizeof(m_cpuBrandName) - 1] = '\0';
		}
	}

	arm.lse     = ReadSysctlBool("hw.optional.arm.FEAT_LSE");
	arm.lse2    = ReadSysctlBool("hw.optional.arm.FEAT_LSE2");
	arm.aes     = ReadSysctlBool("hw.optional.arm.FEAT_AES");
	arm.sha256  = ReadSysctlBool("hw.optional.arm.FEAT_SHA256");
	arm.sha512  = ReadSysctlBool("hw.optional.arm.FEAT_SHA512");
	arm.crc32   = ReadSysctlBool("hw.optional.armv8_crc32");
	arm.dotprod = ReadSysctlBool("hw.optional.arm.FEAT_DotProd");
	arm.fp16    = ReadSysctlBool("hw.optional.arm.FEAT_FP16");
	arm.i8mm    = ReadSysctlBool("hw.optional.arm.FEAT_I8MM");
	arm.bf16    = ReadSysctlBool("hw.optional.arm.FEAT_BF16");

	physicalCores    = ReadSysctlU32("hw.physicalcpu");
	performanceCores = ReadSysctlU32("hw.perflevel0.physicalcpu");
	efficiencyCores  = ReadSysctlU32("hw.perflevel1.physicalcpu");
	pageSize         = ReadSysctlU32("hw.pagesize");
	cacheLineSize    = ReadSysctlU32("hw.cachelinesize");

	// A machine without a heterogeneous topology reports no perflevel nodes.
	if (performanceCores == 0)
		performanceCores = physicalCores;
}

std::string CPUFeaturesImpl::GetCPUName()
{
	return { m_cpuBrandName };
}

std::string CPUFeaturesImpl::GetCommaSeparatedExtensionList()
{
	std::string tmp;
	auto appendExt = [&tmp](const char* str)
	{
		if (!tmp.empty())
			tmp.append(", ");
		tmp.append(str);
	};
	if (arm.lse)     appendExt("LSE");
	if (arm.lse2)    appendExt("LSE2");
	if (arm.aes)     appendExt("AES");
	if (arm.sha256)  appendExt("SHA256");
	if (arm.sha512)  appendExt("SHA512");
	if (arm.crc32)   appendExt("CRC32");
	if (arm.dotprod) appendExt("DotProd");
	if (arm.fp16)    appendExt("FP16");
	if (arm.i8mm)    appendExt("I8MM");
	if (arm.bf16)    appendExt("BF16");
	return tmp;
}

CPUFeaturesImpl g_CPUFeatures;
