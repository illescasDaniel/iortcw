#if defined(GL_ARB_texture_gather)
#extension GL_ARB_texture_gather : enable
#define USE_GATHER
#endif

uniform sampler2D u_ScreenImageMap; // half resolution AO
uniform sampler2D u_ScreenDepthMap; // full resolution depth
uniform sampler2D u_HalfDepthMap;   // half resolution depth, the one the AO was computed with

uniform vec4   u_ViewInfo;  // zfar / znear, zfar
uniform vec2   u_InvTexRes; // 1 / size of the AO image

varying vec2   var_Tex1;

// same depth encoding rules as ssao_fp.glsl: the view model (<= 0.3) and the sky
// (~1.0) carry no occlusion
#define VIEWMODEL_DEPTH 0.3
#define SKY_DEPTH       0.99999

// AO fades out with distance, it is pointless (and wrong) on far geometry and fog
#define FADE_START 1500.0
#define FADE_END   4000.0

float linearDepth(const float d)
{
	if (d < VIEWMODEL_DEPTH || d > SKY_DEPTH)
		return -1.0;

	return u_ViewInfo.y / mix(u_ViewInfo.x, 1.0, d);
}

float viewDepth(const sampler2D depthMap, const vec2 tex)
{
	return linearDepth(texture2D(depthMap, tex).r);
}

void main()
{
	float z = viewDepth(u_ScreenDepthMap, var_Tex1);

	if (z < 0.0)
	{
		gl_FragColor = vec4(1.0);
		return;
	}

	// bilateral upsample: bilinear weights of the 4 nearest AO texels, scaled by
	// how close each texel's depth is to this pixel's depth, so AO from the other
	// side of a silhouette does not bleed over
	vec2 size = 1.0 / u_InvTexRes;
	vec2 p = var_Tex1 * size - 0.5;
	vec2 base = floor(p);
	vec2 f = p - base;

	float sum = 0.0;
	float total = 0.0;
	float plain = 0.0;

#if defined(USE_GATHER)
	// the same 4 texels with 2 fetches instead of 8; gather order is
	// (0,1) (1,1) (1,0) (0,0), relative to base
	vec2 gtc = (base + 1.0) * u_InvTexRes;
	vec4 aos = textureGather(u_ScreenImageMap, gtc).wzxy;
	vec4 zs = textureGather(u_HalfDepthMap, gtc).wzxy;
#endif

	for (int i = 0; i < 4; i++)
	{
		vec2 offs = vec2(float(i - (i / 2) * 2), float(i / 2));

		vec2 b = mix(1.0 - f, f, offs);
		float bilinear = b.x * b.y;

#if defined(USE_GATHER)
		float ao = aos[i];
		float zi = linearDepth(zs[i]);
#else
		vec2 tc = (base + offs + 0.5) * u_InvTexRes;
		float ao = texture2D(u_ScreenImageMap, tc).r;
		float zi = viewDepth(u_HalfDepthMap, tc);
#endif

		plain += ao * bilinear;

		if (zi < 0.0)
			continue;

		float w = bilinear / (0.01 + abs(zi - z) / z);

		sum += ao * w;
		total += w;
	}

	float ao = total > 0.0 ? sum / total : plain;
	float fade = 1.0 - smoothstep(FADE_START, FADE_END, z);

	gl_FragColor = vec4(vec3(mix(1.0, ao, fade)), 1.0);
}
