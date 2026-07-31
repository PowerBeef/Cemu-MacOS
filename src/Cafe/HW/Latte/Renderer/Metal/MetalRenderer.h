#pragma once

#include "Cafe/HW/Latte/Renderer/Renderer.h"

#include "Cafe/HW/Latte/Renderer/Metal/MetalLayerHandle.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalPerformanceMonitor.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalOutputShaderCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalAttachmentsInfo.h"

enum MetalGeneralShaderType
{
    METAL_GENERAL_SHADER_TYPE_VERTEX,
    METAL_GENERAL_SHADER_TYPE_GEOMETRY,
    METAL_GENERAL_SHADER_TYPE_FRAGMENT,

    METAL_GENERAL_SHADER_TYPE_TOTAL
};

inline MetalGeneralShaderType GetMtlGeneralShaderType(LatteConst::ShaderType shaderType)
{
    switch (shaderType)
    {
    case LatteConst::ShaderType::Vertex:
        return METAL_GENERAL_SHADER_TYPE_VERTEX;
    case LatteConst::ShaderType::Geometry:
        return METAL_GENERAL_SHADER_TYPE_GEOMETRY;
    case LatteConst::ShaderType::Pixel:
        return METAL_GENERAL_SHADER_TYPE_FRAGMENT;
    default:
        return METAL_GENERAL_SHADER_TYPE_TOTAL;
    }
}

enum MetalShaderType
{
    METAL_SHADER_TYPE_VERTEX,
    METAL_SHADER_TYPE_OBJECT,
    METAL_SHADER_TYPE_MESH,
    METAL_SHADER_TYPE_FRAGMENT,

    METAL_SHADER_TYPE_TOTAL
};

inline MetalShaderType GetMtlShaderType(LatteConst::ShaderType shaderType, bool usesGeometryShader)
{
    switch (shaderType)
    {
    case LatteConst::ShaderType::Vertex:
        if (usesGeometryShader)
            return METAL_SHADER_TYPE_OBJECT;
        else
            return METAL_SHADER_TYPE_VERTEX;
    case LatteConst::ShaderType::Geometry:
        return METAL_SHADER_TYPE_MESH;
    case LatteConst::ShaderType::Pixel:
        return METAL_SHADER_TYPE_FRAGMENT;
    default:
        return METAL_SHADER_TYPE_TOTAL;
    }
}

struct MetalEncoderState
{
    MTL::RenderPipelineState* m_renderPipelineState = nullptr;
    MTL::DepthStencilState* m_depthStencilState = nullptr;
    MTL::CullMode m_cullMode = MTL::CullModeNone;
    MTL::Winding m_frontFaceWinding = MTL::WindingClockwise;
    MTL::Viewport m_viewport;
    MTL::ScissorRect m_scissor;
    uint32 m_stencilRefFront = 0;
    uint32 m_stencilRefBack = 0;
    uint32 m_blendColor[4] = {0};
    uint32 m_depthBias = 0;
   	uint32 m_depthSlope = 0;
   	uint32 m_depthClamp = 0;
    bool m_depthClipEnable = true;
    struct {
        MTL::Buffer* m_buffer;
        size_t m_offset;
    } m_buffers[METAL_SHADER_TYPE_TOTAL][MAX_MTL_BUFFERS];
    MTL::Texture* m_textures[METAL_SHADER_TYPE_TOTAL][MAX_MTL_TEXTURES];
    MTL::SamplerState* m_samplers[METAL_SHADER_TYPE_TOTAL][MAX_MTL_SAMPLERS];
};

struct MetalStreamoutState
{
	struct
	{
		bool enabled;
		uint32 ringBufferOffset;
	} buffers[LATTE_NUM_STREAMOUT_BUFFER];
	sint32 verticesPerInstance;
};

struct MetalActiveFBOState
{
    class CachedFBOMtl* m_fbo = nullptr;
    MetalAttachmentsInfo m_attachmentsInfo;
};

struct MetalState
{
    MetalEncoderState m_encoderState{};

    bool m_usesSRGB = false;

    bool m_skipDrawSequence = false;
    bool m_isFirstDrawInRenderPass = true;

    MetalActiveFBOState m_activeFBO;
    // If the FBO changes, but it's the same FBO as the last one with some omitted attachments, this FBO doesn't change
    MetalActiveFBOState m_lastUsedFBO;
    bool m_fboChanged = false;

    size_t m_vertexBufferOffsets[MAX_MTL_VERTEX_BUFFERS];
    class LatteTextureViewMtl* m_textures[LATTE_NUM_MAX_TEX_UNITS * 3] = {nullptr};
    size_t m_uniformBufferOffsets[METAL_GENERAL_SHADER_TYPE_TOTAL][MAX_MTL_BUFFERS];

    MTL::Viewport m_viewport;
    MTL::ScissorRect m_scissor;

    MetalStreamoutState m_streamoutState;
};

struct MetalCommandBuffer
{
    MTL::CommandBuffer* m_commandBuffer = nullptr;
    bool m_commited = false;
};

enum class MetalEncoderType
{
    None,
    Render,
    Compute,
    Blit,
};

class MetalRenderer : public Renderer
{
public:
    static constexpr uint32 OCCLUSION_QUERY_POOL_SIZE = 1024;
    static constexpr uint32 TEXTURE_READBACK_SIZE = 32 * 1024 * 1024; // 32 MB

    struct DeviceInfo
    {
        std::string name;
        uint64 uuid;
    };

    static std::vector<DeviceInfo> GetDevices();

    MetalRenderer();
	~MetalRenderer() override;

	static MetalRenderer* GetInstance() {
	    return static_cast<MetalRenderer*>(g_renderer.get());
	}

	// Helper functions
	MTL::Device* GetDevice() const {
        return m_device;
    }

	void InitializeLayer(const Vector2i& size, bool mainWindow);
	void ShutdownLayer(bool mainWindow);
	void ResizeLayer(const Vector2i& size, bool mainWindow);

	void Initialize() override;
	void Shutdown() override;
	bool IsPadWindowActive() override;

	bool GetVRAMInfo(int& usageInMB, int& totalInMB) const override;

	void ClearColorbuffer(bool padView) override;
	void DrawEmptyFrame(bool mainWindow) override;
	void SwapBuffers(bool swapTV, bool swapDRC) override;

	void HandleScreenshotRequest(LatteTextureView* texView, bool padView) override;

	void DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter,
									sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight,
									bool padView, bool clearBackground) override;
	bool BeginFrame(bool mainWindow) override;

	// flush control
	void Flush(bool waitIdle = false) override;		// called when explicit flush is required (e.g. by imgui)
	void NotifyLatteCommandProcessorIdle(CommandProcessorIdleReason reason) override;

	// imgui
	bool ImguiBegin(bool mainWindow) override;
	void ImguiEnd() override;
	ImTextureID GenerateTexture(const std::vector<uint8>& data, const Vector2i& size) override;
	void DeleteTexture(ImTextureID id) override;
	void DeleteFontTextures() override;

	bool UseTFViaSSBO() const override { return true; }
	void AppendOverlayDebugInfo() override;

	// rendertarget
	void renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ = false) override;
	void renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight) override;

	LatteCachedFBO* rendertarget_createCachedFBO(uint64 key) override;
	void rendertarget_deleteCachedFBO(LatteCachedFBO* fbo) override;
	void rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo) override;

	// texture functions
	void* texture_acquireTextureUploadBuffer(uint32 size) override;
	void texture_releaseTextureUploadBuffer(uint8* mem) override;

	TextureDecoder* texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height) override;

	void texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex) override;
	void texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize) override;
	void texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a) override;
	void texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue) override;

	LatteTexture* texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth) override;

	void texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit) override;
	void texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth) override;

	LatteTextureReadbackInfo* texture_createReadback(LatteTextureView* textureView) override;

	// surface copy
	void surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height) override;

	// buffer cache
	void bufferCache_init(const sint32 bufferSize) override;
	void bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset) override;
	void bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size) override;
	void bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size) override;

	void buffer_bindVertexBuffer(uint32 bufferIndex, uint32 offset, uint32 size) override;
	void buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size) override;

	// shader
	RendererShader* shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool compileAsync, bool isGfxPackSource) override;

	// streamout
	void streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize) override;
	void streamout_begin() override;
	void streamout_rendererFinishDrawcall() override;

	// core drawing logic
	void draw_beginSequence() override;
	void draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext) override;
	void draw_endSequence() override;

	void draw_updateVertexBuffersDirectAccess();
	void draw_updateUniformBuffersDirectAccess(LatteDecompilerShader* shader, const uint32 uniformBufferRegOffset);

	void draw_handleSpecialState5();

	// index
	IndexAllocation indexData_reserveIndexMemory(uint32 size) override;
	void indexData_releaseIndexMemory(IndexAllocation& allocation) override;
	void indexData_uploadIndexMemory(IndexAllocation& allocation) override;

	// occlusion queries
	LatteQueryObject* occlusionQuery_create() override;
	void occlusionQuery_destroy(LatteQueryObject* queryObj) override;
	void occlusionQuery_flush() override;
	void occlusionQuery_updateState() override;

	// Helpers
	MetalPerformanceMonitor& GetPerformanceMonitor() { return m_performanceMonitor; }

	void SetShouldMaximizeConcurrentCompilation(bool shouldMaximizeConcurrentCompilation)
	{
	    if (m_supportsMetal3)
	        m_device->setShouldMaximizeConcurrentCompilation(shouldMaximizeConcurrentCompilation);
	}

	bool IsCommandBufferActive() const
	{
        return (m_currentCommandBuffer.m_commandBuffer && !m_currentCommandBuffer.m_commited);
    }

	MTL::CommandBuffer* GetCurrentCommandBuffer() const
    {
        cemu_assert_debug(m_currentCommandBuffer.m_commandBuffer);

        return m_currentCommandBuffer.m_commandBuffer;
    }

    // The id the work being recorded right now will be submitted under. Safe to call with no
    // buffer open: an unsubmitted id can never satisfy HasCommandBufferFinished, which is the
    // conservative answer.
    uint64 GetCurrentCommandBufferId() const
    {
        return m_submittedCount;
    }

    bool HasCommandBufferFinished(uint64 id) const
    {
        return id < m_retiredCount;
    }

    // Blocks until the given id has retired. Submits first if it is still being recorded.
    void WaitCommandBufferFinished(uint64 id);

    uint32 GetRecordedDrawcalls() const
    {
        return m_recordedDrawcalls;
    }

    void RequestSoonCommit()
    {
        m_commitTreshold = m_recordedDrawcalls + 8;
    }

    MTL::CommandEncoder* GetCommandEncoder()
    {
        return m_commandEncoder;
    }

    MetalEncoderType GetEncoderType()
    {
        return m_encoderType;
    }

    void ResetEncoderState()
    {
        m_state.m_encoderState = {};

        // TODO: set viewport and scissor to render target dimensions if render commands

        for (uint32 i = 0; i < METAL_SHADER_TYPE_TOTAL; i++)
        {
            for (uint32 j = 0; j < MAX_MTL_BUFFERS; j++)
                m_state.m_encoderState.m_buffers[i][j] = {nullptr};
            for (uint32 j = 0; j < MAX_MTL_TEXTURES; j++)
                m_state.m_encoderState.m_textures[i][j] = nullptr;
            for (uint32 j = 0; j < MAX_MTL_SAMPLERS; j++)
                m_state.m_encoderState.m_samplers[i][j] = nullptr;
        }
    }

    MetalEncoderState& GetEncoderState()
    {
        return m_state.m_encoderState;
    }

    void SetBuffer(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::Buffer* buffer, size_t offset, uint32 index);
    void SetTexture(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::Texture* texture, uint32 index);
    void SetSamplerState(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::SamplerState* samplerState, uint32 index);

	MTL::CommandBuffer* GetCommandBuffer();
	MTL::RenderCommandEncoder* GetTemporaryRenderCommandEncoder(MTL::RenderPassDescriptor* renderPassDescriptor);
    void NotePassStructure(class LatteCachedFBO* fbo);
    void NoteSplitCause();
	MTL::RenderCommandEncoder* GetRenderCommandEncoder(bool forceRecreate = false);
    MTL::ComputeCommandEncoder* GetComputeCommandEncoder();
    MTL::BlitCommandEncoder* GetBlitCommandEncoder();
    void EndEncoding();
    void CommitCommandBuffer();
    void ProcessFinishedCommandBuffers();

    bool AcquireDrawable(bool mainWindow);

    //bool CheckIfRenderPassNeedsFlush(LatteDecompilerShader* shader);
    void BindStageResources(MTL::RenderCommandEncoder* renderCommandEncoder, LatteDecompilerShader* shader, bool usesGeometryShader);

    void ClearColorTextureInternal(MTL::Texture* mtlTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a);

    void CopyBufferToBuffer(MTL::Buffer* src, uint32 srcOffset, MTL::Buffer* dst, uint32 dstOffset, uint32 size, MTL::RenderStages after, MTL::RenderStages before);

    // Getters
    bool GetPositionInvariance() const
    {
        return m_positionInvariance;
    }

    bool IsAppleGPU() const
    {
        return m_isAppleGPU;
    }

    bool SupportsFramebufferFetch() const
    {
        return m_supportsFramebufferFetch;
    }

    bool HasUnifiedMemory() const
    {
        return m_hasUnifiedMemory;
    }

    bool SupportsMetal3() const
    {
        return m_supportsMetal3;
    }

    bool SupportsMeshShaders() const
    {
        return m_supportsMeshShaders;
    }

    //MTL::StorageMode GetOptimalTextureStorageMode() const
    //{
    //    return (m_isAppleGPU ? MTL::StorageModeShared : MTL::StorageModePrivate);
    //}

    MTL::ResourceOptions GetOptimalBufferStorageMode() const
    {
        return (m_hasUnifiedMemory ? MTL::ResourceStorageModeShared : MTL::ResourceStorageModeManaged);
    }

    MTL::Texture* GetNullTexture2D() const
    {
        return m_nullTexture2D;
    }

    MTL::Buffer* GetTextureReadbackBuffer()
    {
        if (!m_readbackBuffer)
        {
            m_readbackBuffer = m_device->newBuffer(TEXTURE_READBACK_SIZE, MTL::ResourceStorageModeShared);
#ifdef CEMU_DEBUG_ASSERT
            m_readbackBuffer->setLabel(GetLabel("Texture readback buffer", m_readbackBuffer));
#endif
        }

        return m_readbackBuffer;
    }

    MTL::Buffer* GetXfbRingBuffer()
    {
        if (!m_xfbRingBuffer)
        {
            // HACK: using just LatteStreamout_GetRingBufferSize will cause page faults
            m_xfbRingBuffer = m_device->newBuffer(LatteStreamout_GetRingBufferSize() * 4, MTL::ResourceStorageModePrivate);
#ifdef CEMU_DEBUG_ASSERT
            m_xfbRingBuffer->setLabel(GetLabel("Transform feedback buffer", m_xfbRingBuffer));
#endif
        }

        return m_xfbRingBuffer;
    }

    MTL::Buffer* GetOcclusionQueryResultBuffer() const
    {
        return m_occlusionQuery.m_resultBuffer;
    }

    uint64* GetOcclusionQueryResultsPtr()
    {
        return m_occlusionQuery.m_resultsPtr;
    }

    uint32 GetOcclusionQueryIndex()
    {
        return m_occlusionQuery.m_currentIndex;
    }

    void BeginOcclusionQuery()
    {
        m_occlusionQuery.m_active = true;
    }

    void EndOcclusionQuery()
    {
        m_occlusionQuery.m_active = false;

        // An id, so there is nothing to retain and nothing to release. The old code kept the
        // command buffer alive purely to be able to ask whether it had finished.
        m_occlusionQuery.m_lastCommandBufferId = GetCurrentCommandBufferId();
    }

    // GPU capture
    void CaptureFrame()
    {
        m_captureFrame = true;
    }

private:
	MetalLayerHandle m_mainLayer;
	MetalLayerHandle m_padLayer;

	MetalPerformanceMonitor m_performanceMonitor;

	// Options
	bool m_positionInvariance;

	// Metal objects
	MTL::Device* m_device = nullptr;
	MTL::CommandQueue* m_commandQueue;

	// Feature support
	bool m_isAppleGPU;
	bool m_supportsFramebufferFetch;
	bool m_hasUnifiedMemory;
	bool m_supportsMetal3;
	bool m_supportsMeshShaders;
	uint32 m_recommendedMaxVRAMUsage;
	MetalPixelFormatSupport m_pixelFormatSupport;

	// Managers and caches
	class MetalMemoryManager* m_memoryManager;
	class MetalOutputShaderCache* m_outputShaderCache;
	class MetalPipelineCache* m_pipelineCache;
	class MetalDepthStencilCache* m_depthStencilCache;
	class MetalSamplerCache* m_samplerCache;

	// Pipelines
	MTL::RenderPipelineDescriptor* m_copyDepthToColorDesc;
	std::map<MTL::PixelFormat, MTL::RenderPipelineState*> m_copyDepthToColorPipelines;

	// Void vertex pipelines
	class MetalVoidVertexPipeline* m_copyBufferToBufferPipeline;

	// Synchronization resources
	MTL::Event* m_event;
	// Monotonic, per MTLEvent's contract. Starts at 0 = "nothing signalled yet", so the first
	// command buffer skips the wait.
	uint64 m_eventValue = 0;

	// Resources
	MTL::SamplerState* m_nearestSampler;
	MTL::SamplerState* m_linearSampler;

	// Null resources
	MTL::Texture* m_nullTexture1D;
	MTL::Texture* m_nullTexture2D;

	// Texture readback
	MTL::Buffer* m_readbackBuffer = nullptr;
	uint32 m_readbackBufferWriteOffset = 0;

	// Transform feedback
	MTL::Buffer* m_xfbRingBuffer = nullptr;

	// Occlusion queries
	struct
	{
    	MTL::Buffer* m_resultBuffer;
    	uint64* m_resultsPtr;
    	uint32 m_currentIndex = 0;
        bool m_active = false;
        uint64 m_lastCommandBufferId = UINT64_MAX; // UINT64_MAX = no query has ended yet
	} m_occlusionQuery;

	// Active objects
	MetalCommandBuffer m_currentCommandBuffer{};
	// In-flight command buffers, oldest first. Carries the host time at commit() so the wait
	// between "the CPU handed this over" and "the GPU started it" can be measured -- that
	// figure is the hard ceiling on anything a change to command-buffer ordering could
	// recover, and it is worth knowing before paying for such a change. The frame-end tag
	// separates the benign inter-frame GPU idle from a genuine mid-frame bubble.
	struct InFlightCommandBuffer
	{
		MTL::CommandBuffer* m_commandBuffer;
		uint64 m_id;
		uint64 m_commitHostNs;
		bool m_isFrameEnd;
	};
	std::vector<InFlightCommandBuffer> m_executingCommandBuffers;
	// Set by SwapBuffer, consumed by the next CommitCommandBuffer: the buffer carrying the
	// present is the one that ends a frame.
	bool m_nextCommitEndsFrame = false;
	// The FBO of the PREVIOUS render pass, surviving EndEncoding -- m_lastUsedFBO is reset as part
	// of the encoder state, so it cannot answer "did the pass before this one target the same FBO".
	class LatteCachedFBO* m_previousRenderPassFbo = nullptr;
	// Who tore down the last RENDER pass. Captured at teardown because that is the only moment the
	// caller is on the stack; symbolicated later, and only when the next pass turns out to target
	// the same FBO (i.e. the teardown was waste).
	void* m_lastRenderTeardownStack[10] = {};
	int m_lastRenderTeardownDepth = 0;
	// Host time at the first GetCommandBuffer() of the current frame, for the critical path.
	uint64 m_frameFirstCommandBufferNs = 0;
	// Did the buffer retired just before this one carry the frame's present?
	bool m_lastRetiredEndedFrame = false;

	// A software timeline, replacing MTL::CommandBuffer* as the identity of a submission.
	// Ids in [0, m_retiredCount) have completed. The pointer was never a safe key: it is
	// released in ProcessFinishedCommandBuffers while m_currentCommandBuffer still holds it,
	// so GetCurrentCommandBuffer() can hand out a dangling pointer, and Metal is free to
	// return the same address for a later buffer. An id has neither problem, and it lets a
	// resource say "safe once N has retired" without holding anything alive.
	uint64 m_submittedCount = 0;   // id the buffer currently being recorded will take
	uint64 m_retiredCount = 0;     // everything below this has completed
	MetalEncoderType m_encoderType = MetalEncoderType::None;
	MTL::CommandEncoder* m_commandEncoder = nullptr;

    // GPUEndTime of the most recently completed command buffer, for the inter-buffer gap
    // measurement in ProcessFinishedCommandBuffers. Mach absolute seconds, not a tick count.
    double m_lastGpuEndTime = 0.0;
    uint32 m_recordedDrawcalls = 0;
    uint32 m_defaultCommitTreshlod = 0;
    uint32 m_commitTreshold = 0;

	// State
	MetalState m_state;

	// GPU capture
	bool m_captureFrame = false;
	bool m_capturing = false;

	// Helpers
	MetalLayerHandle& GetLayer(bool mainWindow)
	{
	    return (mainWindow ? m_mainLayer : m_padLayer);
	}

	void SwapBuffer(bool mainWindow);

	void EnsureImGuiBackend();

	// GPU capture
	void StartCapture();
	void EndCapture();
};
