uniform sampler2D u_ScreenImageMap; // captured scene: rgb colour, a = view model mask

uniform mat4   u_WarpMatrix;  // upper left 3x3: new view NDC (x, y, 1) -> old view homogeneous NDC
uniform vec4   u_WarpBox;     // 3D viewport in pixels: x, y, w, h
uniform vec4   u_WarpParams;  // x = 1: visualise (mask red, edge clamp blue, hole fill green)
uniform vec2   u_InvTexRes;   // 1 / size of the captured image

varying vec2   var_Tex1;

#define HOLE_STEPS 16

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
