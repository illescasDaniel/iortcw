// SMAA 1x pass 2: blending weight calculation (fragment). Uses the SMAA library in smaa_lib.glsl.
uniform sampler2D u_TextureMap;       // edges
uniform sampler2D u_LevelsMap;        // area texture
uniform sampler2D u_ScreenImageMap;   // search texture

varying vec2   var_TexCoords;
varying vec2   var_PixCoords;
varying vec4   var_Offset0;
varying vec4   var_Offset1;
varying vec4   var_Offset2;

void main()
{
	vec4 offset[3];
	offset[0] = var_Offset0;
	offset[1] = var_Offset1;
	offset[2] = var_Offset2;

	gl_FragColor = SMAABlendingWeightCalculationPS(var_TexCoords, var_PixCoords, offset,
		u_TextureMap, u_LevelsMap, u_ScreenImageMap, vec4(0.0));
}
