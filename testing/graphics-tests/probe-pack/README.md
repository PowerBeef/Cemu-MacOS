# probe-pack — a graphic pack that exercises the custom output-shader path

Installing this pack and booting *anything* forces `LatteRenderTarget_copyToBackbuffer` down the
graphic-pack branch instead of the built-in one. That branch had two defects, and this is how both
were reproduced.

```sh
cp -R testing/graphics-tests/probe-pack \
      ~/Library/Application\ Support/TesseraEmu/graphicPacks/zz-tessera-probe

./bin/TesseraEmu_relwithdebinfo --game testing/graphics-tests/self_dependency.rpx \
    --telemetry /tmp/probe.jsonl --telemetry-areas accuracy

rm -rf ~/Library/Application\ Support/TesseraEmu/graphicPacks/zz-tessera-probe   # <- do not skip
```

**Remove it when you are done.** `titleIds = *` plus `default = 1` means it is universal and enabled
without a settings change, which is what makes it a convenient probe and a bad thing to leave
installed: every later run on the machine would quietly use it.

The `rules.txt` needs no `settings.xml` edit for exactly that reason. Nothing here modifies user
configuration.

## What it catches

`acc.output_shader_custom` counts entries to the branch. It read **1409 over 1409 frames** — once per
presented frame, not once per session, which is what made both defects continuous rather than
one-shot.

**A hard crash.** All six construction sites in `GraphicPack2.cpp` passed
`RendererOutputShader::GetOpenGlVertexSource` regardless of renderer, while the built-in shaders pick
theirs by renderer in `RendererOuputShader.cpp`'s `InitShaders`. The Metal compiler cannot compile
GLSL, so the vertex function came back nil and Metal aborted the process inside
`newRenderPipelineState`:

```
validateWithDevice:5044: failed assertion `Render Pipeline Descriptor Validation
vertexFunction must not be nil.'
```

`RendererOutputShader`'s own `if (!m_vertex_shader->WaitForCompiled()) throw` did **not** catch it:
`WaitForCompiled()` returns true on a failed Metal compile, so the guard that exists for this case
does not work on this backend. Worth remembering before relying on it elsewhere.

**An out-of-bounds write.** `DrawBackbufferQuad` leaves `shaderIndex` at its `255` sentinel when the
shader is none of the six statics, and `MetalOutputShaderCache::GetPipeline` used it to index a
12-element array. Line 16 bound a *reference* and line 30 assigned through it, so this was a write,
not just a read. With sRGB the `uint8` sum wrapped to 5 instead and aliased the `s_hermit_shader_ud`
slot in both directions.

Both are upstream defects too: upstream's Metal tree is byte-identical to our fork point, and its
`GraphicPack2.cpp` still has all six `GetOpenGlVertexSource` call sites.

## Why the fragment source is MSL

`output.glsl` keeps that filename because `GraphicPack2::LoadShaders` looks for it, but on a Metal
build `RendererOutputShader` passes the fragment source through **verbatim** (`RendererOuputShader.cpp`,
the `RendererAPI::Metal` case). So the contents must be MSL. That is inherited behaviour, not
something this fork chose, and it means real GLSL packs from the community cannot work here as
written. It is a separate problem from the two above and is not fixed.

## Why the ROM matters

Use `self_dependency.rpx`, not `rom_tests.rpx`. The ROM test suite prints its verdicts and exits
without ever presenting a frame, so `DrawBackbufferQuad` is never called and the probe reads zero —
a clean run that has tested nothing. The first attempt here did exactly that (`"frames":0`).
