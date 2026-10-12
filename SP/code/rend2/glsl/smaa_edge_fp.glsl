// SMAA 1x pass 1: luma edge detection (fragment). Uses the SMAA library in smaa_lib.glsl.
uniform sampler2D u_TextureMap;

varying vec2   var_TexCoords;
varying vec4   var_Offset0;
varying vec4   var_Offset1;
varying vec4   var_Offset2;

void main()
{
	vec4 offset[3];
	offset[0] = var_Offset0;
	offset[1] = var_Offset1;
	offset[2] = var_Offset2;

	gl_FragColor = vec4(SMAALumaEdgeDetectionPS(var_TexCoords, offset, u_TextureMap), 0.0, 1.0);
}
