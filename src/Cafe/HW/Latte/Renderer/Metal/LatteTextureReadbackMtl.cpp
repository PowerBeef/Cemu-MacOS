#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cemu/Telemetry/Telemetry.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureReadbackMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"

LatteTextureReadbackInfoMtl::~LatteTextureReadbackInfoMtl()
{
    if (m_commandBuffer)
        m_commandBuffer->release();
}

void LatteTextureReadbackInfoMtl::StartTransfer()
{
	cemu_assert(m_textureView);

	auto* baseTexture = (LatteTextureMtl*)m_textureView->baseTexture;

	cemu_assert_debug(m_textureView->firstSlice == 0);
	cemu_assert_debug(m_textureView->firstMip == 0);
	cemu_assert_debug(m_textureView->baseTexture->dim != Latte::E_DIM::DIM_3D);

	size_t bytesPerRow = GetMtlTextureBytesPerRow(baseTexture->format, baseTexture->isDepth, baseTexture->width);
	size_t bytesPerImage = GetMtlTextureBytesPerImage(baseTexture->format, baseTexture->isDepth, baseTexture->height, bytesPerRow);

	// How much unrelated work is already recorded into the command buffer this blit joins. The
	// blit runs after all of it, because a command buffer's encoders execute in order.
	//
	// Measured, and it is NOT why GX2DrawDone's drain is expensive: 169 draws/frame summed over
	// all readbacks (~28 each) against 3,516 draws/frame. Giving the blit its own command buffer
	// *outside* the m_event chain -- so it waited on neither the preceding buffer nor the ~500
	// draws in it -- was implemented and A/B'd at n=3: every metric overlapped between arms
	// (frame 49.90 both, critical path 35.13-35.22 vs 35.10-35.23, force-finish 6.21-6.34 vs
	// 6.14-6.27), and only gpu.command_buffers separated, 7 -> 9, proving the arm was live.
	// The wait is the GPU genuinely still having ~6 ms of work to do, not the blit being queued
	// behind anything. Don't re-raise it; reordering cannot help, only less GPU work can.
	TLM_ADD(Gpu, GpuReadbackDrawsAhead, m_mtlr->GetRecordedDrawcalls());

	auto blitCommandEncoder = m_mtlr->GetBlitCommandEncoder();

	blitCommandEncoder->copyFromTexture(baseTexture->GetTexture(), 0, 0, MTL::Origin{0, 0, 0}, MTL::Size{(uint32)baseTexture->width, (uint32)baseTexture->height, 1}, m_mtlr->GetTextureReadbackBuffer(), m_bufferOffset, bytesPerRow, bytesPerImage);

	// Deliberately still a retained command buffer rather than a timeline id. An id would make
	// IsFinished() depend on ProcessFinishedCommandBuffers having run, where the retained
	// pointer polls Metal directly -- and a readback that merely *looks* unfinished costs a
	// forced blocking wait at the next GX2DrawDone. Safe here because GetBlitCommandEncoder
	// above has already ensured a live, uncommitted buffer exists.
	m_commandBuffer = m_mtlr->GetCurrentCommandBuffer()->retain();
	// TODO: uncomment?
	//m_mtlr->RequestSoonCommit();
	m_mtlr->CommitCommandBuffer();
}

bool LatteTextureReadbackInfoMtl::IsFinished()
{
    // Command buffer wasn't even comitted, let's commit immediately
    //if (m_mtlr->GetCurrentCommandBuffer() == m_commandBuffer)
    //    m_mtlr->CommitCommandBuffer();

    return CommandBufferCompleted(m_commandBuffer);
}

void LatteTextureReadbackInfoMtl::ForceFinish()
{
    // The other half of GX2DrawDone's drain: a blocking GPU wait on the Latte thread, once
    // per in-flight readback.
    TLM_INC(Gpu, GpuReadbackForceFinishes);
    TLM_SCOPED_TIMER(Gpu, GpuReadbackForceFinishNs);
    m_commandBuffer->waitUntilCompleted();
}

uint8* LatteTextureReadbackInfoMtl::GetData()
{
	return (uint8*)m_mtlr->GetTextureReadbackBuffer()->contents() + m_bufferOffset;
}
