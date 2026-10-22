uniform sampler2D u_DiffuseMap;

#if defined(USE_LIGHTMAP)
uniform sampler2D u_LightMap;
#endif

#if defined(USE_NORMALMAP)
uniform sampler2D u_NormalMap;
#endif

#if defined(USE_DELUXEMAP)
uniform sampler2D u_DeluxeMap;
#endif

#if defined(USE_SPECULARMAP)
uniform sampler2D u_SpecularMap;
#endif

#if defined(USE_SHADOWMAP)
uniform sampler2D u_ShadowMap;
#endif

#if defined(USE_CUBEMAP)
uniform samplerCube u_CubeMap;
#endif

#if defined(USE_NORMALMAP) || defined(USE_DELUXEMAP) || defined(USE_SPECULARMAP) || defined(USE_CUBEMAP)
// y = deluxe, w = cube
uniform vec4      u_EnableTextures; 
#endif

#if defined(USE_PRIMARY_LIGHT) || defined(USE_SHADOWMAP)
uniform vec3  u_PrimaryLightColor;
uniform vec3  u_PrimaryLightAmbient;
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
uniform vec4      u_NormalScale;
uniform vec4      u_SpecularScale;
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
#if defined(USE_CUBEMAP)
uniform vec4      u_CubeMapInfo;
#endif
#endif

uniform int       u_AlphaTest;

// ambient only AO: scales the baked / ambient light, never the sun or dynamic lights
uniform sampler2D u_AoMap;
uniform vec4      u_AoParams;

varying vec4      var_TexCoords;

varying vec4      var_Color;
#if (defined(USE_LIGHT) && !defined(USE_FAST_LIGHT))
varying vec4      var_ColorAmbient;
#endif

#if (defined(USE_LIGHT) && !defined(USE_FAST_LIGHT))
varying vec4   var_Normal;
varying vec4   var_Tangent;
varying vec4   var_Bitangent;
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
varying vec4      var_LightDir;
#endif

#if defined(USE_PRIMARY_LIGHT) || defined(USE_SHADOWMAP)
varying vec4      var_PrimaryLightDir;
#endif


#define EPSILON 0.00000001

#if defined(USE_PARALLAXMAP)
// codmod: parallax occlusion mapping. No gl_FragDepth writes (the depth buffer stays flat so early-Z, the
// depth prepass and the DEPTHFUNC_EQUAL passes keep working).
// x = max linear steps, y = fade distance (<= 0: no fade), z = floor depth scale, w = cavity strength
uniform vec4 u_ParallaxParams;
// phase 2: x = stage max depth (0..1], y = steps per height texel (<= 0: legacy angle-only steps),
// z = march LOD floor (< 0: legacy textureGrad march), w = shadow fade end as a fraction of the fade distance (0 = off)
uniform vec4 u_ParallaxParams2;

// derivatives of the undisplaced coordinates, computed once in uniform flow before any branch
vec2 pomDx;
vec2 pomDy;
// footprint data for the march (GLSL >= 130): normal map size, LOD for the march fetches (< 0: use textureGrad)
// and LOD used to count steps
vec2 pomTexSize = vec2(1.0);
float pomMarchLod = -1.0;
float pomStepLod = 0.0;

  #if __VERSION__ >= 130
    #define POM_FETCH(s, t) textureGrad(s, t, pomDx, pomDy)
  #else
    #define POM_FETCH(s, t) texture2D(s, t)
  #endif

// 0 = top of the volume, 1 = bottom
float SampleDepth(vec2 t)
{
  #if defined(SWIZZLE_NORMALMAP)
	return 1.0 - POM_FETCH(u_NormalMap, t).r;
  #else
	return 1.0 - POM_FETCH(u_NormalMap, t).a;
  #endif
}

// height fetch used by the marches: a fixed LOD when enabled (no implicit derivatives, safe in divergent flow)
float SampleDepthMarch(vec2 t)
{
  #if __VERSION__ >= 130
	if (pomMarchLod >= 0.0)
	  #if defined(SWIZZLE_NORMALMAP)
		return 1.0 - textureLod(u_NormalMap, t, pomMarchLod).r;
	  #else
		return 1.0 - textureLod(u_NormalMap, t, pomMarchLod).a;
	  #endif
  #endif
	return SampleDepth(t);
}

// Linear search for the first sample below the surface, then binary refinement and a final secant step.
// Returns the depth fraction (0..1) along ds.
float ParallaxMarch(vec2 uv0, vec2 ds, float stepsF, float span)
{
  #if defined(USE_RELIEFMAP)
	const int refineSteps = 4;
  #else
	const int refineSteps = 2;
  #endif

	// adjust position if offset above surface
	uv0 -= ds * r_parallaxMapOffset;

	// top plateau: nothing recessed here, no march
	float prevS = SampleDepthMarch(uv0);
	if (prevS <= 1.0 / 255.0)
		return 0.0;

	// the search only spans [0, span] of the depth range (span = the texture's own max depth); d keeps its units
	int numSteps = int(stepsF);
	float size = span / float(numSteps);

	float prevD = 0.0;
	float d = 0.0;
	float hitS = 0.0;
	bool hit = false;

	for (int i = 0; i < 128; ++i)
	{
		if (i >= numSteps)
			break;

		d += size;

		float s = SampleDepthMarch(uv0 + ds * d);
		if (d >= s)
		{
			hit = true;
			hitS = s;
			break;
		}

		prevD = d;
		prevS = s;
	}

	if (!hit)
		return span;

	float lo = prevD;
	float hi = d;
	float loS = prevS;
	float hiS = hitS;

	for (int i = 0; i < refineSteps; ++i)
	{
		float mid = 0.5 * (lo + hi);
		float midS = SampleDepthMarch(uv0 + ds * mid);
		if (mid >= midS)
		{
			hi = mid;
			hiS = midS;
		}
		else
		{
			lo = mid;
			loS = midS;
		}
	}

	// secant on the narrowed bracket
	float a = lo - loS;
	float b = hi - hiS;
	float denom = a - b;
	float t = (abs(denom) > 0.00001) ? clamp(a / denom, 0.0, 1.0) : 1.0;
	return mix(lo, hi, t);
}

// Soft self shadow: marches from the displaced point toward the light. lightTS is the light direction in
// tangent space, scale the same depth scale used for the view march.
float ParallaxSoftShadow(vec2 uv, vec3 lightTS, float scale, int numSteps, float startDepth)
{
	if (lightTS.z <= 0.05)
		return 1.0;

	vec2 ds = lightTS.xy * scale / max(lightTS.z, 0.25);
	float size = 1.0 / float(numSteps);
  #if __VERSION__ >= 130
	if (u_ParallaxParams2.y > 0.0)
	{
		// steps follow the texel footprint of the shift over the shadow height range; numSteps is the cap
		float n = clamp(length(ds * pomTexSize) * startDepth * exp2(-pomStepLod) * u_ParallaxParams2.y, 3.0, float(numSteps));
		numSteps = int(n);
		size = startDepth / float(numSteps);
	}
  #endif
	float shadow = 1.0;
	float height = 0.0;

	for (int i = 0; i < 32; ++i)
	{
		if (i >= numSteps)
			break;

		height += size;

		if (startDepth < height)
			break;

		// positive when the surface at this point is above the ray
		float occ = (startDepth - height) - SampleDepthMarch(uv + ds * height);
		shadow = min(shadow, 1.0 - clamp(occ * 6.0 / height, 0.0, 1.0));
	}

	return shadow;
}
#endif

vec3 CalcDiffuse(vec3 diffuseAlbedo, float NH, float EH, float roughness)
{
#if defined(USE_BURLEY)
	// modified from https://disney-animation.s3.amazonaws.com/library/s2012_pbs_disney_brdf_notes_v2.pdf
	float fd90 = -0.5 + EH * EH * roughness;
	float burley = 1.0 + fd90 * 0.04 / NH;
	burley *= burley;
	return diffuseAlbedo * burley;
#else
	return diffuseAlbedo;
#endif
}

vec3 EnvironmentBRDF(float roughness, float NE, vec3 specular)
{
	// from http://community.arm.com/servlet/JiveServlet/download/96891546-19496/siggraph2015-mmg-renaldas-slides.pdf
	float v = 1.0 - max(roughness, NE);
	v *= v * v;
	return vec3(v) + specular;
}

vec3 CalcSpecular(vec3 specular, float NH, float EH, float roughness)
{
	// from http://community.arm.com/servlet/JiveServlet/download/96891546-19496/siggraph2015-mmg-renaldas-slides.pdf
	float rr = roughness*roughness;
	float rrrr = rr*rr;
	float d = (NH * NH) * (rrrr - 1.0) + 1.0;
	float v = (EH * EH) * (roughness + 0.5) + EPSILON;
	return specular * (rrrr / (4.0 * d * d * v));
}


float CalcLightAttenuation(float point, float normDist)
{
	// zero light at 1.0, approximating q3 style
	// also don't attenuate directional light
	float attenuation = (0.5 * normDist - 1.5) * point + 1.0;

	// clamp attenuation
	#if defined(NO_LIGHT_CLAMP)
	attenuation = max(attenuation, 0.0);
	#else
	attenuation = clamp(attenuation, 0.0, 1.0);
	#endif

	return attenuation;
}

#if defined(USE_LIGHT_VECTOR)
// codmod: spotlight cone. lightToFrag is normalized direction from light → fragment.
// u_LightSpot.w < -1.5 → omnidirectional (disabled).
uniform vec4 u_LightSpot;

float CalcSpotAttenuation(vec3 lightToFrag)
{
	if (u_LightSpot.w < -1.5)
		return 1.0;
	float cd = dot(normalize(u_LightSpot.xyz), lightToFrag);
	float inner = mix(u_LightSpot.w, 1.0, 0.35);
	return smoothstep(u_LightSpot.w, inner, cd);
}
#endif

#if defined(USE_BOX_CUBEMAP_PARALLAX)
vec4 hitCube(vec3 ray, vec3 pos, vec3 invSize, float lod, samplerCube tex)
{
	// find any hits on cubemap faces facing the camera
	vec3 scale = (sign(ray) - pos) / ray;

	// find the nearest hit
	float minScale = min(min(scale.x, scale.y), scale.z);

	// if the nearest hit is behind the camera, ignore
	// should not be necessary as long as pos is inside the cube
	//if (minScale < 0.0)
		//return vec4(0.0);

	// calculate the hit position, that's our texture coordinates
	vec3 tc = pos + ray * minScale;

	// if the texture coordinates are outside the cube, ignore
	// necessary since we're not fading out outside the cube
	if (any(greaterThan(abs(tc), vec3(1.00001))))
		return vec4(0.0);

	// fade out when approaching the cubemap edges
	//vec3 fade3 = abs(pos);
	//float fade = max(max(fade3.x, fade3.y), fade3.z);
	//fade = clamp(1.0 - fade, 0.0, 1.0);
			
	//return vec4(textureCubeLod(tex, tc, lod).rgb * fade, fade);
	return vec4(textureCubeLod(tex, tc, lod).rgb, 1.0);
}
#endif

void main()
{
	vec3 viewDir, lightColor, ambientColor, reflectance;
	vec3 L, N, E, H;
	float NL, NH, NE, EH, attenuation;

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
	vec3 surfNormal = (!gl_FrontFacing ? var_Normal : -var_Normal).xyz;
	mat3 tangentToWorld = mat3(var_Tangent.xyz, var_Bitangent.xyz, surfNormal);
	viewDir = vec3(var_Normal.w, var_Tangent.w, var_Bitangent.w);
	E = normalize(viewDir);
#endif

	lightColor = var_Color.rgb;

#if defined(USE_LIGHTMAP)
	vec4 lightmapColor = texture2D(u_LightMap, var_TexCoords.zw);
  #if defined(RGBM_LIGHTMAP)
	lightmapColor.rgb *= lightmapColor.a;
  #endif
  #if defined(USE_PBR) && !defined(USE_FAST_LIGHT)
	lightmapColor.rgb *= lightmapColor.rgb;
  #endif
	lightColor *= lightmapColor.rgb;
#endif

	float aoValue = 1.0;
	if (u_AoParams.x > 0.5)
		aoValue = texture2D(u_AoMap, gl_FragCoord.xy * r_FBufScale).r;

  #if !defined(USE_LIGHT_VECTOR)
	lightColor *= aoValue;
  #endif

	vec2 texCoords = var_TexCoords.xy;

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
	// codmod: light vector and attenuation come first so the parallax march can be skipped for unlit pixels
	L = var_LightDir.xyz;
  #if defined(USE_DELUXEMAP)
	L += (texture2D(u_DeluxeMap, var_TexCoords.zw).xyz - vec3(0.5)) * u_EnableTextures.y;
  #endif
	float sqrLightDist = dot(L, L);
	// codmod: |L| of the baked/grid light vector encodes directionality (tr_light.c R_LightDirForPoint):
	// 0.25 = no real direction, 1.0 = fully directional
	float lightDirectional = clamp((sqrt(sqrLightDist) - 0.25) * (1.0 / 0.75), 0.0, 1.0);
	L /= sqrt(sqrLightDist);

  #if defined(USE_LIGHT_VECTOR)
	attenuation  = CalcLightAttenuation(float(var_LightDir.w > 0.0), var_LightDir.w / sqrLightDist);
	attenuation *= CalcSpotAttenuation(-L);
	// codmod: flashlight shadow mask
	if (u_AoParams.y > 0.5)
		attenuation *= texture2D(u_AoMap, gl_FragCoord.xy * r_FBufScale).r;
  #else
	attenuation  = 1.0;
  #endif
#endif


#if !defined(USE_PARALLAXMAP)
	#define POM_FETCH(s, t) texture2D(s, t)
#else
	pomDx = dFdx(texCoords);
	pomDy = dFdy(texCoords);
  #if __VERSION__ >= 130
	{
		// texel footprint of the pixel on the normal map (uniform flow): minor/major axis LODs
		pomTexSize = vec2(textureSize(u_NormalMap, 0));
		float fpX = length(pomDx * pomTexSize);
		float fpY = length(pomDy * pomTexSize);
		float lodMin = log2(max(min(fpX, fpY), 1.0));
		float lodMax = log2(max(max(fpX, fpY), 1.0));
		float lodAvg = 0.5 * (lodMin + lodMax);
		if (u_ParallaxParams2.z >= 0.0)
		{
			pomMarchLod = max(lodAvg, u_ParallaxParams2.z);
			pomStepLod = pomMarchLod;
		}
		else
			pomStepLod = lodAvg;
	}
  #endif

	float pomShadowFade = 1.0; // distance fade of the parallax self shadows
	float pomScale = 0.0;
	float pomCavity = 1.0;
	float pomStartDepth = -1.0; // SampleDepth at the displaced point, fetched at most once
  #if defined(USE_LIGHT_VECTOR)
	const float pomStepFactor = 0.5;
  #else
	const float pomStepFactor = 1.0;
  #endif
	{
		vec3 tv = E * tangentToWorld;
		float distFade = (u_ParallaxParams.y > 0.0)
			? 1.0 - smoothstep(u_ParallaxParams.y * 0.6, u_ParallaxParams.y, length(viewDir))
			: 1.0;
		// fade the depth out toward grazing angles, where the march is least reliable
		float fade = distFade * smoothstep(0.0, 0.15, tv.z);
		// shaders shared between floors and walls: floors get shallower depth
		if (u_ParallaxParams2.w > 0.0 && u_ParallaxParams.y > 0.0)
		{
			float shEnd = u_ParallaxParams2.w * u_ParallaxParams.y;
			pomShadowFade = 1.0 - smoothstep(0.5 * shEnd, shEnd, length(viewDir));
		}
		float floorMix = mix(1.0, u_ParallaxParams.z, abs(surfNormal.z));
		pomScale = u_NormalScale.a * floorMix * fade;
	  #if defined(USE_LIGHT_VECTOR) && defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
		// no dynamic light reaches this pixel: skip both marches
		if (attenuation <= 0.0)
			pomScale = 0.0;
	  #endif

		if (pomScale > 0.0005)
		{
			// more steps at grazing angles, fewer face-on
			float maxSteps = clamp(u_ParallaxParams.x * pomStepFactor, 8.0, 128.0);
			float steps = mix(maxSteps, max(8.0, maxSteps * 0.25), clamp(tv.z, 0.0, 1.0));
			vec2 ds = -tv.xy * pomScale / max(tv.z, 0.25);
			float pomSpan = (u_ParallaxParams2.x > 0.0) ? min(u_ParallaxParams2.x, 1.0) : 1.0;
		  #if __VERSION__ >= 130
			if (u_ParallaxParams2.y > 0.0)
				steps = clamp(length(ds * pomTexSize) * pomSpan * exp2(-pomStepLod) * u_ParallaxParams2.y, 4.0, steps);
		  #endif
			float pomD = ParallaxMarch(texCoords, ds, steps, pomSpan);
			texCoords += ds * (pomD - r_parallaxMapOffset);
			pomCavity = 1.0 - clamp(u_ParallaxParams.w, 0.0, 1.0) * pomD * distFade;
		}
	}
#endif

	vec4 diffuse = POM_FETCH(u_DiffuseMap, texCoords);
	
	float alpha = diffuse.a * var_Color.a;
	if (u_AlphaTest == 1)
	{
		if (alpha == 0.0)
			discard;
	}
	else if (u_AlphaTest == 2)
	{
		if (alpha >= 0.5)
			discard;
	}
	else if (u_AlphaTest == 3)
	{
		if (alpha < 0.5)
			discard;
	}

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
  #if defined(USE_NORMALMAP)
	// codmod: uniform branch, stages without a normal map skip the fetch
	if (u_EnableTextures.x > 0.0)
	{
    #if defined(SWIZZLE_NORMALMAP)
		N.xy = POM_FETCH(u_NormalMap, texCoords).ag - vec2(0.5);
    #else
		N.xy = POM_FETCH(u_NormalMap, texCoords).rg - vec2(0.5);
    #endif
		N.xy *= u_NormalScale.xy;
		N.z = sqrt(clamp((0.25 - N.x * N.x) - N.y * N.y, 0.0, 1.0));
		N = tangentToWorld * N;
	}
	else
		N = surfNormal;
  #else
	N = surfNormal;
  #endif

	N = normalize(N);

	// codmod: geometric specular AA (Kaplanyan 2016 / Tokuyoshi-Kaplanyan 2019). Derivatives taken here, in
	// uniform flow, before any branch. u_AoParams.w = r_specularAA strength (0 = off).
	vec3 dNdx = dFdx(N);
	vec3 dNdy = dFdy(N);
	float specAAVar = dot(dNdx, dNdx) + dot(dNdy, dNdy);

  #if defined(USE_SHADOWMAP) 
	vec2 shadowTex = gl_FragCoord.xy * r_FBufScale;
	float shadowValue = texture2D(u_ShadowMap, shadowTex).r;

	// surfaces not facing the light are always shadowed
	shadowValue *= clamp(dot(N, var_PrimaryLightDir.xyz), 0.0, 1.0);

    #if defined(SHADOWMAP_MODULATE)
	lightColor *= shadowValue * (1.0 - u_PrimaryLightAmbient.r) + u_PrimaryLightAmbient.r;
    #endif
  #endif

  #if defined(USE_PARALLAXMAP) && defined(USE_PARALLAXMAP_SHADOWS)
	// flashlight / dlight, or the baked light direction when it is a real one (|L| ~ 0.25 means no direction)
	if (pomScale > 0.0005)
	{
	  #if defined(USE_LIGHT_VECTOR)
		float pomShadowW = pomShadowFade;
	  #else
		float pomShadowW = lightDirectional * pomShadowFade;
	  #endif
		if (pomShadowW > 0.05)
		{
			int shadowSteps = int(clamp(u_ParallaxParams.x * 0.4 * pomStepFactor, 8.0, 24.0));
			if (pomStartDepth < 0.0)
				pomStartDepth = SampleDepth(texCoords);
			attenuation *= mix(1.0, ParallaxSoftShadow(texCoords, L * tangentToWorld, pomScale, shadowSteps, pomStartDepth), pomShadowW);
		}
	}
  #endif


  #if !defined(USE_LIGHT_VECTOR)
	ambientColor = lightColor;
	float surfNL = clamp(dot(surfNormal, L), 0.0, 1.0);

	// reserve 25% ambient to avoid black areas on normalmaps
	lightColor *= 0.75;

	// Scale the incoming light to compensate for the baked-in light angle
	// attenuation.
	lightColor /= max(surfNL, 0.25);

	// Recover any unused light as ambient, in case attenuation is over 4x or
	// light is below the surface
	ambientColor = max(ambientColor - lightColor * surfNL, vec3(0.0));
  #else
	ambientColor = var_ColorAmbient.rgb * aoValue;
  #endif

  #if defined(USE_PARALLAXMAP)
	// cheap cavity darkening: recessed texels get less ambient
	ambientColor *= pomCavity;
  #endif

	NL = clamp(dot(N, L), 0.0, 1.0);
	NE = clamp(dot(N, E), 0.0, 1.0);
	H = normalize(L + E);
	EH = clamp(dot(E, H), 0.0, 1.0);
	NH = clamp(dot(N, H), 0.0, 1.0);

  #if defined(USE_SPECULARMAP)
	// codmod: uniform branch, surfaces without a spec map skip the fetch (white image * scale otherwise)
	vec4 specular = (u_EnableTextures.z > 0.0) ? POM_FETCH(u_SpecularMap, texCoords) : vec4(1.0);
  #else
	vec4 specular = vec4(1.0);
  #endif
	specular *= u_SpecularScale;

  #if defined(USE_PBR)
	diffuse.rgb *= diffuse.rgb;
  #endif

  #if defined(USE_PBR)
	// diffuse rgb is base color
	// specular red is gloss
	// specular green is metallicness
	float gloss = specular.r;
	float metal = specular.g;
	specular.rgb = metal * diffuse.rgb + vec3(0.04 - 0.04 * metal);
	diffuse.rgb *= 1.0 - metal;
  #else
	// diffuse rgb is diffuse
	// specular rgb is specular reflectance at normal incidence
	// specular alpha is gloss
	float gloss = specular.a;

	// adjust diffuse by specular reflectance, to maintain energy conservation
	diffuse.rgb *= vec3(1.0) - specular.rgb;
  #endif

  #if defined(GLOSS_IS_GLOSS)
	float roughness = exp2(-3.0 * gloss);
  #elif defined(GLOSS_IS_SMOOTHNESS)
	float roughness = 1.0 - gloss;
  #elif defined(GLOSS_IS_ROUGHNESS)
	float roughness = gloss;
  #elif defined(GLOSS_IS_SHININESS)
	float roughness = pow(2.0 / (8190.0 * gloss + 2.0), 0.25);
  #endif

	// codmod: widen the specular lobe by the screen-space normal variance: alpha2' = alpha2 + min(2 * 0.25 * s * var, 0.18)
	// CalcSpecular squares roughness to get alpha, and uses alpha^2 in the GGX D term, so alpha2 = roughness^4.
	float specRoughness = roughness;
	if (u_AoParams.w > 0.0)
	{
		float alpha2 = roughness * roughness;
		alpha2 *= alpha2;
		alpha2 = clamp(alpha2 + min(0.5 * u_AoParams.w * specAAVar, 0.18), 0.0, 1.0);
		specRoughness = sqrt(sqrt(alpha2));
	}

  #if defined(USE_LIGHT_VECTOR)
	// codmod: near-black albedo barely reflects the flashlight; lift it for dynamic light only
	if (u_AoParams.z > 0.0)
		reflectance  = CalcDiffuse(pow(max(diffuse.rgb, vec3(0.001)), vec3(u_AoParams.z)), NH, EH, roughness);
	else
  #endif
	reflectance  = CalcDiffuse(diffuse.rgb, NH, EH, roughness);

  #if defined(r_deluxeSpecular)
    #if defined(USE_LIGHT_VECTOR)
	reflectance += CalcSpecular(specular.rgb, NH, EH, specRoughness) * r_deluxeSpecular;
    #else
	// codmod: only where a real directional light exists, never from the N~L~E degenerate case
	reflectance += CalcSpecular(specular.rgb, NH, EH, pow(specRoughness, r_deluxeSpecular)) * lightDirectional;
    #endif
  #endif

	gl_FragColor.rgb  = lightColor   * reflectance * (attenuation * NL);
	gl_FragColor.rgb += ambientColor * diffuse.rgb;

  #if defined(USE_CUBEMAP)
	// codmod: uniform branch, surfaces without a probe skip the BRDF and the cubemap fetch
	if (u_EnableTextures.w > 0.0)
	{
	reflectance = EnvironmentBRDF(specRoughness, NE, specular.rgb);

	vec3 R = reflect(E, N);

	// parallax corrected cubemap (cheaper trick)
	// from http://seblagarde.wordpress.com/2012/09/29/image-based-lighting-approaches-and-parallax-corrected-cubemap/
	vec3 parallax = u_CubeMapInfo.xyz + u_CubeMapInfo.w * viewDir;

  #if defined(USE_BOX_CUBEMAP_PARALLAX)
	vec3 cubeLightColor = hitCube(R * u_CubeMapInfo.w, parallax, u_CubeMapInfo.www, ROUGHNESS_MIPS * specRoughness, u_CubeMap).rgb * u_EnableTextures.w;
  #else
	vec3 cubeLightColor = textureCubeLod(u_CubeMap, R + parallax, ROUGHNESS_MIPS * specRoughness).rgb * u_EnableTextures.w;
  #endif

	// normalize cubemap based on last roughness mip (~diffuse)
	// multiplying cubemap values by lighting below depends on either this or the cubemap being normalized at generation
	//vec3 cubeLightDiffuse = max(textureCubeLod(u_CubeMap, N, ROUGHNESS_MIPS).rgb, 0.5 / 255.0);
	//cubeLightColor /= dot(cubeLightDiffuse, vec3(0.2125, 0.7154, 0.0721));

    #if defined(USE_PBR)
	cubeLightColor *= cubeLightColor;
    #endif

	// multiply cubemap values by lighting
	// not technically correct, but helps make reflections look less unnatural
	//cubeLightColor *= lightColor * (attenuation * NL) + ambientColor;

	gl_FragColor.rgb += cubeLightColor * reflectance;
	}
  #endif

  #if defined(USE_PRIMARY_LIGHT) || defined(SHADOWMAP_MODULATE)
	vec3 L2, H2;
	float NL2, EH2, NH2;

	L2 = var_PrimaryLightDir.xyz;

	// enable when point lights are supported as primary lights
	//sqrLightDist = dot(L2, L2);
	//L2 /= sqrt(sqrLightDist);

	NL2 = clamp(dot(N, L2), 0.0, 1.0);
	H2 = normalize(L2 + E);
	EH2 = clamp(dot(E, H2), 0.0, 1.0);
	NH2 = clamp(dot(N, H2), 0.0, 1.0);

	reflectance  = CalcSpecular(specular.rgb, NH2, EH2, specRoughness);

	// bit of a hack, with modulated shadowmaps, ignore diffuse
    #if !defined(SHADOWMAP_MODULATE)
	reflectance += CalcDiffuse(diffuse.rgb, NH2, EH2, roughness);
    #endif

	lightColor = u_PrimaryLightColor;

    #if defined(USE_SHADOWMAP)
	lightColor *= shadowValue;
    #endif

	// enable when point lights are supported as primary lights
	//lightColor *= CalcLightAttenuation(float(u_PrimaryLightDir.w > 0.0), u_PrimaryLightDir.w / sqrLightDist);

  #if defined(USE_PARALLAXMAP) && defined(USE_PARALLAXMAP_SHADOWS) && defined(USE_PARALLAXMAP_SUNSHADOWS)
	if (pomScale > 0.0005 && pomShadowFade > 0.0)
	{
		if (pomStartDepth < 0.0)
			pomStartDepth = SampleDepth(texCoords);
		lightColor *= mix(1.0, ParallaxSoftShadow(texCoords, L2 * tangentToWorld, pomScale, int(clamp(u_ParallaxParams.x * 0.4 * pomStepFactor, 8.0, 24.0)), pomStartDepth), pomShadowFade);
	}
  #endif

	gl_FragColor.rgb += lightColor * reflectance * NL2;
  #endif

  #if defined(USE_PBR)
	gl_FragColor.rgb = sqrt(gl_FragColor.rgb);
  #endif

#else

	gl_FragColor.rgb = diffuse.rgb * lightColor;

#endif

	gl_FragColor.a = alpha;
}
