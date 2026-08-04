<!-- Generated during the Apple Silicon / macOS 26 fork research pass.
     Findings verified against the source tree at commit b8f2cf4 and the
     macOS 26.5.2 / Xcode 26.6 SDK on an Apple M2. See 00-master-plan.md. -->

# TesseraEmu — GRAPHICS / METAL workstream

## Headline findings (read these first — three change the plan materially)

### F1. The `MetalRenderer.cpp:267` "garbage data" HACK is a fixed-size-array overflow. Root-caused.

`/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h:117`

```cpp
size_t m_uniformBufferOffsets[METAL_GENERAL_SHADER_TYPE_TOTAL][MAX_MTL_BUFFERS];  // [3][31]
```

`/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp:240-243`

```cpp
for (uint32 i = 0; i < METAL_SHADER_TYPE_TOTAL; i++)   // 4, not 3
{
    for (uint32 j = 0; j < MAX_MTL_BUFFERS; j++)
        m_state.m_uniformBufferOffsets[i][j] = INVALID_OFFSET;
}
```

`METAL_SHADER_TYPE_TOTAL` is 4 (`MetalRenderer.h:41`), `METAL_GENERAL_SHADER_TYPE_TOTAL` is 3 (`MetalRenderer.h:16`). The `i == 3` iteration writes `31 * 8 = 248` bytes of `0xFFFF...FF` past the end of the array. Walking the `MetalState` layout (`MetalRenderer.h:101-123`), the bytes immediately after `m_uniformBufferOffsets` are `m_viewport` (48) + `m_scissor` (32) + `m_streamoutState` (~40) ≈ 120 bytes — so **~128 bytes spill past the end of `m_state` entirely**. `m_state` is the last large member of `MetalRenderer` (`:543`), followed by `m_captureFrame` (`:546`) and `m_capturing` (`:547`).

That is exactly and only the symptom the HACK comment describes, plus ~126 bytes of **heap corruption past the end of the `MetalRenderer` allocation**. The `m_occlusionQuery.m_lastCommandBuffer = nullptr` line in the same block is cargo-cult (that member is before `m_state` and already has an NSDMI).

Fix: change the loop bound to `METAL_GENERAL_SHADER_TYPE_TOTAL` and delete lines 265-268. Effort **S**. This is a hard prerequisite — do not build anything on top of the current state.

Three related latent defects found while confirming this, all in the same file:

- **Device over-release.** `MetalRenderer.cpp:264-277`: on the "user picked a GPU" path, `m_device` is taken out of an `NS_STACK_SCOPED` `MTL::CopyAllDevices()` array **without `retain()`**; the fallback `MTL::CreateSystemDefaultDevice()` returns +1. `~MetalRenderer` (`:301`) releases unconditionally. Asymmetric ownership → use-after-free / over-release whenever `mtl_graphic_device_uuid != 0`.
- **Null-deref in `GetAndRetainCurrentCommandBufferIfNotCompleted()`** (`MetalRenderer.h:288-295`): before the first command buffer exists, `m_commited == false`, so the early-out is skipped and it does `GetCurrentCommandBuffer()->retain()` on `nullptr`. Reachable from `EndOcclusionQuery()`.
- **Uninitialized reads:** `m_recordedDrawcalls` and `m_commitTreshold` (`MetalRenderer.h:538,540`) have no initializers and are only set inside `GetCommandBuffer()` (`:1732-1733`). `Flush()` (`:533`) reads `m_recordedDrawcalls` before that.

### F2. `dependencies/metal-cpp` is pinned at **`a63bd172` = "metal-cpp_macOS14.2_iOS17.2"** — two years stale.

Verified against the upstream mirror: that tree has no `MTL4*.hpp`, no `MTLResidencySet.hpp`, no `MTLAllocation.hpp`. Available upstream commits:

| SHA | Release |
|---|---|
| `a63bd172` | macOS 14.2 / iOS 17.2 ← **currently pinned** |
| `3d8da919` | macOS 15 / iOS 18 |
| `5caea74c` | macOS 15.2 / iOS 18.2 |
| `9a8bc57c` | **metal-cpp_26** |
| `2948dd1e` | **metal-cpp_26.4** ← **pin here** |
| `c9727bc9` | metal-cpp for macOS 27 / iOS 27 (ahead of your Xcode 26.6 SDK — do not use) |

`2948dd1e` has the full `MTL4*` surface, `MTLResidencySet.hpp`, `MTLAllocation.hpp`, a `MetalFX/` directory (`MTLFXSpatialScaler.hpp`, `MTL4FX*.hpp`), and a `QuartzCore/CAMetalLayer.hpp` exposing `displaySyncEnabled`, `maximumDrawableCount`, `colorspace`, `wantsExtendedDynamicRangeContent`, `allowsNextDrawableTimeout`, `residencySet`. **This bump is a Phase-0 prerequisite; several plan items are blocked on it.** Not exposed even at HEAD: `CAMetalDisplayLink` and `CAEDRMetadata` — those need an ObjC `.mm` shim (precedent already exists at `src/Cafe/HW/Latte/Renderer/Metal/MetalLayer.mm`).

### F3. `MTLSamplerDescriptor.lodBias` is **new in macOS 26.0**.

`.../MacOSX.sdk/.../Metal.framework/Headers/MTLSampler.h:204`:

```objc
@property (nonatomic) float lodBias API_AVAILABLE(macos(26.0), ios(26.0));
```

This is *why* `MetalSamplerCache.cpp:156` says `// TODO: set lod bias` — the API did not exist. Your minimum deployment target is exactly the version that adds it, and `2948dd1e` metal-cpp exposes `MTL::SamplerDescriptor::setLodBias`. A long-standing correctness gap (mip selection wrong in every game that uses LOD bias — very common for terrain/detail textures) becomes a 3-line fix.

### F4. `MTLBinaryArchive` is not deprecated and is the right answer, and it carries the AIR slice too.

Verified: `MTLBinaryArchive.h` carries no `API_DEPRECATED`; macOS 26 continues to extend it (`addMeshRenderPipelineFunctions` macOS 15.0). Apple's *Using the Metal 4 compilation API* article states harvested sets serialize to binary archives and "**both Metal 3 and 4 can load binary archives**". Critically, *Creating binary archives from device-built pipeline state objects* states:

> Note that binary archives still contain a Metal IR slice, `air64_v26`. Metal may invalidate binaries when upgrading a device's operating system, and shaders recompile from the Metal IR in the archive.

So a runtime-harvested `MTLBinaryArchive` **subsumes the deleted RAM-disk/`xcrun metal` AIR cache entirely**: it stores GPU binaries *and* AIR, and degrades gracefully to "skip the MSL frontend, redo only the backend" after an OS update. That is strictly better than what the disabled code was trying to build, with zero `system()` calls.

> **This paragraph is disputed and must be re-verified before the item is scheduled (2026-08-03).**
> The code path argues against it. `addRenderPipelineFunctions` takes a *descriptor*, and the
> descriptor's functions are set from `m_vertexShaderMtl->GetFunction()`
> (`MetalPipelineCompiler.cpp:353-370`), whose only source in this fork is
> `RendererShaderMtl::LibraryFromSource()` → `newLibrary(m_mslCode)` (`RendererShaderMtl.cpp:296`).
> If an `MTLFunction` must exist *before* the archive can be consulted, then **the MSL frontend
> compile happens either way** and the archive skips only the backend — which is the opposite of the
> claim above, and `RendererShaderMtl.cpp:36-38`'s own comment asserts the frontend is the expensive
> half. `newLibrary(dispatch_data)` at `:312` can *load* AIR, but nothing at runtime *produces* AIR
> without invoking the compiler toolchain, which is exactly what the rejected RAM-disk approach did.
>
> Do not schedule this item on the strength of the payoff sentence. Confirm the API semantics against
> Apple's documentation (sosumi) first, **and** measure the real cold-launch cost — which is
> currently unmeasurable, because the shader-cache load timer and its log line are inside
> `#if BOOST_OS_WINDOWS` (`LatteShaderCache.cpp:492-500`) and so have never run on this fork's only
> platform. The timer also stops before `LatteShaderCache_LoadPipelineCache` at `:505`, so it
> excluded pipeline replay even on Windows. Un-gating those five lines is the cheap first step.

### F5. Graphic-pack output shaders are already broken on Metal.

`src/Cafe/GraphicPack/GraphicPack2.cpp:1185,1195,1209,1219` unconditionally construct `RendererOutputShader(RendererOutputShader::GetOpenGlVertexSource(...), m_output_shader_source)` — GLSL, fed to `newLibrary()`. Any pack shipping `output.glsl`/`upscaling.glsl`/`downscaling.glsl` fails to compile under Metal today. (Per-shader replacements are fine — `GraphicPack2.cpp:757` already detects the `_msl` filename suffix.)

---

## Phase 0 — De-risk the foundation (do not skip)

Every subsequent phase assumes the renderer is not corrupting its own heap and that metal-cpp exposes macOS 26 API.

| # | Change | Files | Effort | Payoff | Verify |
|---|---|---|---|---|---|
| 0.1 | Fix the `m_uniformBufferOffsets` overflow; delete the HACK block | `MetalRenderer.cpp:240-243`, `:265-268` | S | Removes heap corruption + UB; unblocks everything | ASan clean on a 60 s BotW run; `m_captureFrame` stays false without the manual reset |
| 0.2 | Retain `m_device` on the CopyAllDevices path | `MetalRenderer.cpp:264-277` | S | Fixes over-release when a GPU is explicitly selected | Set `mtl_graphic_device_uuid`, boot, exit cleanly under Guard Malloc |
| 0.3 | Null-guard `GetAndRetainCurrentCommandBufferIfNotCompleted`; initialize `m_recordedDrawcalls{0}`, `m_commitTreshold{0}` | `MetalRenderer.h:288-295,538-540` | S | Removes latent crash + UB read | Occlusion-query game (Mario Kart 8) boot from cold |
| 0.4 | Bump `dependencies/metal-cpp` to `2948dd1e` | `.gitmodules`, submodule | S | Unblocks 3.x, 5.x, 6.x, 7.x, 8.x | Build green; `MTL::SamplerDescriptor::setLodBias` resolves |
| 0.5 | Stand up the validation/debug harness | dev-only | S | Every later phase depends on it | See below |

**0.5 detail.** Set up a documented debug configuration:

- `MTL_DEBUG_LAYER=1` + `MTL_DEBUG_LAYER_ERROR_MODE=assert` + `MTL_SHADER_VALIDATION=1`. Expect a wave of pre-existing diagnostics (load/store action mismatches, the `CachedFBOMtl` dummy attachment, unbound-texture warnings) — triage into the Phase-3 backlog rather than fixing inline.
- ASan (`-fsanitize=address`) — **flag for the CPU/JIT owner**: ASan's shadow-memory reservation and the interceptor set interact badly with the recompiler's W^X executable mappings (`MAP_JIT`/`pthread_jit_write_protect_np` on arm64). There is currently no `MAP_JIT` in `src/`, so the exact allocation path must be checked with them. Mitigation if it breaks: build with `-fsanitize=address` **only for `src/Cafe/HW/Latte/`** and stub the recompiler (interpreter-only run) for sanitizer sessions. Emulation will be extremely slow; that's fine — 0.1 reproduces on the *first frame*, so a 30-second run is enough.
- UBSan (`-fsanitize=undefined,implicit-conversion`) is cheap and has no JIT interaction; enable it unconditionally in Debug.
- Wire up the existing `MTL::CaptureManager` menu item (`MetalRenderer.cpp:2307-2361`) into a scripted flow, and pick **8-10 golden scenes** (BotW field + shrine, MK8 Mario Circuit, Splatoon ink, Smash Bros training stage, Wind Waker HD sea, Xenoblade X, Super Mario 3D World, a 2D/menu-heavy title). Capture reference PNGs at a fixed frame index via `HandleScreenshotRequest`. This is the regression harness for the entire workstream.

---

## Phase 1 — Metal-only: delete OpenGL and Vulkan

Roughly 14 087 lines of Vulkan + 5 649 lines of OpenGL + the 163 KB GLSL emitter. Order matters so the build stays green at every commit.

**1.1 — Flip defaults first (S).** `src/config/CemuConfig.h:68-82` → `enum GraphicAPI { kMetal = 0 };` single value, `kDefaultGraphicsAPI = kMetal`. `src/config/ActiveSettings.cpp:109-133` collapses to `return kMetal;`. `src/Cafe/HW/Latte/Renderer/Renderer.h`'s `RendererAPI` loses `OpenGL`/`Vulkan`. Ship and test this alone: it proves Metal actually boots your golden scenes before you delete the fallbacks. **This is the single highest-value commit in Phase 1** — today an out-of-the-box macOS build runs MoltenVK, so nobody has been exercising the Metal path by default.

**1.2 — Remove backend selection from CMake (S).** `CMakeLists.txt:110-133` → drop `ENABLE_OPENGL`/`ENABLE_VULKAN` options, hard-set `ENABLE_METAL`. `src/Cafe/CMakeLists.txt:494-562` → delete the GL and VK source blocks. Remove `dependencies/Vulkan-Headers` and `glslang` from `.gitmodules` / `vcpkg.json` (`glslang` is used only by `RendererShaderVk.cpp`, `VulkanRenderer.cpp`, `imgui_impl_vulkan.cpp`, and `LatteDecompilerEmitGLSLHeader.hpp`). Removing glslang + Vulkan-Headers is a meaningful build-time and dependency-surface win.

**1.3 — Delete directories and canvases (S).** `src/Cafe/HW/Latte/Renderer/{OpenGL,Vulkan}/`, `src/gui/wxgui/canvas/{OpenGLCanvas,VulkanCanvas}.{cpp,h}`, `src/imgui/imgui_impl_{opengl3,vulkan}.*`. Then sweep the 21 files that reference `ENABLE_VULKAN`/`ENABLE_OPENGL` (`LatteBufferData.cpp`, `LatteRenderTarget.cpp`, `LatteShader.cpp`, `LatteShaderCache.cpp`, `LatteTexture.cpp`, `LatteTextureLegacy.cpp`, `LatteTiming.cpp`, `LatteDecompiler.cpp`, `RendererCore.cpp`, `RendererOuputShader.cpp`, `CemuApp.cpp`, `GeneralSettings2.cpp`, `MainWindow.cpp`, `PadViewFrame.cpp`, `main.cpp`, `imgui_extension.cpp`).

**1.4 — Delete the GLSL emitter (M).** `LatteDecompilerEmitGLSL.cpp` (163 KB), `LatteDecompilerEmitGLSLAttrDecoder.cpp`, `LatteDecompilerEmitGLSLHeader.hpp`. Collapse the dispatch at `LatteDecompiler.cpp:1073-1085` to a bare `LatteDecompiler_emitMSLShader(...)`.

**1.5 — Collapse the resource-mapping tables (M, do last, carefully).** `LatteDecompiler.h:280-284` has `resourceMappingGL` / `resourceMappingVK` / `resourceMappingMTL`, populated in parallel throughout `LatteDecompilerAnalyzer.cpp:490-660` with three independent binding-point counters (`currentBindingPointVK`, `currentTextureBindingPointMTL`, `currentBufferBindingPointMTL`). Delete GL and VK; keep MTL and its counters. Also delete `uniformOffsetsGL`/`uniformOffsetsVK`. `LatteShader.cpp:745-756` collapses to a single assignment. **This is the riskiest edit in Phase 1** — the analyzer interleaves the three tables and it is easy to drop an MTL line while removing an adjacent VK line. Do it as one mechanical commit, then diff generated MSL for all golden scenes byte-for-byte against pre-change output. If any shader text differs, you broke something.

**1.6 — Config and GUI cleanup (S).** `GeneralSettings2.cpp:1735-1825` currently has three vsync branches; keep only the Metal one (and rework it in Phase 6). Drop the API selector. `GraphicPack2.cpp:300-311` `rendererFilter` — keep parsing all three values (packs in the wild use them) but treat anything other than `metal` as "pack disabled"; do not error.

### What Metal must inherit from Vulkan before / as you delete it

These are the pieces where the Vulkan backend is genuinely ahead. **Extract them into notes before deletion.**

1. **The render-pass self-dependency mechanism.** This is the big one. Note that the Metal backend's commented-out block (`MetalRenderer.cpp:1130-1160`, `:1970-2019`) is the *old*, per-draw, shader-texture-scanning design. Vulkan has since moved to a much better one: `CachedFBOVk::CheckForSelfDependency()` (`CachedFBOVk.cpp:198-240`) stamps every attachment's base texture with a monotonic `s_selfDependencyCheckIndex` + aspect mask, then intersects that against the bound descriptor sets' `list_fboCandidates` — O(bound textures), computed once per FBO/descriptor change, not per draw. `VulkanRendererCore.cpp:1187-1222` consumes it and distinguishes pixel-only self-dependency (handled by the feedback-loop extension) from vertex/geometry self-dependency (requires a pass split). **Port the new design, not the commented-out old one.** Details in item 3.1.
2. **`VulkanPipelineStableCache`'s architecture** (`VulkanPipelineStableCache.{cpp,h}`) — the "serialize GPU register state, replay it into pipeline creation on next launch" pattern. `MetalPipelineCache` already mirrors this correctly (`_mtlpipeline.bin`, `MetalPipelineCache.cpp:264-350`). Nothing to port; just don't lose the `LatteShaderCache_getPipelineCacheExtraVersion` keying convention.
3. **`NotifyLatteCommandProcessorIdle`** — Vulkan commits opportunistically when the command processor stalls; Metal's override is an empty function (`MetalRenderer.cpp:540-544`). Item 6.3.
4. **`VsyncDriver`** — pure Windows D3DKMT (`VsyncDriver.cpp:1` is `#if BOOST_OS_WINDOWS`). Delete outright; `LatteTiming.cpp:56-63` becomes a no-op. Its replacement is `CAMetalDisplayLink` (item 7.4), not a port.
5. **`vk_accurate_barriers` config key** (`CemuConfig.h:458`) — rename to `accurate_barriers` and keep; item 3.1 needs it.

**Sequencing.** 1.1 → verify golden scenes → 1.2/1.3 → 1.6 → 1.4 → 1.5. Verification at each step: full build + all golden scenes render identically to Phase-0 references.

---

## Phase 2 — Shader binary caching (highest-value item after Phase 0)

**Today, every MSL shader is recompiled from source on every launch.** A large game has thousands of shaders (`_mtlshaders.bin` holds Latte bytecode only); Vulkan caches SPIR-V (`_spirv.bin`); Metal caches nothing. This dominates cold-start time and is the main source of in-game shader stutter.

### The evaluation

| Option | Verdict |
|---|---|
| Revive the RAM-disk + `xcrun metal`/`metallib` AIR cache (`RendererShaderMtl.cpp:363-401`) | **Do not.** It shells out to `diskutil erasevolume` and `hdiutil attach ram://` — creates a user-visible mounted volume, needs the full Xcode toolchain at runtime, is unusable in a sandboxed/notarized app, and only caches AIR (not GPU binaries), so the backend compile still happens every launch. It is a trap. Delete the code. |
| `newLibrary(dispatch_data_t)` with hand-built `.metallib` | Only viable if you produce the metallib yourself — i.e. the option above. Same objection. Keep `LibraryFromAIR` (`:300-315`) as a *loader* only. |
| Metal 4 `MTL4Compiler` + `MTL4PipelineDataSetSerializer` | Technically the modern path, and Apple explicitly recommends compilation as the first Metal 4 adoption step. But it changes descriptor types (`MTL4PipelineDescriptor` vs `MTLRenderPipelineDescriptor`), and whether a PSO built from an `MTL4PipelineDescriptor` is usable from a *classic* `MTLRenderCommandEncoder` is not documented either way. **Defer to Phase 8, behind a spike.** |
| Offline precompilation (`metal-tt` + `.mtlp-json`) | **Not applicable.** Cemu's shaders are generated at runtime from guest bytecode; there is nothing to precompile at build time. Explicitly not worth doing. |
| **`MTLBinaryArchive` (Metal 3 API), runtime harvest + serialize** | **Recommended.** |

### Recommended design

`MTLBinaryArchive` is the only option that caches *both* the AIR and the GPU binary, needs no external toolchain, is not deprecated, and — per F4 — recovers gracefully from OS upgrades by falling back to the archive's own IR slice.

**Where it plugs in.** The stubbed `m_binaryArchive` scaffolding at `MetalPipelineCompiler.cpp:268-285` is at the right level: `MTLBinaryArchive` is a *pipeline*-level cache, not a *function*-level one, so it belongs where `newRenderPipelineState` is called (`MetalPipelineCompiler.cpp:359,373`), not in `RendererShaderMtl`. Own it from `MetalPipelineCache` (one archive per title), not per-compiler-instance.

**Flow:**

1. `MetalPipelineCache::BeginLoading()` (`:264`): after opening `_mtlpipeline.bin`, also open the binary archive. `MTLBinaryArchiveDescriptor` with `url = shaderCache/precompiled/{titleId}_{archKey}.binary.metallib`; on failure (missing / stale / corrupt) fall back to a `nil`-url descriptor (create-empty). Failing to load is explicitly non-fatal per Apple.
2. Add the loaded archive to every `MTLRenderPipelineDescriptor` / `MTLMeshRenderPipelineDescriptor` via `binaryArchives` before `newRenderPipelineState` (`MetalPipelineCompiler.cpp:346-375`). Do **not** use `MTLPipelineOptionFailOnBinaryArchiveMiss` in production — a miss must silently fall through to normal compilation. Use it in a dev-only assert build to measure hit rate.
3. Maintain a second, write-side archive (created with `nil` url). After each successful `newRenderPipelineState`, call `addRenderPipelineFunctions:` / `addMeshRenderPipelineFunctions:` on it. Apple's note applies directly: *create the archive before creating the pipeline states* to get the compile-speed and memory optimizations.
4. Serialize on a background thread at: (a) end of shader-cache preload, (b) every N new pipelines (N ≈ 256) debounced, (c) clean shutdown. `serializeToURL` writes the whole archive, so debounce it — do not call it per-pipeline. Write to a temp path and `rename()` to make it crash-atomic.

**Cache key / invalidation.** `serialize` produces a single-GPU-arch slice (e.g. `applegpu_g14g` for M2), and Metal may invalidate binaries across OS updates. Filename key = `{titleId}_{gpuArchTag}_{osBuild}_{cemuCacheVersion}.binary.metallib`, where:

- `gpuArchTag` — derive from `MTL::Device::name()` + `registryID()`; simplest robust choice is a hash of the device name string.
- `osBuild` — the OS *build* string (e.g. `25F74`), via `NSProcessInfo.operatingSystemVersionString` in an `.mm` shim, not the marketing version. Metal invalidates on minor updates too.
- `cemuCacheVersion` — reuse `LatteShaderCache_getPipelineCacheExtraVersion(cacheTitleId)` (`MetalPipelineCache.cpp:295`) so MSL-emitter changes invalidate the archive automatically. **This coupling is essential**: any edit to `LatteDecompilerEmitMSL.cpp` changes the generated source and must invalidate binaries.

Deleting stale archives: on startup, glob `shaderCache/precompiled/{titleId}_*.binary.metallib` and unlink any whose key doesn't match. Keep it simple — no LRU.

**Relationship to `FileCache`.** Do **not** put the archive inside `FileCache`. `MTLBinaryArchive::serialize` writes a file itself, and the archive is already an internally-keyed container. `FileCache` stays as-is for `_mtlshaders.bin` (Latte bytecode) and `_mtlpipeline.bin` (register state). The three-file model becomes: bytecode → MSL → (archive) → PSO.

**Also delete in this phase:** all the commented AIR-cache code — `RendererShaderMtl.cpp:5-6,12-19,38-51,64-71,108-136,142-152,148-152,163-176,193-203,300-315,322-335,346-352,363-401`, `MetalCommon.h:107-183` (`executeCommand()` + `MemoryMappedFile`). Keeping `system()`-shelling helpers around is a liability.

Effort **M** (~2-3 days). Payoff on M2: cold start on a large title should go from tens of seconds of MSL compilation to archive load + a handful of misses; in-game first-encounter stutter largely eliminated after the first playthrough of an area. Verify: instrument `g_compiled_shaders_total` (`RendererShaderMtl.cpp:360`) and `g_compiling_pipelines` (`MetalPipelineCompiler.cpp:392`) — on a second launch with a warm archive both should be near zero; measure wall-clock to first frame.

---

## Phase 3 — Correctness backlog

Ordered by payoff/risk.

### 3.1 Render-pass self-dependency / "accurate barriers" — **L, highest correctness payoff**

`MetalRenderer.cpp:1207-1237` (block commented out), `:2302-2352` (`CheckIfRenderPassNeedsFlush` commented out), `MetalRenderer.h:363` (declaration commented out).

> **Status (2026-07-31): this is an OPEN CORRECTNESS GAP, not a latent option.** Confirmed by
> `git blame`: all of it arrived already-commented in `26e40a4` ("Add Metal backend (#1287)") and has
> never executed in this fork. `nm` finds no `CheckIfRenderPassNeedsFlush` symbol in the binary.
>
> The fork therefore runs **permanently in upstream's "accurate barriers off" regime** — it is
> already receiving that configuration's performance (upstream reports ~2x in BotW when disabled) and
> already paying its correctness cost. There is no ~2x left to win here; there is a visual defect to
> fix, and fixing it will *cost* frame rate.
>
> Three surfaces used to imply otherwise and were removed on 2026-07-31: a Debug menu checkbox
> labelled "Accurate barriers (Vulkan)" that was not gated on the active renderer and controlled
> nothing; a `cemuLog` line in the `kMetal` branch of `InfoLog_PrintActiveSettings` that reported the
> state of `vk_accurate_barriers`, which nothing on Metal reads; and an unreachable `kVulkan` branch
> beside it (`ENABLE_VULKAN` is never defined, so `GetGraphicsAPI()` cannot return `kVulkan`). The log
> now states the limitation plainly instead. `ConfigValue<bool> vk_accurate_barriers`
> (`CemuConfig.h:455`) is kept but marked inert, so settings.xml round-trips and this item has a
> setting to hang off — **do not put a UI back on it until it does something.**
>
> `m_state.m_isFirstDrawInRenderPass` (`MetalRenderer.h:108`) is still maintained but has no reader
> outside the commented block.

> **Correction (2026-08-03): "never executed in this fork" is true of the *splitter* and false of the
> mechanism, and the part that does run is itself wrong.** The commented-out code is only half the
> story. `LatteDecompilerAnalyzer.cpp:888-957` **does** detect pixel-stage self-dependency at
> decompile time — matching a sampled texture's physAddr + format + tileMode against the active
> colour buffers, restricted to `lastMip == 0` — and records it in `textureRenderTargetIndex`.
> `LatteDecompilerEmitMSL.cpp:2322-2328` then rewrites the sample into a framebuffer fetch. So the
> pixel/colour case is *covered*, and the bullet below claiming framebuffer fetch "already handles it
> correctly with zero cost" is the claim that needs fixing, not the status line.
>
> **It does not handle it correctly. The texture coordinate is discarded:**
>
> ```cpp
> uint8 renderTargetIndex = shaderContext->shader->textureRenderTargetIndex[...];
> if (SupportsFramebufferFetch() && renderTargetIndex != 255)
> { src->addFmt("col{}", renderTargetIndex); }     // <- the coordinate is never read
> ```
>
> Every sample of a bound render target returns the *current fragment's* colour regardless of what
> coordinate the guest asked for. A blur, a downsample or any offset tap silently becomes a no-op
> read of self. The two `// TODO`s on the same lines (comparison samplers, swizzling) are further
> gaps in the same substitution, and `LatteDecompilerEmitMSL.cpp:2708` compounds it by returning a
> hardcoded `int4(1920,1080,1,1)` from `GET_TEXTURE_INFO`, so a shader deriving texel offsets from
> `textureSize` gets wrong offsets *and* then has them ignored.
>
> **This changes what 3.1 is.** "We never split, so effects read stale data" is a scheduling problem
> a pass-splitter solves. "We substitute a coordinate-free read and bake it into the shader" is a
> *decompiler* problem that a splitter does not touch at all. Restricting the substitution to the
> cases where it is actually valid is plausibly the larger correctness win, and it is a different
> piece of work. Size both before building either.
>
> **Detector landed 2026-08-03; run, and reads zero.** `MetalRenderer::NoteSelfDependency` records the
> uncovered case as `acc.render_self_dependency`, the covered case as `acc.self_dep_fbfetch` at the
> framebuffer-fetch `continue`, and the vertex/geometry subset as `acc.self_dep_nonpixel`. It lives
> inside the existing `BindStageResources` loop rather than in a new `CachedFBOMtl` method — see the
> correction to the design bullet below. **Run 2026-08-03 against a purpose-built homebrew ROM**
> (`testing/graphics-tests/`, which needs no game image): **all three counters read zero.** Both
> sides of the covered/uncovered split reading zero means no alias was seen at all — either the test
> does not create the aliasing the emulator recognises, or the detector does not fire. Not yet
> separated; see `testing/graphics-tests/README.md`. **Until it is, a zero from these counters is not
> evidence about this item**, and every frequency claim here remains a guess.

Metal has no automatic hazard tracking *within* a render pass. Today the renderer merges render passes aggressively (`GetRenderCommandEncoder()`, `:1768-1836`) and never splits them for read-after-write on an attachment. Any game that samples a render target it is simultaneously drawing to reads stale or undefined data. This is a whole class of visual bugs (BotW lava/waterfall are the known cases, hence the special-case shader hashes in the dead code).

**Design — port the *current* Vulkan mechanism, not the commented-out one:**

- Add `uint32 m_selfDependencyCheckIndex` + `uint8 m_selfDependencyAspect` to `LatteTextureMtl` (`LatteTextureMtl.h`), mirroring `LatteTextureVk`.
- Add `CachedFBOMtl::CheckForSelfDependency(...)` mirroring `CachedFBOVk.cpp:200-240`: stamp each attachment's base texture with a fresh monotonic index + aspect (color / depth / stencil), then scan the bound texture set for matches. ~~Metal binds textures through `m_state.m_textures[LATTE_NUM_MAX_TEX_UNITS * 3]` (`MetalRenderer.h:116`) — iterate that directly; it's the analogue of Vulkan's `list_fboCandidates` and is cheaper.~~
  > **Wrong, corrected 2026-08-03 — do not iterate `m_state.m_textures`.** It is not the analogue of
  > `list_fboCandidates`. Vulkan built that list per descriptor set from the shader's *own* units;
  > `m_state.m_textures` is a **sticky global array** — `texture_setLatteTexture`
  > (`MetalRenderer.cpp:942-947`) only ever writes, never clears, and
  > `LatteTexture_updateTexturesForStage` skips units with `physAddr == MPTR_NULL`. Scanning it blind
  > yields false positives from bindings a previous shader left behind, i.e. spurious pass splits —
  > precisely the failure mode this item's own risk register warns about. Use the shader's
  > resource-mapped unit list instead, which is what `BindStageResources` already walks and what the
  > landed detector uses.
  >
  > (That array was also mis-sized: the stage bases are strided by 32 with GS at 64, so 18*3 = 54
  > slots could not hold a geometry-shader binding at all and every GS texture write landed in
  > `m_uniformBufferOffsets`. Fixed 2026-08-03 with a `static_assert` tying the size to the bases.)
- Call it from `draw_execute` **only when the texture bindings or the active FBO changed**, not per draw. Track a `m_state.m_textureBindingsChanged` flag set in `texture_setLatteTexture` (`MetalRenderer.cpp:~1000`).
- **Apple-specific win — but read the correction above first.** Where the self-dependency is *pixel-only* **and the sampled texel really is the one at the current fragment coordinate**, framebuffer fetch handles it with zero cost (`LatteDecompilerEmitMSL.cpp:2322-2328`, `LatteDecompilerEmitMSLHeader.hpp:463`). The emitter does **not** check that precondition — it substitutes unconditionally and drops the coordinate — so today this is a correctness bug wearing a performance win's clothes. This is the direct analogue of Vulkan's `feedbackLoopHandlesSelfDependency` (`VulkanRendererCore.cpp:1203`). So: `needsPassSplit = hasSelfDependency && !(framebufferFetchCoversIt)`. On an Apple7+ GPU with framebuffer fetch enabled, the *common* case costs nothing — this is why Metal can be faster than Vulkan here, not slower.
- Vertex/geometry-stage self-dependency always forces `EndEncoding()`.
- Gate the non-framebuffer-fetch split behind the renamed `accurate_barriers` config with the same per-shader `neverSkipAccurateBarrier` override list (the two BotW hashes at `MetalRenderer.cpp:1136-1141`).

Verify: BotW lava pools and waterfall foam render correctly; the golden-scene set shows no regression; `m_performanceMonitor.m_renderPasses` per frame increases only modestly (if it doubles, the pixel-only/framebuffer-fetch fast path isn't firing).

### 3.2 LOD bias — **S, high payoff, near-zero risk**

`MetalSamplerCache.cpp:102` (`iLodBias` read, commented out), `:156` (`// TODO: set lod bias`), and the hash at `:180-200`.

Per F3, `setLodBias` exists in macOS 26. Uncomment `iLodBias`, add `samplerDescriptor->setLodBias((float)iLodBias / 64.0f);` (matching Vulkan's `mipLodBias` conversion). `CalculateSamplerHash` already hashes `WORD1.getRawValue()` which contains `LOD_BIAS`, so the cache key is already correct — no change needed there. Verify: any game with a `LOD_BIAS != 0` (grep GPU captures, or add a one-shot log when `iLodBias != 0`) — textures should stop being over- or under-sharpened at distance. Compare side-by-side against upstream Cemu on Vulkan.

### 3.3 Format-table gaps — **M**

`LatteToMtl.cpp`:

- `:168` `D24_S8_FLOAT` uses `TextureDecoder_NullData64` (`// TODO: why?`) — depth uploads for this format silently produce garbage/zero. Determine the real GX2 layout and write a decoder.
- `:173-179` when `!supportsDepth24Unorm_Stencil8`, `D24_S8_UNORM` is remapped to `Depth32Float_Stencil8` with the decoder line commented out — the pixel format and the decoder disagree. **On Apple Silicon `depth24Stencil8PixelFormatSupported` is `false`, so this branch is always taken on your target hardware.** This is a live corruption path on every M-series Mac, not a fallback. Implement `TextureDecoder_D24_S8_To_D32_S8`. High priority.
- `:86-87` `R10_G10_B10_A2_UINT/SINT`, `A2_B10_G10_R10_UNORM/UINT` unimplemented — `A2_B10_G10_R10_UNORM` is used by Resident Evil Revelations (there's already a comp-sel special case for it at `LatteTextureViewMtl.cpp:33-38`, so the swizzle path expects it to work). Map to `MTL::PixelFormatRGB10A2Unorm`/`Uint` with a channel-reversing decoder.
- ~~`:414-415` blend factors `0x0B`/`0x0C` → `BlendFactorZero`. These are `CONSTANT_ALPHA`/`ONE_MINUS_CONSTANT_ALPHA` in R700; map to `MTL::BlendFactorBlendAlpha` / `OneMinusBlendAlpha`.~~ **Wrong — following this introduces a bug.** `LatteReg.h:778-779` names `0x0B`/`0x0C` `BLEND_BOTH_SRC_ALPHA` / `BLEND_BOTH_INV_SRC_ALPHA`. Constant alpha is `0x13`/`0x14` and is **already** mapped to `BlendAlpha`/`OneMinusBlendAlpha` at `LatteToMtl.cpp:437-438`, so the prescribed change would duplicate an existing entry onto the wrong tokens. `BOTH_SRC_ALPHA` is the legacy GL form that sets RGB and alpha factors differently from one value, so it cannot be a single table entry in Metal at all; upstream Vulkan mapped it to `VK_BLEND_FACTOR_MAX_ENUM`, which is worse than the current `Zero`. Leave the TODOs and treat this as unresolved, not as a two-line fix.
- `:182-200` unknown formats silently fall back to `R8Unorm`/`Depth16Unorm`. Add `cemuLog_logOnce(LogType::Force, ...)` so unknown formats become visible instead of rendering wrong. **Do this first** — it converts "mysterious visual bug" into a log line and will likely surface the real-world priority order for the rest of this item.
- The ~20 `// TODO: correct?` on BC1-BC5: these are almost certainly fine (the mappings are the obvious ones and match the Vulkan table). Verify once against `VKRPipelineInfo.cpp`'s table and delete the comments. **Not worth deep investigation.**

### 3.4 Framebuffer-fetch shader stubs — **M**

- `LatteDecompilerEmitMSL.cpp:2679-2683` — `GET_TEXTURE_INFO` on a framebuffer-fetch target returns a hardcoded `int4(1920,1080,1,1)`. Any shader that computes texel offsets from render-target dimensions produces wrong results at non-1920×1080 resolutions — i.e. at every graphics-pack resolution. **Fix:** push the active render-target dimensions into the existing support buffer (`supportBufferData[512*4]`, `MetalRenderer.cpp:35`) per render pass and emit a read from it. This is the same trick already used for other per-draw uniforms.
- `:2750-2753` — LOD query on a framebuffer-fetch target returns `float4(0,0,0,0)`. The comment's assumption (framebuffer-fetch targets are always sampled at pixel coords, so LOD is 0) is actually correct by construction. **Leave it, delete the TODO.**
- `:2297-2302` — `// TODO: support comparison samplers` and `// TODO: support swizzling` on the framebuffer-fetch path. Swizzling is the real risk: `LatteTextureViewMtl::CreateSwizzledView` builds a swizzled `MTL::Texture` view, but the `col{N} [[color(N)]]` framebuffer-fetch input is unswizzled, so a swizzled RT sampled via framebuffer fetch gets wrong channels. **Fix:** apply the swizzle inline in MSL at the `col{N}` read site (the swizzle is known at decompile time). Comparison samplers on a framebuffer-fetch target: refuse the framebuffer-fetch path and fall back to a normal texture sample + pass split (item 3.1 handles it).

### 3.5 The two HACKs — **M each**

- **XFB 4× over-allocation** (`MetalRenderer.h:419-420`). `LatteStreamout_GetRingBufferSize()` is the correct size; the page faults mean *offsets* are being computed wrong somewhere. Suspect `streamout_setupXfbBuffer`/`streamout_rendererFinishDrawcall` (`MetalRenderer.cpp:~2194`) and the MSL emitter's streamout writes. Diagnose by allocating the correct size *with* the Metal validation layer on — it will name the out-of-bounds write. Payoff is memory (streamout ring buffers are large; on an 8 GB M2 this matters) plus removing a masked bug.
- **Smash Bros dummy color attachment** (`CachedFBOMtl.cpp:48-55`). The comment says "works fine on MoltenVK without this hack". A render pass with no attachments is legal in Metal only if you set `renderTargetWidth`/`renderTargetHeight` explicitly on the `MTLRenderPassDescriptor` — otherwise Metal infers a zero-size render target and discards everything. **That is almost certainly the actual fix.** Try `setRenderTargetWidth`/`setRenderTargetHeight`/`setDefaultRasterSampleCount(1)` and delete the dummy attachment. Effort drops to **S** if that's it. Worth trying early — it's a 5-line experiment.

### 3.6 Silent drop of GS/RECTS draws — **S**

`MetalRenderer.cpp:1167-1168`, `MetalPipelineCompiler.cpp:293-294,317-318`: when `!m_supportsMeshShaders`, geometry-shader and RECTS draws are silently `return`ed. **On your target (Apple7+, Metal3 guaranteed) this can never fire.** Convert to `cemu_assert(m_supportsMeshShaders)` and delete the `force_mesh_shaders` config + the Intel exclusion at `MetalRenderer.cpp:172`. Also simplify `m_supportsMetal3`, `m_isAppleGPU`, `m_hasUnifiedMemory`, `m_supportsFramebufferFetch`, and the whole `GfxVendor` string-sniffing block (`:155-165`) to compile-time `true`/`Apple`. Removes a large amount of dead branching from the hot path.

### 3.7 Vertex-fetch gaps — **S**

`MetalPipelineCompiler.cpp:434` (`aluDivisor == 1` only) and `:460` (`cemu_assert(false)` on unknown fetch type). `aluDivisor != 1` means instanced attributes advancing every N instances; Metal supports this natively via `layout->setStepRate(aluDivisor)`. Two-line fix, removes a class of "wrong geometry in instanced draws" bugs.

### 3.8 Mesh-shader occupancy — **M, defer**

`LatteDecompilerEmitMSL.cpp:4112,4125`: object shader is `max_total_threads_per_threadgroup(VERTICES_PER_VERTEX_PRIMITIVE)` (3), mesh shader is `max_total_threads_per_threadgroup(1)`. One threadgroup **per primitive**, one thread in the mesh stage. On an 8-core M2 this is terrible occupancy for GS-heavy titles.

Proper fix: batch K primitives per threadgroup (K = 32 or so), with the object stage fetching K×3 vertices in parallel and the mesh stage emitting K×(2..4) triangles. This requires reworking `ObjectPayload` from `VertexOut vertexOut[3]` to `[3*K]`, the `mesh.set_index`/`set_primitive_count` indexing in `rectsEmulationGS_*` (`MetalPipelineCompiler.cpp:56-183`), and the `drawMeshThreadgroups` dispatch math at `MetalRenderer.cpp:1425-1433`.

**Recommendation: defer to after Phase 6.** It's a genuine perf win but it is invasive, touches the MSL emitter (invalidating the whole binary archive), and only helps GS/RECTS-heavy titles. Measure first: `m_performanceMonitor.m_meshDraws` (`MetalRenderer.cpp:663`) tells you per game whether it's worth it. If mesh draws are <5 % of draws in your golden set, **skip it entirely.**

---

## Phase 4 — TBDR and unified-memory exploitation

This is where an Apple-native renderer pulls ahead. All of these are M2-relevant; the 8 GB machine makes the memory items matter more than usual.

### 4.1 Memoryless depth/stencil — **the single biggest classic TBDR win. M.**

Today `LatteTextureMtl.cpp:11` sets `MTL::StorageModePrivate` unconditionally, and `CachedFBOMtl.cpp:33-42` sets `LoadActionLoad` / `StoreActionStore` on the depth attachment unconditionally. On a TBDR GPU, depth that is never read back after the pass should live **only in tile memory** — `MTLStorageModeMemoryless` + `LoadActionClear` + `StoreActionDontCare`. That eliminates the depth buffer's entire allocation *and* all its main-memory traffic.

The catch: Cemu can't know a priori whether the guest will later sample the depth buffer as a texture. Approach:

- Allocate depth textures Private by default (as now).
- Track, per `LatteTextureMtl`, whether the texture has *ever* been bound as a shader texture or read back. `LatteTexture` already has the hooks (`texture_setLatteTexture`, `texture_createReadback`).
- On the second and subsequent frames, for depth textures that have never been sampled, recreate them as `StorageModeMemoryless` and flip the render-pass to `LoadActionClear`/`StoreActionDontCare`.
- Conservative fallback: if such a texture is *ever* sampled, recreate it Private and clear the memoryless flag permanently (persist the decision per-texture-key across the session).

Expected on M2: for a 1080p-internal-res title, one 1920×1080 D32S8 = ~16 MB freed per depth target, plus its full write bandwidth every frame. On an 8 GB machine with graphics packs at 2× or 4× resolution this is a **large** win — a 4K depth buffer is 64 MB.

**Do the load/store-action audit as part of this** (it is cheap and independent): `CachedFBOMtl.cpp:19-46` unconditionally uses `LoadActionLoad`/`StoreActionStore` for *every* attachment. Latte tracks which attachments a pass clears (`texture_clearColorSlice`, `texture_clearDepthSlice`, `LatteDraw_handleSpecialState8_clearAsDepth`). Where the pass begins with a clear, use `LoadActionClear` and skip the separate clear pass entirely (`ClearColorTextureInternal`, `MetalRenderer.cpp:2238-2255`, currently spins up a whole temporary render encoder per clear — see `m_performanceMonitor.m_clears`). **On a TBDR GPU `LoadActionLoad` on an attachment you're about to fully overwrite is pure wasted bandwidth**, and it's happening on every attachment of every pass today.

Measure with: Metal System Trace / Instruments "Metal Application" template → look at tile-memory and system-memory bandwidth counters; and `m_performanceMonitor.m_clears` should drop toward zero.

### 4.2 Texture storage-mode policy — **S, but measure**

`MetalRenderer.h:387-390` has `GetOptimalTextureStorageMode()` (Apple → Shared) commented out. Uncommenting it and using Shared for *all* textures is **wrong** and I recommend against it: Shared textures on Apple GPUs are linear/untiled-accessible and lose the GPU-optimal tiling, hurting sampling throughput on frequently-sampled textures. The correct policy is:

- Render targets, and any texture sampled more than once: **Private** (as now).
- Textures uploaded once and never re-uploaded, and small textures (< 64 KB): **Shared**, and upload via `replaceRegion` on the CPU — which lets you take the dead fast path at `MetalRenderer.cpp:788-793` and **avoid breaking the render pass** (see 5.1).
- The imgui font/texture path (`MetalRenderer.cpp:621`) already uses `replaceRegion`; that's correct, delete its `// TODO: do a GPU copy?`.

Effort S once 5.1's plumbing exists. Payoff: fewer render-pass breaks, less staging traffic.

### 4.3 Lazy texture allocation — **M, do it**

`LatteTextureMtl.cpp:103-107`: `AllocateOnHost()` is a no-op; every `LatteTextureMtl` allocates its `MTL::Texture` eagerly in the constructor (`:85`). The only caller is `LatteTextureLoader.cpp:614`. On an 8 GB M2 running a texture-heavy title with graphics packs this is a real ceiling. Move `newTexture` out of the ctor into `AllocateOnHost()`, null-guard `GetTexture()`, and have `CreateView`/`GetRGBAView` force allocation. Verify with `GetVRAMInfo` (`MetalRenderer.cpp:346-353`) and `MTL::Device::currentAllocatedSize()` against `recommendedMaxWorkingSetSize()`.

### 4.4 `MTLHeap` for transient allocations — **defer / probably not worth it**

`MetalBufferChunkedHeap` (`MetalBufferAllocator.h:18-63`) already sub-allocates from a small number of large `MTL::Buffer`s, which captures most of the benefit. `MTLHeap` would additionally let you alias transient render targets, but Cemu's render targets are long-lived and content-addressed by guest address — aliasing is not applicable. **Recommend not doing this.**

### 4.5 Argument buffers — **not worth it. Skip.**

The theoretical win is CPU binding cost. But `BindStageResources` (`MetalRenderer.cpp:2200-2236`) already redundancy-filters every bind against `m_state.m_encoderState.m_buffers/m_textures/m_samplers`, and the binding counts are small (≤28 vertex buffers, ≤31 textures, ≤16 samplers). Converting to argument buffers would require changing the MSL emitter's binding model (`LatteDecompilerEmitMSLHeader.hpp`), invalidating every cached binary, plus adding explicit `useResource` residency calls that Metal currently handles automatically. **Large effort, speculative payoff, guaranteed to break the Phase-2 archive. Explicitly do not do this.**

### 4.6 `MTLResidencySet` — **not worth it. Skip.**

Verified it works with the Metal 3 `MTLCommandQueue` (`MTLCommandQueue.h:58-82`, macOS 15+). But residency sets only pay off when you have opted *out* of automatic residency tracking — i.e. argument buffers / bindless. Cemu binds resources explicitly, so Metal already tracks residency correctly and cheaply. Adding a residency set here is pure overhead. **Skip unless 4.5 is ever adopted, which it shouldn't be.**

---

## Phase 5 — Encoder and command-buffer architecture

### 5.1 Stop breaking the render pass on uploads — **M, high payoff**

The core structural problem: exactly one encoder is alive at a time (`m_encoderType`, `MetalRenderer.h:535`), and `GetBlitCommandEncoder()`/`GetComputeCommandEncoder()` (`:1836-1880`) call `EndEncoding()` on the current render encoder. So **every mid-frame texture upload, buffer-cache upload, readback, screenshot, or buffer-to-buffer copy tears down and rebuilds the render pass** — on a TBDR GPU that means a full tile flush + reload. `MetalMemoryManager::UploadToBufferCache` (`:101-112`) does this on the DevicePrivate path, which is the default for Apple GPUs.

Three candidate fixes; **recommend a combination of (a) and (b):**

**(a) A dedicated upload command buffer.** Keep a second `MTL::CommandBuffer` purely for blits, committed *before* the render CB each frame, ordered via the existing `MTL::Event`. This turns "N render-pass breaks" into "zero", at the cost of one extra CB per frame. This is also the natural consumer of `m_event` (`MetalRenderer.h:509`). **Correction (2026-07-31): the claim that `m_event` is "never waited on" was wrong.** `GetCommandBuffer` has done `encodeWait(m_event, m_eventValue)` on every command buffer since `26e40a4`, i.e. since the Metal backend landed — this doc was written against `b8f2cf4` and read only the signal site. So the primitive is not dead, it is a total order over every command buffer, and a dedicated upload CB would have to be threaded through that existing chain rather than adopting a spare event. ~~Note also `EVENT_VALUE_WRAP` is 4096 and the counter wraps every ~29 s at 7 buffers/frame, which breaks `MTLEvent`'s monotonicity contract; fix that before reasoning about the chain at all.~~ **Fixed in `839466d`** — monotonic `uint64`. Every `gpu.busy_ns` recorded before that commit is inflated (overlapping buffers were double-counted) and must not be compared across the boundary.

**Before proposing a dedicated upload/blit CB, read this:** the closest experiment has been run. Giving the *readback* blit its own command buffer, deliberately outside the `m_event` chain, was implemented and A/B'd at n=3 per arm (`b7c0367`). Every performance range overlapped between arms; only `gpu.command_buffers` separated, 7 → 9. Splitting work into more command buffers is not free and did not pay here — the GPU was the constraint, not the ordering. A dedicated upload CB is still worth trying for **(a)**'s actual stated purpose (eliminating render-pass teardown), but do not expect the *ordering* half to buy anything.

**(b) Batch uploads at pass boundaries.** Latte's upload calls arrive in bursts. Queue `texture_loadSlice`/`bufferCache_upload` requests into a list, and flush the whole list to a single blit encoder at the next natural pass boundary (`GetRenderCommandEncoder` with a real FBO change, or `draw_endSequence`). Requires care: an upload must be flushed before the first draw that reads it. Track a per-texture/per-range "dirty, pending upload" bit and force a flush on first use.

**(c) Buffer-cache mode `Host`.** `MetalMemoryManager.cpp:67-79` already imports the whole MEM2 range as a Shared `MTL::Buffer` — the true zero-copy unified-memory path, with *no* uploads at all. It's currently opt-in and `Auto` picks `DevicePrivate` on Apple GPUs (`:41-58`). **Worth re-benchmarking on M2 now that the heap corruption from F1 is fixed** — that corruption may well be why `Host` mode was found unreliable. If `Host` is stable, making it the `Auto` default on Apple Silicon eliminates the entire buffer-upload path and much of 5.1's motivation. **Test this before investing in (a)/(b).** It's a one-line config experiment with potentially the largest payoff in the phase.

Measure with `m_performanceMonitor.m_renderPasses` per frame (add a counter for "passes ended by encoder-type switch" specifically) and GPU-time in Metal System Trace.

### 5.2 Replace polled completion with `addCompletedHandler` — **S**

`MetalRenderer.cpp:1913-1918` has `addCompletedHandler` commented out with *"it seems like Metal doesn't always call the completion handler"*; `ProcessFinishedCommandBuffers()` (`:1935-1952`) polls `commandBuffer->status()` instead, and is only called from `CommitCommandBuffer()` — so **staging-buffer memory is only reclaimed when a new CB is committed.** If the emulator idles (paused, or a low-drawcall menu), staging buffers are never freed.

Metal does reliably call completion handlers. The reported unreliability is almost certainly one of: (i) the block captured `commandBuffer` by value in a way that raced with the `m_currentCommandBuffer = {mtlCommandBuffer}` reassignment (note the commented code references `commandBuffer.m_commandBuffer`, a stale local); (ii) handlers firing on a Metal-internal thread while `MetalMemoryManager`/`MetalSynchronizedRingAllocator` are not thread-safe; or (iii) the F1 heap corruption.

**Recommendation:** re-enable `addCompletedHandler`, but have the handler do nothing except push the command buffer onto a lock-protected "completed" queue. Drain that queue on the Latte thread from `ProcessFinishedCommandBuffers()`. This gets prompt reclamation with no cross-thread mutation of the allocators. Keep the `status()` poll as a belt-and-braces fallback in the same function. Verify: pause the emulator mid-frame and confirm `MetalBufferChunkedHeap::GetStats` free bytes recover.

### 5.3 Revive `NotifyLatteCommandProcessorIdle` — **S**

~~`MetalRenderer.cpp:540-544` is an empty override; Vulkan commits opportunistically when the command processor stalls. Restore it: `if (m_recordedDrawcalls > 0) CommitCommandBuffer();`~~ **Retired — the recommendation was wrong on both counts.** Vulkan did *not* commit on every idle: `NotifyLatteCommandProcessorIdle` was gated on `m_submitOnIdle`, armed only by a pending readback or occlusion query and cleared at every submit (`084b514:VulkanRenderer.cpp:2274,3059-3071`). And an unconditional commit-on-idle would fire ~129,000 times a frame, because the notification sits inside the ring-starvation spin. Implemented and measured behind `--commit-on-fence-stall` (`064d9a7`): work held at idle drops 85% and **nothing else moves at all** — frame time, fps, `cp_fence` and GPU busy are flat. Left off by default.

### 5.4 Readback path — **S**

`LatteTextureReadbackMtl.cpp:30-32`: `RequestSoonCommit()` is commented out and the code calls `CommitCommandBuffer()` unconditionally, forcing a full flush on every readback. Switch to `RequestSoonCommit()` (sets the threshold to `drawcalls + 8`), which lets a few more draws batch in before the flush. `ForceFinish()`'s `waitUntilCompleted()` is unavoidable — readback is inherently synchronous in Cemu's design. **Expect no frame-rate effect:** the readback path's cost has since been measured three ways (start delay, position within the command buffer, and inter-buffer ordering) and none of them is what the drain waits for — see `00-master-plan.md`, "the readbacks are real". Do this one for tidiness, not for performance. Also uncomment the `hasReadback` term in `draw_endSequence` (`MetalRenderer.cpp:1479`) once this is in.

### 5.5 Occlusion queries — **S, low priority**

`CachedFBOMtl.cpp:58` sets a `visibilityResultBuffer` on **every** FBO's render-pass descriptor, whether or not queries are active. This forces the driver to allocate visibility-result storage for every pass. Set it conditionally (only when `m_occlusionQuery.m_active`), which means the render pass descriptor can no longer be fully cached in the FBO ctor — mutate it in `GetRenderCommandEncoder`. Also `MetalQuery.h:26` tracks only one command buffer per query; if a query spans a commit boundary the earlier CB's results are lost. Low frequency; note it and move on.

---

## Phase 6 — Presentation and frame pacing

### 6.1 Fix the per-frame `pixelFormat` mutation — **S, do this first in the phase**

`MetalRenderer.cpp:1954-1968`: `AcquireDrawable()` calls `layer->setPixelFormat(BGRA8Unorm_sRGB | BGRA8Unorm)` whenever `LatteGPUState.tvBufferUsesSRGB` flips. Mutating a live `CAMetalLayer`'s pixel format mid-flight forces the layer to tear down and reallocate its drawable pool — a hitch every time a game toggles sRGB, and it can hand you a drawable whose format doesn't match your pipeline.

**Fix:** set the layer to a fixed `MTL::PixelFormatBGRA8Unorm` once at `InitializeLayer` (`:551-556`, already does this), add `MTL::TextureUsagePixelFormatView` semantics by leaving `framebufferOnly` as-is *unless* views are needed, and instead select the sRGB behaviour **in the output pipeline**: `MetalOutputShaderCache` already keys pipelines on `usesSRGB` (`MetalOutputShaderCache.cpp:15,27`) — have the sRGB variant do the encode in the fragment shader and write to a linear `BGRA8Unorm` drawable. This removes the layer mutation entirely and is strictly more correct.

If you prefer a texture view instead: `framebufferOnly` must be set to `false` on the layer (`MetalLayerHandle.cpp:13`) to create a `PixelFormatView` of the drawable texture — that costs some compositor optimization. **Prefer the shader-side approach.**

### 6.2 Wire `displaySyncEnabled` so the vsync setting stops being a no-op — **S**

`grep vsync` in the Metal directory returns nothing. `CAMetalLayer.displaySyncEnabled` is never set (defaults to `YES`), so vsync is always on and the Off/On control in `GeneralSettings2.cpp:1817-1820` does nothing. Set `m_layer->setDisplaySyncEnabled(GetConfig().vsync.GetValue() != 0)` in `MetalLayerHandle`'s ctor and on config change. Available in the bumped metal-cpp (`CAMetalLayer.hpp:63`).

### 6.3 `maximumDrawableCount` — **S**

Never set; defaults to 3. Expose 2 (lower latency, more stalls) vs 3 (smoother) as a setting, or hard-set 3. Note that with `displaySyncEnabled = NO`, 2 is generally better. Available at `CAMetalLayer.hpp:60`.

### 6.4 `presentDrawable:afterMinimumDuration:` — **S, recommended**

`MetalLayerHandle.cpp:44` uses plain `presentDrawable:`. The SDK header (`MTLCommandBuffer.h:329-336`) notes the `afterMinimumDuration:` variant *"defers calculation of the presentation time until the previous frame's actual presentation time is known, thus to be able to maintain a more consistent and stable frame time."* That is exactly the frame-pacing problem an emulator has. Pass `1.0 / targetFPS` derived from the guest's vsync interval (`LatteTime_CalculateTimeBetweenVSync`, `LatteTiming.cpp:17-33`, honouring `s_customVsyncFrequency`). Low risk, direct improvement to judder.

### 6.5 ProMotion / `CAFrameRateRange` — **M, conditional**

`CAMetalLayer` has no `preferredFrameRateRange` on macOS. The mechanism is `CAMetalDisplayLink` (macOS 14+, `QuartzCore/CAMetalDisplayLink.h:47`), which is **not exposed by metal-cpp** — needs an ObjC `.mm` shim alongside `MetalLayer.mm`.

**Recommendation: do the cheap part, defer the expensive part.** The cheap part: query `NSScreen.maximumFramesPerSecond` / `minimumRefreshInterval` / `maximumRefreshInterval` (`AppKit/NSScreen.h:100-119`) via a small `.mm` helper, and use it to (a) inform 6.4's minimum duration, and (b) tell `LatteTiming` whether the host can actually hit the guest's rate. Your built-in M2 display is 60 Hz fixed, so the ProMotion path is untestable on the dev machine — building a `CAMetalDisplayLink` driver you can't verify is a trap. Defer until an external VRR display is available.

The right long-term architecture: `CAMetalDisplayLink` replaces the deleted Windows-only `VsyncDriver` and calls `LatteTiming_NotifyHostVSync()` (`LatteTiming.cpp:56-63,127-165`), restoring host-driven vsync on macOS. Note this cleanly: it's the correct design, just not now.

### 6.6 EDR / HDR — **explicitly not worth doing**

`wantsExtendedDynamicRangeContent` is available (`CAMetalLayer.hpp:72`), but the Wii U's framebuffer is 8-bit SDR sRGB. There is no HDR signal to recover. You'd be inventing tone-mapping, which belongs in a graphics pack, not the renderer. **Skip.** (`colorspace` is worth setting to an explicit `CGColorSpaceCreateWithName(kCGColorSpaceSRGB)` for color-management correctness — that's a one-liner and worth doing, but it isn't HDR.)

---

## Phase 7 — MetalFX

Confirmed from the docs:

- **`MTLFXSpatialScaler`** — inputs: `colorTexture` only, plus `colorProcessingMode` (`.linear` / `.perceptual` / `.hdr`). No motion vectors, no depth. Encoded as a standalone pass: `scaler.encode(commandBuffer:)`.
- **`MTLFXTemporalScaler`** — requires `motionTexture`, `depthTexture`, `jitterOffsetX/Y`, `motionVectorScaleX/Y`, `isDepthReversed`.
- **`MTLFXFrameInterpolator`** — requires `motionTexture`, `depthTexture`, `viewToClipMatrix`, `worldToViewMatrix`, `nearPlane`/`farPlane`/`fieldOfView`.

**Recommendation: adopt spatial only, as an optional upscaling filter. Reject temporal and frame interpolation outright.**

Temporal/interpolation need per-pixel motion vectors, a camera matrix, and jitter control. A Wii U emulator has none of these — the guest's shaders are arbitrary R700 bytecode; there is no semantic "camera" and no way to synthesize meaningful motion vectors. Attempting it would produce smearing artifacts and is a **trap**. The `worldToViewMatrix`/`fieldOfView` requirements make this unambiguous.

**Where spatial plugs in.** `MetalRenderer::DrawBackbufferQuad` (`MetalRenderer.cpp:463-521`) currently runs one of six hand-written output pipelines (copy / bicubic / hermite × upside-down) selected in `RendererOuputShader.cpp:493-513` and cached in the 12-entry `MetalOutputShaderCache`. Add a seventh mode:

1. `EndEncoding()` first — `MTLFXSpatialScaler::encode` takes a command buffer, not an encoder, and creates its own passes.
2. Scale `presentTexture` (the Latte RT view) into an intermediate `MTL::Texture` at drawable size, `colorProcessingMode = .perceptual` (the Wii U output is sRGB-encoded, non-linear).
3. Then run the existing trivial copy pipeline from that intermediate into the drawable, so imgui overlay and the sRGB handling from 6.1 continue to work unchanged.

Gate on `MTLFXSpatialScalerDescriptor::supportsDevice(m_device)`. Recreate the scaler only when input or output dimensions change (creation is expensive). Expose as a new entry in the existing upscale-filter setting alongside Bilinear/Bicubic/Hermite.

Effort **M** (needs the MetalFX framework linked — currently only `-framework Metal` and `-framework QuartzCore` at `src/Cafe/CMakeLists.txt:615-618` — plus metal-cpp's `MetalFX/` headers from the 0.4 bump). Payoff: noticeably better upscaling than the hand-written bicubic when running below native resolution, which on an 8 GB / 8-core M2 is the common case for demanding titles. Verify: side-by-side screenshots at 720p→1440p against the bicubic path.

---

## Phase 8 — Metal 4: evaluate, don't commit

**Recommendation: do not migrate the renderer to Metal 4 in this workstream. Metal 3 plus the targeted fixes above captures nearly all the win.**

Reasoning, weighed item by item:

| Metal 4 feature | Value here | Verdict |
|---|---|---|
| Explicit barrier model (`MTLStages`, stage-to-stage barriers) | This is the genuinely attractive one — the renderer's biggest correctness gap (item 3.1) is a missing barrier mechanism. | **But item 3.1's real problem is intra-render-pass attachment self-dependency, which Metal 4 barriers do *not* solve either** — that still requires a pass split or framebuffer fetch, exactly as in Metal 3. Metal 4 barriers help *between* encoders, which is where Metal 3's automatic tracking already works correctly. **Little net gain.** |
| `MTL4ArgumentTable` | Mandatory for MTL4 encoders. Would require rewriting `BindStageResources`, `SetBuffer`/`SetTexture`/`SetSamplerState` (`MetalRenderer.cpp:1600-1712`) and possibly the MSL emitter's binding model. | **Large cost, no benefit** at Cemu's binding counts. |
| `MTLResidencySet` | Becomes mandatory under MTL4 (no automatic residency). | Pure new cost, per 4.6. |
| `MTL4CommandQueue`/`CommandBuffer`/`CommandAllocator` | Parallel encoding across threads. Cemu has exactly one render thread (`LatteThread`). | **No benefit.** |
| Unified compute encoder (blit + compute in one) | Would help item 5.1's encoder thrashing. | Real but modest; 5.1's fixes get most of it. |
| Color-attachment mapping (`setColorAttachmentMap`) | Would let one PSO serve multiple attachment layouts, reducing the pipeline permutation explosion in `MetalPipelineCache`. | Genuinely interesting for an emulator, but not worth the whole migration. |
| **`MTL4Compiler` + `MTL4PipelineDataSetSerializer`** | QoS-aware compilation queues (directly relevant to item 10 and the QoS workstream), flexible/unspecialized pipeline states, and streamlined harvesting. Apple explicitly calls compilation *"perhaps the easiest first step"* and says it's adoptable **independently** of MTL4 command encoding. | **The one piece worth a spike.** |

**Blocker status on metal-cpp:** *not* blocked at HEAD — `2948dd1e` has all 30 `MTL4*.hpp` headers, `MTLResidencySet.hpp`, and `MTL4FX*` — but it *is* blocked at the currently-pinned `a63bd172`. So the 0.4 bump is the gate.

**Concrete Metal 4 action item — one timeboxed spike (S), after Phase 2 ships:**

Determine empirically whether an `MTLRenderPipelineState` produced by `MTL4Compiler::newRenderPipelineStateWithDescriptor:` (which returns a plain `id<MTLRenderPipelineState>`, per `MTL4Compiler.h:170`) is usable from a **classic** `MTLRenderCommandEncoder`. Apple documents neither compatibility nor incompatibility. If it *is* compatible, you can adopt `MTL4Compiler` purely as a compilation backend — gaining QoS-aware scheduling and `MTL4PipelineDataSetSerializer` harvesting — with zero changes to command encoding. That would be a clean, contained upgrade to Phase 2. If it is *not*, stay on `MTLBinaryArchive` (which Apple confirms both Metal 3 and 4 can load) and revisit only if you ever migrate encoding.

Timebox: one day. Do not let it grow.

---

## Phase 9 — Shader compile throughput

There are **three separate** thread pools with three different, uncoordinated sizing policies:

| Pool | Where | Count | Priority |
|---|---|---|---|
| MSL → `MTL::Function` | `RendererShaderMtl.cpp:31-35` | hardcoded **2** | default |
| Metal pipeline compile (runtime) | `MetalPipelineCache.cpp:52-66` | `2 + (cores-3)`, cap 8 | `// TODO: set thread priority` (`:31`) |
| Pipeline cache preload | `MetalPipelineCache.cpp:280-291` | `clamp(cores, 1, 8)` | default |

On your 4P+4E M2, `GetPhysicalCoreCount()` returns 8, so pool 2 = 7 threads and pool 3 = 8 threads, plus 2 from pool 1 — up to 17 compile threads competing with the guest PPC threads and the Latte thread. That's oversubscription, and with no QoS annotations the scheduler will happily put emulation work on E-cores while compilers monopolize P-cores.

**Changes (S each, coordinate with the QoS workstream owner):**

1. **Unify the sizing.** One helper that reports P-core and E-core counts (`sysctlbyname("hw.perflevel0.physicalcpu")` / `hw.perflevel1.physicalcpu`), and size all three pools from it. On an M2: total compile threads ≈ 4 (E-core count), never more than 6.
2. **Set QoS, don't set POSIX priority.** Replace the `// TODO: set thread priority` at `MetalPipelineCache.cpp:31` and the dead `SCHED_FIFO`/`SCHED_RR` block at `RendererShaderMtl.cpp:41-50` (which is wrong anyway — `SCHED_FIFO` on a compile thread is actively harmful) with `pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0)`. **`QOS_CLASS_UTILITY`, not `BACKGROUND`** — background QoS on macOS gets aggressively deprioritized and throttled onto E-cores with I/O throttling, which would starve compilation exactly when you need it. The exception is the *shader-cache preload* pool during the loading screen, which should be `QOS_CLASS_USER_INITIATED` since nothing else is running.
3. **Raise the MSL pool from 2.** Two threads is the current bottleneck — MSL→AIR frontend compilation is the expensive half, and it feeds the pipeline pool. Set it to the same P-core-informed count as the others.
4. **Priority-inversion safety:** when `PreponeCompilation(isRenderThread=true)` (`RendererShaderMtl.cpp:234-258`) blocks the render thread on `waitUntilValue(DONE)`, the compiling thread is at UTILITY QoS while the waiter is at USER_INTERACTIVE. macOS's automatic priority donation handles this for `pthread_mutex`/`dispatch_sync` but **not** for a hand-rolled `CounterSemaphore`/condvar. Verify `m_compilationState.waitUntilValue`'s implementation; if it doesn't use a donating primitive, either switch it to one or temporarily bump the target thread's QoS. This is a real stutter source and is easy to miss.
5. **`SetShouldMaximizeConcurrentCompilation`** (`MetalRenderer.h:270-274`) is toggled on around cache load and off after (`RendererShaderMtl.cpp:179,187`). That's correct; leave it. Note that under Metal 4 this is superseded by `MTL4Compiler`'s QoS model — another argument for the Phase-8 spike.
6. **`IsAsyncPipelineAllowed` heuristics** (`MetalPipelineCache.cpp:79-92`): the Splatoon 1600×1600 and `indexCount <= 6` blacklists are load-bearing hacks that prevent permanently-corrupted persistent textures. **Leave them alone.** Once Phase 2's binary archive is warm, async compilation matters far less anyway — most pipelines come back instantly. **Do not spend effort tuning these.**

Verification for the whole phase: measure time-to-first-frame and the `g_compiling_pipelines_syncTimeSum` counter (`MetalPipelineCompiler.cpp:388`) with a cold cache; measure frame-time percentiles (p99) during first traversal of a new BotW region with a warm archive.

---

## Risk register

**Most likely to regress visually, in order:**

| Risk | Source | Detection |
|---|---|---|
| Wrong MSL emitted after the resource-mapping collapse | 1.5 — three interleaved binding tables, one shared analyzer | **Byte-diff generated MSL** for every golden scene before/after. Dump `m_mslCode` to disk behind a debug flag before `FinishCompilation()` clears it (`RendererShaderMtl.cpp:403-407`). This is the single most important verification gate in the plan. |
| Over-aggressive render-pass splitting or missed splits | 3.1 | BotW lava/waterfall; `m_performanceMonitor.m_renderPasses` delta; Metal validation layer |
| Memoryless depth applied to a depth buffer that *is* later sampled | 4.1 | Shadow-map-heavy scenes (Xenoblade X, Wind Waker HD). The recreate-on-first-sample fallback must be exercised deliberately in a test. |
| Format-table changes flipping a working fallback into a wrong-but-plausible format | 3.3 | The `logOnce` on unknown formats, added first, gives you the actual list before you change anything |
| sRGB handling change | 6.1 | Golden-scene screenshots specifically in sRGB-on and sRGB-off titles; compare gamma with a histogram, not by eye |
| Binary archive serving a stale binary after an MSL-emitter change | 2 | Key the archive filename on `LatteShaderCache_getPipelineCacheExtraVersion`; add a startup assert that the archive's key matches |
| Completion-handler race on staging buffers | 5.2 | ASan + a soak test with the emulator paused/resumed repeatedly |

**Tooling, in order of usefulness:**

1. **Golden-scene screenshot diffs** (Phase 0.5) — cheap, automatable via `HandleScreenshotRequest`, catches everything above. Build this before touching anything else.
2. **Metal validation layer + shader validation** — catches load/store mismatches, unbound resources, out-of-bounds (will find the XFB 4× hack's real cause).
3. **GPU capture** (`MetalRenderer.cpp:2307-2361`, already wired to a MainWindow menu item) — for anything the first two flag but don't explain.
4. **Metal System Trace / Instruments** — for Phase 4 and 5 bandwidth and GPU-time claims. Do not accept a TBDR optimization without a before/after bandwidth number.
5. **Metal Performance HUD** (`MTL_HUD_ENABLED=1`) — free, always-on frame-time and memory overlay; useful during Phase 6.

**One process rule worth stating:** every phase from 3 onward that touches `LatteDecompilerEmitMSL.cpp` invalidates the Phase-2 binary archive for all users. Batch MSL-emitter changes together and bump the cache version once per release, not once per fix.

---

## Recommended ordering (payoff / risk)

```
0.1  ctor array overflow ....................... S  CRITICAL, blocks everything
0.4  metal-cpp bump to 2948dd1e ................ S  blocks 3.2, 4.x, 6.x, 7, 8
0.2  0.3  device retain / null guards .......... S
0.5  validation + golden scenes ................ S  blocks all verification
1.1  kDefaultGraphicsAPI = kMetal .............. S  makes Metal the tested path
3.2  LOD bias (new API in macOS 26) ............ S  correctness, near-zero risk
3.6  drop mesh-shader fallbacks ................ S  removes dead hot-path branches
2    MTLBinaryArchive shader cache ............. M  largest user-visible win
5.1c re-test Host buffer-cache mode ............ S  may obviate much of 5.1
6.1  6.2  6.3  6.4  presentation fixes ......... S  vsync stops being a no-op
1.2-1.6  delete GL + Vulkan .................... M  1.5 is the risky one
5.2  5.3  5.4  completion handlers / idle ...... S
3.3  format table (log first, then fix D24S8) .. M  D24S8 path is live on all M-series
3.1  render-pass self-dependency ............... L  largest correctness win
4.1  memoryless depth + load/store audit ....... M  largest TBDR perf win
3.4  framebuffer-fetch stubs ................... M
9    compile threading + QoS ................... S  coordinate with QoS owner
4.3  lazy texture allocation ................... M  matters on 8 GB
3.5  XFB 4x + Smash dummy attachment ........... M  try the renderTargetWidth fix first
5.1a 5.1b  upload batching / upload CB ......... M  only if Host mode fails
7    MetalFX spatial ........................... M
4.2  storage-mode policy ....................... S
3.8  mesh-shader occupancy ..................... M  measure first; may be skippable
8    MTL4Compiler spike ........................ S  timeboxed, one day
6.5  ProMotion / CAMetalDisplayLink ............ M  defer, untestable on this Mac

NOT WORTH DOING: 4.4 MTLHeap · 4.5 argument buffers · 4.6 MTLResidencySet ·
                 6.6 EDR/HDR · MetalFX temporal + frame interpolation ·
                 RAM-disk AIR cache revival · offline shader precompilation ·
                 tuning IsAsyncPipelineAllowed · investigating the BC1-BC5 "correct?" TODOs ·
                 full Metal 4 command-encoding migration
```

---

### Critical Files for Implementation

- `/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp` — the ctor overflow (`:240-243`), encoder/commit architecture (`:1715-1952`), presentation (`:1954-1968`, `:2289-2295`), dead fast paths (`:773-810`, `:2257-2287`)
- `/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h` — the array-bound mismatch (`:117` vs `:41`), device/CB ownership (`:288-295`, `:538-540`), XFB hack (`:419`), `GetOptimalTextureStorageMode` (`:387-390`)
- `/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.cpp` + `/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCompiler.cpp` — home of the `MTLBinaryArchive` cache (`MetalPipelineCompiler.cpp:268-285` scaffolding, `:346-375` pipeline creation), compile-thread sizing/QoS
- `/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerAnalyzer.cpp` — the three interleaved `resourceMappingGL/VK/MTL` tables (`:490-660`); the riskiest edit in the GL/VK deletion
- `/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Latte/Renderer/Vulkan/CachedFBOVk.cpp` + `/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRendererCore.cpp` — **read before deleting**: `CheckForSelfDependency` (`CachedFBOVk.cpp:198-240`) and its consumer (`VulkanRendererCore.cpp:1187-1222`) are the reference design for the Metal barrier mechanism