// SMAA 1x pass 1: luma edge detection (vertex). Uses the SMAA library in smaa_lib.glsl.
attribute vec3 attr_Position;
attribute vec4 attr_TexCoord0;

uniform mat4   u_ModelViewProjectionMatrix;

varying vec2   var_TexCoords;
varying vec4   var_Offset0;
varying vec4   var_Offset1;
varying vec4   var_Offset2;

void main()
{
	vec4 offset[3];

	gl_Position = u_ModelViewProjectionMatrix * vec4(attr_Position, 1.0);
	var_TexCoords = attr_TexCoord0.st;

	SMAAEdgeDetectionVS(var_TexCoords, offset);
	var_Offset0 = offset[0];
	var_Offset1 = offset[1];
	var_Offset2 = offset[2];
}
