#pragma once

#include "Cafe/HW/Latte/Core/LatteQueryObject.h"

#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"

class LatteQueryObjectMtl : public LatteQueryObject
{
public:
	LatteQueryObjectMtl(class MetalRenderer* mtlRenderer) : m_mtlr{mtlRenderer} {}

	bool getResult(uint64& numSamplesPassed) override;
	void begin() override;
	void end() override;

	void GrowRange()
	{
	    m_range.end++;
	}

private:
	class MetalRenderer* m_mtlr;

	MetalQueryRange m_range = {INVALID_UINT32, INVALID_UINT32};
	// The submission this query's results become valid after. An id rather than a retained
	// MTL::CommandBuffer*: the query no longer has to keep a command buffer alive just to be
	// able to ask whether it finished, and there is no destructor obligation.
	uint64 m_commandBufferId = UINT64_MAX;
};
