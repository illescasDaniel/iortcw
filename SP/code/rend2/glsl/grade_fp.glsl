uniform sampler2D u_DiffuseMap; // scene (tone-mapped)
uniform sampler2D u_LightMap;   // blurred bloom (half resolution), only read when u_Color.x > 0
uniform vec4      u_Color;      // x = bloom intensity, y = brightness (gamma lift, 1 = none), zw unused
uniform vec4      u_ViewInfo;   // x = contrast, y = saturation, z = warm/cool tint (-1..1), w = vignette

varying vec2      var_Tex1;

const vec3 LUMA = vec3(0.2125, 0.7154, 0.0721);

// codmod: CoD4 "filmTweak"-style grade + bloom composite in one pass.
void main()
{
	vec3 c = texture2D(u_DiffuseMap, var_Tex1).rgb;

	if (u_Color.x > 0.0)
	{
		vec3 b = texture2D(u_LightMap, var_Tex1).rgb;
		// screen blend keeps the sum inside 0..1 so bright areas glow without clipping to flat white
		c = c + b * u_Color.x * (1.0 - c);
	}

	// brightness: gamma lift (raises shadows and mids more than highlights, this game is dark)
	c = pow(max(c, vec3(0.0)), vec3(1.0 / max(u_Color.y, 0.1)));

	// contrast, then saturation
	c = (c - 0.35) * u_ViewInfo.x + 0.35; // pivot below mid grey: this game is dark, do not crush the shadows
	float l = dot(c, LUMA);
	c = mix(vec3(l), c, u_ViewInfo.y);

	// warm/cool split tone: highlights lean one way, shadows the other
	vec3 warm = vec3(1.06, 1.0, 0.92);
	vec3 cool = vec3(0.92, 1.0, 1.08);
	float t = u_ViewInfo.z;
	vec3 hi = mix(vec3(1.0), t > 0.0 ? warm : cool, abs(t));
	vec3 lo = mix(vec3(1.0), t > 0.0 ? cool : warm, abs(t) * 0.5);
	c *= mix(lo, hi, clamp(l, 0.0, 1.0));

	// vignette
	vec2 d = var_Tex1 - 0.5;
	c *= 1.0 - u_ViewInfo.w * smoothstep(0.25, 0.85, dot(d, d) * 2.0);

	gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
