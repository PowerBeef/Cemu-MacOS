# Graphics tests — render-pass self-dependency

Draws into a colour buffer while sampling that same surface as a texture. This is the case
`docs/porting/03-graphics-metal.md` item 3.1 is about, and the case the `acc.self_dep_*` counters
were added to size.

## Status: reproduces — every self-dependency here is rewritten to a framebuffer fetch

**Measured 2026-08-04**, n=1, 1,426 frames.

| counter | per frame | total |
|---|---|---|
| `acc.self_dep_fbfetch` | **1.0** | 1,426 |
| `acc.render_self_dependency` | 0 | 0 |
| `acc.self_dep_nonpixel` | 0 | 0 |

Exactly one of the first two is non-zero, which is the shape that says the detector is working: a
draw aliasing its own render target is either covered by the framebuffer-fetch rewrite or it is not.

**Every draw in this test is covered — including Case B, where the rewrite is wrong.** The ROM
alternates Case A (samples its own fragment) and Case B (samples 16 texels away) frame by frame, and
both report exactly one covered unit. So the decompiler applies the substitution unconditionally,
and for Case B that means discarding the texture coordinate and silently returning the current
fragment instead of the neighbour. That is `rm-fbfetch-coordinate`, and this is its measurement.

`acc.render_self_dependency` reading zero is correct here rather than suspicious: this test is
pixel-stage only, and the pixel stage is exactly what the rewrite covers. A vertex- or
geometry-stage self-dependency would land in that counter, and nothing in this ROM creates one.

## The pixel oracle — the substitution is not just applied, the output is wrong

A counter says a rewrite happened. It cannot say the result is incorrect. The oracle reads pixels
back and compares them exactly:

```
TEST selfdep_seed                PASS
TEST selfdep_caseA_same_texel    PASS
TEST selfdep_caseB_offset_texel  FAIL expected=16/16 texels x+16 (sampled the neighbour)
                                      got=neighbour=0 self=16 other=0
```

**16 of 16 texels returned their own value instead of the neighbour's.** Unanimous, with nothing in
the `other` bucket, so there is no partial or racy result to argue about.

How it works. The target is `UINT_R32`, so comparison is bit-exact and there is no filtering
tolerance to negotiate — the trick borrowed from piglit's `blending-in-shader` test. A seed pass
writes each texel's own X. Then a strip at `[0,16)` samples 16 texels to the right and writes what it
read, giving two distinguishable outcomes:

| | `output[x]` |
|---|---|
| sampled the neighbour, as written | `x + 16` |
| coordinate discarded, read of self | `x` |

The strip reads from `[16,32)`, which the strip does not write, **so there is no feedback race**: the
source texels are untouched by this draw whichever path the emulator takes. A failure here cannot be
explained away as undefined ordering.

Two controls, and they are the point of trusting the third line:

- **`selfdep_seed`** checks a texel outside both strips. If the seed did not land, every later verdict
  is noise. It caught exactly that on the first run, when inherited render state left colour writes
  off and the whole surface read zero.
- **`selfdep_caseA_same_texel`** samples its *own* texel. A real sample and a framebuffer fetch must
  agree here — that agreement is what makes the substitution valid for Case A. It passes, which is
  what says the harness reads correct pixels correctly. **If Case A ever fails, suspect this harness
  before suspecting the emulator.**

`run.sh` reports all three and warns loudly if a *control* fails. It does not gate on
`selfdep_caseB_offset_texel`, because that test is expected to fail until `rm-fbfetch-coordinate`
lands — at which point it becomes the regression test that proves the fix.

## Case C — the vertex stage, and the one case that proves the splitter

```
TEST selfdep_caseC_vertex_stage  PASS
```

A **vertex** shader samples the surface being rendered into. That case is uncovered by construction
rather than by luck: `LatteDecompilerAnalyzer` fills `textureRenderTargetIndex` only for
`shaderType == Pixel`, so the framebuffer-fetch rewrite can never reach it. It is therefore the only
draw in this ROM that the pass splitter has to notice, and the only one that can show the splitter
produces *correct pixels* rather than merely firing.

It passes: all 16 texels of the strip return the seeded value the vertex shader read.

| counter | reading |
|---|---|
| `acc.self_dep_pass_splits` | 1 — exactly the one uncovered draw |
| `acc.render_self_dependency` | 1 — **first non-zero in this fork's history** |
| `acc.self_dep_nonpixel` | 1 |
| `acc.self_dep_fbfetch` | 394 — every pixel-stage alias, correctly *not* split |

### It did not pass at first, and the reason was worth finding

The vertex shader produced no Metal function, so its pipeline was refused and the draw silently
vanished. The first diagnosis — *"vertex-stage texture sampling does not survive the MSL emitter"* —
was **wrong and far too broad**. Vertex texture sampling works.

What actually failed is texel-coordinate access (`GPU7_TEX_INST_LD`, or a `SAMPLE` with all four
coordinates unnormalized) in any **non-pixel** stage. `LatteDecompilerAnalyzer.cpp:593` gated the
`texUnitUsesTexelCoordinates → hasUniformVarBlock` promotion on the pixel stage, while the
`float2 tex{}Scale` SupportBuffer member and the `*supportBuffer.tex{}Scale` body reference are
emitted for *every* stage. Only the `constant SupportBuffer&` **parameter** was gated. The MSL
therefore referenced a parameter the function had never been given:

```
error: use of undeclared identifier 'supportBuffer'
```

Established by running three variants, not by reading alone: `usampler2D`+`texelFetch` failed,
`sampler2D`+`texelFetch` failed **identically**, and `sampler2D`+`texture()` compiled. So the
discriminator is texel coordinates, not the integer sampler. One gate removed fixes it.

It stayed hidden because any *other* reason to set `hasUniformVarBlock` — streamout, point size, a
disabled viewport scale, a remapped uniform mode — rescues the shader. Only a minimal non-pixel
texelFetch trips it, which is exactly what a purpose-built test ROM writes and what a real game
rarely does.

> The GPU writes `UINT_R32` in the opposite byte order to how the PPC core reads a `uint32_t`, so a
> seeded 1 comes back as `0x01000000`. The swap is done on read, in `rd()`, deliberately: the shader
> is the thing under test and should stay the simplest possible expression of "write x", with no
> endian arithmetic of its own that could be wrong. The raw-dump line in the output exists so this
> class of mistake shows up as obviously-shifted values rather than as a mysterious failure.

### Why the first attempt read zero

All three counters read zero at first, and the cause was placement, not the test. `BindStageResources`
iterates `resourceMapping.getTextureCount()`, and `_initTextureBindingPointsMTL`
(`LatteDecompilerAnalyzer.cpp:527`) **skips assigning a binding point to any unit whose
`textureRenderTargetIndex != 255`**. A framebuffer-fetch unit is therefore stripped from the resource
mapping entirely — `textureCount` was **0** — so both hooks sat inside a loop the covered case can
never enter. `acc.self_dep_fbfetch` was unreachable code.

The covered case is now counted from `shader->textureRenderTargetIndex[]`, which survives into the
shader object. The uncovered check stays in the loop, where a unit that *does* get a binding point
and still aliases is genuinely visible.

Worth keeping in mind generally: a counter reading zero can mean the hook is in a place the case
never reaches, which looks identical to the case not occurring.

## What it does

- Allocates one `GX2Texture` with `GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV` and a `GX2ColorBuffer`
  aliasing the same surface.
- Compiles two pixel shaders with CafeGLSL at runtime, alternating per frame:
  - **Case A — same texel.** Samples at its own fragment coordinate. Metal expresses this natively
    with programmable blending, and the decompiler's framebuffer-fetch substitution is *valid*.
  - **Case B — offset texel.** Samples 16 texels away. Framebuffer fetch cannot express this, and
    the emitter's substitution discards the coordinate — so it silently returns the current fragment
    instead of the neighbour. **This is the defect the audit found**, and it is why the two cases are
    separate tests rather than one.
- Reports over `OSReport` (`WHBLogCafeInit`), so `--forward-console-logging` captures it.

It deliberately does **not** verify pixel values. That needs GPU→CPU readback and an exact integer
format; it is worth doing, but it is a separate piece of work and conflating the two would have
produced a test that fails for two unrelated reasons at once.

## Requirements

`cafeLibs/glslcompiler.rpl` from [CafeGLSL](https://github.com/Exzap/CafeGLSL) v0.2.0, in the
emulator's user data directory. `LoadSharedLibrariesEnabled()` defaults to `true`, so no config
change is needed. The ROM reports `RESULT=ERROR reason=glslcompiler.rpl-not-found` if it is absent.

## Building and running

Toolchain setup — including with no root — is in [`testing/toolchain/`](../toolchain/README.md).

```sh
export DEVKITPRO=$HOME/.local/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
export PATH="$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH"

make
./run.sh                 # bounded run; prints the acc.self_dep_* readings
TIMEOUT=90 ./run.sh      # longer
```

**The ROM does not stop on its own** — it renders until killed, because the point is to accumulate
counter samples. `run.sh` bounds and stops it. Killing is safe here: telemetry is written by a
dedicated thread doing raw `write()`, so at most the in-flight queue is lost.

Do **not** read these counters with `testing/telemetry-report.py` — it prints "non-zero only" and
skips any counter whose total is zero, which is exactly the current result. `run.sh` reads the
per-frame arrays directly so the zeros are visible.

## `probe-pack/`

A sibling fixture, not part of this ROM. Installing [`probe-pack/`](probe-pack/README.md) forces the
graphic-pack branch of the backbuffer path and is how three inherited defects on it were reproduced,
including one that aborted the process on every boot. It uses this ROM as its workload, because a ROM
that prints and exits presents no frames and measures nothing.
