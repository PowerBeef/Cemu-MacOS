# 03 — Latte: the GPU

Latte is an AMD **R700 / TeraScale** derivative, heavily customised, fabricated by TSMC on 40 nm.
Nintendo calls the hardware **GPU7** and the API **GX2**; the "7" is assumed to come from R7xx
`[RE]`. The die also carries the entire Wii GPU (Hollywood) as a separate block, plus 32 MB of
eDRAM (MEM1) and the audio DSP.

| | Value | Source |
|---|---|---|
| Architecture | R700 / TeraScale, unified shader model | `[RE]` |
| Clock | **549.999775 MHz** | `[RE]` |
| Process / die | 40 nm TSMC, 11.88 × 12.33 mm = 146.48 mm² | `[RE]` |
| Shader ALUs | **160 or 320** | `[CONFLICT]` |
| eDRAM on die | 32 MB (MEM1), 40.72 mm² = 27.8% of die | `[RE]` |
| Texture units | 2 pipes, 8 KB L1 each, 32 KB shared L2 | `[RE]` |
| MSAA | up to 16× | `[RE]` |
| Register MMIO | `0x0C200000`, length `0x80000`, big-endian; userspace `0xFC200000` | `[RE]` |
| Vsync | 59.94 Hz | `[SRC LatteTiming.cpp:16-32]` |

> **The ALU count is genuinely disputed.** One reading of the die shot gives "160 ALUs in 40 clusters
> of 4"; another gives "two blocks of 16 SIMD units, each ALU containing four sub-ALUs" — which is
> 320. No first-party figure exists and the die is visibly customised relative to any shipped R700
> part. Both are recorded; neither is asserted. It does not affect emulation, since we translate
> shaders rather than model execution units.

Feature level maps to roughly **OpenGL 3.3 / Shader Model 4** — vertex, geometry and pixel shaders,
no tessellation, no OpenCL `[RE]`.

## What the emulator actually models

Almost none of the above. We do not simulate the GPU; we **translate its command stream and shader
ISA** onto Metal. So the parts of Latte that matter here are:

1. The **register file** — a 64K-dword address space that PM4 packets write into.
2. The **shader ISA** — R700 CF/ALU/TEX/VTX instruction words, decompiled to MSL.
3. The **memory tiling scheme** — how surfaces are laid out, which we must reproduce exactly.
4. The **hardware limits** — how many textures, samplers, targets a shader may reference.

## The register file

`LatteGPUState` holds a `uint32 contextRegister[0x10000]` unioned with a typed
`LatteContextRegister` `[SRC Latte.h:17-69]`, so a register is addressable either as a raw dword
index or as a named bitfield struct.

Register space is divided into windows, each with its own PM4 `SET`/`LOAD` opcode
`[SRC LattePM4.h:36-41]`:

| Window | Base | Contents |
|---|---|---|
| CONFIG | `0x2000` | `VGT_PRIMITIVE_TYPE`, global GPU config |
| CONTEXT | `0xA000` | the bulk of render state — blend, depth, raster, targets |
| ALU_CONST | `0xC000` | shader uniform constants |
| RESOURCE | `0xE000` | texture and vertex-buffer descriptors, 7 dwords each |
| SAMPLER | `0xF000` | sampler descriptors, 3 dwords each |
| LOOP_CONST | `0xF880` | loop bounds |

Two headers describe the same file from opposite ends:

- **`ISA/LatteReg.h`** (1692 lines) — the modern typed model. ~60 `LATTE_*` bitfield structs, each
  annotated with its offset (`LATTE_DB_DEPTH_CONTROL // 0xA200`,
  `LATTE_SQ_TEX_RESOURCE_WORD0_N // 0xE000 + index * 7`,
  `LATTE_SQ_TEX_SAMPLER_WORD0_0 // 0xF000 + n*3`). Ends with `struct LatteContextRegister`, a
  byte-exact shadow with offsets in comments.
- **`ISA/RegDefines.h`** (328 lines) — legacy `mm*` defines, marked superseded. Still the quickest
  way to find an offset. Notable: `mmSQ_VTX_BASE_VTX_LOC 0xF3FC` (baseVertex) and
  `mmSQ_VTX_START_INST_LOC 0xF3FD` (baseInstance).

## Hardware limits

`[SRC LatteConst.h:8-19]` and `[SRC LatteReg.h:353-359]`:

| Limit | Value | Note |
|---|---|---|
| GPRs per shader | 128 | |
| **Texture units per stage** | **18** | Source comment: *"this might be higher than 18? BotW is the only game which uses more than 16?"* |
| **Samplers per stage** | **18** | Confirmed by `[HW]`: SDK 2.09.07 raised `GX2_MAX_SAMPLERS` "from 16 to 18 to reflect the actual hardware limit" |
| Uniform buffers per stage | 16 | |
| Colour targets | 8 | |
| Vertex buffers | 16 | |
| Streamout buffers | 4 | |
| VS attributes | 32 | `// todo: verify` |
| Attribute locations | 256 | `// should this be 128 since there are only 128 GPRs?` |
| Register space | `0x10000` dwords | `LATTE_MAX_REGISTER` |

The sampler count is a good example of `[RE]` guesswork being settled by `[HW]`: the source comment
in `LatteReg.h:355` asks "is this 16 or 18?", and the SDK release notes answer it — 18, and the
early SDK's 16 was the bug.

Cemu adds its own texture-unit numbering on top, since Metal has one flat binding space per stage:
`LATTE_CEMU_PS_TEX_UNIT_BASE 0`, `VS 32`, `GS 64` `[SRC LatteConst.h:22-24]`.

## Surface tiling

The single most exacting part of Latte emulation: a surface's bytes must be laid out exactly as
hardware would, or textures decode to garbage. Parameters `[SRC LatteAddrLib.h:6-16]`:

```cpp
m_banks                = 4;      m_banksBitcount              = 2;
m_pipes                = 2;      m_pipesBitcount              = 1;
m_pipeInterleaveBytes  = 256;    m_pipeInterleaveBytesBitcount = 8;
m_rowSize              = 2048;
m_swapSize             = 256;
m_splitSize            = 2048;
m_chipFamily           = 2;
```

The scheme, from the source's own notes `[SRC LatteAddrLib.cpp:6-23]`:

- A **micro-tile** is 8 × 8 texels.
- A **macro-tile** holds one micro-tile per bank/pipe combination — 4 banks × 2 pipes = 8, arranged
  1×8, 2×4 or 4×2 depending on mode.
- Address bit layout is `.... aaaaabbc aaaaaaaa`.
- Swizzle equations: `channel0 = x[3] ^ y[3]`, `bank0 = x[3] ^ y[5]`, `bank1 = x[4] ^ y[4]`.

`E_HWTILEMODE` and `E_GX2TILEMODE` enumerate the modes `[SRC LatteReg.h:26, :51]`. This is a direct
port of AMD's AddrLib and should not be "cleaned up" — it is bit-exact by necessity.

## Formats

`Latte::E_GX2SURFFMT` encodes a GX2 format as a hardware base format plus attribute bits
`[SRC LatteReg.h:166-309]`:

```
E_GX2SURFFMT = HW format | FMT_BIT_INT(0x100) | FMT_BIT_SIGNED(0x200)
                         | FMT_BIT_SRGB(0x400) | FMT_BIT_FLOAT(0x800)
```

Base formats run `0x00`–`0x30` for uncompressed, with `HWFMT_BC1..BC5 = 0x31..0x35` for block
compression. Vertex formats are a separate `FMT_*` list `[SRC LatteConst.h:28-72]`.

> **Accuracy gap — `D24_S8`.** `depth24Stencil8PixelFormatSupported` is **false on all Apple
> Silicon**, so `D24_S8_UNORM` is remapped to `Depth32Float_Stencil8` — but the decoder that would
> convert the data is commented out:
> ```cpp
> MTL_DEPTH_FORMAT_TABLE[D24_S8_UNORM].pixelFormat = MTL::PixelFormatDepth32Float_Stencil8;
> // TODO: implement the decoder
> //MTL_DEPTH_FORMAT_TABLE[D24_S8_UNORM].textureDecoder = TextureDecoder_D24_S8_To_D32_S8::getInstance();
> ```
> `[SRC LatteToMtl.cpp:173-179]`. This is a live corruption path on every M-series Mac, taken
> unconditionally. → chapter 09.

## Shader ISA

R700 instruction words, decoded in `ISA/LatteInstructions.h` (924 lines): control-flow (CF), ALU,
TEX and VTX clauses. Opcode constants are in `Core/LatteShaderAssembly.h` with a `GPU7_` prefix —
`CF_INST_TEX 0x01`, `ALU 0x08|mask`, `CALL_FS 0x13`, `EMIT_VERTEX 0x15`, `MEM_STREAM0_WRITE 0x20`,
`MEM_RING_WRITE 0x26`, `EXPORT 0x27`, `EXPORT_DONE 0x28`.

Shaders are decompiled to **MSL source text** and compiled at runtime. There is no compiled-shader
disk cache on this fork, so **every shader recompiles on every launch** — which is why
`testing/drive-botw.sh` needs a 90-second settle before measuring anything.

Geometry shaders are emulated with Metal mesh shaders or a vertex-shader ring-buffer scheme; the
`RECTS` primitive has no Metal equivalent. Both have gaps — see `[SRC ../porting/03-graphics-metal.md]`.

## The vWii block

The die contains the complete Wii GPU (Hollywood), including its fixed-function TEV pipeline, active
only in vWii mode, where it addresses MEM0's 3 MB of 1T-SRAM `[RE]`. Entirely out of scope: this
emulator does not implement Wii mode.

## Performance behaviour worth knowing

From the Cafe SDK release notes `[HW]` — real hardware characteristics that shape how Wii U titles
are written, and therefore what our command streams look like:

- **Clipped primitives cost 24×–170× more** than unclipped ones, depending on how many clip planes
  are crossed. The SDK adjusted guard-band setup specifically to reduce this.
- **Vertex reuse buffer holds 14 vertices** (raised from 6 in SDK 1.7). Primitives that re-reference
  a vertex within that window skip re-shading.
- **Quad pipe mode defaults to 2 pipes, not 3**, for stability; some geometry-shader workloads hang
  in 3-pipe mode on pre-CAT-DEV V4 hardware.
- **Z-only rendering still requires `GX2SetColorBuffer` on target 0** to configure AA registers, even
  with no colour output.
- Shaders exceeding **10 vertex/geometry outputs** trigger a compiler warning and a documented
  hardware performance bug.
- **`GX2SetTVGamma` is clamped to [0.7, 1.3]**.

## Modelled / Approximated / Absent

| Behaviour | Status | Note |
|---|---|---|
| Register file (64K dwords) | **Modelled** | Full shadow, typed accessors |
| PM4 register writes | **Modelled** | All `SET`/`LOAD` windows |
| Shader ISA decode | **Modelled** | CF/ALU/TEX/VTX → MSL |
| Surface tiling / AddrLib | **Modelled** | Bit-exact port |
| Texture formats | **Approximated** | `D24_S8` decoder missing → ch. 09 |
| Texture/sampler limits (18) | **Modelled** | |
| Geometry shaders | **Approximated** | Mesh-shader or ring-buffer emulation |
| `RECTS` primitive | **Absent** | Silently dropped → ch. 09 |
| MSAA | **Approximated** | |
| Occlusion queries | **Modelled** | `IT_HLE_BEGIN/END_OCCLUSION_QUERY` |
| Streamout | **Modelled** | 4 buffers |
| Render-pass self-dependency | **Absent** | Vulkan design preserved, not ported → ch. 09 |
| Shader compile cache (disk) | **Absent** | Every shader recompiles per launch |
| GPU clock / ALU throughput | **Absent** | Not simulated — we run on the host GPU |
| eDRAM (MEM1) as fast memory | **Absent** | No bandwidth distinction (ch. 02) |
| GPU perf counters (`GX2Sample*GPUCycle`) | **Absent** | See ch. 04 |
| vWii / Hollywood block | **Absent** | Out of scope |
