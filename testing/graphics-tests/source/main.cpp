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
 * This ROM does two separate things, and the difference matters:
 *
 *   1. A PIXEL ORACLE, run once at startup. Integer format, GPU->CPU readback,
 *      bit-exact compare. It answers "is the output wrong", and the answer is yes:
 *
 *        TEST selfdep_seed                PASS
 *        TEST selfdep_caseA_same_texel    PASS   <- substitution valid here
 *        TEST selfdep_caseB_offset_texel  FAIL   <- 16/16 texels returned self
 *
 *      Case A passing is the control. If it ever fails, suspect this harness
 *      before suspecting the emulator.
 *
 *   2. A COUNTER LOOP, running forever after. It answers "how often does the
 *      substitution get applied", which the oracle cannot, because the oracle is
 *      three draws:
 *
 *        --telemetry out.jsonl --telemetry-areas accuracy,gpu
 *          acc.self_dep_fbfetch        -> covered by framebuffer fetch
 *          acc.render_self_dependency  -> NOT covered; reads undefined data
 *          acc.self_dep_nonpixel       -> vertex/geometry stage, never coverable
 *
 * An earlier revision of this comment said the ROM "deliberately does NOT verify
 * pixel values". That was true until the oracle below landed.
 */

#include <stdio.h>
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

/* ==========================================================================
 * The pixel oracle.
 *
 * The counter loop below proves the framebuffer-fetch substitution is APPLIED
 * to the offset case. It cannot prove the resulting pixel is wrong, because it
 * never reads one back. This does.
 *
 * Integer format, so the comparison is exact and there is no filtering slop to
 * argue about -- the trick borrowed from piglit's blending-in-shader test.
 *
 * Seed the surface so every texel holds its own X coordinate. Then draw a strip
 * that samples 16 texels to the right and writes what it read:
 *
 *   correct        output[x] == x + 16   (it really sampled the neighbour)
 *   substitution   output[x] == x        (coordinate discarded, read of self)
 *
 * The strip is [0,16) and it samples from [16,32), which the strip does not
 * write. So there is no feedback race to explain a result away with: the source
 * texels are untouched by this draw whichever path the emulator takes.
 * ========================================================================== */
#define ORACLE_STRIP_W  CASE_B_OFFSET   /* [0,16) reads [16,32) */
#define ORACLE_A_X0     32              /* Case A control strip: [32,48) */
#define ORACLE_PROBE_Y  128             /* any row; the pattern is column-only */
#define ORACLE_A_BIAS   1000u
#define ORACLE_C_X0     64              /* Case C strip: [64,80), vertex-stage read */
#define ORACLE_C_SRC_X  200             /* the texel the vertex shader reads */
#define ORACLE_D_X0     96              /* Case D strip: [96,112), different-format alias */
#define ORACLE_D_MARKER 4242u           /* proves the Case D draw executed at all */

/* Writes each texel's own X. uvec4 out, because the target is UINT_R32. */
static const char *s_ps_seed = R"(
#version 450
layout(location = 0) out uvec4 FragColor;
void main()
{
   FragColor = uvec4(uint(gl_FragCoord.x), 0u, 0u, 0u);
}
)";

/* Case B, exactly: read a DIFFERENT texel of the surface being rendered to. */
static const char *s_ps_oracle_offset = R"(
#version 450
#extension GL_ARB_shading_language_420pack: enable
layout(location = 0) out uvec4 FragColor;
layout(binding = 0) uniform usampler2D selfTexture;
void main()
{
   ivec2 c = ivec2(int(gl_FragCoord.x) + 16, int(gl_FragCoord.y));
   FragColor = uvec4(texelFetch(selfTexture, c, 0).r, 0u, 0u, 0u);
}
)";

/* Case C: the VERTEX stage samples the render target.
 *
 * This is the case the framebuffer-fetch rewrite can never cover, and it is
 * uncovered by construction rather than by luck: LatteDecompilerAnalyzer only
 * fills textureRenderTargetIndex for `shaderType == Pixel`. So this is what
 * exercises the pass splitter, and the only case in this ROM that does -- every
 * pixel-stage alias here is swallowed by the rewrite before the renderer sees it.
 *
 * Reads one fixed texel the seed pass wrote, hands it to the fragment stage, and
 * the fragment stage writes it out, so a pixel oracle can check a vertex-stage
 * read. Without a pass split the vertex fetch races the seed's writes; with one
 * it must see the finished surface. */
static const char *s_vs_selfread = R"(
#version 450
#extension GL_ARB_shading_language_420pack: enable
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 0) flat out float SampledValue;
layout(binding = 0) uniform usampler2D selfTexture;
void main()
{
   SampledValue = float(texelFetch(selfTexture, ivec2(200, 128), 0).r);
   gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
}
)";

static const char *s_ps_passthrough = R"(
#version 450
layout(location = 0) flat in float SampledValue;
layout(location = 0) out uvec4 FragColor;
void main()
{
   FragColor = uvec4(uint(SampledValue + 0.5), 0u, 0u, 0u);
}
)";

/* Case D: sample a DIFFERENT-FORMAT view of the very memory being rendered into.
 *
 * The render target is UINT_R32; this samples the same physAddr declared as
 * UNORM_R8_G8_B8_A8. Same 32 bits per texel, different format enum, and that
 * is enough to make LatteTexture_CanTextureBeRepresentedAsView return
 * VIEW_NOT_COMPATIBLE at LatteTexture.cpp:858 -- so Cemu builds a SECOND
 * LatteTexture over the same guest memory.
 *
 * Both halves of the self-dependency machinery are then blind to it:
 *
 *   the analyzer compares physAddr AND format when setting
 *   textureRenderTargetIndex, so the unit is left uncovered;
 *
 *   AliasesActiveAttachment compares baseTexture POINTERS, and this is a
 *   different object, so the renderer does not see the alias either.
 *
 * The write is a fixed marker rather than the sampled value: what is being
 * tested is whether the emulator NOTICES the alias, which only the
 * acc.render_self_dependency counter can answer. The marker exists so a
 * missing counter cannot be confused with a draw that never ran. */
static const char *s_ps_alias = R"(
#version 450
#extension GL_ARB_shading_language_420pack: enable
layout(location = 0) out uvec4 FragColor;
layout(binding = 0) uniform sampler2D aliasTexture;
void main()
{
   vec4 v = texture(aliasTexture, vec2(0.5, 0.5));
   FragColor = uvec4(4242u + uint(v.r * 255.0 + 0.5), 0u, 0u, 0u);
}
)";

/* Case A control: read this fragment's OWN texel. A real sample and a
 * framebuffer fetch must agree here -- that is what makes the substitution
 * valid for this case. If this one fails, the harness is broken, not Latte. */
static const char *s_ps_oracle_same = R"(
#version 450
#extension GL_ARB_shading_language_420pack: enable
layout(location = 0) out uvec4 FragColor;
layout(binding = 0) uniform usampler2D selfTexture;
void main()
{
   ivec2 c = ivec2(int(gl_FragCoord.x), int(gl_FragCoord.y));
   FragColor = uvec4(texelFetch(selfTexture, c, 0).r + 1000u, 0u, 0u, 0u);
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

/* --- oracle surfaces: an integer RT, plus a linear copy the CPU can read ---- */
static GX2Texture s_ort;        /* UINT_R32, both sampled and rendered to */
static GX2ColorBuffer s_ocb;    /* aliases s_ort.surface -- the self-dependency */
static GX2Surface s_olin;       /* LINEAR_SPECIAL destination for GX2CopySurface */
static GX2Texture s_oalias;     /* same memory as s_ort, declared UNORM_R8_G8_B8_A8 */

static int s_pass, s_fail;

static void skip_test(const char *name, const char *why)
{
   WHBLogPrintf("TEST %s SKIP reason=%s", name, why);
}

static void verdict(const char *name, BOOL ok, const char *expected, const char *got)
{
   if (ok) { s_pass++; WHBLogPrintf("TEST %s PASS", name); }
   else    { s_fail++; WHBLogPrintf("TEST %s FAIL expected=%s got=%s", name, expected, got); }
}

static BOOL oracle_surfaces_init(void)
{
   memset(&s_ort, 0, sizeof(s_ort));
   s_ort.surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
   s_ort.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
   s_ort.surface.width = RT_SIZE;
   s_ort.surface.height = RT_SIZE;
   s_ort.surface.depth = 1;
   s_ort.surface.mipLevels = 1;
   s_ort.surface.format = GX2_SURFACE_FORMAT_UINT_R32;
   s_ort.surface.aa = GX2_AA_MODE1X;
   s_ort.surface.tileMode = GX2_TILE_MODE_DEFAULT;
   s_ort.viewNumMips = 1;
   s_ort.viewNumSlices = 1;
   s_ort.compMap = 0x00010203;
   GX2CalcSurfaceSizeAndAlignment(&s_ort.surface);
   GX2InitTextureRegs(&s_ort);
   s_ort.surface.image = memalign(s_ort.surface.alignment, s_ort.surface.imageSize);
   if (!s_ort.surface.image) return FALSE;
   memset(s_ort.surface.image, 0, s_ort.surface.imageSize);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, s_ort.surface.image, s_ort.surface.imageSize);

   memset(&s_ocb, 0, sizeof(s_ocb));
   s_ocb.surface = s_ort.surface;
   s_ocb.viewNumSlices = 1;
   GX2InitColorBufferRegs(&s_ocb);

   /* LINEAR_SPECIAL so GX2CopySurface untiles for us. Doing the untiling on the
    * CPU instead would mean reimplementing addrlib to read a test result, and a
    * bug in that would look exactly like a bug in the thing under test. */
   /* Same image pointer, same geometry, DIFFERENT format. That single difference is what
    * makes Cemu build a second LatteTexture instead of a view (LatteTexture.cpp:858). */
   memset(&s_oalias, 0, sizeof(s_oalias));
   s_oalias.surface = s_ort.surface;
   s_oalias.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
   s_oalias.surface.use = GX2_SURFACE_USE_TEXTURE;
   s_oalias.viewNumMips = 1;
   s_oalias.viewNumSlices = 1;
   s_oalias.compMap = 0x00010203;
   GX2InitTextureRegs(&s_oalias);

   memset(&s_olin, 0, sizeof(s_olin));
   s_olin.use = GX2_SURFACE_USE_TEXTURE;
   s_olin.dim = GX2_SURFACE_DIM_TEXTURE_2D;
   s_olin.width = RT_SIZE;
   s_olin.height = RT_SIZE;
   s_olin.depth = 1;
   s_olin.mipLevels = 1;
   s_olin.format = GX2_SURFACE_FORMAT_UINT_R32;
   s_olin.aa = GX2_AA_MODE1X;
   s_olin.tileMode = GX2_TILE_MODE_LINEAR_SPECIAL;
   GX2CalcSurfaceSizeAndAlignment(&s_olin);
   s_olin.image = memalign(s_olin.alignment, s_olin.imageSize);
   if (!s_olin.image) return FALSE;
   memset(s_olin.image, 0, s_olin.imageSize);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, s_olin.image, s_olin.imageSize);
   return TRUE;
}

/* The GPU writes UINT_R32 in the opposite byte order to how the PPC core reads a
 * uint32_t, so a seeded value of 1 comes back as 0x01000000. Byte-swap on read
 * rather than in the shader: the shader is the thing under test and should stay
 * the simplest possible expression of "write x", with no endian arithmetic in it
 * that could itself be wrong. First run of this oracle had every texel "wrong" for
 * exactly this reason, which is what the raw dump above exists to catch. */
static inline uint32_t rd(const uint32_t *px, uint32_t pitch, int x, int y)
{
   return __builtin_bswap32(px[(uint32_t)y * pitch + (uint32_t)x]);
}

/* One pass over the strip, so a failure reports how many texels agreed with each
 * hypothesis rather than the value of a single lucky pixel. */
static void tally(const uint32_t *px, uint32_t pitch, int x0, int w,
                  uint32_t (*expect)(int), uint32_t (*bug)(int),
                  int *nExpect, int *nBug, int *nOther, uint32_t *firstOther)
{
   *nExpect = *nBug = *nOther = 0;
   for (int x = x0; x < x0 + w; x++) {
      uint32_t v = rd(px, pitch, x, ORACLE_PROBE_Y);
      if (v == expect(x))   (*nExpect)++;
      else if (v == bug(x)) (*nBug)++;
      else { if (*nOther == 0) *firstOther = v; (*nOther)++; }
   }
}

static uint32_t exp_offset(int x) { return (uint32_t)(x + CASE_B_OFFSET); }
static uint32_t bug_offset(int x) { return (uint32_t)x; }
static uint32_t exp_same(int x)   { return (uint32_t)x + ORACLE_A_BIAS; }
static uint32_t bug_same(int x)   { return (uint32_t)x; }

static void run_pixel_oracle(WHBGfxShaderGroup *group, WHBGfxShaderGroup *groupC,
                             GX2RBuffer *posBuf, GX2RBuffer *uvBuf,
                             GX2PixelShader *psSeed, GX2PixelShader *psOff, GX2PixelShader *psSame,
                             GX2PixelShader *psAlias, GX2Sampler *sampler)
{
   WHBLogPrintf("TESSERA-GFXTEST oracle begin fmt=UINT_R32 strip=[0,%d) reads=[%d,%d)",
                ORACLE_STRIP_W, CASE_B_OFFSET, CASE_B_OFFSET + ORACLE_STRIP_W);
   /* If this reads 0 the alias fetch was optimised away and Case D tests nothing. */
   WHBLogPrintf("TESSERA-GFXTEST oracle caseD_samplerVars=%u",
                (unsigned)(psAlias ? psAlias->samplerVarCount : 0));

   WHBGfxBeginRender();
   GX2SetColorBuffer(&s_ocb, GX2_RENDER_TARGET_0);
   GX2SetViewport(0.0f, 0.0f, (float)RT_SIZE, (float)RT_SIZE, 0.0f, 1.0f);

   /* Do not inherit whatever WHBGfxBeginRender left set. A render-to-texture with
    * depth testing on, colour writes off or the red channel masked produces exactly
    * what the first run of this oracle produced: a surface of zeros and three
    * failures that look like emulator bugs and are not. */
   GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
   GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
   GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, GX2_CHANNEL_MASK_RGBA, GX2_CHANNEL_MASK_RGBA,
                            GX2_CHANNEL_MASK_RGBA, GX2_CHANNEL_MASK_RGBA, GX2_CHANNEL_MASK_RGBA,
                            GX2_CHANNEL_MASK_RGBA, GX2_CHANNEL_MASK_RGBA);

   GX2SetFetchShader(&group->fetchShader);
   GX2SetVertexShader(group->vertexShader);
   GX2RSetAttributeBuffer(posBuf, 0, posBuf->elemSize, 0);
   GX2RSetAttributeBuffer(uvBuf, 1, uvBuf->elemSize, 0);

   /* 1. Seed: every texel gets its own X. No texture bound, so no self-dependency. */
   GX2SetScissor(0, 0, RT_SIZE, RT_SIZE);
   GX2SetPixelShader(psSeed);
   GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
   /* The test draws must observe the seed. Without this they may sample a surface
    * whose writes are still in flight, and a wrong answer would be unattributable. */
   GX2DrawDone();

   /* 2. Case A control, strip [32,48): read own texel, add a bias. */
   GX2SetScissor(ORACLE_A_X0, 0, ORACLE_STRIP_W, RT_SIZE);
   GX2SetPixelShader(psSame);
   if (psSame->samplerVarCount > 0) {
      uint32_t loc = psSame->samplerVars[0].location;
      GX2SetPixelTexture(&s_ort, loc);
      GX2SetPixelSampler(sampler, loc);
   }
   GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
   GX2DrawDone();

   /* 3. Case B, strip [0,16): read 16 to the right. This is the one under test. */
   GX2SetScissor(0, 0, ORACLE_STRIP_W, RT_SIZE);
   GX2SetPixelShader(psOff);
   if (psOff->samplerVarCount > 0) {
      uint32_t loc = psOff->samplerVars[0].location;
      GX2SetPixelTexture(&s_ort, loc);
      GX2SetPixelSampler(sampler, loc);
   }
   GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
   GX2DrawDone();

   /* 3b. Case C, strip [64,80): the VERTEX stage reads the surface being drawn to.
    * Uncovered by construction -- the analyzer only rewrites pixel-stage samples --
    * so this is the draw the pass splitter has to notice. */
   if (groupC) {
      GX2SetScissor(ORACLE_C_X0, 0, ORACLE_STRIP_W, RT_SIZE);
      GX2SetFetchShader(&groupC->fetchShader);
      GX2SetVertexShader(groupC->vertexShader);
      GX2SetPixelShader(groupC->pixelShader);
      GX2RSetAttributeBuffer(posBuf, 0, posBuf->elemSize, 0);
      GX2RSetAttributeBuffer(uvBuf, 1, uvBuf->elemSize, 0);
      if (groupC->vertexShader->samplerVarCount > 0) {
         uint32_t loc = groupC->vertexShader->samplerVars[0].location;
         GX2SetVertexTexture(&s_ort, loc);
         GX2SetVertexSampler(sampler, loc);
      }
      GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
      GX2DrawDone();
   }

   /* 3c. Case D, strip [96,112): sample a different-format view of this same memory.
    * Neither the analyzer (compares format) nor the renderer (compares pointers) can see
    * this alias. Read acc.render_self_dependency to find out; the marker only proves the
    * draw happened. */
   if (psAlias) {
      GX2SetScissor(ORACLE_D_X0, 0, ORACLE_STRIP_W, RT_SIZE);
      GX2SetFetchShader(&group->fetchShader);
      GX2SetVertexShader(group->vertexShader);
      GX2SetPixelShader(psAlias);
      GX2RSetAttributeBuffer(posBuf, 0, posBuf->elemSize, 0);
      GX2RSetAttributeBuffer(uvBuf, 1, uvBuf->elemSize, 0);
      if (psAlias->samplerVarCount > 0) {
         uint32_t loc = psAlias->samplerVars[0].location;
         GX2SetPixelTexture(&s_oalias, loc);
         GX2SetPixelSampler(sampler, loc);
      }
      GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
      GX2DrawDone();
   }

   /* 4. Untile into a surface the CPU can index. */
   GX2CopySurface(&s_ort.surface, 0, 0, &s_olin, 0, 0);
   GX2DrawDone();
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, s_olin.image, s_olin.imageSize);

   WHBGfxBeginRenderTV();  WHBGfxClearColor(0,0,0,1); WHBGfxFinishRenderTV();
   WHBGfxBeginRenderDRC(); WHBGfxClearColor(0,0,0,1); WHBGfxFinishRenderDRC();
   WHBGfxFinishRender();

   const uint32_t *px = (const uint32_t *)s_olin.image;
   const uint32_t pitch = s_olin.pitch;

   /* Raw state before any verdict. A surface of zeros is the failure mode with the
    * most possible causes -- seed did not draw, copy did not run, pitch is wrong,
    * cache not invalidated -- and these three lines separate them. */
   {
      uint32_t nzTiled = 0, nzLin = 0;
      const uint32_t *t = (const uint32_t *)s_ort.surface.image;
      for (uint32_t i = 0; i < s_ort.surface.imageSize / 4; i++) if (t[i]) nzTiled++;
      for (uint32_t i = 0; i < s_olin.imageSize / 4; i++)         if (px[i]) nzLin++;
      WHBLogPrintf("TESSERA-GFXTEST oracle raw pitch=%u linSize=%u tiledSize=%u nonzero_tiled=%u nonzero_linear=%u",
                   (unsigned)pitch, (unsigned)s_olin.imageSize, (unsigned)s_ort.surface.imageSize,
                   (unsigned)nzTiled, (unsigned)nzLin);
      WHBLogPrintf("TESSERA-GFXTEST oracle row%d[0..7] = %u %u %u %u %u %u %u %u",
                   ORACLE_PROBE_Y,
                   (unsigned)rd(px, pitch, 0, ORACLE_PROBE_Y), (unsigned)rd(px, pitch, 1, ORACLE_PROBE_Y),
                   (unsigned)rd(px, pitch, 2, ORACLE_PROBE_Y), (unsigned)rd(px, pitch, 3, ORACLE_PROBE_Y),
                   (unsigned)rd(px, pitch, 4, ORACLE_PROBE_Y), (unsigned)rd(px, pitch, 5, ORACLE_PROBE_Y),
                   (unsigned)rd(px, pitch, 6, ORACLE_PROBE_Y), (unsigned)rd(px, pitch, 7, ORACLE_PROBE_Y));
      WHBLogPrintf("TESSERA-GFXTEST oracle row%d[96..103] = %u %u %u %u %u %u %u %u",
                   ORACLE_PROBE_Y,
                   (unsigned)rd(px, pitch, 96, ORACLE_PROBE_Y),  (unsigned)rd(px, pitch, 97, ORACLE_PROBE_Y),
                   (unsigned)rd(px, pitch, 98, ORACLE_PROBE_Y),  (unsigned)rd(px, pitch, 99, ORACLE_PROBE_Y),
                   (unsigned)rd(px, pitch, 100, ORACLE_PROBE_Y), (unsigned)rd(px, pitch, 101, ORACLE_PROBE_Y),
                   (unsigned)rd(px, pitch, 102, ORACLE_PROBE_Y), (unsigned)rd(px, pitch, 103, ORACLE_PROBE_Y));
   }
   char expected[64], got[96];
   int nE, nB, nO; uint32_t other = 0;

   /* Control first. A seed that did not land makes every later verdict noise, so
    * this is checked outside both strips and reported as its own test. */
   /* Must sit outside EVERY strip: B=[0,16), A=[32,48), C=[64,80), D=[96,112), and clear of
    * texel 200 which Case C reads. x=180 satisfies all of it. Using 100 put the probe inside
    * Case D's strip, so the seed control failed against Case D's own marker. */
   const int sx = 180;
   uint32_t seen = rd(px, pitch, sx, ORACLE_PROBE_Y);
   snprintf(expected, sizeof(expected), "%d", sx);
   snprintf(got, sizeof(got), "%u", (unsigned)seen);
   verdict("selfdep_seed", seen == (uint32_t)sx, expected, got);

   tally(px, pitch, ORACLE_A_X0, ORACLE_STRIP_W, exp_same, bug_same, &nE, &nB, &nO, &other);
   snprintf(expected, sizeof(expected), "%d/%d texels x+%u", ORACLE_STRIP_W, ORACLE_STRIP_W,
            (unsigned)ORACLE_A_BIAS);
   snprintf(got, sizeof(got), "correct=%d unbiased=%d other=%d first_other=%u",
            nE, nB, nO, (unsigned)other);
   verdict("selfdep_caseA_same_texel", nE == ORACLE_STRIP_W, expected, got);

   tally(px, pitch, 0, ORACLE_STRIP_W, exp_offset, bug_offset, &nE, &nB, &nO, &other);
   snprintf(expected, sizeof(expected), "%d/%d texels x+%d (sampled the neighbour)",
            ORACLE_STRIP_W, ORACLE_STRIP_W, CASE_B_OFFSET);
   snprintf(got, sizeof(got), "neighbour=%d self=%d other=%d first_other=%u",
            nE, nB, nO, (unsigned)other);
   verdict("selfdep_caseB_offset_texel", nE == ORACLE_STRIP_W, expected, got);

   if (groupC) {
      int nC = 0;
      for (int x = ORACLE_C_X0; x < ORACLE_C_X0 + ORACLE_STRIP_W; x++)
         if (rd(px, pitch, x, ORACLE_PROBE_Y) == (uint32_t)ORACLE_C_SRC_X) nC++;
      /* If the strip still holds its seeded value, the draw never executed -- which on this
       * emulator means the pipeline was refused because the vertex shader has no Metal
       * function (acc.pipeline_no_function). That is an unsupported-shader problem, not a
       * self-dependency result, and reporting it as FAIL would blame the wrong subsystem. */
      int nSeed = 0;
      for (int x = ORACLE_C_X0; x < ORACLE_C_X0 + ORACLE_STRIP_W; x++)
         if (rd(px, pitch, x, ORACLE_PROBE_Y) == (uint32_t)x) nSeed++;
      snprintf(expected, sizeof(expected), "%d/%d texels == %d", ORACLE_STRIP_W, ORACLE_STRIP_W,
               ORACLE_C_SRC_X);
      snprintf(got, sizeof(got), "matched=%d first=%u", nC,
               (unsigned)rd(px, pitch, ORACLE_C_X0, ORACLE_PROBE_Y));
      if (nSeed == ORACLE_STRIP_W)
         skip_test("selfdep_caseC_vertex_stage",
                   "draw-did-not-execute-check-acc.pipeline_no_function");
      else
         verdict("selfdep_caseC_vertex_stage", nC == ORACLE_STRIP_W, expected, got);
   } else {
      WHBLogPrintf("TEST selfdep_caseC_vertex_stage SKIP reason=vertex-selfread-shader-unavailable");
   }

   if (psAlias) {
      int nD = 0;
      for (int x = ORACLE_D_X0; x < ORACLE_D_X0 + ORACLE_STRIP_W; x++) {
         uint32_t v = rd(px, pitch, x, ORACLE_PROBE_Y);
         if (v >= ORACLE_D_MARKER && v <= ORACLE_D_MARKER + 255u) nD++;
      }
      snprintf(expected, sizeof(expected), "%d/%d texels in [%u,%u] (draw ran)", ORACLE_STRIP_W,
               ORACLE_STRIP_W, (unsigned)ORACLE_D_MARKER, (unsigned)ORACLE_D_MARKER + 255u);
      snprintf(got, sizeof(got), "marked=%d first=%u", nD,
               (unsigned)rd(px, pitch, ORACLE_D_X0, ORACLE_PROBE_Y));
      /* This asserts only that the draw EXECUTED. Whether the emulator noticed the alias is
       * a question only acc.render_self_dependency answers, and the whole point of the case
       * is that it is expected NOT to. */
      verdict("selfdep_caseD_alias_draw_ran", nD == ORACLE_STRIP_W, expected, got);
   }

   if (nB == ORACLE_STRIP_W) {
      WHBLogPrintf("TESSERA-GFXTEST oracle DIAGNOSIS: every texel in the strip returned its own "
                   "value, not the neighbour's. That is the coordinate-discarding framebuffer-fetch "
                   "substitution, observed in pixels rather than inferred from a counter.");
   }
   WHBLogPrintf("TESSERA-GFXTEST oracle end pass=%d fail=%d", s_pass, s_fail);
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

   /* Oracle shaders are compiled separately and are allowed to fail without taking
    * the counter loop with them: they need usampler2D and texelFetch, and whether
    * CafeGLSL supports those is a property of CafeGLSL, not of this emulator. A
    * SKIP that says so is worth more than a suite that will not build. */
   char olog[1024] = {0};
   GX2PixelShader *psSeed       = CompilePS(s_ps_seed,           olog, sizeof(olog), 0);
   GX2PixelShader *psOracleOff  = CompilePS(s_ps_oracle_offset,  olog, sizeof(olog), 0);
   GX2PixelShader *psOracleSame = CompilePS(s_ps_oracle_same,    olog, sizeof(olog), 0);
   GX2PixelShader *psAlias      = CompilePS(s_ps_alias,          olog, sizeof(olog), 0);
   GX2VertexShader *vsSelfRead  = CompileVS(s_vs_selfread,       olog, sizeof(olog), 0);
   GX2PixelShader *psPassthru   = CompilePS(s_ps_passthrough,    olog, sizeof(olog), 0);
   BOOL oracleReady = (psSeed && psOracleOff && psOracleSame) ? oracle_surfaces_init() : FALSE;
   if (oracleReady) {
      GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, psSeed->program, psSeed->size);
      GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, psOracleOff->program, psOracleOff->size);
      GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, psOracleSame->program, psOracleSame->size);
      if (vsSelfRead) GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, vsSelfRead->program, vsSelfRead->size);
      if (psPassthru) GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, psPassthru->program, psPassthru->size);
   } else if (!psSeed || !psOracleOff || !psOracleSame) {
      WHBLogPrintf("TEST selfdep_seed SKIP reason=oracle-shader-compile-failed");
      WHBLogPrintf("TEST selfdep_caseA_same_texel SKIP reason=oracle-shader-compile-failed");
      WHBLogPrintf("TEST selfdep_caseB_offset_texel SKIP reason=oracle-shader-compile-failed");
      WHBLogPrintf("TESSERA-GFXTEST oracle skipped log=%s", olog);
   } else {
      WHBLogPrintf("TEST selfdep_seed SKIP reason=oracle-surface-alloc-failed");
      WHBLogPrintf("TEST selfdep_caseA_same_texel SKIP reason=oracle-surface-alloc-failed");
      WHBLogPrintf("TEST selfdep_caseB_offset_texel SKIP reason=oracle-surface-alloc-failed");
   }

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

   /* POINT, not LINEAR. The oracle compares exact integers, and a filtered read of
    * an integer surface is not a thing worth reasoning about. */
   GX2Sampler pointSampler;
   GX2InitSampler(&pointSampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);

   /* Runs once, before the counter loop, and prints TEST lines in the same shape as
    * testing/rom-tests so one runner can consume both. */
   WHBGfxShaderGroup groupC;
   BOOL haveC = FALSE;
   if (oracleReady && vsSelfRead && psPassthru) {
      memset(&groupC, 0, sizeof(groupC));
      groupC.vertexShader = vsSelfRead;
      groupC.pixelShader = psPassthru;
      WHBGfxInitShaderAttribute(&groupC, "aPos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
      WHBGfxInitShaderAttribute(&groupC, "aTexCoord", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
      WHBGfxInitFetchShader(&groupC);
      haveC = TRUE;
   }
   if (oracleReady)
      run_pixel_oracle(&group, haveC ? &groupC : NULL, &posBuf, &uvBuf,
                       psSeed, psOracleOff, psOracleSame, psAlias, &pointSampler);

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
   if (s_ort.surface.image) free(s_ort.surface.image);
   if (s_olin.image) free(s_olin.image);
   GX2RDestroyBufferEx(&posBuf, (GX2RResourceFlags)0);
   GX2RDestroyBufferEx(&uvBuf, (GX2RResourceFlags)0);
   WHBGfxShutdown();
   WHBLogCafeDeinit();
   WHBProcShutdown();
   return 0;
}
