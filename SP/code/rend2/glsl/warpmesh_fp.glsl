uniform sampler2D u_ScreenImageMap; // captured scene: rgb colour, a = view model mask

uniform vec4   u_WarpMove;    // 6-DoF mesh warp: xyz = camera translation in the capture's view space
uniform vec4   u_WarpProj;    // x = tanX, y = tanY, z = zNear, w = zFar

varying vec2   var_Tex1;

// stub (milestone M0): plain copy of the capture
void main()
{
	gl_FragColor = vec4(texture2D(u_ScreenImageMap, var_Tex1).rgb, 1.0);
}
