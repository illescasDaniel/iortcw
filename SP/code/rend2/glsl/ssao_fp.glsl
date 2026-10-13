uniform sampler2D u_ScreenDepthMap;

uniform vec4   u_ViewInfo;   // zfar / znear, zfar, tan(fovX / 2), tan(fovY / 2)
uniform vec4   u_SsaoParams; // radius (world units), strength, then: hemisphere: bias, samples; GTAO: slices, steps per side
uniform vec2   u_InvTexRes;  // 1 / size of the depth texture

varying vec2   var_ScreenTex;

#define MAX_SAMPLES 32

// The view model is drawn with glDepthRange(0, 0.3), so everything it writes to
// the depth buffer is <= 0.3. Real geometry that close to the camera can't exist
// (znear 4 -> 0.3 is ~6 units away), so such depth is the weapon: not occluded
// and not an occluder.
#define VIEWMODEL_DEPTH 0.3
#define SKY_DEPTH       0.99999

// Interleaved gradient noise (Jimenez 2014): well-distributed, cheap, no trig
float noise(const vec2 pixel)
{
	return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// view space depth in world units, or -1.0 for view model / sky pixels
float viewDepth(const vec2 tex)
{
	float d = texture2D(u_ScreenDepthMap, tex).r;

	if (d < VIEWMODEL_DEPTH || d > SKY_DEPTH)
		return -1.0;

	return u_ViewInfo.y / mix(u_ViewInfo.x, 1.0, d);
}

vec3 viewPos(const vec2 tex, const float z)
{
	return vec3((tex * 2.0 - 1.0) * u_ViewInfo.zw * z, z);
}

// normal from the smaller depth difference on each axis, so it does not smear
// across silhouette edges
vec3 viewNormal(const vec2 tex, const vec3 pc)
{
	vec2 t = u_InvTexRes;
	float zl = viewDepth(tex - vec2(t.x, 0.0));
	float zr = viewDepth(tex + vec2(t.x, 0.0));
	float zd = viewDepth(tex - vec2(0.0, t.y));
	float zu = viewDepth(tex + vec2(0.0, t.y));

	vec3 dx, dy;

	if (zr > 0.0 && (zl < 0.0 || abs(zr - pc.z) < abs(pc.z - zl)))
		dx = viewPos(tex + vec2(t.x, 0.0), zr) - pc;
	else if (zl > 0.0)
		dx = pc - viewPos(tex - vec2(t.x, 0.0), zl);
	else
		dx = vec3(1.0, 0.0, 0.0);

	if (zu > 0.0 && (zd < 0.0 || abs(zu - pc.z) < abs(pc.z - zd)))
		dy = viewPos(tex + vec2(0.0, t.y), zu) - pc;
	else if (zd > 0.0)
		dy = pc - viewPos(tex - vec2(0.0, t.y), zd);
	else
		dy = vec3(0.0, 1.0, 0.0);

	vec3 n = normalize(cross(dx, dy));

	// face the camera (the view ray, not the z axis: off axis pixels differ)
	return dot(n, pc) > 0.0 ? -n : n;
}

#if defined(USE_GTAO)

// Ground Truth Ambient Occlusion (Jimenez et al. 2016, "Practical Real-Time
// Strategies for Accurate Indirect Occlusion"), following the structure of Intel's
// XeGTAO: per pixel a few screen space slices, on each slice the maximum horizon
// angle on both sides, then the exact cosine weighted visible arc between them.

#define PI      3.14159265
#define HALF_PI 1.57079633
#define MAX_SLICES 4
#define MAX_STEPS  12

void main()
{
	vec2 tex = var_ScreenTex;
	float z = viewDepth(tex);

	if (z < 0.0)
	{
		gl_FragColor = vec4(1.0);
		return;
	}

	float radius = u_SsaoParams.x;
	float strength = u_SsaoParams.y;
	int numSlices = int(u_SsaoParams.z);
	int numSteps = int(u_SsaoParams.w);

	// world radius -> screen space radius, so it does not depend on the FOV
	vec2 screenRadius = radius / z * 0.5 / u_ViewInfo.zw;
	screenRadius = min(screenRadius, vec2(0.15));

	// too small on screen to see anything: far away
	if (screenRadius.x < 2.0 * u_InvTexRes.x)
	{
		gl_FragColor = vec4(1.0);
		return;
	}

	vec3 pc = viewPos(tex, z);
	vec3 n = viewNormal(tex, pc);
	vec3 v = normalize(-pc);

	float noiseSlice = noise(gl_FragCoord.xy);
	float noiseStep = noise(gl_FragCoord.xy + vec2(5.588238));

	float invRadius = 1.0 / radius;
	float visibility = 0.0;

	for (int slice = 0; slice < MAX_SLICES; slice++)
	{
		if (slice >= numSlices)
			break;

		float phi = (float(slice) + noiseSlice) / float(numSlices) * PI;
		vec2 omega = vec2(cos(phi), sin(phi));

		// the slice plane contains the view vector and this direction
		vec3 dirVec = vec3(omega, 0.0);
		vec3 orthoDir = dirVec - dot(dirVec, v) * v;
		vec3 axis = normalize(cross(dirVec, v));
		vec3 projN = n - axis * dot(n, axis);
		float projLen = length(projN);

		if (projLen < 0.0001)
		{
			visibility += 1.0;
			continue;
		}

		float signN = dot(orthoDir, projN) < 0.0 ? -1.0 : 1.0;
		float cosN = clamp(dot(projN, v) / projLen, 0.0, 1.0);
		float angN = signN * acos(cosN);

		// horizon cosines start at the tangent plane, so only real occluders raise them
		float lowCos0 = cos(angN + HALF_PI);
		float lowCos1 = cos(angN - HALF_PI);
		float horizonCos0 = lowCos0;
		float horizonCos1 = lowCos1;

		for (int i = 0; i < MAX_STEPS; i++)
		{
			if (i >= numSteps)
				break;

			// more taps close to the pixel
			float t = (float(i) + noiseStep) / float(numSteps);
			t *= t;
			vec2 off = omega * screenRadius * (t + 0.5 * u_InvTexRes.x / screenRadius.x);

			vec2 tex0 = tex + off;
			vec2 tex1 = tex - off;
			float z0 = viewDepth(tex0);
			float z1 = viewDepth(tex1);

			if (z0 > 0.0)
			{
				vec3 d = viewPos(tex0, z0) - pc;
				float len = length(d);

				if (len > 0.01)
				{
					float shc = dot(d, v) / len;
					float w = clamp(1.0 - len * invRadius, 0.0, 1.0);
					w *= w;

					horizonCos0 = max(horizonCos0, mix(lowCos0, shc, w));
				}
			}

			if (z1 > 0.0)
			{
				vec3 d = viewPos(tex1, z1) - pc;
				float len = length(d);

				if (len > 0.01)
				{
					float shc = dot(d, v) / len;
					float w = clamp(1.0 - len * invRadius, 0.0, 1.0);
					w *= w;

					horizonCos1 = max(horizonCos1, mix(lowCos1, shc, w));
				}
			}
		}

		float h0 = -acos(clamp(horizonCos1, -1.0, 1.0));
		float h1 = acos(clamp(horizonCos0, -1.0, 1.0));

		h0 = angN + clamp(h0 - angN, -HALF_PI, HALF_PI);
		h1 = angN + clamp(h1 - angN, -HALF_PI, HALF_PI);

		float sinN = sin(angN);
		float arc0 = (cosN + 2.0 * h0 * sinN - cos(2.0 * h0 - angN)) * 0.25;
		float arc1 = (cosN + 2.0 * h1 * sinN - cos(2.0 * h1 - angN)) * 0.25;

		visibility += projLen * (arc0 + arc1);
	}

	visibility = clamp(visibility / float(numSlices), 0.0, 1.0);

	// GTAO is physically based: the power darkens it to taste (and to match the look
	// of a lit, not ambient only, scene)
	gl_FragColor = vec4(vec3(pow(visibility, 2.0 * strength)), 1.0);
}

#else

void main()
{
	vec2 tex = var_ScreenTex;
	float z = viewDepth(tex);

	if (z < 0.0)
	{
		gl_FragColor = vec4(1.0);
		return;
	}

	float radius = u_SsaoParams.x;
	float strength = u_SsaoParams.y;
	float bias = u_SsaoParams.z;
	int numSamples = int(u_SsaoParams.w);

	vec3 pc = viewPos(tex, z);
	vec3 n = viewNormal(tex, pc);

	// world radius -> screen space radius, so it does not depend on the FOV
	vec2 screenRadius = radius / z * 0.5 / u_ViewInfo.zw;
	screenRadius = min(screenRadius, vec2(0.12));

	float angle0 = noise(gl_FragCoord.xy) * 6.2831853;
	float invSamples = 1.0 / float(numSamples);
	float invRadius2 = 1.0 / (radius * radius);

	float occlusion = 0.0;

	for (int i = 0; i < MAX_SAMPLES; i++)
	{
		if (i >= numSamples)
			break;

		// spiral with a golden angle step, sqrt() for an even area distribution
		float a = angle0 + float(i) * 2.3999632;
		float r = sqrt((float(i) + 0.5) * invSamples);
		vec2 stex = tex + vec2(cos(a), sin(a)) * r * screenRadius;

		float sz = viewDepth(stex);

		if (sz < 0.0)
			continue;

		vec3 v = viewPos(stex, sz) - pc;
		float dist2 = dot(v, v);

		// how far above the surface plane the sample is, ignoring points on it
		float height = dot(n, v) * inversesqrt(dist2 + 0.0001) - bias;

		// fade out with distance so far away depth jumps do not leave halos
		float falloff = clamp(1.0 - dist2 * invRadius2, 0.0, 1.0);

		occlusion += max(height, 0.0) * falloff;
	}

	occlusion = occlusion * invSamples * 2.0 * strength;

	gl_FragColor = vec4(vec3(clamp(1.0 - occlusion, 0.0, 1.0)), 1.0);
}

#endif
