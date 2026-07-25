# Reference: Vulkan render-pass self-dependency mechanism

Extracted from the Vulkan backend before deletion. This is the design the Metal
backend must adopt (plan item G3.1) — NOT the older per-draw shader-hash-scanning
version that is commented out in MetalRenderer.cpp:1130-1160 / :1970-2019.

Key property: a monotonic stamp on each attachment's base texture, intersected
against the bound texture set. O(bound textures), computed once per FBO/binding
change, not per draw.

## CachedFBOVk.cpp — CheckForSelfDependency()
```cpp
	m_vkRenderingInfo.pNext = nullptr;
	m_vkRenderingInfo.flags = 0;
	m_vkRenderingInfo.renderArea.offset = { 0, 0 };
	m_vkRenderingInfo.renderArea.extent = m_extend;
	m_vkRenderingInfo.viewMask = 0; // multiview disabled
	m_vkRenderingInfo.layerCount = 1;
}

static uint32 s_selfDependencyCheckIndex = 1;

CachedFBOVk::RendertargetSelfDependencyMask CachedFBOVk::CheckForSelfDependency(VkDescriptorSetInfo* vsDS, VkDescriptorSetInfo* gsDS, VkDescriptorSetInfo* psDS) const
{
	s_selfDependencyCheckIndex++;
	const uint32 curColIndex = s_selfDependencyCheckIndex;
	for (auto& colorAttachment : colorBuffer)
	{
		if (colorAttachment.texture)
		{
			LatteTextureVk* vkTex = static_cast<LatteTextureVk*>(colorAttachment.texture->baseTexture);
			vkTex->m_selfDependencyCheckIndex = curColIndex;
			vkTex->m_selfDependencyCheckAspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		}
	}
	if (depthBuffer.texture)
	{
		LatteTextureVk* vkTex = static_cast<LatteTextureVk*>(depthBuffer.texture->baseTexture);
		vkTex->m_selfDependencyCheckIndex = curColIndex;
		vkTex->m_selfDependencyCheckAspectMask = depthBuffer.hasStencil ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT): VK_IMAGE_ASPECT_DEPTH_BIT;
	}

	auto getSelfDependencyMask = [curColIndex](VkDescriptorSetInfo* ds) -> VkImageAspectFlags
	{
		VkImageAspectFlags aspectMask = 0;
		if (!ds)
			return aspectMask;
		for (auto& itr : ds->list_fboCandidates)
		{
			if (itr->m_selfDependencyCheckIndex == curColIndex)
				aspectMask |= itr->m_selfDependencyCheckAspectMask;
		}
		return aspectMask;
	};

	RendertargetSelfDependencyMask selfDepInfo{};
	VkImageAspectFlags vertexAspectFlags = getSelfDependencyMask(vsDS);
	VkImageAspectFlags geometryAspectFlags = getSelfDependencyMask(gsDS);
	VkImageAspectFlags pixelAspectFlags = getSelfDependencyMask(psDS);
	selfDepInfo.aspectMaskFlags = vertexAspectFlags | geometryAspectFlags | pixelAspectFlags;
	selfDepInfo.hasNonPixelSelfDependency = (vertexAspectFlags | geometryAspectFlags) != 0;
	return selfDepInfo;
}
```

## CachedFBOVk.h
```cpp
#pragma once

#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VKRBase.h"

class CachedFBOVk : public LatteCachedFBO
{
public:
	CachedFBOVk(uint64 key, VkDevice device)
		: LatteCachedFBO(key), m_device(device)
	{
		CreateRenderPass();
		CreateFramebuffer();
		InitDynamicRenderingData();
	}

	~CachedFBOVk();

	static inline FSpinlock s_spinlockDependency;

	VKRObjectFramebuffer* GetFramebufferObj() const
	{
		return m_vkrObjFramebuffer;
	}

	VKRObjectRenderPass* GetRenderPassObj() const
	{
		return m_vkrObjRenderPass;
	}

	// for KHR_dynamic_rendering
	VkRenderingInfoKHR* GetRenderingInfo()
	{
		return &m_vkRenderingInfo;
	}


	void TrackDependency(class PipelineInfo* pipelineInfo)
	{
		s_spinlockDependency.lock();
		m_usedByPipelines.emplace_back(pipelineInfo);
		s_spinlockDependency.unlock();
	}

	void RemoveDependency(class PipelineInfo* pipelineInfo)
	{
		s_spinlockDependency.lock();
		vectorRemoveByValue(m_usedByPipelines, pipelineInfo);
		s_spinlockDependency.unlock();
	}

	[[nodiscard]] const VkExtent2D& GetExtend() const { return m_extend;}

	struct RendertargetSelfDependencyMask
	{
		VkImageAspectFlags aspectMaskFlags{}; // aspect flags which are simultaneously sampled and written
		bool hasNonPixelSelfDependency{false};

		VkImageAspectFlags GetAspectMask() const
		{
			return aspectMaskFlags;
		}

		bool HasSelfDependency() const
		{
			return GetAspectMask() != 0;
		}

		bool HasVertexOrGeometrySelfDependency() const
		{
			return hasNonPixelSelfDependency; // vertex or geometry shader samples texture which is written to
		}
	};

	// checks if any of the sampled textures are output by the FBO
	RendertargetSelfDependencyMask CheckForSelfDependency(VkDescriptorSetInfo* vsDS, VkDescriptorSetInfo* gsDS, VkDescriptorSetInfo* psDS) const;

private:

	void CreateRenderPass();
```

## VulkanRendererCore.cpp — consumer (pixel-only vs vertex/geometry dependency)
```cpp
	frontScale /= 16.0f;

	vkCmdSetDepthBias(m_state.currentCommandBuffer, frontOffset, offsetClamp, frontScale);
}

bool s_syncOnNextDraw = false;

void VulkanRenderer::draw_setRenderPass()
{
	CachedFBOVk* fboVk = m_state.activeFBO;
	// note - pixel self dependency can be handled via feedback_loop extension
	// vertex/geometry self dependency needs renderpass split
	CachedFBOVk::RendertargetSelfDependencyMask renderSelfDependencyInfo{};

	// update self-dependency state
	if (m_state.descriptorSetsChanged || m_state.activeRenderpassFBO != fboVk)
	{
		renderSelfDependencyInfo = fboVk->CheckForSelfDependency(m_state.activeVertexDS, m_state.activeGeometryDS, m_state.activePixelDS);
	}

	auto vkObjRenderPass = fboVk->GetRenderPassObj();
	auto vkObjFramebuffer = fboVk->GetFramebufferObj();

	bool feedbackLoopHandlesSelfDependency = UseAttachmentFeedbackLoop() && renderSelfDependencyInfo.HasSelfDependency() && !renderSelfDependencyInfo.HasVertexOrGeometrySelfDependency();
	bool selfDependencyNeedsPassSplit = renderSelfDependencyInfo.HasSelfDependency() && !feedbackLoopHandlesSelfDependency;
	bool overridePassReuse = selfDependencyNeedsPassSplit && (GetConfig().vk_accurate_barriers || m_state.activePipelineInfo->neverSkipAccurateBarrier);

	if (!overridePassReuse && m_state.activeRenderpassFBO == fboVk)
	{
		if (m_state.descriptorSetsChanged)
			sync_inputTexturesChanged(feedbackLoopHandlesSelfDependency);
		if (UseAttachmentFeedbackLoop() && renderSelfDependencyInfo.GetAspectMask() != m_state.feedbackLoopImageAspect)
		{
			m_state.feedbackLoopImageAspect = renderSelfDependencyInfo.GetAspectMask();
			vkCmdSetAttachmentFeedbackLoopEnableEXT(m_state.currentCommandBuffer, renderSelfDependencyInfo.GetAspectMask());
		}
		return;
	}
	draw_endRenderPass();
	if (m_state.descriptorSetsChanged)
		sync_inputTexturesChanged();

	// assume that FBO changed, update self-dependency state
	renderSelfDependencyInfo = fboVk->CheckForSelfDependency(m_state.activeVertexDS, m_state.activeGeometryDS, m_state.activePixelDS);

	sync_RenderPassLoadTextures(fboVk);

	if (m_featureControl.deviceExtensions.dynamic_rendering)
	{
		vkCmdBeginRenderingKHR(m_state.currentCommandBuffer, fboVk->GetRenderingInfo());
	}
```
