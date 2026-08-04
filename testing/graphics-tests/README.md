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
