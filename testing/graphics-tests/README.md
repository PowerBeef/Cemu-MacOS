# Graphics tests — render-pass self-dependency

Draws into a colour buffer while sampling that same surface as a texture. This is the case
`docs/porting/03-graphics-metal.md` item 3.1 is about, and the case the `acc.self_dep_*` counters
were added to size.

## Status: builds and runs, but does not yet reproduce

**Measured 2026-08-03** at commit `a2e441b`, n=1.

**The ROM works. The counters read zero.** That is an unresolved result, not a passing test, and it
is written down here rather than quietly filed as done.

| | |
|---|---|
| ROM builds and boots | yes |
| CafeGLSL compiles both shaders at runtime | yes — `samplerVars=1`, `location=0`, `name=selfTexture` |
| draws issued | 1/frame over 2,049 frames |
| `acc.self_dep_fbfetch` | **0** |
| `acc.render_self_dependency` | **0** |
| `acc.self_dep_nonpixel` | **0** |

Both counters being zero is the informative part. They are on opposite sides of one `continue`:
`acc.self_dep_fbfetch` fires when the decompiler *did* rewrite the sample into a framebuffer fetch,
`acc.render_self_dependency` when it did not and the texture was bound anyway. **A draw that aliased
its own render target should increment exactly one of them.** Neither moving means the emulator did
not see an alias at all.

Two candidate explanations, not yet separated:

1. **The test does not create the aliasing the emulator sees.** The colour buffer and the sampled
   texture share one surface allocation here, but the emulator may resolve them to two distinct
   `LatteTexture` objects (the detector compares `view->baseTexture` pointers). Or the active FBO at
   draw time is not the one this code sets — the draw happens between `WHBGfxBeginRender()` and
   `WHBGfxBeginRenderTV()`, and `gpu.render_passes` is 1/frame where the TV and DRC clears alone
   should produce more, which is mildly suspicious.
2. **The detector does not fire when it should.** It has never produced a non-zero reading on any
   workload, so "it works" is an assumption. Its positive control (`gpu.depth_sampled_draws`, which
   reads 392/frame in BotW) shares the loop but not the comparison.

Distinguishing them needs one instrumented run logging the FBO attachment and bound-texture
`baseTexture` pointers at draw time. Until that is done, **do not treat a zero from these counters as
evidence about item 3.1.**

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
