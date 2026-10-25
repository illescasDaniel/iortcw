uniform sampler2D u_ScreenImageMap; // final 3D scene colour
uniform sampler2D u_ScreenDepthMap; // full resolution scene depth

uniform vec2   u_InvTexRes;   // 1 / size of the scene image
uniform vec4   u_WarpParams;  // x = gun depth threshold, y = dilate radius in pixels

varying vec2   var_Tex1;

// alpha = view model mask: 1 when any tap within the dilate radius is nearer than the view model depth
// range (the gun is drawn with glDepthRange(0, 0.3)), so the mask covers the gun's silhouette edge too
void main()
{
	vec2 uv = gl_FragCoord.xy * u_InvTexRes;
	float gun = 0.0;
	int r = int(u_WarpParams.y + 0.5);

	for (int y = -3; y <= 3; y++)
	{
		for (int x = -3; x <= 3; x++)
		{
			if (abs(x) <= r && abs(y) <= r)
			{
				float d = texture2D(u_ScreenDepthMap, uv + vec2(float(x), float(y)) * u_InvTexRes).r;
				if (d < u_WarpParams.x)
					gun = 1.0;
			}
		}
	}

	gl_FragColor = vec4(texture2D(u_ScreenImageMap, uv).rgb, gun);
}
