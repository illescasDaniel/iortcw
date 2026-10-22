uniform sampler2D u_DiffuseMap;

uniform int       u_AlphaTest;

varying vec2      var_DiffuseTex;

varying vec4      var_Color;

#if defined(USE_VELOCITY)
varying vec4      var_VelCur;
varying vec4      var_VelPrev;

// NDC motion since the previous frame (jitter removed). Exactly (0,0) is what the cleared buffer holds and
// means "no velocity here" (the resolve falls back to depth reprojection), so a real zero is nudged off it.
vec2 CalcVelocity()
{
	if (var_VelPrev.w < 0.01 || var_VelCur.w < 0.01)
		return vec2(0.0);
	vec2 v = var_VelCur.xy / var_VelCur.w - var_VelPrev.xy / var_VelPrev.w;
	if (abs(v.x) + abs(v.y) < 1e-4)
		v = vec2(1e-4, 0.0);
	return v;
}
#endif


void main()
{
	vec4 color  = texture2D(u_DiffuseMap, var_DiffuseTex);

	float alpha = color.a * var_Color.a;
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
	
	gl_FragColor.rgb = color.rgb * var_Color.rgb;
	gl_FragColor.a = alpha;

#if defined(USE_VELOCITY)
	out_Velocity = CalcVelocity();
#endif
}
