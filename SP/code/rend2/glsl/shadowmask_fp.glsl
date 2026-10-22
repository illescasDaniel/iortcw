uniform sampler2D u_ScreenDepthMap;

uniform sampler2DShadow u_ShadowMap;
#if defined(USE_SHADOW_CASCADE)
uniform sampler2DShadow u_ShadowMap2;
uniform sampler2DShadow u_ShadowMap3;
uniform sampler2DShadow u_ShadowMap4;
#endif

uniform mat4      u_ShadowMvp;
#if defined(USE_SHADOW_CASCADE)
uniform mat4      u_ShadowMvp2;
uniform mat4      u_ShadowMvp3;
uniform mat4      u_ShadowMvp4;
#endif

uniform vec3   u_ViewOrigin;
uniform vec4   u_ViewInfo; // zfar / znear, zfar

varying vec2   var_DepthTex;
varying vec3   var_ViewDir;

// depth is GL_DEPTH_COMPONENT24
// so the maximum error is 1.0 / 2^24
#define DEPTH_MAX_ERROR 0.000000059604644775390625

// Input: It uses texture coords as the random number seed.
// Output: Random number: [0,1), that is between 0.0 and 0.999999... inclusive.
// Author: Michael Pohoreski
// Copyright: Copyleft 2012 :-)
// Source: http://stackoverflow.com/questions/5149544/can-i-generate-a-random-number-inside-a-pixel-shader

float random( const vec2 p )
{
  // We need irrationals for pseudo randomness.
  // Most (all?) known transcendental numbers will (generally) work.
  const vec2 r = vec2(
    23.1406926327792690,  // e^pi (Gelfond's constant)
     2.6651441426902251); // 2^sqrt(2) (Gelfond-Schneider constant)
  //return fract( cos( mod( 123456789., 1e-7 + 256. * dot(p,r) ) ) );
  return mod( 123456789., 1e-7 + 256. * dot(p,r) );  
}

#if defined(USE_SHADOW_SMOOTH)
// codmod: optimized bilinear PCF (Castano, "Shadow Mapping Summary", The Witness). Weighted
// hardware 2x2 PCF taps give a smooth tent kernel with no per-pixel noise, so the result is
// stable under camera motion. 1: 3x3 kernel (4 taps), 2: 5x5 (9 taps), 3: 7x7 (16 taps).
// Needs GL_LINEAR on the shadow map (tr_image.c).
float PCF(const sampler2DShadow shadowmap, const vec2 st, const float dist)
{
#if defined(USE_SHADOW_FILTER)
	vec2 uv = st * r_shadowMapSize;
	vec2 base = floor(uv + 0.5);
	float s = uv.x + 0.5 - base.x;
	float t = uv.y + 0.5 - base.y;
	const float inv = 1.0 / r_shadowMapSize;
	base = (base - 0.5) * inv;

  #define TAP(u, v) shadow2D(shadowmap, vec3(base + vec2(u, v) * inv, dist))

  #if defined(USE_SHADOW_FILTER3)
	float uw0 = 5.0 * s - 6.0, uw1 = 11.0 * s - 28.0, uw2 = -(11.0 * s + 17.0), uw3 = -(5.0 * s + 1.0);
	float u0 = (4.0 * s - 5.0) / uw0 - 3.0, u1 = (4.0 * s - 16.0) / uw1 - 1.0;
	float u2 = -(7.0 * s + 5.0) / uw2 + 1.0, u3 = -s / uw3 + 3.0;
	float vw0 = 5.0 * t - 6.0, vw1 = 11.0 * t - 28.0, vw2 = -(11.0 * t + 17.0), vw3 = -(5.0 * t + 1.0);
	float v0 = (4.0 * t - 5.0) / vw0 - 3.0, v1 = (4.0 * t - 16.0) / vw1 - 1.0;
	float v2 = -(7.0 * t + 5.0) / vw2 + 1.0, v3 = -t / vw3 + 3.0;

	float sum = uw0 * vw0 * TAP(u0, v0) + uw1 * vw0 * TAP(u1, v0) + uw2 * vw0 * TAP(u2, v0) + uw3 * vw0 * TAP(u3, v0)
	          + uw0 * vw1 * TAP(u0, v1) + uw1 * vw1 * TAP(u1, v1) + uw2 * vw1 * TAP(u2, v1) + uw3 * vw1 * TAP(u3, v1)
	          + uw0 * vw2 * TAP(u0, v2) + uw1 * vw2 * TAP(u1, v2) + uw2 * vw2 * TAP(u2, v2) + uw3 * vw2 * TAP(u3, v2)
	          + uw0 * vw3 * TAP(u0, v3) + uw1 * vw3 * TAP(u1, v3) + uw2 * vw3 * TAP(u2, v3) + uw3 * vw3 * TAP(u3, v3);
	return sum * (1.0 / 2704.0);
  #elif defined(USE_SHADOW_FILTER2)
	float uw0 = 4.0 - 3.0 * s, uw1 = 7.0, uw2 = 1.0 + 3.0 * s;
	float u0 = (3.0 - 2.0 * s) / uw0 - 2.0, u1 = (3.0 + s) / uw1, u2 = s / uw2 + 2.0;
	float vw0 = 4.0 - 3.0 * t, vw1 = 7.0, vw2 = 1.0 + 3.0 * t;
	float v0 = (3.0 - 2.0 * t) / vw0 - 2.0, v1 = (3.0 + t) / vw1, v2 = t / vw2 + 2.0;

	float sum = uw0 * vw0 * TAP(u0, v0) + uw1 * vw0 * TAP(u1, v0) + uw2 * vw0 * TAP(u2, v0)
	          + uw0 * vw1 * TAP(u0, v1) + uw1 * vw1 * TAP(u1, v1) + uw2 * vw1 * TAP(u2, v1)
	          + uw0 * vw2 * TAP(u0, v2) + uw1 * vw2 * TAP(u1, v2) + uw2 * vw2 * TAP(u2, v2);
	return sum * (1.0 / 144.0);
  #else
	float uw0 = 3.0 - 2.0 * s, uw1 = 1.0 + 2.0 * s;
	float u0 = (2.0 - s) / uw0 - 1.0, u1 = s / uw1 + 1.0;
	float vw0 = 3.0 - 2.0 * t, vw1 = 1.0 + 2.0 * t;
	float v0 = (2.0 - t) / vw0 - 1.0, v1 = t / vw1 + 1.0;

	float sum = uw0 * vw0 * TAP(u0, v0) + uw1 * vw0 * TAP(u1, v0)
	          + uw0 * vw1 * TAP(u0, v1) + uw1 * vw1 * TAP(u1, v1);
	return sum * (1.0 / 16.0);
  #endif
  #undef TAP
#else
	return shadow2D(shadowmap, vec3(st, dist));
#endif
}
#else
float PCF(const sampler2DShadow shadowmap, const vec2 st, const float dist)
{
	float mult;
	float scale = 2.0 / r_shadowMapSize;

#if 0
	// from http://http.developer.nvidia.com/GPUGems/gpugems_ch11.html
	vec2 offset = vec2(greaterThan(fract(var_DepthTex.xy * r_FBufScale * 0.5), vec2(0.25)));
	offset.y += offset.x;
	if (offset.y > 1.1) offset.y = 0.0;
	
	mult = shadow2D(shadowmap, vec3(st + (offset + vec2(-1.5,  0.5)) * scale, dist))
	     + shadow2D(shadowmap, vec3(st + (offset + vec2( 0.5,  0.5)) * scale, dist))
	     + shadow2D(shadowmap, vec3(st + (offset + vec2(-1.5, -1.5)) * scale, dist))
	     + shadow2D(shadowmap, vec3(st + (offset + vec2( 0.5, -1.5)) * scale, dist));
	 
	mult *= 0.25;
#endif

#if defined(USE_SHADOW_FILTER)
	float r = random(var_DepthTex.xy);
	float sinr = sin(r) * scale;
	float cosr = cos(r) * scale;
	mat2 rmat = mat2(cosr, sinr, -sinr, cosr);

	mult =  shadow2D(shadowmap, vec3(st + rmat * vec2(-0.7055767, 0.196515), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.3524343, -0.7791386), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.2391056, 0.9189604), dist));
  #if defined(USE_SHADOW_FILTER2)
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(-0.07580382, -0.09224417), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.5784913, -0.002528916), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.192888, 0.4064181), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(-0.6335801, -0.5247476), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(-0.5579782, 0.7491854), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.7320465, 0.6317794), dist));

	mult *= 0.11111;
  #else
    mult *= 0.33333;
  #endif
#else
	mult = shadow2D(shadowmap, vec3(st, dist));
#endif

	return mult;
}
#endif

float getLinearDepth(sampler2D depthMap, vec2 tex, float zFarDivZNear)
{
	float sampleZDivW = texture2D(depthMap, tex).r - DEPTH_MAX_ERROR;
	return 1.0 / mix(zFarDivZNear, 1.0, sampleZDivW);
}

#if defined(USE_SPOT_SHADOW)
// codmod: flashlight spot shadow, single perspective map. Result is 1 = lit, 0 = shadowed.
uniform vec4 u_LightOrigin;

void main()
{
	float depth = getLinearDepth(u_ScreenDepthMap, var_DepthTex, u_ViewInfo.x);
	vec3 pos = u_ViewOrigin + var_ViewDir * depth;
	vec3 toLight = u_LightOrigin.xyz - pos;
	float dist = length(toLight);
	pos += toLight * ((1.5 + dist * 0.004) / max(dist, 1.0));

	vec4 sp = u_ShadowMvp * vec4(pos, 1.0);
	float result = 1.0;

	if (sp.w > 0.0)
	{
		sp.xyz = sp.xyz * (0.5 / sp.w) + vec3(0.5);
		if (all(greaterThan(sp.xyz, vec3(0.0))) && all(lessThan(sp.xyz, vec3(1.0))))
		{
			float t = 1.0 / r_flashShadowMapSize;
			result = 0.0;
			for (int x = -1; x <= 1; x++)
				for (int y = -1; y <= 1; y++)
					result += shadow2D(u_ShadowMap, vec3(sp.xy + vec2(float(x), float(y)) * t, sp.z));
			result *= (1.0 / 9.0);
		}
	}

	gl_FragColor = vec4(vec3(result), 1.0);
}
#elif defined(USE_SHADOW_CASCADE_BLEND) && defined(USE_SHADOW_CASCADE)
// codmod: cascade lookup that fades into the next cascade over the last CASCADE_BLEND of the
// map's half-width, so the change in texel size (and filter width) has no hard seam.
#define CASCADE_BLEND 0.1

bool InCascade(const vec4 sp)
{
	return all(lessThan(abs(sp.xyz), vec3(abs(sp.w))));
}

float CascadeEdge(const vec4 sp)
{
	vec2 a = abs(sp.xy / sp.w);
	return clamp((max(a.x, a.y) - (1.0 - CASCADE_BLEND)) * (1.0 / CASCADE_BLEND), 0.0, 1.0);
}

float SampleCascade(const sampler2DShadow shadowmap, vec4 sp)
{
	sp.xyz = sp.xyz * (0.5 / sp.w) + vec3(0.5);
	return PCF(shadowmap, sp.xy, sp.z);
}

void main()
{
	float result;

	float depth = getLinearDepth(u_ScreenDepthMap, var_DepthTex, u_ViewInfo.x);
	vec4 biasPos = vec4(u_ViewOrigin + var_ViewDir * (depth - 0.5 / u_ViewInfo.x), 1.0);

	vec4 sp1 = u_ShadowMvp  * biasPos;
	vec4 sp2 = u_ShadowMvp2 * biasPos;
	vec4 sp3 = u_ShadowMvp3 * biasPos;

	if (InCascade(sp1))
	{
		result = SampleCascade(u_ShadowMap, sp1);
		float f = CascadeEdge(sp1);
		if (f > 0.0 && InCascade(sp2))
			result = mix(result, SampleCascade(u_ShadowMap2, sp2), f);
	}
	else if (InCascade(sp2))
	{
		result = SampleCascade(u_ShadowMap2, sp2);
		float f = CascadeEdge(sp2);
		if (f > 0.0 && InCascade(sp3))
			result = mix(result, SampleCascade(u_ShadowMap3, sp3), f);
	}
	else if (InCascade(sp3))
	{
		result = SampleCascade(u_ShadowMap3, sp3);
		float f = CascadeEdge(sp3);
		if (f > 0.0)
			result = mix(result, SampleCascade(u_ShadowMap4, u_ShadowMvp4 * biasPos), f);
	}
	else
	{
		result = SampleCascade(u_ShadowMap4, u_ShadowMvp4 * biasPos);
	}

	gl_FragColor = vec4(vec3(result), 1.0);
}
#else
void main()
{
	float result;

	float depth = getLinearDepth(u_ScreenDepthMap, var_DepthTex, u_ViewInfo.x);
	vec4 biasPos = vec4(u_ViewOrigin + var_ViewDir * (depth - 0.5 / u_ViewInfo.x), 1.0);

	vec4 shadowpos = u_ShadowMvp * biasPos;

#if defined(USE_SHADOW_CASCADE)
	if (all(lessThan(abs(shadowpos.xyz), vec3(abs(shadowpos.w)))))
	{
#endif
		shadowpos.xyz = shadowpos.xyz * (0.5 / shadowpos.w) + vec3(0.5);
		result = PCF(u_ShadowMap, shadowpos.xy, shadowpos.z);
#if defined(USE_SHADOW_CASCADE)
	}
	else
	{
		shadowpos = u_ShadowMvp2 * biasPos;

		if (all(lessThan(abs(shadowpos.xyz), vec3(abs(shadowpos.w)))))
		{
			shadowpos.xyz = shadowpos.xyz * (0.5 / shadowpos.w) + vec3(0.5);
			result = PCF(u_ShadowMap2, shadowpos.xy, shadowpos.z);
		}
		else
		{
			shadowpos = u_ShadowMvp3 * biasPos;

			if (all(lessThan(abs(shadowpos.xyz), vec3(abs(shadowpos.w)))))
			{
				shadowpos.xyz = shadowpos.xyz * (0.5 / shadowpos.w) + vec3(0.5);
				result = PCF(u_ShadowMap3, shadowpos.xy, shadowpos.z);
			}
			else
			{
				shadowpos = u_ShadowMvp4 * biasPos;
				shadowpos.xyz = shadowpos.xyz * (0.5 / shadowpos.w) + vec3(0.5);
				result = PCF(u_ShadowMap4, shadowpos.xy, shadowpos.z);
			}
		}
	}
#endif

	gl_FragColor = vec4(vec3(result), 1.0);
}
#endif
