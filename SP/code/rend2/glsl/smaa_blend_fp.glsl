// SMAA 1x pass 3: neighborhood blending (fragment). Uses the SMAA library in smaa_lib.glsl.
uniform sampler2D u_TextureMap;   // scene color
uniform sampler2D u_LevelsMap;    // blend weights

varying vec2   var_TexCoords;
varying vec4   var_Offset;

void main()
{
	gl_FragColor = SMAANeighborhoodBlendingPS(var_TexCoords, var_Offset, u_TextureMap, u_LevelsMap);
}
