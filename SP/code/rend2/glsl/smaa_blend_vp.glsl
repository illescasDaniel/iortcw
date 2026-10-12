// SMAA 1x pass 3: neighborhood blending (vertex). Uses the SMAA library in smaa_lib.glsl.
attribute vec3 attr_Position;
attribute vec4 attr_TexCoord0;

uniform mat4   u_ModelViewProjectionMatrix;

varying vec2   var_TexCoords;
varying vec4   var_Offset;

void main()
{
	gl_Position = u_ModelViewProjectionMatrix * vec4(attr_Position, 1.0);
	var_TexCoords = attr_TexCoord0.st;

	SMAANeighborhoodBlendingVS(var_TexCoords, var_Offset);
}
