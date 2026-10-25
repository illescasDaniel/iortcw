uniform sampler2D u_ScreenImageMap; // captured scene: rgb colour, a = view model mask

uniform mat4   u_WarpMatrix;  // upper left 3x3: new view NDC (x, y, 1) -> old view homogeneous NDC
uniform vec4   u_WarpBox;     // 3D viewport in pixels: x, y, w, h
uniform vec4   u_WarpParams;  // x = 1: visualise (mask red, edge clamp blue, hole fill green), y = edge blur strength (0 = plain clamp)
uniform vec2   u_InvTexRes;   // 1 / size of the captured image

varying vec2   var_Tex1;

#define HOLE_STEPS 16
#define EDGE_TAPS 8

float edgeNoise(const vec2 fc)
{
	return fract(52.9829189 * fract(dot(fc, vec2(0.06711056, 0.00583715))));
}

// Where the warp reveals pixels outside the captured frame, the clamp stretches the border row into
// streaks. Blur across the streaks (perpendicular to the overshoot), wider the further outside.
vec3 edgeBlur(const vec2 p, const vec2 pc, const vec2 lo, const vec2 hi, const vec3 base, const float strength)
{
	vec2 over = abs(p - pc);
	vec2 w = min(over * 0.5 * strength, vec2(96.0));
	vec3 sum = base;
	float n = edgeNoise(gl_FragCoord.xy);
	for (int i = 0; i < EDGE_TAPS; i++)
	{
		float t = fract(n + float(i) / float(EDGE_TAPS)) * 2.0 - 1.0;
		vec2 q = pc + vec2(t * w.y, t * w.x);
		q = clamp(q, lo, hi);
		vec4 c = texture2D(u_ScreenImageMap, q * u_InvTexRes);
		sum += c.a > 0.5 ? base : c.rgb;
	}
	return sum / float(EDGE_TAPS + 1);
}

float gunMask(const vec2 uv)
{
	return texture2D(u_ScreenImageMap, uv).a;
}

void main()
{
	vec2 uvNew = gl_FragCoord.xy * u_InvTexRes;
	vec4 cNew = texture2D(u_ScreenImageMap, uvNew);
	vec3 color = cNew.rgb;
	vec3 tint = vec3(0.0);
	float tintAmount = 0.0;

	bool inside = all(greaterThanEqual(gl_FragCoord.xy, u_WarpBox.xy)) &&
		all(lessThan(gl_FragCoord.xy, u_WarpBox.xy + u_WarpBox.zw));

	if (inside && cNew.a > 0.5)
	{
		// the view model stays locked to the screen
		tint = vec3(1.0, 0.0, 0.0);
		tintAmount = 0.6;
	}
	else if (inside)
	{
		vec2 ndc = (gl_FragCoord.xy - u_WarpBox.xy) / u_WarpBox.zw * 2.0 - 1.0;
		vec3 h = mat3(u_WarpMatrix) * vec3(ndc, 1.0);

		if (h.z > 0.0)
		{
			vec2 p = u_WarpBox.xy + (h.xy / h.z * 0.5 + 0.5) * u_WarpBox.zw;
			vec2 lo = u_WarpBox.xy + 0.5;
			vec2 hi = u_WarpBox.xy + u_WarpBox.zw - 0.5;
			vec2 pc = clamp(p, lo, hi);
			vec2 uvOld = pc * u_InvTexRes;
			vec4 cOld = texture2D(u_ScreenImageMap, uvOld);

			// clamping to the viewport edge stretches the border pixels instead of showing black
			if (any(notEqual(p, pc)))
			{
				tint = vec3(0.0, 0.0, 1.0);
				tintAmount = 0.5;
				if (u_WarpParams.y > 0.0 && cOld.a < 0.5)
					cOld.rgb = edgeBlur(p, pc, lo, hi, cOld.rgb, u_WarpParams.y);
			}

			if (cOld.a > 0.5)
			{
				// background the gun covered in the captured frame: walk towards the unwarped
				// position and take the first texel that is not gun
				vec4 c = cNew;
				for (int i = 1; i <= HOLE_STEPS; i++)
				{
					vec4 t = texture2D(u_ScreenImageMap, mix(uvOld, uvNew, float(i) / float(HOLE_STEPS)));
					if (t.a < 0.5)
					{
						c = t;
						break;
					}
				}
				cOld = c;
				tint = vec3(0.0, 1.0, 0.0);
				tintAmount = 0.6;
			}

			color = cOld.rgb;
		}
	}

	if (u_WarpParams.x > 0.5)
		color = mix(color, tint, tintAmount);

	gl_FragColor = vec4(color, 1.0);
}
