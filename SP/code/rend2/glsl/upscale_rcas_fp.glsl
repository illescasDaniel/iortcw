// r_renderScale FSR1 RCAS (Robust Contrast Adaptive Sharpening), FP32 port of AMD FidelityFX FSR 1.0 ffx_fsr1.h
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. MIT license:
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
// to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above
// copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
//
// Sharpens the EASU output (same size as the target); no denoise. Input is the tone-mapped image, saturated to 0..1.

uniform sampler2D u_TextureMap;   // EASU output (window-sized)
uniform vec4      u_RcasCon;      // x = sharpness, linear (exp2(-stops))
uniform vec4      u_RcasBox;      // clamp box in texels (x0, y0, x1, y1)

varying vec2      var_Tex1;

#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

vec4 RcasFetch(vec2 p)
{
	ivec2 t = ivec2(clamp(p, u_RcasBox.xy, u_RcasBox.zw));
	return texelFetch(u_TextureMap, t, 0);
}

void main()
{
	//    b
	//  d e f
	//    h
	vec2 sp = floor(gl_FragCoord.xy) + 0.5;
	vec3 b = clamp(RcasFetch(sp + vec2( 0.0, -1.0)).rgb, 0.0, 1.0);
	vec3 d = clamp(RcasFetch(sp + vec2(-1.0,  0.0)).rgb, 0.0, 1.0);
	vec4 ee = RcasFetch(sp);
	vec3 e = clamp(ee.rgb, 0.0, 1.0);
	vec3 f = clamp(RcasFetch(sp + vec2( 1.0,  0.0)).rgb, 0.0, 1.0);
	vec3 h = clamp(RcasFetch(sp + vec2( 0.0,  1.0)).rgb, 0.0, 1.0);

	// min and max of the ring
	vec3 mn4 = min(min(b, d), min(f, h));
	vec3 mx4 = max(max(b, d), max(f, h));

	// limiters: the most negative lobe that keeps the result in range
	vec2 peakC = vec2(1.0, -1.0 * 4.0);
	vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0e-6));
	vec3 hitMax = (peakC.x - max(mx4, e)) / min(4.0 * mn4 + peakC.y, vec3(-1.0e-6));
	vec3 lobeRGB = max(-hitMin, hitMax);
	float lobe = max(-FSR_RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * u_RcasCon.x;

	float rcpL = 1.0 / (4.0 * lobe + 1.0);
	vec3 c = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;

	gl_FragColor = vec4(c, ee.a);
}
