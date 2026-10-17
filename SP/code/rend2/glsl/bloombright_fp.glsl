uniform sampler2D u_DiffuseMap;
uniform vec4      u_Color;     // bright pass: x = threshold (0..1 max channel), y = soft knee; up pass: x = weight
uniform vec2      u_InvTexRes; // 1 / size of the source texture (times the radius for the up pass)

varying vec2      var_Tex1;

#if defined(UP_PASS)
// codmod bloom: 3x3 tent upsample of the next smaller mip, added onto this one (blend ONE, ONE).
// Chaining it from the smallest level up gives a smooth falloff: tight glow from the big levels,
// a wide faint halo from the small ones.
void main()
{
	vec2 o = u_InvTexRes;
	vec3 sum = texture2D(u_DiffuseMap, var_Tex1).rgb * 4.0;
	sum += (texture2D(u_DiffuseMap, var_Tex1 + vec2( o.x, 0.0)).rgb + texture2D(u_DiffuseMap, var_Tex1 - vec2( o.x, 0.0)).rgb
	      + texture2D(u_DiffuseMap, var_Tex1 + vec2(0.0,  o.y)).rgb + texture2D(u_DiffuseMap, var_Tex1 - vec2(0.0,  o.y)).rgb) * 2.0;
	sum += texture2D(u_DiffuseMap, var_Tex1 + vec2( o.x,  o.y)).rgb + texture2D(u_DiffuseMap, var_Tex1 + vec2(-o.x,  o.y)).rgb
	     + texture2D(u_DiffuseMap, var_Tex1 + vec2( o.x, -o.y)).rgb + texture2D(u_DiffuseMap, var_Tex1 + vec2(-o.x, -o.y)).rgb;
	gl_FragColor = vec4(sum * (u_Color.x / 16.0), 1.0);
}
#else
// codmod bloom bright pass: soft-knee threshold on the tone-mapped scene, writes mip 0 (half resolution).
// Four bilinear taps (a 4x4 box) are thresholded one by one and averaged, so small bright lights neither
// shimmer nor fire-fly. u_InvTexRes is the size of one source texel.
vec3 bright(vec2 uv)
{
	vec3 c = texture2D(u_DiffuseMap, uv).rgb;
	float l = max(c.r, max(c.g, c.b));
	float knee = max(u_Color.y, 0.0001);
	float soft = clamp(l - u_Color.x + knee, 0.0, 2.0 * knee);
	soft = soft * soft / (4.0 * knee);
	float w = max(soft, l - u_Color.x) / max(l, 0.0001);
	return c * w;
}

void main()
{
	vec2 o = u_InvTexRes;
	vec3 sum = bright(var_Tex1 + vec2(-o.x, -o.y)) + bright(var_Tex1 + vec2(o.x, -o.y))
	         + bright(var_Tex1 + vec2(-o.x,  o.y)) + bright(var_Tex1 + vec2(o.x,  o.y));
	gl_FragColor = vec4(sum * 0.25, 1.0);
}
#endif
