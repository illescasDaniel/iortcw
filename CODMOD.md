# Castle Operations Definitive (codmod)

This branch carries the engine and game-code changes for
[Castle Operations Definitive](https://github.com/illescasDaniel/castle-operations-definitive),
applied on top of ioRTCW `438e7d413b5f7277187c35b032eb0ef9093ae778`.

The same changes are also kept as a patch series in that repo's `patches/` directory.

## Running this fork on its own

`SP/run-sp.sh [--windowed 1280x720]` runs the `make` build as plain RTCW (`fs_game` main, no mod) from this repo
alone: it links the build and an installed RTCW's paks (Steam auto-detected, or `RTCW_DATA`) into `SP/build/run/` and
keeps config and saves in `SP/build/home/`. It also builds and loads the engine menus (`SP/menus/`: deltas on
RTCW's own menus plus `menudelta.py`; RTCW's menus are not redistributable, so only deltas are in git). Use it to check that the engine works with vanilla defaults and needs
nothing from a mod.

## rend2 additions

- **Per-pixel dynamic lights on models:** `R_DlightEntity` (tr_light.c) feeds MD3/MDS entities into the dlight passes
  (`r_dlightEntities`), plus an additive specular pass (`r_dlightSpecular`) and an optional albedo lift (default off) for near-black
  surfaces (`r_dlightAlbedoLift`).
- **Flashlight shadows:** the flashlight cones carry `REF_SHADOW_DLIGHT`. `R_RenderFlashlightShadowMap` (tr_main.c)
  renders one perspective depth map from the widest shadowed spot (the local player's body/weapon are excluded with
  `VPF_NOPLAYER`). `RB_DrawSurfs` turns it into a full-res screen-space mask (`shadowmask` shader,
  `USE_SPOT_SHADOW`) that `dlight_fp.glsl` and `lightall_fp.glsl` multiply into the light. The mask shares the
  `TB_AOMAP` unit via `u_AoParams.y`. Cvars: `r_flashlightShadow` (0 off, 2 shows the mask), `r_flashlightShadowSize`.
  The stock `r_dlightMode 2` cube-map shadows remain non-functional and are not used.
- **Dynamic lights on GPU (VAO) models:** static MD3 props (the `escape1` cages and iron gates) stayed dark under the
  flashlight even though `R_DlightEntity` gave them a dlight mask. Three stock rend2 gaps, all fixed:
  1. `RB_RenderDrawSurfList` (tr_backend.c) ORs the entity's `needDlights` into `tess.dlightBits` *before* the surface
     call, because VAO surfaces draw inside it.
  2. Every `RB_EndSurface(); RB_BeginSurface()` restart (`RB_CheckOverflow`, `RB_CheckVao`, `RB_SurfaceVaoCached`,
     `RB_SurfaceGrid`, `RB_SurfaceVaoMdvMesh`, `RB_IQMSurfaceAnimVao`) keeps `tess.dlightBits`; `RB_BeginSurface`
     used to zero them.
  3. `ForwardDlight` (tr_shade.c) picks the entity lightall variant (`LIGHTDEF_ENTITY_VERTEX/BONE_ANIMATION`,
     plus the bone matrix) for models; the world variant skips `u_ModelMatrix`, so model-space positions were lit
     as if they were world space and the light missed them.

  `ForwardDlight` also ignores baked vertex colour (`rgbGen vertex`) as a light modulator, so dark baked surfaces
  reflect the flashlight. Result: the flashlight lights and shadows the whole scene, world and models alike.

## GPU utilisation (2026-10-14)

Uncapped, the game is GPU-bound yet the GPU was only ~76-80% busy: two stock rend2 CPU/GPU sync points left it idle
each frame. Fixing both brought it to 97% busy and +35% average FPS.

Benchmark (`cl_benchDemo` replay, 2561x1601, SMAA 3, RTX 4070 Laptop, median of measured runs):

| | avg FPS | 1% low | P99 | GPU busy |
|---|---|---|---|---|
| Before | 183 | 106 | 7.6 ms | 76% |
| After | 247 | 163-167 | 5.4 ms | 97% |
| Flares disabled (ceiling) | 249 | 165 | 5.5 ms | 97% |

1. **Per-frame `glFinish` in `RB_SwapBuffers` (tr_backend.c).** Stock Q3 waits for the GPU to go idle before every
   swap, so the next frame's CPU work runs while the GPU has nothing queued. Now skipped (`r_swapFinish 0`; `1` is the
   stock behaviour, ~76% -> ~88% busy on its own). The driver's frame queue bounds the CPU's lead; with a cap or
   `r_vsync 4` the frame pacer still syncs (`com_maxfpsGpuSync`). Uncapped input latency can be a frame or two higher.
2. **Flare/corona depth readback (`RB_TestFlare`, tr_flares.c).** One `glReadPixels` of the depth buffer per visible
   flare per frame (~3 in the bench demo) drains the pipeline. Reading into a pixel pack buffer behind a fence did not
   help (the depth read itself stalls the driver). Visibility is now a `GL_SAMPLES_PASSED` occlusion query per flare: a
   one-pixel depth-tested quad at the flare's depth minus the stock 24-unit tolerance, collected once available, so
   visibility lags 1-2 frames (hidden by the flare fade). Results are keyed by flare id, scene and portal flag, not by
   `flare_t` slot, because hidden flares are freed every frame. `r_flaresAsync 1`; `0` is the stock readback.

No effect at the time (measured): non-blocking sun-flare query (later shipped, see below), `__GL_MaxFramesAllowed=3`, XWayland vs native Wayland, reading the
depth as `GL_UNSIGNED_INT`.

## GPU pass profiling and lossless pass savings (2026-10-14)

`r_gpuProfile 1` (tr_gpuprof.c) times each pass with `GL_TIMESTAMP` queries in an 8-frame ring (read without
blocking); `gpuprof` prints the table, which is also printed at renderer shutdown. Marks are `RB_GpuMark("name")`
calls in tr_backend.c and tr_postprocess.c. Bench 247 -> 303 FPS, GPU time ~4.07 -> 3.21 ms/frame:

1. **`RB_UpdateSunFlareVis`**: stock read the previous frame's sun query with a blocking `GL_QUERY_RESULT`. Now a
   4-query ring (`tr.sunFlareQuery[]`), read when available. Sun rays now run only when the sun is visible (stock
   counted not-ready results as visible).
2. **`RB_SMAA(src, dst, box)`** writes the blend to `dst` directly; `RB_PostProcess` tone-maps to scratch and SMAA
   writes back, removing two full-screen copies.
3. **SMAA stencil** (`r_smaaStencil 1`): `smaaEdgesFbo` owns a depth-stencil renderbuffer shared with `smaaBlendFbo`;
   edges write stencil 1, weights run where stencil == 1.
4. **GTAO depth mips** (`r_ssaoDepthMips 1`): `ssaoDepthImage` has `SSAO_DEPTH_MIPS` (4) point-downsampled levels,
   one FBO each (`tr.ssaoDepthMipFbo[]`), rebuilt per frame with nearest blits. `ssao_fp.glsl` `viewDepthMip()` reads
   level `floor(clamp(log2(tap px) - 3.3, 0, mips))`, point-sampled; level 0 taps are unchanged. Not bit-exact
   (differences of 1-2/255 on a few % of pixels), visually identical.
5. **`ssaoup_fp.glsl`** uses `textureGather` (ARB_texture_gather, with the old 8-fetch loop as fallback).

All but 4 give bit-identical screenshots (`scripts/shots.sh` + `scripts/shots-diff.py` in the outer repo).
