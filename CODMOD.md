# Castle Operations Definitive (codmod)

This branch carries the engine and game-code changes for
[Castle Operations Definitive](https://github.com/illescasDaniel/castle-operations-definitive),
applied on top of ioRTCW `438e7d413b5f7277187c35b032eb0ef9093ae778`.

The same changes are also kept as a patch series in that repo's `patches/` directory.

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
