uniform sampler2D u_ScreenDepthMap; // full resolution scene depth (raw)

uniform vec2   u_InvTexRes;   // 1 / size of the scene depth
uniform vec4   u_WarpParams;  // x = gun depth threshold (raw), y = zNear, z = zFar, w = grid step in pixels

varying vec2   var_Tex1;

// one R32F grid texel per step x step source pixels: nearest forward (view space) distance over the footprint
// result: > 0 linear depth, 0 = sky, -1 = only view model taps
void main()
{
	float step_px = u_WarpParams.w;
	vec2 size = 1.0 / u_InvTexRes;
	vec2 base = floor(gl_FragCoord.xy) * step_px + 0.5;

	float half_px = step_px * 0.5;
	int taps = int(half_px + 0.5) + 1;      // taps every 2 px across -step/2 .. +step/2
	float nearest = 1.0e30;
	bool anyValid = false;
	bool anySolid = false;

	for (int j = 0; j < 9; j++)
	{
		if (j >= taps)
			break;
		for (int i = 0; i < 9; i++)
		{
			if (i >= taps)
				break;

			vec2 p = base + (vec2(float(i), float(j)) * 2.0 - half_px);
			p = clamp(p, vec2(0.5), size - 0.5);

			float d = texture2D(u_ScreenDepthMap, p * u_InvTexRes).r;
			if (d < u_WarpParams.x)
				continue;       // view model

			anyValid = true;
			if (d >= 1.0 - 5.9604645e-8)
				continue;       // sky counts as infinity

			float ndc = d * 2.0 - 1.0;
			float z = 2.0 * u_WarpParams.y * u_WarpParams.z / ((u_WarpParams.z + u_WarpParams.y) - ndc * (u_WarpParams.z - u_WarpParams.y));
			nearest = min(nearest, z);
			anySolid = true;
		}
	}

	float result = -1.0;
	if (anyValid)
		result = anySolid ? nearest : 0.0;

	gl_FragColor = vec4(result, 0.0, 0.0, 1.0);
}
