/*
 * Render-pass self-dependency reproducer.
 *
 * Draws into a colour buffer while simultaneously sampling the very same surface
 * as a texture. That is the case `docs/porting/03-graphics-metal.md` item 3.1 is
 * about, and the case the acc.self_dep_* counters were added to size.
 *
 * Two variants, because they are NOT the same bug:
 *
 *   Case A -- same texel. The shader samples at exactly its own fragment
 *             coordinate. Metal expresses this natively with programmable
 *             blending, and the decompiler's framebuffer-fetch substitution is
 *             VALID here.
 *
 *   Case B -- offset texel. The shader samples the render target somewhere else
 *             (a blur, a refraction, any screen-space distortion). Framebuffer
 *             fetch CANNOT express this, and the emitter's substitution discards
 *             the texture coordinate -- so it silently returns the current
 *             fragment instead of the neighbour.
 *
 * What this ROM is for: making the counters fire, so `rm-self-dep-measure` gets a
 * number and the covered/uncovered split becomes visible. It deliberately does NOT
 * verify pixel values -- that needs GPU->CPU readback and an exact integer format,
 * and is tracked separately. Read the counters, not the screen:
 *
 *   --telemetry out.jsonl --telemetry-areas accuracy,gpu
 *     acc.self_dep_fbfetch        -> covered by framebuffer fetch
 *     acc.render_self_dependency  -> NOT covered; reads undefined data
 *     acc.self_dep_nonpixel       -> vertex/geometry stage, never coverable
 */

#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <coreinit/dynload.h>
#include <coreinit/memdefaultheap.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <whb/gfx.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/proc.h>

#define RT_SIZE 256
/* Offset used by Case B, in texels. Large enough that "sampled self" and "sampled
 * neighbour" cannot be confused with filtering slop. */
#define CASE_B_OFFSET 16

static const char *s_vs = R"(
#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 0) out vec2 TexCoord;
void main()
{
   TexCoord = aTexCoord;
   gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
}
)";

/* Case A: samples its own fragment. Framebuffer fetch is a valid substitution. */
static const char *s_ps_same = R"(
#version 450
#extension GL_ARB_shading_language_420pack: enable
layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;
layout(binding = 0) uniform sampler2D selfTexture;
void main()
{
   FragColor = texture(selfTexture, TexCoord) * 0.5 + vec4(0.1, 0.0, 0.0, 1.0);
}
)";

/* Case B: samples a DIFFERENT texel of the same surface. Framebuffer fetch cannot
 * express this; substituting it silently returns the current fragment. */
static const char *s_ps_offset = R"(
#version 450
#extension GL_ARB_shading_language_420pack: enable
layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;
layout(binding = 0) uniform sampler2D selfTexture;
void main()
{
   vec2 uv = TexCoord + vec2(16.0 / 256.0, 0.0);
   FragColor = texture(selfTexture, uv) * 0.5 + vec4(0.0, 0.1, 0.0, 1.0);
}
)";

/* --- CafeGLSL, loaded from cafeLibs/glslcompiler.rpl ---------------------- */
static OSDynLoad_Module s_glsl;
static GX2VertexShader *(*CompileVS)(const char *, char *, int, int);
static GX2PixelShader *(*CompilePS)(const char *, char *, int, int);

static BOOL glsl_init(void)
{
   void (*init)(void) = NULL;
   if (OSDynLoad_Acquire("glslcompiler", &s_glsl) != OS_DYNLOAD_OK) {
      WHBLogPrintf("TESSERA-GFXTEST RESULT=ERROR reason=glslcompiler.rpl-not-found");
      WHBLogPrintf("  place it in <userdata>/cafeLibs/ -- see docs/testing/00-test-strategy.md");
      return FALSE;
   }
   OSDynLoad_FindExport(s_glsl, OS_DYNLOAD_EXPORT_FUNC, "InitGLSLCompiler", (void **)&init);
   OSDynLoad_FindExport(s_glsl, OS_DYNLOAD_EXPORT_FUNC, "CompileVertexShader", (void **)&CompileVS);
   OSDynLoad_FindExport(s_glsl, OS_DYNLOAD_EXPORT_FUNC, "CompilePixelShader", (void **)&CompilePS);
   if (!init || !CompileVS || !CompilePS) {
      WHBLogPrintf("TESSERA-GFXTEST RESULT=ERROR reason=glslcompiler-exports-missing");
      return FALSE;
   }
   init();
   return TRUE;
}

/* --- the render target that is also sampled -------------------------------- */
static GX2Texture s_rt;
static GX2ColorBuffer s_cb;

static BOOL rt_init(void)
{
   memset(&s_rt, 0, sizeof(s_rt));
   s_rt.surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
   s_rt.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
   s_rt.surface.width = RT_SIZE;
   s_rt.surface.height = RT_SIZE;
   s_rt.surface.depth = 1;
   s_rt.surface.mipLevels = 1;
   s_rt.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
   s_rt.surface.aa = GX2_AA_MODE1X;
   s_rt.surface.tileMode = GX2_TILE_MODE_DEFAULT;
   s_rt.viewNumMips = 1;
   s_rt.viewNumSlices = 1;
   s_rt.compMap = 0x00010203;
   GX2CalcSurfaceSizeAndAlignment(&s_rt.surface);
   GX2InitTextureRegs(&s_rt);

   s_rt.surface.image = memalign(s_rt.surface.alignment, s_rt.surface.imageSize);
   if (!s_rt.surface.image) {
      WHBLogPrintf("TESSERA-GFXTEST RESULT=ERROR reason=rt-alloc-failed");
      return FALSE;
   }
   memset(s_rt.surface.image, 0, s_rt.surface.imageSize);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, s_rt.surface.image, s_rt.surface.imageSize);

   /* The colour buffer aliases the *same* surface as the texture above. That
    * aliasing is the entire point of this test. */
   memset(&s_cb, 0, sizeof(s_cb));
   s_cb.surface = s_rt.surface;
   s_cb.viewNumSlices = 1;
   GX2InitColorBufferRegs(&s_cb);
   return TRUE;
}

int main(int argc, char **argv)
{
   WHBProcInit();
   WHBLogCafeInit();
   WHBGfxInit();

   WHBLogPrintf("TESSERA-GFXTEST begin");
   WHBLogPrintf("test=self-dependency rt=%dx%d offset=%d", RT_SIZE, RT_SIZE, CASE_B_OFFSET);

   if (!glsl_init() || !rt_init()) {
      WHBLogPrintf("TESSERA-GFXTEST end");
      WHBLogCafeDeinit();
      WHBProcShutdown();
      return 1;
   }

   char log[1024];
   GX2VertexShader *vs = CompileVS(s_vs, log, sizeof(log), 0);
   GX2PixelShader *psSame = CompilePS(s_ps_same, log, sizeof(log), 0);
   GX2PixelShader *psOffset = CompilePS(s_ps_offset, log, sizeof(log), 0);
   if (!vs || !psSame || !psOffset) {
      WHBLogPrintf("TESSERA-GFXTEST RESULT=ERROR reason=shader-compile-failed log=%s", log);
      WHBLogPrintf("TESSERA-GFXTEST end");
      WHBLogCafeDeinit();
      WHBProcShutdown();
      return 1;
   }
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, vs->program, vs->size);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, psSame->program, psSame->size);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, psOffset->program, psOffset->size);

   WHBGfxShaderGroup group;
   memset(&group, 0, sizeof(group));
   group.vertexShader = vs;
   group.pixelShader = psSame;
   WHBGfxInitShaderAttribute(&group, "aPos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
   WHBGfxInitShaderAttribute(&group, "aTexCoord", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
   WHBGfxInitFetchShader(&group);

   static const float pos[] = { -1, -1,  1, -1,  1, 1,  -1, 1 };
   static const float uv[]  = {  0,  0,  1,  0,  1, 1,   0, 1 };

   GX2RBuffer posBuf{}, uvBuf{};
   const GX2RResourceFlags kVtxFlags = (GX2RResourceFlags)(
      GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_WRITE |
      GX2R_RESOURCE_USAGE_GPU_READ);
   posBuf.flags = kVtxFlags;
   posBuf.elemSize = 2 * sizeof(float); posBuf.elemCount = 4;
   GX2RCreateBuffer(&posBuf);
   memcpy(GX2RLockBufferEx(&posBuf, (GX2RResourceFlags)0), pos, sizeof(pos));
   GX2RUnlockBufferEx(&posBuf, (GX2RResourceFlags)0);

   uvBuf.flags = kVtxFlags;
   uvBuf.elemSize = 2 * sizeof(float); uvBuf.elemCount = 4;
   GX2RCreateBuffer(&uvBuf);
   memcpy(GX2RLockBufferEx(&uvBuf, (GX2RResourceFlags)0), uv, sizeof(uv));
   GX2RUnlockBufferEx(&uvBuf, (GX2RResourceFlags)0);

   GX2Sampler sampler;
   GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);

   uint32_t frame = 0;
   WHBLogPrintf("TESSERA-GFXTEST shader_info psSame.samplerVars=%u psOffset.samplerVars=%u",
                (unsigned)psSame->samplerVarCount, (unsigned)psOffset->samplerVarCount);
   if (psSame->samplerVarCount > 0)
      WHBLogPrintf("TESSERA-GFXTEST sampler0 location=%u name=%s",
                   (unsigned)psSame->samplerVars[0].location,
                   psSame->samplerVars[0].name ? psSame->samplerVars[0].name : "(null)");
   WHBLogPrintf("TESSERA-GFXTEST RESULT=RUNNING (read acc.self_dep_* from telemetry)");

   while (WHBProcIsRunning()) {
      /* Alternate the two cases so a single run exercises both. Which one is live
       * is derivable from the frame index if a counter ever needs attributing. */
      BOOL caseB = (frame & 1) != 0;
      group.pixelShader = caseB ? psOffset : psSame;

      WHBGfxBeginRender();

      /* Render INTO s_cb while s_rt -- the same surface -- is bound as a texture. */
      GX2SetColorBuffer(&s_cb, GX2_RENDER_TARGET_0);
      GX2SetViewport(0.0f, 0.0f, (float)RT_SIZE, (float)RT_SIZE, 0.0f, 1.0f);
      GX2SetScissor(0, 0, RT_SIZE, RT_SIZE);
      GX2SetFetchShader(&group.fetchShader);
      GX2SetVertexShader(group.vertexShader);
      GX2SetPixelShader(group.pixelShader);
      GX2RSetAttributeBuffer(&posBuf, 0, posBuf.elemSize, 0);
      GX2RSetAttributeBuffer(&uvBuf, 1, uvBuf.elemSize, 0);
      if (group.pixelShader->samplerVarCount > 0) {
         uint32_t loc = group.pixelShader->samplerVars[0].location;
         GX2SetPixelTexture(&s_rt, loc);
         GX2SetPixelSampler(&sampler, loc);
      }
      GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);

      /* Present something so the title behaves like a normal app. */
      WHBGfxBeginRenderTV();
      WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      WHBGfxFinishRenderTV();
      WHBGfxBeginRenderDRC();
      WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      WHBGfxFinishRenderDRC();
      WHBGfxFinishRender();

      if (++frame == 600) {
         WHBLogPrintf("TESSERA-GFXTEST frames=%u caseA=%u caseB=%u",
                      (unsigned)frame, (unsigned)(frame + 1) / 2, (unsigned)frame / 2);
      }
   }

   WHBLogPrintf("TESSERA-GFXTEST end frames=%u", (unsigned)frame);
   free(s_rt.surface.image);
   GX2RDestroyBufferEx(&posBuf, (GX2RResourceFlags)0);
   GX2RDestroyBufferEx(&uvBuf, (GX2RResourceFlags)0);
   WHBGfxShutdown();
   WHBLogCafeDeinit();
   WHBProcShutdown();
   return 0;
}
