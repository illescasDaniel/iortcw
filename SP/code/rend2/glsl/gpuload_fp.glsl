uniform sampler2D u_DiffuseMap;
uniform vec4      u_Color;     // x = iterations, y = per-frame seed

varying vec2      var_Tex1;

// r_gpuLoad: dependent ALU chain whose length comes from a uniform (cannot be folded away at compile time).
// The result goes to a small offscreen target that is never read; the output depends on every iteration.
void main()
{
	float a = u_Color.y + var_Tex1.x;
	float b = 0.5 + var_Tex1.y;
	int n = int(u_Color.x);

	for (int i = 0; i < n; i++)
	{
		a = sin(a * 1.7 + b) + a * 0.31;
		b = fract(b * 3.17 + a) + 0.07;
	}

	gl_FragColor = vec4(a, b, a * b, 1.0) * texture2D(u_DiffuseMap, var_Tex1);
}
