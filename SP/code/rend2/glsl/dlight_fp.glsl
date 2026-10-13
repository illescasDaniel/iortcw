uniform sampler2D u_DiffuseMap;

uniform int       u_AlphaTest;
uniform vec4      u_SpecularScale;

// codmod: flashlight shadow mask (u_AoParams.y = 1 when valid)
uniform sampler2D u_AoMap;
uniform vec4      u_AoParams;

varying vec2      var_Tex1;
varying vec4      var_Color;
varying vec3      var_SpecColor;


void main()
{
	float shadowMask = 1.0;
	if (u_AoParams.y > 0.5)
		shadowMask = texture2D(u_AoMap, gl_FragCoord.xy * r_FBufScale).r;

	if (u_SpecularScale.x > 0.0)
	{
		gl_FragColor = vec4(var_SpecColor * shadowMask, 1.0); // codmod: additive specular pass
		return;
	}

	vec4 color = texture2D(u_DiffuseMap, var_Tex1);

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
	
	gl_FragColor.rgb = color.rgb * var_Color.rgb * shadowMask;
	gl_FragColor.a = alpha;
}
