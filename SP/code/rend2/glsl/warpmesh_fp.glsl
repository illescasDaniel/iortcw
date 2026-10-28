// 6-DoF mesh warp, fragment stage. Shades the forward-reprojected depth mesh (see warpmesh_vp.glsl).
//
// var_Px = where this fragment sits in the captured image (pixels, interpolated from the vertices' original
// positions); sampling there reproduces the captured colour. Triangles that the warp stretches across a depth
// discontinuity cover many screen pixels per texel (small |d(px)/d(screen)| determinant): those are disocclusions,
// and with fill enabled they sample the background position var_PxBg instead.

uniform sampler2D u_ScreenImageMap; // captured scene: rgb colour, a = view model mask (1 = gun)

uniform vec4   u_WarpBox;     // 3D viewport in pixels: x, y, w, h
uniform vec4   u_WarpParams;  // x = debug (1: mask red / edge clamp blue / hole green, 3: filled magenta), y = edge blur strength,
                              // z = fill disocclusions (0 = keep the stretched sample), w = max stretch
uniform vec2   u_InvTexRes;   // 1 / size of the captured image

varying vec3   var_Px;      // (zo * px, zo), see warpmesh_vp.glsl
varying vec3   var_PxBg;
varying float  var_Skirt;

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

void main()
{
	// derivatives first: they need uniform control flow
	vec2 pxOld = var_Px.xy / var_Px.z;
	vec2 pxBg = var_PxBg.xy / var_PxBg.z;
	vec2 dx = dFdx(pxOld);
	vec2 dy = dFdy(pxOld);
	float det = abs(dx.x * dy.y - dx.y * dy.x);

	vec2 uvNew = gl_FragCoord.xy * u_InvTexRes;
	vec4 cNew = texture2D(u_ScreenImageMap, uvNew);
	vec3 color = cNew.rgb;
	vec3 tint = vec3(0.0);
	float tintAmount = 0.0;
	int dbg = int(u_WarpParams.x + 0.5);

	bool inside = all(greaterThanEqual(gl_FragCoord.xy, u_WarpBox.xy)) &&
		all(lessThan(gl_FragCoord.xy, u_WarpBox.xy + u_WarpBox.zw));

	if (inside && cNew.a > 0.5)
	{
		// the view model stays locked to the screen
		tint = vec3(1.0, 0.0, 0.0);
		tintAmount = 0.6;
	}
	else
	{
		bool stretched = u_WarpParams.z != 0.0 && var_Skirt < 0.5 &&
			det * max(u_WarpParams.w, 1.0e-3) < 1.0;
		vec2 p = stretched ? pxBg : pxOld;
		vec2 lo = u_WarpBox.xy + 0.5;
		vec2 hi = u_WarpBox.xy + u_WarpBox.zw - 0.5;
		vec2 pc = clamp(p, lo, hi);
		vec2 uvOld = pc * u_InvTexRes;
		vec4 cOld = texture2D(u_ScreenImageMap, uvOld);

		if (stretched)
		{
			tint = vec3(1.0, 0.0, 1.0);
			tintAmount = 0.6;
		}

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

		// debug 1 shows the mask / edge / gun-hole tints, debug 3 only the disocclusion fill
		if (dbg == 3 && !stretched)
			tintAmount = 0.0;
	}

	if (dbg == 1 || dbg == 3)
		color = mix(color, tint, tintAmount);

	gl_FragColor = vec4(color, 1.0);
}
