/*
  Image sharpening filter from GeForce Experience. Provided by NVIDIA Corporation.
  
  Copyright 2019 Suketu J. Shah. All rights reserved.
  Redistribution and use in source and binary forms, with or without modification, are permitted provided
  that the following conditions are met:
    1. Redistributions of source code must retain the above copyright notice, this list of conditions
       and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions
       and the following disclaimer in the documentation and/or other materials provided with the distribution.
    3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse
       or promote products derived from this software without specific prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
  PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// r_dlsStrength: Denoised Luma Sharpening (NVIDIA image sharpening as ported in vkBasalt, dls.frag.glsl), ported to this
// renderer's GLSL: sharpen/denoise arrive as kernelSharpness / kernelDenoise (computed on the CPU with the formulas
// from the original comments), texelFetch clamped to the view box. Runs on the display-resolution, tone-mapped 3D
// image (display-referred, 0..1) before any 2D drawing, so the luma maths matches the original's expectations.

uniform sampler2D u_TextureMap;   // 3D scene colour (window-sized)
uniform vec4      u_DlsCon;       // x = kernelSharpness, y = kernelDenoise
uniform vec4      u_DlsBox;       // clamp box in texels (x0, y0, x1, y1)

varying vec2      var_Tex1;

float GetLumaComponents(float r, float g, float b)
{
	// Y from JPEG spec
	return 0.299 * r + 0.587 * g + 0.114 * b;
}

float GetLuma(vec3 p)
{
	return GetLumaComponents(p.x, p.y, p.z);
}

float Square(float v)
{
	return v * v;
}

// highlight fall-off start (prevents halos and noise in bright areas)
#define kHighBlock 0.65
// offset reducing sharpening in the shadows
#define kLowBlock (1.0 / 256.0)

vec3 DlsFetch(vec2 p)
{
	ivec2 t = ivec2(clamp(p, u_DlsBox.xy, u_DlsBox.zw));
	return clamp(texelFetch(u_TextureMap, t, 0).rgb, 0.0, 1.0);
}

void main()
{
	//  e  d  h
	//  a (x) b
	//  g  c  f
	vec2 sp = floor(gl_FragCoord.xy) + 0.5;
	vec4 xx = texelFetch(u_TextureMap, ivec2(clamp(sp, u_DlsBox.xy, u_DlsBox.zw)), 0);
	vec3 x = clamp(xx.rgb, 0.0, 1.0);

	vec3 a = DlsFetch(sp + vec2(-1.0,  0.0));
	vec3 b = DlsFetch(sp + vec2( 1.0,  0.0));
	vec3 c = DlsFetch(sp + vec2( 0.0,  1.0));
	vec3 d = DlsFetch(sp + vec2( 0.0, -1.0));

	vec3 e = DlsFetch(sp + vec2(-1.0, -1.0));
	vec3 f = DlsFetch(sp + vec2( 1.0,  1.0));
	vec3 g = DlsFetch(sp + vec2(-1.0,  1.0));
	vec3 h = DlsFetch(sp + vec2( 1.0, -1.0));

	float lx = GetLuma(x);

	float la = GetLuma(a);
	float lb = GetLuma(b);
	float lc = GetLuma(c);
	float ld = GetLuma(d);

	float le = GetLuma(e);
	float lf = GetLuma(f);
	float lg = GetLuma(g);
	float lh = GetLuma(h);

	// cross min/max
	float ncmin = min(min(le, lf), min(lg, lh));
	float ncmax = max(max(le, lf), max(lg, lh));

	// plus min/max
	float npmin = min(min(min(la, lb), min(lc, ld)), lx);
	float npmax = max(max(max(la, lb), max(lc, ld)), lx);

	// compute "soft" local dynamic range -- average of 3x3 and plus shape
	float lmin = 0.5 * min(ncmin, npmin) + 0.5 * npmin;
	float lmax = 0.5 * max(ncmax, npmax) + 0.5 * npmax;

	// compute local contrast enhancement kernel
	float lw = lmin / (lmax + kLowBlock);
	float hw = Square(1.0 - Square(max(lmax - kHighBlock, 0.0) / ((1.0 - kHighBlock))));

	// noise suppression (u_DlsCon.y = kernelDenoise, from the CPU: 1.0 / (kDenoiseMin + (kDenoiseMax - kDenoiseMin) * denoise))
	float nw = Square((lmax - lmin) * u_DlsCon.y);

	// pick conservative boost
	float boost = min(min(lw, hw), nw);

	// run variable-sigma 3x3 sharpening convolution (u_DlsCon.x = kernelSharpness, from the CPU:
	// kSharpnessMin + (kSharpnessMax - kSharpnessMin) * sharpen)
	float k = boost * u_DlsCon.x;

	float accum = lx;
	accum += la * k;
	accum += lb * k;
	accum += lc * k;
	accum += ld * k;
	accum += le * (k * 0.5);
	accum += lf * (k * 0.5);
	accum += lg * (k * 0.5);
	accum += lh * (k * 0.5);

	// normalize (divide the accumulator by the sum of convolution weights)
	accum /= 1.0 + 6.0 * k;

	float delta = accum - lx;
	gl_FragColor = vec4(xx.rgb + vec3(delta), xx.a);
}
