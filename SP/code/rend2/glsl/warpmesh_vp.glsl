attribute vec3 attr_Position;
attribute vec4 attr_TexCoord0;

uniform vec4   u_WarpMove;    // 6-DoF mesh warp: xyz = camera translation in the capture's view space
uniform vec4   u_WarpProj;    // x = tanX, y = tanY, z = zNear, w = zFar

varying vec2   var_Tex1;

// stub (milestone M0): passes the grid vertex through
void main()
{
	gl_Position = vec4(attr_Position.xy, 0.0, 1.0);
	var_Tex1 = attr_TexCoord0.st;
}
