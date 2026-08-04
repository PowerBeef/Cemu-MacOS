#include "Cafe/HW/Latte/Renderer/Metal/MetalOutputShaderCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.h"
#include "Cemu/Telemetry/Telemetry.h"

MetalOutputShaderCache::~MetalOutputShaderCache()
{
    for (uint8 i = 0; i < METAL_OUTPUT_SHADER_CACHE_SIZE; i++)
    {
        if (m_cache[i])
            m_cache[i]->release();
    }

    for (auto& cache : m_customCache)
    {
        for (auto& [shader, pipeline] : cache)
        {
            if (pipeline)
                pipeline->release();
        }
    }
}

MTL::RenderPipelineState* MetalOutputShaderCache::CreatePipeline(RendererOutputShader* shader, bool usesSRGB)
{
    auto vertexShaderMtl = static_cast<RendererShaderMtl*>(shader->GetVertexShader())->GetFunction();
    auto fragmentShaderMtl = static_cast<RendererShaderMtl*>(shader->GetFragmentShader())->GetFunction();

    NS_STACK_SCOPED auto renderPipelineDescriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    renderPipelineDescriptor->setVertexFunction(vertexShaderMtl);
    renderPipelineDescriptor->setFragmentFunction(fragmentShaderMtl);
    renderPipelineDescriptor->colorAttachments()->object(0)->setPixelFormat(usesSRGB ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm);

    NS::Error* error = nullptr;
    MTL::RenderPipelineState* pipeline = m_mtlr->GetDevice()->newRenderPipelineState(renderPipelineDescriptor, &error);
    if (error)
    {
        cemuLog_log(LogType::Force, "error creating output render pipeline state: {}", error->localizedDescription()->utf8String());
    }

    return pipeline;
}

MTL::RenderPipelineState* MetalOutputShaderCache::GetPipeline(RendererOutputShader* shader, uint8 shaderIndex, bool usesSRGB)
{
    // A graphic pack's own output/upscaling/downscaling shader is none of the six statics
    // DrawBackbufferQuad tests against, so it leaves shaderIndex at its 255 sentinel
    // (LatteRenderTarget.cpp takes the pack's shader before the built-in selection runs).
    //
    // Indexing m_cache with 255 was an out-of-bounds read *and* a write: the reference bound
    // here is assigned the new pipeline below. With sRGB the uint8 sum wrapped to 5 instead,
    // which silently aliased the s_hermit_shader_ud slot in both directions -- the pack's
    // pipeline served later hermite draws, and vice versa.
    //
    // A pack shader has no index to key on, so key it by identity.
    if (shaderIndex >= METAL_SHADER_TYPE_COUNT)
    {
        TLM_INC(Accuracy, AccOutputShaderCustom);

        auto& cache = m_customCache[usesSRGB ? 1 : 0];
        auto it = cache.find(shader);
        if (it != cache.end())
            return it->second;

        MTL::RenderPipelineState* pipeline = CreatePipeline(shader, usesSRGB);
        cache.emplace(shader, pipeline);

        return pipeline;
    }

    auto& renderPipelineState = m_cache[(usesSRGB ? METAL_SHADER_TYPE_COUNT : 0) + shaderIndex];
    if (renderPipelineState)
        return renderPipelineState;

    renderPipelineState = CreatePipeline(shader, usesSRGB);

    return renderPipelineState;
}
