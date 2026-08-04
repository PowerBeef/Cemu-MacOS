#pragma once

#include <unordered_map>

#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"

// The six RendererOutputShader statics (copy, bicubic and hermite, each with an upside-down
// variant), doubled to cover the sRGB and non-sRGB backbuffer formats.
constexpr uint8 METAL_SHADER_TYPE_COUNT = 6;
constexpr uint8 METAL_OUTPUT_SHADER_CACHE_SIZE = 2 * METAL_SHADER_TYPE_COUNT;

class MetalOutputShaderCache
{
public:
    MetalOutputShaderCache(class MetalRenderer* metalRenderer) : m_mtlr{metalRenderer} {}
    ~MetalOutputShaderCache();

    MTL::RenderPipelineState* GetPipeline(RendererOutputShader* shader, uint8 shaderIndex, bool usesSRGB);

private:
    MTL::RenderPipelineState* CreatePipeline(RendererOutputShader* shader, bool usesSRGB);

    class MetalRenderer* m_mtlr;

    // Indexed by (usesSRGB * METAL_SHADER_TYPE_COUNT) + shaderIndex, and only ever with a
    // shaderIndex the caller resolved to one of the six statics.
    MTL::RenderPipelineState* m_cache[METAL_OUTPUT_SHADER_CACHE_SIZE] = {nullptr};

    // Graphic packs supply their own output, upscaling and downscaling shaders, and those are
    // none of the six. They have no index to key on, so they are keyed by identity. Indexed
    // by usesSRGB.
    std::unordered_map<RendererOutputShader*, MTL::RenderPipelineState*> m_customCache[2];
};
