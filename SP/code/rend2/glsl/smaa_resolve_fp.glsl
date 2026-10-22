// SMAA T2x temporal resolve (fragment).
// Where the depth prepass wrote a motion vector (entities: moving props, doors, vertex-animated models, the
// viewmodel) the history is fetched at uv - velocity/2. Everywhere else (world, sky; the velocity buffer holds
// exactly 0) the pixel's position is rebuilt from the hardware depth and carried into the previous frame with
// u_ReprojMatrix (previous unjittered viewProj * inverse current unjittered viewProj). The history sample is then
// clamped to the YCoCg bounding box of the current 3x3 neighborhood (kills ghosting on anything that moved on its
// own, or was disoccluded), and its weight drops for fast pixels.
uniform sampler2D u_TextureMap;      // current frame (SMAA 1x output)
uniform sampler2D u_LevelsMap;       // history: previous resolved frame
uniform sampler2D u_ScreenImageMap;  // hardware depth copy (R32F)
uniform sampler2D u_VelocityMap;     // RG16F NDC motion of entities, (0,0) = none

uniform mat4   u_ReprojMatrix;
uniform mat4   u_ReprojRotMatrix;
uniform vec4   u_TemporalParams;     // x = history weight, y = history valid, z = debug (3 clamp, 4 velocity), w = velocity buffer valid
uniform vec4   u_SmaaMetrics;        // 1/w, 1/h, w, h

varying vec2   var_TexCoords;

vec3 ToYCoCg(vec3 c)
{
	return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
	            0.5 * c.r - 0.5 * c.b,
	            -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 FromYCoCg(vec3 c)
{
	return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

void main()
{
	vec2 uv = var_TexCoords;
	vec3 cur = texture2D(u_TextureMap, uv).rgb;

	if (u_TemporalParams.y < 0.5)
	{
		gl_FragColor = vec4(cur, 1.0);
		return;
	}

	ivec2 pix = ivec2(uv * u_SmaaMetrics.zw);
	vec2 vel = vec2(0.0);
	if (u_TemporalParams.w > 0.5)
		vel = texelFetch(u_VelocityMap, pix, 0).rg;
	bool hasVel = (vel.x != 0.0 || vel.y != 0.0);

	if (u_TemporalParams.z > 3.5)
	{
		// velocity view: flat colour per pixel, grey = entity pixel standing still, red/green = x/y motion
		// (4 px per frame saturates), dim image = no velocity (world/sky, camera motion from depth)
		vec2 px = vel * 0.5 * u_SmaaMetrics.zw;
		if (hasVel)
			gl_FragColor = vec4(clamp(vec3(0.5 + px * 0.125, 0.5), 0.0, 1.0), 1.0);
		else
			gl_FragColor = vec4(cur * 0.3 + vec3(0.0, 0.0, 0.12), 1.0);
		return;
	}

	vec2 prevUv;
	bool ok;
	if (hasVel)
	{
		prevUv = uv - vel * 0.5;
		ok = all(greaterThan(prevUv, vec2(0.0))) && all(lessThan(prevUv, vec2(1.0)));
	}
	else
	{
		float d = texelFetch(u_ScreenImageMap, pix, 0).r;
		vec4 p;
		if (d >= 0.99999)
			p = u_ReprojRotMatrix * vec4(uv * 2.0 - 1.0, 1.0, 1.0);   // sky / far: rotation only
		else
			p = u_ReprojMatrix * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);

		prevUv = p.xy / p.w * 0.5 + 0.5;
		ok = p.w > 0.0 && all(greaterThan(prevUv, vec2(0.0))) && all(lessThan(prevUv, vec2(1.0)));
	}
	if (!ok)
	{
		gl_FragColor = vec4(u_TemporalParams.z > 2.5 ? vec3(0.0, 0.0, 0.5) : cur, 1.0);
		return;
	}

	vec3 hist = texture2D(u_LevelsMap, prevUv).rgb;

	// 3x3 neighborhood bounds of the current frame in YCoCg
	vec3 cMin = vec3(1e5), cMax = vec3(-1e5);
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			vec3 n = ToYCoCg(texture2D(u_TextureMap, uv + vec2(float(x), float(y)) * u_SmaaMetrics.xy).rgb);
			cMin = min(cMin, n);
			cMax = max(cMax, n);
		}
	}
	vec3 histY = ToYCoCg(hist);
	vec3 clamped = clamp(histY, cMin, cMax);

	if (u_TemporalParams.z > 2.5)
	{
		// red = how far the history was pulled back, dim image otherwise
		float amount = length(histY - clamped) * 6.0;
		gl_FragColor = vec4(vec3(amount, 0.0, 0.0) + cur * 0.35, 1.0);
		return;
	}

	// fast pixels get less history: the two jitter samples no longer line up and the clamp alone smears
	float speedPx = length((prevUv - uv) * u_SmaaMetrics.zw);
	float weight = u_TemporalParams.x * (1.0 - 0.7 * smoothstep(1.0, 16.0, speedPx));

	gl_FragColor = vec4(mix(cur, FromYCoCg(clamped), weight), 1.0);
}
