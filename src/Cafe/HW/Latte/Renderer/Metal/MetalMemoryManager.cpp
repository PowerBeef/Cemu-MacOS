#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryManager.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalVoidVertexPipeline.h"

#include "Cemu/Logging/CemuLogging.h"
#include "Common/precompiled.h"
#include "HW/MMU/MMU.h"
#include "config/CemuConfig.h"

MetalMemoryManager::~MetalMemoryManager()
{
    if (m_bufferCache)
    {
        m_bufferCache->release();
    }
}

void* MetalMemoryManager::AcquireTextureUploadBuffer(size_t size)
{
    if (m_textureUploadBuffer.size() < size)
    {
        m_textureUploadBuffer.resize(size);
    }

    return m_textureUploadBuffer.data();
}

void MetalMemoryManager::ReleaseTextureUploadBuffer(uint8* mem)
{
    cemu_assert_debug(m_textureUploadBuffer.data() == mem);
	m_textureUploadBuffer.clear();
}

void MetalMemoryManager::InitBufferCache(size_t size)
{
    cemu_assert_debug(!m_bufferCache);

    m_metalBufferCacheMode = g_current_game_profile->GetBufferCacheMode();

    if (m_metalBufferCacheMode == MetalBufferCacheMode::Auto)
    {
        // Every device this fork supports has unified memory, so a device-private cache buys
        // nothing: the upload cannot be a memcpy, it has to go through a staging buffer and a
        // blit -- and asking for a blit encoder tears down whatever render pass is live. That
        // made buffer uploads, not texture copies, the single largest source of render-pass
        // churn. Measured on BotW at the Shrine of Resurrection (see 00-master-plan.md):
        //
        //                    passes/frame   same-FBO splits/frame   GPU ms/frame
        //   DevicePrivate       179.4              38.6                18.5
        //   DeviceShared        149.4              12.8                15.6
        //
        // The trade is a weaker ordering guarantee. A staging blit is ordered on the GPU
        // timeline, so an already-encoded draw still reads the old contents; a memcpy into
        // shared storage lands immediately and an in-flight draw may observe the new data one
        // draw early. No difference was visible across ~8000 frames of BotW, and Cemu already
        // shipped this mode for Wind Waker HD, but a title that shows artifacts can be pinned
        // back to "device private" through its game profile.
        m_metalBufferCacheMode = MetalBufferCacheMode::DeviceShared;
    }

    // First, try to import the host memory as a buffer
    if (m_metalBufferCacheMode == MetalBufferCacheMode::Host)
    {
        if (m_mtlr->HasUnifiedMemory())
        {
            m_importedMemBaseAddress = mmuRange_MEM2.getBase();
           	m_hostAllocationSize = mmuRange_MEM2.getSize();
            m_bufferCache = m_mtlr->GetDevice()->newBuffer(memory_getPointerFromVirtualOffset(m_importedMemBaseAddress), m_hostAllocationSize, MTL::ResourceStorageModeShared, nullptr);
            if (!m_bufferCache)
            {
                cemuLog_log(LogType::Force, "Failed to import host memory as a buffer, using device shared mode instead");
                m_metalBufferCacheMode = MetalBufferCacheMode::DeviceShared;
            }
        }
        else
        {
            cemuLog_log(LogType::Force, "Host buffer cache mode is only available on unified memory systems, using device shared mode instead");
            m_metalBufferCacheMode = MetalBufferCacheMode::DeviceShared;
        }
    }

    if (!m_bufferCache)
        m_bufferCache = m_mtlr->GetDevice()->newBuffer(size, (m_metalBufferCacheMode == MetalBufferCacheMode::DevicePrivate ? MTL::ResourceStorageModePrivate : MTL::ResourceStorageModeShared));

#ifdef CEMU_DEBUG_ASSERT
    m_bufferCache->setLabel(GetLabel("Buffer cache", m_bufferCache));
#endif
}

void MetalMemoryManager::UploadToBufferCache(const void* data, size_t offset, size_t size)
{
    cemu_assert_debug(m_metalBufferCacheMode != MetalBufferCacheMode::Host);
    cemu_assert_debug(m_bufferCache);
    cemu_assert_debug((offset + size) <= m_bufferCache->length());

    if (m_metalBufferCacheMode == MetalBufferCacheMode::DevicePrivate)
    {
        auto blitCommandEncoder = m_mtlr->GetBlitCommandEncoder();

        auto allocation = m_stagingAllocator.AllocateBufferMemory(size, 1);
        memcpy(allocation.memPtr, data, size);
        m_stagingAllocator.FlushReservation(allocation);

        blitCommandEncoder->copyFromBuffer(allocation.mtlBuffer, allocation.bufferOffset, m_bufferCache, offset, size);

        //m_mtlr->CopyBufferToBuffer(allocation.mtlBuffer, allocation.bufferOffset, m_bufferCache, offset, size, ALL_MTL_RENDER_STAGES, ALL_MTL_RENDER_STAGES);
    }
    else
    {
        memcpy((uint8*)m_bufferCache->contents() + offset, data, size);
    }
}

void MetalMemoryManager::CopyBufferCache(size_t srcOffset, size_t dstOffset, size_t size)
{
    cemu_assert_debug(m_metalBufferCacheMode != MetalBufferCacheMode::Host);
    cemu_assert_debug(m_bufferCache);

    if (m_metalBufferCacheMode == MetalBufferCacheMode::DevicePrivate)
        m_mtlr->CopyBufferToBuffer(m_bufferCache, srcOffset, m_bufferCache, dstOffset, size, ALL_MTL_RENDER_STAGES, ALL_MTL_RENDER_STAGES);
    else
        memcpy((uint8*)m_bufferCache->contents() + dstOffset, (uint8*)m_bufferCache->contents() + srcOffset, size);
}
