// r_renderScale FSR1 EASU (Edge Adaptive Spatial Upsampling), FP32 port of AMD FidelityFX FSR 1.0 ffx_fsr1.h
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. MIT license:
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
// to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above
// copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
//
// Reads the tone-mapped scene with texelFetch (no textureGather), clamped to the scene viewport.

uniform sampler2D u_TextureMap;   // scene colour (scene-sized)
uniform vec4      u_EasuCon0;     // xy = input pixels per output pixel, zw = 0.5 * xy - 0.5
uniform vec4      u_EasuCon1;     // input clamp box in texels (x0, y0, x1, y1)
uniform vec4      u_EasuCon2;     // xy = output box origin in pixels

varying vec2      var_Tex1;

vec3 EasuFetch(vec2 p)
{
	ivec2 t = ivec2(clamp(p, u_EasuCon1.xy, u_EasuCon1.zw));
	return texelFetch(u_TextureMap, t, 0).rgb;
}

// accumulates direction and length from one bilinear corner of the 2x2 centre quad
void EasuSet(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
	// lA: up, lB: left, lC: centre, lD: right, lE: down
	float dc = lD - lC;
	float cb = lC - lB;
	float lenX = max(abs(dc), abs(cb));
	lenX = 1.0 / max(lenX, 1.0e-20);
	float dirX = lD - lB;
	dir.x += dirX * w;
	lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
	lenX *= lenX;
	len += lenX * w;

	float ec = lE - lC;
	float ca = lC - lA;
	float lenY = max(abs(ec), abs(ca));
	lenY = 1.0 / max(lenY, 1.0e-20);
	float dirY = lE - lA;
	dir.y += dirY * w;
	lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
	lenY *= lenY;
	len += lenY * w;
}

void EasuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c)
{
	// rotate the offset into the edge direction and scale by the anisotropic length
	vec2 v;
	v.x = (off.x * dir.x) + (off.y * dir.y);
	v.y = (off.x * (-dir.y)) + (off.y * dir.x);
	v *= len;
	float d2 = v.x * v.x + v.y * v.y;
	d2 = min(d2, clp);
	// approximate lanczos2: (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (lob * x^2 - 1)^2
	float wB = (2.0 / 5.0) * d2 - 1.0;
	float wA = lob * d2 - 1.0;
	wB *= wB;
	wA *= wA;
	wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
	float w = wB * wA;
	aC += c * w;
	aW += w;
}

float EasuLuma(vec3 c)
{
	return c.b * 0.5 + (c.r * 0.5 + c.g);
}

void main()
{
	// output pixel relative to the output box -> input position relative to the input box
	vec2 ip = floor(gl_FragCoord.xy - u_EasuCon2.xy);
	vec2 pp = ip * u_EasuCon0.xy + u_EasuCon0.zw;
	vec2 fp = floor(pp);
	pp -= fp;
	fp += u_EasuCon1.xy + 0.5;

	//      b c
	//    e f g h
	//    i j k l
	//      n o
	vec3 b = EasuFetch(fp + vec2( 0.0, -1.0));
	vec3 c = EasuFetch(fp + vec2( 1.0, -1.0));
	vec3 e = EasuFetch(fp + vec2(-1.0,  0.0));
	vec3 f = EasuFetch(fp);
	vec3 g = EasuFetch(fp + vec2( 1.0,  0.0));
	vec3 h = EasuFetch(fp + vec2( 2.0,  0.0));
	vec3 i = EasuFetch(fp + vec2(-1.0,  1.0));
	vec3 j = EasuFetch(fp + vec2( 0.0,  1.0));
	vec3 k = EasuFetch(fp + vec2( 1.0,  1.0));
	vec3 l = EasuFetch(fp + vec2( 2.0,  1.0));
	vec3 n = EasuFetch(fp + vec2( 0.0,  2.0));
	vec3 o = EasuFetch(fp + vec2( 1.0,  2.0));

	float bL = EasuLuma(b), cL = EasuLuma(c), eL = EasuLuma(e), fL = EasuLuma(f);
	float gL = EasuLuma(g), hL = EasuLuma(h), iL = EasuLuma(i), jL = EasuLuma(j);
	float kL = EasuLuma(k), lL = EasuLuma(l), nL = EasuLuma(n), oL = EasuLuma(o);

	// direction and length, bilinearly weighted over the four centre taps f, g, j, k
	vec2 dir = vec2(0.0);
	float len = 0.0;
	EasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
	EasuSet(dir, len, pp.x * (1.0 - pp.y),         cL, fL, gL, hL, kL);
	EasuSet(dir, len, (1.0 - pp.x) * pp.y,         fL, iL, jL, kL, nL);
	EasuSet(dir, len, pp.x * pp.y,                 gL, jL, kL, lL, oL);

	vec2 dir2 = dir * dir;
	float dirR = dir2.x + dir2.y;
	bool zro = dirR < (1.0 / 32768.0);
	dirR = zro ? 1.0 : inversesqrt(dirR);
	dir.x = zro ? 1.0 : dir.x;
	dir *= dirR;

	len = len * 0.5;
	len *= len;
	float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
	vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
	float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
	float clp = 1.0 / lob;

	// dering: clamp to the centre 2x2
	vec3 min4 = min(min(f, g), min(j, k));
	vec3 max4 = max(max(f, g), max(j, k));

	vec3 aC = vec3(0.0);
	float aW = 0.0;
	EasuTap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
	EasuTap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
	EasuTap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);
	EasuTap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
	EasuTap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
	EasuTap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
	EasuTap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
	EasuTap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
	EasuTap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
	EasuTap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
	EasuTap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);
	EasuTap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);

	float alpha = texelFetch(u_TextureMap, ivec2(clamp(fp, u_EasuCon1.xy, u_EasuCon1.zw)), 0).a;
	gl_FragColor = vec4(min(max4, max(min4, aC / aW)), alpha);
}
