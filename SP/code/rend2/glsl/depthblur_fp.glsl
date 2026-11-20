uniform sampler2D u_ScreenImageMap;
uniform sampler2D u_ScreenDepthMap;

// zfar / znear, zfar, 1/width, 1/height. The SSAO blur passes set 1/height < 0 for r_ssaoDepthInAO (the image's
// g channel already holds the raw depth, so a tap is one fetch). Other users leave it positive.
uniform vec4   u_ViewInfo;
varying vec2   var_ScreenTex;

//float gauss[8] = float[8](0.17, 0.17, 0.16, 0.14, 0.12, 0.1, 0.08, 0.06);
//float gauss[5] = float[5](0.30, 0.23, 0.097, 0.024, 0.0033);
//float gauss[4] = float[4](0.40, 0.24, 0.054, 0.0044);
//float gauss[3] = float[3](0.60, 0.19, 0.0066);
#define BLUR_SIZE 4

#if !defined(USE_DEPTH)
//#define USE_GAUSS
#endif

float getLinearDepth(sampler2D depthMap, const vec2 tex, const float zFarDivZNear)
{
	float sampleZDivW = texture2D(depthMap, tex).r;
	return 1.0 / mix(zFarDivZNear, 1.0, sampleZDivW);
}

vec4 depthGaussian1D(sampler2D imageMap, sampler2D depthMap, vec2 tex, float zFarDivZNear, float zFar, vec2 scale)
{
	float gauss[4];

	gauss[0] = 0.40;
	gauss[1] = 0.24;
	gauss[2] = 0.054;
	gauss[3] = 0.0044;

#if defined(USE_DEPTH)
	bool packedDepth = scale.y < 0.0;

	scale = abs(scale);
	vec4 centerTex = texture2D(imageMap, tex);
	float depthCenter;

	if (packedDepth)
		depthCenter = 1.0 / mix(zFarDivZNear, 1.0, centerTex.g);
	else
		depthCenter = getLinearDepth(depthMap, tex, zFarDivZNear);
	vec2 slope = vec2(dFdx(depthCenter), dFdy(depthCenter)) / vec2(dFdx(tex.x), dFdy(tex.y));
	scale /= clamp(zFarDivZNear * depthCenter / 32.0, 1.0, 2.0);
#endif

#if defined(USE_HORIZONTAL_BLUR)
	vec2 direction = vec2(scale.x * 2.0, 0.0);
	vec2 nudge = vec2(0.0, scale.y * 0.5);
#else // if defined(USE_VERTICAL_BLUR)
	vec2 direction = vec2(0.0, scale.y * 2.0);
	vec2 nudge = vec2(-scale.x * 0.5, 0.0);
#endif

#if defined(USE_GAUSS)
	vec4 result = texture2D(imageMap, tex) * gauss[0];
	float total = gauss[0];
#else
#if defined(USE_DEPTH)
	vec4 result = centerTex;
#else
	vec4 result = texture2D(imageMap, tex);
#endif
	float total = 1.0;
#endif

#if defined(USE_DEPTH)
	// depth tolerance relative to the distance (3% + 1.5 units): tight close up
	// where edges are visible, looser far away where it is only noise
	float zLimit = depthCenter * 0.03 + 1.5 / zFar;
#else
	float zLimit = 5.0 / zFar;
#endif
	int i, j;
	for (i = 0; i < 2; i++)
	{
		for (j = 1; j < BLUR_SIZE; j++)
		{
			vec2 offset = direction * (float(j) - 0.25) + nudge;
#if defined(USE_DEPTH)
			vec4 smp = texture2D(imageMap, tex + offset);
			float depthSample;

			if (packedDepth)
				depthSample = 1.0 / mix(zFarDivZNear, 1.0, smp.g);
			else
				depthSample = getLinearDepth(depthMap, tex + offset, zFarDivZNear);

			float depthExpected = depthCenter + dot(slope, offset);
			float useSample = float(abs(depthSample - depthExpected) < zLimit);
#else
			float useSample = 1.0;
#endif
#if defined(USE_GAUSS)
			result += texture2D(imageMap, tex + offset) * (gauss[j] * useSample);
			total += gauss[j] * useSample;
#elif defined(USE_DEPTH)
			result += smp * useSample;
			total += useSample;
#else
			result += texture2D(imageMap, tex + offset) * useSample;
			total += useSample;
#endif
			nudge = -nudge;
		}

		direction = -direction;
		nudge = -nudge;
	}

#if defined(USE_DEPTH)
	// depth-in-AO: only r is an average, g keeps this pixel's own depth for the next pass
	if (packedDepth)
		return vec4(result.r / total, centerTex.g, 0.0, 1.0);
#endif

	return result / total;
}

void main()
{
	gl_FragColor = depthGaussian1D(u_ScreenImageMap, u_ScreenDepthMap, var_ScreenTex, u_ViewInfo.x, u_ViewInfo.y, u_ViewInfo.zw);
}
