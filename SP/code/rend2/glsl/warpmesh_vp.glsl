// 6-DoF mesh warp, vertex stage.
//
// The static grid mesh (tr_warp.c R_WarpInitMesh) has one vertex per depth texel of the captured depth grid
// plus a one-cell skirt ring. attr_Position = (gx, gy, skirt): integer grid coordinates, gx in -1 .. gridW and
// gy in -1 .. gridH; skirt = 1 on the ring, whose vertices reuse the nearest edge vertex's depth.
//
// Conventions (old = the camera of the captured frame, new = the camera being presented):
//   camera space: x = forward, y = left, z = up; screen right = -left.
//   u_WarpMatrix: upper 3x3 R = old camera -> new camera (R[i][j] = dot(newAxis[i], oldAxis[j])); must be uploaded
//     so that mat3(u_WarpMatrix) * v is R * v in this shader.
//   u_WarpMove.xyz = new camera translation T in new-camera coordinates, w = near plane distance.
//   u_WarpProj = (tanHalfFovX, tanHalfFovY, grid step in pixels, unused).
// Depth grid texels: > 0 forward distance of the nearest non-gun surface, 0 = sky, -1 = only gun taps.

attribute vec3 attr_Position;

uniform sampler2D u_ScreenDepthMap; // R32F depth grid (nearest filtered)

uniform mat4   u_WarpMatrix;
uniform vec4   u_WarpBox;     // 3D viewport in pixels: x, y, w, h
uniform vec4   u_WarpMove;
uniform vec4   u_WarpProj;
uniform sampler2D u_VelocityMap;   // RG16F object motion grid (NDC per real frame), same layout as the depth grid
uniform vec4   u_WarpParams2;      // x = extrapolation fraction of a real frame (0 = off), y = max displacement in pixels

// Original image positions, premultiplied by the vertex's old-camera forward depth zo, with zo in .z. GL's
// perspective-correct interpolation (weights 1/w, w = new forward depth) of (zo * px, zo) followed by the divide in
// the fragment stage is exactly the projective map of a planar triangle: zero warp is the identity, and rotation and
// translation are right on slanted surfaces. Plain interpolation of px would skew by the per-vertex depth.
varying vec3   var_Px;        // (zo * px, zo): this vertex's ORIGINAL position in the captured image, pixels (unclamped on the skirt)
varying vec3   var_PxBg;      // same for the farthest 3x3 neighbour: where to sample to fill a disocclusion
varying float  var_Skirt;

#define SKY_DEPTH 1.0e30

// grid coordinate -> ndc of the captured view. The skirt sits at +-2, outside the screen, so edge clamping and
// the edge blur in the fragment stage kick in for what the warp reveals beyond the capture.
// grid texel g was sampled at source pixel g * step, clamped to the image size (the last texel sits on the far
// edge), so derive the position from pixels, not from g / (gridSize - 1): they differ when size % step != 0
vec2 gridNdc(const vec2 g, const vec2 gridSize)
{
	vec2 n = min(g * u_WarpProj.z, u_WarpBox.zw) / u_WarpBox.zw * 2.0 - 1.0;
	return vec2(g.x < 0.0 ? -2.0 : (g.x > gridSize.x - 1.0 ? 2.0 : n.x),
	            g.y < 0.0 ? -2.0 : (g.y > gridSize.y - 1.0 ? 2.0 : n.y));
}

vec2 ndcToPx(const vec2 ndc)
{
	return u_WarpBox.xy + (ndc * 0.5 + 0.5) * u_WarpBox.zw;
}

void main()
{
	vec2 gridSize = vec2(textureSize(u_ScreenDepthMap, 0));
	vec2 g = attr_Position.xy;
	vec2 gc = clamp(g, vec2(0.0), gridSize - 1.0);

	// gather the 3x3 neighbourhood (clamped, so the skirt reuses the edge vertex)
	float nearest = SKY_DEPTH;      // nearest solid depth
	float farthest = -1.0;          // farthest depth, sky counts as infinite, gun taps ignored
	float farSolid = 0.0;           // farthest solid depth, for gun-covered vertices
	vec2 farG = gc;
	vec2 nearG = gc;
	float centre = texelFetch(u_ScreenDepthMap, ivec2(gc), 0).r;
	bool anySky = false;

	for (int j = -1; j <= 1; j++)
	{
		for (int i = -1; i <= 1; i++)
		{
			vec2 q = clamp(gc + vec2(float(i), float(j)), vec2(0.0), gridSize - 1.0);
			float d = texelFetch(u_ScreenDepthMap, ivec2(q), 0).r;

			if (d < -0.5)
				continue;                       // gun only
			if (d > 0.0)
			{
				if (d < nearest)
				{
					nearest = d;
					nearG = q;
				}
				farSolid = max(farSolid, d);
			}
			else
				anySky = true;

			float key = d > 0.0 ? d : SKY_DEPTH;
			if (key > farthest)
			{
				farthest = key;
				farG = q;
			}
		}
	}

	// z: nearest solid tap in the footprint (keeps silhouettes on the foreground); a gun-covered vertex takes the
	// farthest solid neighbour (the background behind the gun); no solid tap at all = sky
	float z = centre < -0.5 ? farSolid : (nearest < SKY_DEPTH ? nearest : 0.0);
	bool sky = z <= 0.0;

	vec2 ndc = gridNdc(g, gridSize);
	vec3 r = vec3(1.0, -ndc.x * u_WarpProj.x, ndc.y * u_WarpProj.y);
	vec3 rr = mat3(u_WarpMatrix) * r;
	vec3 c = sky ? rr : z * rr - u_WarpMove.xyz;

	// object motion: the tap that gave z moved by m (NDC) over the last real frame; push the vertex along it in the
	// OLD camera space (before the camera rotation), scaled to the time since the capture. Skirt and gun-covered
	// vertices stay put.
	if (!sky && u_WarpParams2.x > 0.0 && attr_Position.z < 0.5 && centre > -0.5)
	{
		vec2 m = texelFetch(u_VelocityMap, ivec2(nearG), 0).rg * u_WarpParams2.x;
		float px = length(m * 0.5 * u_WarpBox.zw);
		if (px > u_WarpParams2.y)
			m *= u_WarpParams2.y / px;
		if (m.x != 0.0 || m.y != 0.0)
		{
			vec3 p = z * r + z * vec3(0.0, -m.x * u_WarpProj.x, m.y * u_WarpProj.y);
			c = mat3(u_WarpMatrix) * p - u_WarpMove.xyz;
		}
	}

	float zo = sky ? 1.0 : z;
	var_Px = vec3(ndcToPx(ndc) * zo, zo);
	var_PxBg = vec3(ndcToPx(gridNdc(farG, gridSize)) * zo, zo);
	var_Skirt = attr_Position.z;

	if (c.x <= 1.0e-4)
	{
		// behind the new camera: outside the clip volume (near plane), the triangle is clipped away
		gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
		return;
	}

	// clip.w = forward distance; clip.z = c.x - 2 * near gives a depth that grows with distance (ndc z = -1 at
	// c.x = near), sky is pushed to the far side of everything solid
	gl_Position = vec4(-c.y / u_WarpProj.x, c.z / u_WarpProj.y, sky ? 0.99999 * c.x : c.x - 2.0 * u_WarpMove.w, c.x);
}
