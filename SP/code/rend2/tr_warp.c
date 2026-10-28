/*
===========================================================================
codmod frame warp: re-projects a captured main view (rotation only) to a slightly different view direction.

Chunk 1: capture at the end of RB_PostProcess plus the r_warpDebug synthetic delta preview. Inert unless
r_frameWarp or r_warpDebug is set at renderer init (no textures, no shaders, only a NULL check in the backend).
===========================================================================
*/
#ifdef USE_LOCAL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#include "tr_local.h"
#include "tr_dsa.h"

// defined in tr_fbo.c, not in a header
FBO_t *FBO_Create( const char *name, int width, int height );
qboolean R_CheckFBO( const FBO_t *fbo );
void FBO_CreateBuffer( FBO_t *fbo, int format, int index, int multisample );

// view angles (pitch, yaw) the client built the frame with, set by RE_WarpSetRef before rendering
static float warpPendingRef[2];

#define WARP_LATCH_RING 128
static float latchRing[WARP_LATCH_RING];
static int latchSamples, latchCalls, latchApplied, latchSkipped, latchDenied, latchNoCapture;

// mesh warp: sample counters for speedRing / dtRing (the rings themselves live in tr.warp)
static int meshSpeedSamples, meshDtSamples, meshDtUnknown, meshMoveSamples;

// the 2D commands that followed the captured view, kept so a later chunk can replay them over a warped frame
static void *warpTailBuf[( MAX_RENDER_COMMANDS + sizeof( void * ) - 1 ) / sizeof( void * )];

#define WARP_TAIL_SIZE ( (int)sizeof( warpTailBuf ) )

/*
=============
R_WarpInitMesh

6-DoF mesh warp resources (r_frameWarpMesh > 0): the sampled depth grid target and the static grid mesh.
The grid has gridW x gridH depth samples, one every gridStep pixels; the mesh adds a one-cell skirt ring
around it (grid coordinates -1 and gridW / gridH) whose third vertex component is 1.
=============
*/
static void R_WarpInitMesh( void ) {
	static const int steps[4] = { 0, 16, 8, 4 };
	int level = r_frameWarpMesh->integer;
	int step, gw, gh, nx, ny, x, y;
	int numVerts, numIndexes, vertSize, indexSize;
	float *verts;
	glIndex_t *indexes;
	vao_t *vao;

	if ( level < 1 || level > 3 ) {
		return;
	}

	step = steps[level];
	gw = ( tr.renderFbo->width + step - 1 ) / step + 1;
	gh = ( tr.renderFbo->height + step - 1 ) / step + 1;

	tr.warpDepthImage = R_CreateImage( "*warpDepth", NULL, gw, gh, IMGTYPE_COLORALPHA,
									   IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_R32F );
	qglTextureParameteriEXT( tr.warpDepthImage->texnum, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	qglTextureParameteriEXT( tr.warpDepthImage->texnum, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	tr.warpDepthFbo = FBO_Create( "_warpDepth", gw, gh );
	FBO_AttachImage( tr.warpDepthFbo, tr.warpDepthImage, GL_COLOR_ATTACHMENT0, 0 );
	if ( !R_CheckFBO( tr.warpDepthFbo ) ) {
		tr.warpDepthFbo = NULL;
		tr.warpDepthImage = NULL;
		return;
	}

	nx = gw + 2;
	ny = gh + 2;
	numVerts = nx * ny;
	numIndexes = ( nx - 1 ) * ( ny - 1 ) * 6;
	vertSize = numVerts * 3 * (int)sizeof( float );
	indexSize = numIndexes * (int)sizeof( glIndex_t );

	verts = ri.Hunk_AllocateTempMemory( vertSize );
	indexes = ri.Hunk_AllocateTempMemory( indexSize );

	for ( y = 0; y < ny; y++ ) {
		for ( x = 0; x < nx; x++ ) {
			float *v = verts + ( y * nx + x ) * 3;

			v[0] = (float)( x - 1 );
			v[1] = (float)( y - 1 );
			v[2] = ( x == 0 || y == 0 || x == nx - 1 || y == ny - 1 ) ? 1.0f : 0.0f;
		}
	}

	{
		glIndex_t *out = indexes;

		for ( y = 0; y < ny - 1; y++ ) {
			for ( x = 0; x < nx - 1; x++ ) {
				glIndex_t i0 = y * nx + x, i1 = i0 + 1, i2 = i0 + nx, i3 = i2 + 1;

				*out++ = i0; *out++ = i2; *out++ = i1;
				*out++ = i1; *out++ = i2; *out++ = i3;
			}
		}
	}

	vao = R_CreateVao( "warpMesh_VAO", (byte *)verts, vertSize, (byte *)indexes, indexSize, VAO_USAGE_STATIC );
	vao->attribs[ATTR_INDEX_POSITION].enabled = 1;
	vao->attribs[ATTR_INDEX_POSITION].count = 3;
	vao->attribs[ATTR_INDEX_POSITION].type = GL_FLOAT;
	vao->attribs[ATTR_INDEX_POSITION].normalized = GL_FALSE;
	vao->attribs[ATTR_INDEX_POSITION].offset = 0;
	vao->attribs[ATTR_INDEX_POSITION].stride = 3 * sizeof( float );
	Vao_SetVertexPointers( vao );

	ri.Hunk_FreeTempMemory( indexes );
	ri.Hunk_FreeTempMemory( verts );

	tr.warpMeshVao = vao;
	tr.warpMeshIndexes = numIndexes;
	tr.warpGridW = gw;
	tr.warpGridH = gh;
	tr.warpGridStep = step;
}

/*
=============
R_WarpInitTargets

Called after FBO_Init: creates the capture target when the feature (or its debug preview) is requested.
=============
*/
void R_WarpInitTargets( void ) {
	tr.warpImage = NULL;
	tr.warpFbo = NULL;
	tr.warpOutImage = NULL;
	tr.warpOutFbo = NULL;
	tr.warpDepthImage = NULL;
	tr.warpDepthFbo = NULL;
	tr.warpMeshVao = NULL;
	tr.warpMeshIndexes = 0;
	tr.warpGridW = tr.warpGridH = tr.warpGridStep = 0;
	meshSpeedSamples = meshDtSamples = meshDtUnknown = meshMoveSamples = 0;

	if ( r_frameWarp->integer <= 0 && !r_warpDebug->integer ) {
		return;
	}

	if ( !tr.renderFbo || !tr.renderImage || !tr.renderDepthImage ) {
		ri.Printf( PRINT_WARNING, "frame warp needs the render FBO (r_hdr 1 or MSAA), disabled.\n" );
		tr.warp.reasons[WARP_REASON_NO_FBO]++;
		return;
	}

	tr.warpImage = R_CreateImage( "*warp", NULL, tr.renderFbo->width, tr.renderFbo->height, IMGTYPE_COLORALPHA,
								  IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA8 );
	tr.warpFbo = FBO_Create( "_warp", tr.renderFbo->width, tr.renderFbo->height );
	FBO_AttachImage( tr.warpFbo, tr.warpImage, GL_COLOR_ATTACHMENT0, 0 );
	R_CheckFBO( tr.warpFbo );

	// compositor target: the colour format of the render FBO (the MSAA resolve image has it), never multisampled
	tr.warpOutImage = R_CreateImage( "*warpOut", NULL, tr.renderFbo->width, tr.renderFbo->height, IMGTYPE_COLORALPHA,
									 IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, tr.renderImage->internalFormat );
	tr.warpOutFbo = FBO_Create( "_warpOut", tr.renderFbo->width, tr.renderFbo->height );
	FBO_AttachImage( tr.warpOutFbo, tr.warpOutImage, GL_COLOR_ATTACHMENT0, 0 );

	// the mesh warp depth tests its triangles; the 2D tail replay keeps GLS_DEPTHTEST_DISABLE (RB_SetGL2D)
	if ( r_frameWarpMesh->integer > 0 ) {
		FBO_CreateBuffer( tr.warpOutFbo, GL_DEPTH_COMPONENT24, 0, 0 );
	}

	if ( !R_CheckFBO( tr.warpOutFbo ) ) {
		tr.warpOutFbo = NULL;
	}

	if ( r_frameWarpMesh->integer > 0 && tr.warpOutFbo ) {
		R_WarpInitMesh();
	}
}

/*
=============
Euler helpers (game convention: +pitch = down, +yaw = left)
=============
*/
static void R_WarpAxisToAngles( const vec3_t axis[3], vec3_t angles ) {
	float s = -axis[0][2];

	if ( s > 1.0f ) {
		s = 1.0f;
	} else if ( s < -1.0f ) {
		s = -1.0f;
	}

	angles[PITCH] = RAD2DEG( asin( s ) );
	angles[YAW] = RAD2DEG( atan2( axis[0][1], axis[0][0] ) );
	angles[ROLL] = RAD2DEG( atan2( axis[1][2], axis[2][2] ) );
}

static float R_WarpClamp( float v, float limit ) {
	return v < -limit ? -limit : ( v > limit ? limit : v );
}

/*
=============
R_WarpMatrix

Homography H taking a new view NDC point (X, Y, 1) to the old view's homogeneous NDC.
M[i][j] = dot( oldAxis[i], newAxis[j] ), axis order forward, left, up. Output is the 3x3 in the upper
left of a column-major mat4, so that mat3( u_WarpMatrix ) * vec3( ndc, 1 ) evaluates row . vector.
=============
*/
static void R_WarpMatrix( const vec3_t oldAxis[3], const vec3_t newAxis[3], float fovX, float fovY, mat4_t out ) {
	float M[3][3], H[3][3];
	float tx = tan( DEG2RAD( fovX ) * 0.5f );
	float ty = tan( DEG2RAD( fovY ) * 0.5f );
	int i, j;

	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			M[i][j] = DotProduct( oldAxis[i], newAxis[j] );
		}
	}

	H[0][0] = ( -1.0f / tx ) * ( -tx * M[1][1] );
	H[0][1] = ( -1.0f / tx ) * ( ty * M[1][2] );
	H[0][2] = ( -1.0f / tx ) * ( M[1][0] );
	H[1][0] = ( 1.0f / ty ) * ( -tx * M[2][1] );
	H[1][1] = ( 1.0f / ty ) * ( ty * M[2][2] );
	H[1][2] = ( 1.0f / ty ) * ( M[2][0] );
	H[2][0] = -tx * M[0][1];
	H[2][1] = ty * M[0][2];
	H[2][2] = M[0][0];

	Com_Memset( out, 0, sizeof( mat4_t ) );
	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			out[j * 4 + i] = H[i][j];
		}
	}
	out[15] = 1.0f;
}

/*
=============
R_WarpMeshAvailable
=============
*/
static qboolean R_WarpMeshAvailable( void ) {
	return r_frameWarpMesh->integer > 0 && tr.warpMeshVao && tr.warpMeshIndexes > 0 && tr.warpMeshShader.program &&
		   tr.warpDepthImage && tr.warpOutFbo;
}

/*
=============
RB_WarpMeshTo

6-DoF mesh warp: draws the depth grid mesh into dst (needs a depth attachment: tr.warpOutFbo). The shader gets
R = OLD capture camera -> NEW camera rotation (R[i][j] = dot( newAxis[i], oldAxis[j] ), axis order forward, left,
up; mat3( u_WarpMatrix ) * v with v in old-camera components gives new-camera components) and
T = moveWorld (newOrigin - oldOrigin) in the new camera's (forward, left, up) components, w = capture zNear.
dst's colour and depth are cleared; depth test is left disabled for the 2D tail.
=============
*/
static void RB_WarpMeshTo( FBO_t *dst, float dPitch, float dYaw, const vec3_t moveWorld ) {
	vec3_t angles, newAxis[3];
	vec4_t box, params, move, proj;
	vec2_t invTexRes;
	mat4_t m;
	float maxDeg = r_frameWarpMaxDeg->value;
	int i, j;

	if ( maxDeg < 0.0f ) {
		maxDeg = 0.0f;
	}

	R_WarpAxisToAngles( (const vec3_t *)tr.warp.axis, angles );
	angles[PITCH] = R_WarpClamp( angles[PITCH] + R_WarpClamp( dPitch, maxDeg ), 87.9f );
	angles[YAW] += R_WarpClamp( dYaw, maxDeg );
	AnglesToAxis( angles, newAxis );

	Com_Memset( m, 0, sizeof( mat4_t ) );
	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			m[j * 4 + i] = DotProduct( newAxis[i], tr.warp.axis[j] );
		}
	}
	m[15] = 1.0f;

	VectorSet4( move, DotProduct( newAxis[0], moveWorld ), DotProduct( newAxis[1], moveWorld ),
				DotProduct( newAxis[2], moveWorld ), tr.warp.zNear );
	VectorSet4( proj, tr.warp.tanX, tr.warp.tanY, (float)tr.warpGridStep, 0.0f );
	VectorSet4( box, tr.warp.box[0], tr.warp.box[1], tr.warp.box[2], tr.warp.box[3] );
	VectorSet4( params, r_warpDebug->integer == 3 ? 1.0f : 0.0f, r_frameWarpEdgeBlur->value,
				r_frameWarpMeshFill->value, r_frameWarpMeshStretch->value );
	invTexRes[0] = 1.0f / tr.warpImage->width;
	invTexRes[1] = 1.0f / tr.warpImage->height;

	FBO_Bind( dst );
	qglViewport( 0, 0, dst->width, dst->height );
	qglScissor( 0, 0, dst->width, dst->height );

	// a false depth mask blocks the depth clear
	GL_State( GLS_DEPTHMASK_TRUE | GLS_DEPTHTEST_DISABLE );
	qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	GL_Cull( CT_TWO_SIDED );
	GL_State( GLS_DEPTHMASK_TRUE );

	GLSL_BindProgram( &tr.warpMeshShader );
	GL_BindToTMU( tr.warpImage, TB_COLORMAP );
	GL_BindToTMU( tr.warpDepthImage, TB_LIGHTMAP );
	GLSL_SetUniformMat4( &tr.warpMeshShader, UNIFORM_WARPMATRIX, m );
	GLSL_SetUniformVec4( &tr.warpMeshShader, UNIFORM_WARPBOX, box );
	GLSL_SetUniformVec4( &tr.warpMeshShader, UNIFORM_WARPPARAMS, params );
	GLSL_SetUniformVec2( &tr.warpMeshShader, UNIFORM_INVTEXRES, invTexRes );
	GLSL_SetUniformVec4( &tr.warpMeshShader, UNIFORM_WARPMOVE, move );
	GLSL_SetUniformVec4( &tr.warpMeshShader, UNIFORM_WARPPROJ, proj );

	R_BindVao( tr.warpMeshVao );
	R_DrawElements( tr.warpMeshIndexes, 0 );

	// the 2D tail draws with GLS_DEPTHTEST_DISABLE per call; leave the state that way
	GL_State( GLS_DEPTHTEST_DISABLE );
}

/*
=============
RB_WarpApply

Draws the captured main view into dst as seen from the captured direction plus (dPitch, dYaw)
degrees. Returns qfalse when there is nothing valid to warp. moveWorld (may be NULL) selects the 6-DoF mesh
warp when it is available; NULL always uses the rotation-only homography.
=============
*/
static qboolean RB_WarpApplyTo( FBO_t *dst, float dPitch, float dYaw, const vec3_t moveWorld ) {
	vec3_t angles, newAxis[3];
	vec4_t box, params;
	mat4_t m;
	float maxDeg = r_frameWarpMaxDeg->value;

	if ( !tr.warpFbo || !tr.warp.valid || !dst ) {
		tr.warp.reasons[WARP_REASON_STALE]++;
		return qfalse;
	}

	if ( moveWorld && R_WarpMeshAvailable() && dst == tr.warpOutFbo ) {
		RB_WarpMeshTo( dst, dPitch, dYaw, moveWorld );
		return qtrue;
	}

	if ( maxDeg < 0.0f ) {
		maxDeg = 0.0f;
	}

	R_WarpAxisToAngles( (const vec3_t *)tr.warp.axis, angles );
	angles[PITCH] = R_WarpClamp( angles[PITCH] + R_WarpClamp( dPitch, maxDeg ), 87.9f );
	angles[YAW] += R_WarpClamp( dYaw, maxDeg );
	AnglesToAxis( angles, newAxis );

	R_WarpMatrix( (const vec3_t *)tr.warp.axis, (const vec3_t *)newAxis, tr.warp.fovX, tr.warp.fovY, m );

	VectorSet4( box, tr.warp.box[0], tr.warp.box[1], tr.warp.box[2], tr.warp.box[3] );
	VectorSet4( params, r_warpDebug->integer == 3 ? 1.0f : 0.0f, r_frameWarpEdgeBlur->value, 0.0f, 0.0f );

	GLSL_BindProgram( &tr.warpShader );
	GLSL_SetUniformMat4( &tr.warpShader, UNIFORM_WARPMATRIX, m );
	GLSL_SetUniformVec4( &tr.warpShader, UNIFORM_WARPBOX, box );
	GLSL_SetUniformVec4( &tr.warpShader, UNIFORM_WARPPARAMS, params );

	// the pass covers the whole render target; HUD and gun mask handling happen in the shader
	FBO_BlitFromTexture( tr.warpImage, NULL, NULL, dst, NULL, &tr.warpShader, NULL, GLS_DEPTHTEST_DISABLE );
	return qtrue;
}

qboolean RB_WarpApply( float dPitch, float dYaw ) {
	return RB_WarpApplyTo( tr.renderFbo, dPitch, dYaw, NULL );
}

/*
=============
R_WarpDebugMoveWorld

r_warpDebugMove ("forward left up", in the capture camera's axes) as a world-space translation.
=============
*/
static void R_WarpDebugMoveWorld( vec3_t out ) {
	float f = 0.0f, l = 0.0f, u = 0.0f;

	sscanf( r_warpDebugMove->string, "%f %f %f", &f, &l, &u );
	VectorClear( out );
	VectorMA( out, f, tr.warp.axis[0], out );
	VectorMA( out, l, tr.warp.axis[1], out );
	VectorMA( out, u, tr.warp.axis[2], out );
}

/*
=============
R_WarpPresentMove

World-space camera translation a compositor present applies: velocity * dt (none when the dt is unknown or
r_frameWarpMeshTranslate is 0), clamped to r_frameWarpMeshMaxMove; r_frameWarpApTest 2 uses r_warpDebugMove
instead and 1 none. The applied length goes into the move ring.
=============
*/
static void R_WarpPresentMove( float moveDt, vec3_t out ) {
	float maxMove = r_frameWarpMeshMaxMove->value;
	float len;

	VectorClear( out );

	if ( r_frameWarpApTest->integer == 2 ) {
		R_WarpDebugMoveWorld( out );
	} else if ( r_frameWarpApTest->integer == 0 && moveDt >= 0.0f && r_frameWarpMeshTranslate->integer ) {
		VectorScale( tr.warp.velocity, moveDt, out );
	}

	len = VectorLength( out );
	if ( maxMove < 0.0f ) {
		maxMove = 0.0f;
	}
	if ( len > maxMove ) {
		VectorScale( out, maxMove / len, out );
		len = maxMove;
	}

	tr.warp.moveRing[meshMoveSamples++ % WARP_MESH_RING] = len;
}

/*
=============
R_WarpCaptureFresh

The capture is usable if it was taken in this frame or at most maxAge frames ago (RE_BeginFrame bumps
tr.frameCount, so a warp issued during the next frame's frontend sees age 1).
=============
*/
static qboolean R_WarpCaptureFresh( int maxAge ) {
	int age = tr.frameCount - tr.warp.frameCount;

	return tr.warp.valid && age >= 0 && age <= maxAge;
}

/*
=============
RB_WarpSaveTail

Copies the 2D commands after the post process command (up to and including the swap) into warpTailBuf,
terminated by RC_END_OF_LIST. Sets tr.warp.tailValid and the reason counters.
=============
*/
static void RB_WarpSaveTail( const void *data ) {
	byte *out = (byte *)warpTailBuf;
	int used = 0;
	const int endSize = (int)sizeof( int );

	tr.warp.tailValid = qfalse;
	tr.warp.tailBytes = 0;

	while ( 1 ) {
		int id, size = 0;
		qboolean keep = qtrue;

		data = PADP( data, sizeof( void * ) );
		id = *(const int *)data;

		switch ( id ) {
		case RC_SET_COLOR:
			size = sizeof( setColorCommand_t );
			break;
		case RC_STRETCH_PIC:
		case RC_STRETCH_PIC_GRADIENT:
			size = sizeof( stretchPicCommand_t );
			break;
		case RC_SCREENSHOT:
			size = sizeof( screenshotCommand_t );
			keep = qfalse;
			break;
		case RC_SWAP_BUFFERS:
			size = sizeof( swapBuffersCommand_t );
			break;
		case RC_DRAW_SURFS:
			tr.warp.reasons[WARP_REASON_TAIL_3D]++;
			return;
		case RC_END_OF_LIST:
			tr.warp.reasons[WARP_REASON_MID_FLUSH]++;
			return;
		case RC_VIDEOFRAME:
		case RC_COLORMASK:
		case RC_CLEARDEPTH:
		case RC_POSTPROCESS:
		default:
			tr.warp.reasons[WARP_REASON_TAIL_OTHER]++;
			return;
		}

		if ( keep ) {
			int pad = (int)( (byte *)PADP( out + used, sizeof( void * ) ) - ( out + used ) );

			if ( used + pad + size + endSize > WARP_TAIL_SIZE ) {
				tr.warp.reasons[WARP_REASON_OVERFLOW]++;
				return;
			}
			used += pad;
			Com_Memcpy( out + used, data, size );
			used += size;
		}

		data = (const byte *)data + size;

		if ( id == RC_SWAP_BUFFERS ) {
			break;
		}
	}

	{
		int pad = (int)( (byte *)PADP( out + used, sizeof( void * ) ) - ( out + used ) );

		if ( used + pad + endSize > WARP_TAIL_SIZE ) {
			tr.warp.reasons[WARP_REASON_OVERFLOW]++;
			return;
		}
		used += pad;
		*(int *)( out + used ) = RC_END_OF_LIST;
		used += endSize;
	}

	tr.warp.tailBytes = used;
	tr.warp.tailValid = qtrue;
}

/*
=============
RB_WarpMeshCapture

Mesh warp part of the capture: samples the scene depth into the grid texture and tracks the camera origin and
velocity. Velocity (units/s) is only trusted between consecutive frames with a sane time step and speed;
anything else (teleport, map change, stall) zeroes it and counts a reset.
=============
*/
static void RB_WarpMeshCapture( const postProcessCommand_t *cmd ) {
	const viewParms_t *vp = &cmd->viewParms;
	vec4_t params;
	vec3_t delta;
	float dt, speed, maxSpeed;
	int dtMs;

	VectorSet4( params, r_frameWarpGunDepth->value, r_znear->value, vp->zFar, (float)tr.warpGridStep );
	GLSL_BindProgram( &tr.warpDepthShader );
	GLSL_SetUniformVec4( &tr.warpDepthShader, UNIFORM_WARPPARAMS, params );
	GL_BindToTMU( tr.renderDepthImage, TB_LIGHTMAP );
	FBO_BlitFromTexture( tr.renderDepthImage, NULL, NULL, tr.warpDepthFbo, NULL, &tr.warpDepthShader, NULL, GLS_DEPTHTEST_DISABLE );
	RB_GpuMark( "warp_depth" );

	VectorCopy( vp->or.origin, tr.warp.origin );
	tr.warp.time = cmd->refdef.time;
	tr.warp.zNear = r_znear->value;
	tr.warp.zFar = vp->zFar;
	tr.warp.tanX = tan( DEG2RAD( vp->fovX ) * 0.5f );
	tr.warp.tanY = tan( DEG2RAD( vp->fovY ) * 0.5f );

	dtMs = tr.warp.time - tr.warp.prevTime;
	maxSpeed = r_frameWarpMeshMaxSpeed->value;
	VectorClear( tr.warp.velocity );

	if ( tr.warp.prevFrameCount > 0 && tr.frameCount == tr.warp.prevFrameCount + 1 && dtMs > 0 && dtMs <= 100 ) {
		dt = dtMs * 0.001f;
		VectorSubtract( tr.warp.origin, tr.warp.prevOrigin, delta );
		VectorScale( delta, 1.0f / dt, delta );
		speed = VectorLength( delta );

		if ( speed <= maxSpeed ) {
			VectorCopy( delta, tr.warp.velocity );
			tr.warp.speedRing[meshSpeedSamples++ % WARP_MESH_RING] = speed;
		} else {
			tr.warp.velResets++;
		}
	} else {
		tr.warp.velResets++;
	}

	VectorCopy( tr.warp.origin, tr.warp.prevOrigin );
	tr.warp.prevTime = tr.warp.time;
	tr.warp.prevFrameCount = tr.frameCount;
}

/*
=============
RB_WarpCapture

Called at the end of RB_PostProcess with the final 3D scene in srcFbo (the MSAA resolve target when MSAA is on).
=============
*/
void RB_WarpCapture( const postProcessCommand_t *cmd, FBO_t *srcFbo ) {
	const viewParms_t *vp;
	vec4_t params;
	static int reportCount;

	if ( !tr.warpFbo || !cmd || ( !r_frameWarp->integer && !r_warpDebug->integer ) ) {
		return;
	}

	vp = &cmd->viewParms;

	if ( !srcFbo || !srcFbo->colorImage[0] || !tr.renderFbo ) {
		tr.warp.valid = qfalse;
		tr.warp.tailValid = qfalse;
		tr.warp.reasons[WARP_REASON_NO_FBO]++;
		return;
	}

	// only the full-screen main view (same test as the SMAA T2x jitter in RE_RenderScene)
	if ( ( cmd->refdef.rdflags & ( RDF_SKYBOXPORTAL | RDF_NOWORLDMODEL | RDF_HYPERSPACE ) ) || vp->isPortal ||
		 vp->stereoFrame != STEREO_CENTER || vp->viewportX > 2 || vp->viewportY > 2 ||
		 abs( vp->viewportWidth - tr.renderFbo->width ) > 2 || abs( vp->viewportHeight - tr.renderFbo->height ) > 2 ) {
		tr.warp.reasons[WARP_REASON_NOT_MAIN]++;
		return;
	}

	// a later non-main view in the same frame (rejected above) must not clobber this capture
	tr.warp.valid = qfalse;
	tr.warp.tailValid = qfalse;

	// colour + gun mask
	VectorSet4( params, r_frameWarpGunDepth->value, (float)r_frameWarpGunDilate->integer, 0.0f, 0.0f );
	GLSL_BindProgram( &tr.warpCaptureShader );
	GLSL_SetUniformVec4( &tr.warpCaptureShader, UNIFORM_WARPPARAMS, params );
	GL_BindToTMU( tr.renderDepthImage, TB_LIGHTMAP );
	FBO_BlitFromTexture( srcFbo->colorImage[0], NULL, NULL, tr.warpFbo, NULL, &tr.warpCaptureShader, NULL, GLS_DEPTHTEST_DISABLE );
	RB_GpuMark( "warp_color" );

	if ( tr.warpMeshVao && tr.warpDepthFbo && tr.warpDepthShader.program ) {
		RB_WarpMeshCapture( cmd );
	}

	VectorCopy( vp->or.axis[0], tr.warp.axis[0] );
	VectorCopy( vp->or.axis[1], tr.warp.axis[1] );
	VectorCopy( vp->or.axis[2], tr.warp.axis[2] );
	tr.warp.fovX = vp->fovX;
	tr.warp.fovY = vp->fovY;
	VectorSet4( tr.warp.box, vp->viewportX, vp->viewportY, vp->viewportWidth, vp->viewportHeight );
	VectorSet4( tr.warp.color2D, backEnd.color2D[0] / 255.0f, backEnd.color2D[1] / 255.0f,
				backEnd.color2D[2] / 255.0f, backEnd.color2D[3] / 255.0f );
	tr.warp.frameCount = tr.frameCount;
	tr.warp.refAngles[0] = warpPendingRef[0];
	tr.warp.refAngles[1] = warpPendingRef[1];
	tr.warp.valid = qtrue;

	RB_WarpSaveTail( cmd + 1 );
	RB_GpuMark( "warp_capture" );

	if ( r_warpDebug->integer ) {
		float dPitch = r_warpDebugPitch->value;
		float dYaw = r_warpDebugYaw->value;

		if ( r_warpDebug->integer == 2 ) {
			dYaw *= sin( 2.0 * M_PI * ( ri.Milliseconds() % 1000 ) / 1000.0 );
		}

		tr.warp.reasons[WARP_REASON_DEBUG_ON]++;
		if ( r_warpDebug->integer == 4 && tr.warpDepthImage && tr.warpDepthFbo ) {
			// depth grid: red = distance / 1024 units (clamped), sky and gun-only taps (<= 0) black
			vec4_t scale;

			VectorSet4( scale, 1.0f / 1024.0f, 1.0f / 1024.0f, 1.0f / 1024.0f, 1.0f );
			FBO_BlitFromTexture( tr.warpDepthImage, NULL, NULL, tr.renderFbo, NULL, NULL, scale, GLS_DEPTHTEST_DISABLE );
		} else if ( R_WarpMeshAvailable() ) {
			// mesh preview: draw into the warp target (it has the depth attachment), then copy into the render FBO
			vec3_t moveWorld;

			R_WarpDebugMoveWorld( moveWorld );
			if ( RB_WarpApplyTo( tr.warpOutFbo, dPitch, dYaw, moveWorld ) ) {
				// FBO_Blit, not FBO_BlitFromTexture with NULL corners: that one samples the texture upside down
				FBO_Blit( tr.warpOutFbo, NULL, NULL, tr.renderFbo, NULL, NULL, NULL, GLS_DEPTHTEST_DISABLE );
			}
		} else {
			RB_WarpApply( dPitch, dYaw );
		}
		RB_GpuMark( "warp_debug" );

		if ( ++reportCount % 600 == 1 ) {
			int *r = tr.warp.reasons;
			ri.Printf( PRINT_ALL, "warp: notMain %d noFbo %d midFlush %d tail3D %d tailOther %d stale %d debugOn %d overflow %d tailValid %d tailBytes %d\n",
					   r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], tr.warp.tailValid, tr.warp.tailBytes );
		}
	}
}

/*
=============
RE_WarpFrame

Presents an extra frame between two real ones: warps the last captured main view by (dPitch, dYaw) degrees
into tr.renderFbo, then replays the saved 2D tail (HUD, swap) through the unchanged 2D/present code.
Does not touch tr.frameCount. Returns qfalse (nothing drawn or presented) when the capture is unusable.
=============
*/
qboolean RE_WarpFrame( float dPitch, float dYaw ) {
	// the fixed-k pacer (mode 3) is the only user; modes 1 and 2 warp once per real frame in RB_WarpLatch
	if ( r_frameWarp->integer != 3 || r_warpDebug->integer != 0 || !tr.warpFbo || !tr.renderFbo ) {
		tr.warp.reasons[WARP_REASON_NO_FBO]++;
		return qfalse;
	}

	if ( !R_WarpCaptureFresh( 0 ) ) {
		tr.warp.reasons[WARP_REASON_NO_CAPTURE]++;
		return qfalse;
	}

	if ( !tr.warp.tailValid ) {
		// the specific tail reason (tail3D, midFlush, overflow, ...) was counted at capture time
		tr.warp.reasons[WARP_REASON_NO_TAIL]++;
		return qfalse;
	}

	// RB_WarpApply clamps to r_frameWarpMaxDeg and keeps the pitch within +-87.9
	if ( !RB_WarpApply( dPitch, dYaw ) ) {
		tr.warp.reasons[WARP_REASON_APPLY_FAIL]++;
		return qfalse;
	}
	tr.warp.reasons[WARP_REASON_WARP_OK]++;

	backEnd.projection2D = qfalse;
	backEnd.color2D[0] = tr.warp.color2D[0] * 255.0f + 0.5f;
	backEnd.color2D[1] = tr.warp.color2D[1] * 255.0f + 0.5f;
	backEnd.color2D[2] = tr.warp.color2D[2] * 255.0f + 0.5f;
	backEnd.color2D[3] = tr.warp.color2D[3] * 255.0f + 0.5f;
	glState.finishCalled = qfalse;

	RB_ExecuteRenderCommands( warpTailBuf );
	return qtrue;
}

/*
=============
RE_WarpPresent

Compositor warp (r_frameWarp 2): re-projects the last capture to the absolute view angles (pitch, yaw) into
tr.warpOutFbo, then replays the saved 2D tail over it. The replay runs through the unchanged
RB_ExecuteRenderCommands / RB_SwapBuffers with tr.renderFbo / tr.msaaResolveFbo temporarily pointing at the
warp target, so the 2D and present code stay as they are. May be called from the middle of a real frame
(at a render command boundary with nothing pending in tess): all state it changes is put back.
Returns qfalse when nothing was presented. Always draws the warp, however small the correction.
=============
*/
static int apCalls, apPresented, apDenied;

qboolean RE_WarpReady( void ) {
	return !tr.warp.presenting && tr.warpFbo && tr.warpOutFbo && tr.renderFbo && !r_warpDebug->integer &&
		   R_WarpCaptureFresh( 1 ) && tr.warp.tailValid;
}

qboolean RE_WarpPresent( float pitch, float yaw ) {
	static backEndState_t savedBackEnd;
	glBindings_t bindings;
	FBO_t *savedRenderFbo, *savedResolveFbo, *savedCurrentFbo;
	vao_t *savedVao;
	mat4_t savedModelview, savedProjection;
	uint32_t savedStateBits;
	int savedCulling, savedCullFront;
	qboolean savedFinishCalled, ok, useMesh = qfalse;
	vec3_t moveWorld;
	GLint viewport[4], scissor[4];
	GLboolean colorMask[4];

	if ( tr.warp.presenting ) {
		return qfalse;
	}

	apCalls++;

	if ( !tr.warpFbo || !tr.warpOutFbo || !tr.renderFbo || r_warpDebug->integer ) {
		tr.warp.reasons[WARP_REASON_NO_FBO]++;
		apDenied++;
		return qfalse;
	}

	if ( !R_WarpCaptureFresh( 1 ) ) {
		tr.warp.reasons[WARP_REASON_NO_CAPTURE]++;
		apDenied++;
		return qfalse;
	}

	if ( !tr.warp.tailValid ) {
		tr.warp.reasons[WARP_REASON_NO_TAIL]++;
		apDenied++;
		return qfalse;
	}

	// batched surfaces belong to the real frame and must not be drawn into the warp target
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	// the first warp mark also carries the real GPU work since the previous mark
	RB_GpuMark( "pre_warp" );

	// save everything the warp and the 2D replay touch
	Com_Memcpy( &savedBackEnd, &backEnd, sizeof( backEnd ) );
	savedFinishCalled = glState.finishCalled;
	savedStateBits = glState.glStateBits;
	savedCulling = glState.faceCulling;
	savedCullFront = glState.faceCullFront;
	savedCurrentFbo = glState.currentFBO;
	savedVao = glState.currentVao;
	Mat4Copy( glState.modelview, savedModelview );
	Mat4Copy( glState.projection, savedProjection );
	GL_SaveBindings( &bindings );
	qglGetIntegerv( GL_VIEWPORT, viewport );
	qglGetIntegerv( GL_SCISSOR_BOX, scissor );
	qglGetBooleanv( GL_COLOR_WRITEMASK, colorMask );
	savedRenderFbo = tr.renderFbo;
	savedResolveFbo = tr.msaaResolveFbo;

	tr.warp.presenting = qtrue;
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );

	// asked before the replayed tail: its swap ends the pacer's in-flight present, after which the dt is unknown
	if ( R_WarpMeshAvailable() ) {
		float moveDt = ri.WarpMoveDt ? ri.WarpMoveDt() : -1.0f;

		if ( moveDt >= 0.0f ) {
			tr.warp.dtRing[meshDtSamples++ % WARP_MESH_RING] = moveDt;
		} else {
			meshDtUnknown++;
		}
		if ( moveDt >= 0.0f && r_frameWarpMeshDt->value > 0.0f ) {
			moveDt = r_frameWarpMeshDt->value * 0.001f;
		}
		R_WarpPresentMove( moveDt, moveWorld );
		useMesh = qtrue;
	} else if ( tr.warpMeshVao ) {
		// mesh level 0 resources exist without the shader path: still keep the pacer's dt bookkeeping
		float moveDt = ri.WarpMoveDt ? ri.WarpMoveDt() : -1.0f;

		if ( moveDt >= 0.0f ) {
			tr.warp.dtRing[meshDtSamples++ % WARP_MESH_RING] = moveDt;
		} else {
			meshDtUnknown++;
		}
	}

	ok = RB_WarpApplyTo( tr.warpOutFbo, pitch - tr.warp.refAngles[0], AngleNormalize180( yaw - tr.warp.refAngles[1] ),
						 useMesh ? moveWorld : NULL );

	if ( ok ) {
		tr.renderFbo = tr.warpOutFbo;
		tr.msaaResolveFbo = NULL;

		backEnd.projection2D = qfalse;
		backEnd.color2D[0] = tr.warp.color2D[0] * 255.0f + 0.5f;
		backEnd.color2D[1] = tr.warp.color2D[1] * 255.0f + 0.5f;
		backEnd.color2D[2] = tr.warp.color2D[2] * 255.0f + 0.5f;
		backEnd.color2D[3] = tr.warp.color2D[3] * 255.0f + 0.5f;
		glState.finishCalled = qfalse;

		RB_ExecuteRenderCommands( warpTailBuf );

		tr.renderFbo = savedRenderFbo;
		tr.msaaResolveFbo = savedResolveFbo;
		tr.warp.reasons[WARP_REASON_WARP_OK]++;
		apPresented++;
	} else {
		tr.warp.reasons[WARP_REASON_APPLY_FAIL]++;
		apDenied++;
	}

	// restore: tracked caches first (they now describe the real GL state), then step each back to what was found
	Com_Memcpy( &backEnd, &savedBackEnd, sizeof( backEnd ) );
	glState.finishCalled = savedFinishCalled;
	GL_State( savedStateBits );
	GL_Cull( savedCulling );
	glState.faceCullFront = savedCullFront;
	GL_RestoreBindings( &bindings );
	glState.currentFBO = savedCurrentFbo;
	if ( savedVao && glState.currentVao != savedVao ) {
		R_BindVao( savedVao );
	}
	GL_SetProjectionMatrix( savedProjection );
	GL_SetModelviewMatrix( savedModelview );
	qglViewport( viewport[0], viewport[1], viewport[2], viewport[3] );
	qglScissor( scissor[0], scissor[1], scissor[2], scissor[3] );
	qglColorMask( colorMask[0], colorMask[1], colorMask[2], colorMask[3] );
	tr.warp.presenting = qfalse;

	return ok;
}

/*
=============
R_WarpPresent_f

warppresent <pitch> <yaw>: dev command, one compositor present at absolute view angles.
=============
*/
void R_WarpPresent_f( void ) {
	if ( ri.Cmd_Argc() != 3 ) {
		ri.Printf( PRINT_ALL, "usage: warppresent <pitch> <yaw>  (absolute angles; capture was built with %.2f %.2f)\n",
				   tr.warp.refAngles[0], tr.warp.refAngles[1] );
		return;
	}

	if ( !RE_WarpPresent( atof( ri.Cmd_Argv( 1 ) ), atof( ri.Cmd_Argv( 2 ) ) ) ) {
		ri.Printf( PRINT_ALL, "warppresent: nothing presented (see warprstats)\n" );
	}
}

static int WarpCmpFloat( const void *a, const void *b ) {
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : ( x > y ? 1 : 0 );
}

/*
=============
R_WarpPrintRing

P50 / P98 of a mesh warp sample ring (units multiplied by scale); prints nothing-yet when empty.
=============
*/
static void R_WarpPrintRing( const char *name, const char *unit, const float *ring, int samples, float scale ) {
	float tmp[WARP_MESH_RING];
	int n = samples < WARP_MESH_RING ? samples : WARP_MESH_RING;
	int i, hi;

	if ( n <= 0 ) {
		ri.Printf( PRINT_ALL, "warp mesh: %s: no samples\n", name );
		return;
	}

	for ( i = 0; i < n; i++ ) {
		tmp[i] = ring[i] * scale;
	}
	qsort( tmp, n, sizeof( float ), WarpCmpFloat );
	hi = ( n * 98 ) / 100;
	ri.Printf( PRINT_ALL, "warp mesh: %s (last %d) P50 %.2f P98 %.2f max %.2f %s\n", name, n,
			   tmp[( n - 1 ) / 2], tmp[hi >= n ? n - 1 : hi], tmp[n - 1], unit );
}

/*
=============
R_WarpStats_f

warprstats: counters of why captures/warp frames were rejected (see warpReason_t).
=============
*/
void R_WarpStats_f( void ) {
	const int *r = tr.warp.reasons;

	ri.Printf( PRINT_ALL, "warp renderer: %s, capture %s, tail %s (%d bytes)\n", tr.warpFbo ? "targets ok" : "no targets",
			   tr.warp.valid ? "valid" : "invalid", tr.warp.tailValid ? "valid" : "invalid", tr.warp.tailBytes );
	ri.Printf( PRINT_ALL, "warp renderer: capture rejects: notMain %d noFbo %d midFlush %d tail3D %d tailOther %d overflow %d\n",
			   r[WARP_REASON_NOT_MAIN], r[WARP_REASON_NO_FBO], r[WARP_REASON_MID_FLUSH], r[WARP_REASON_TAIL_3D],
			   r[WARP_REASON_TAIL_OTHER], r[WARP_REASON_OVERFLOW] );
	ri.Printf( PRINT_ALL, "warp renderer: warp frames: ok %d, failed: noCapture %d noTail %d applyFail %d stale %d\n",
			   r[WARP_REASON_WARP_OK], r[WARP_REASON_NO_CAPTURE], r[WARP_REASON_NO_TAIL], r[WARP_REASON_APPLY_FAIL],
			   r[WARP_REASON_STALE] );
	ri.Printf( PRINT_ALL, "warp present: calls %d, presented %d, denied %d\n", apCalls, apPresented, apDenied );
	ri.Printf( PRINT_ALL, "warp checkpoints: command %d/%d, afterDepth %d/%d, afterSsao %d/%d, afterOpaque %d/%d (presented/calls)\n",
			   tr.warp.cpPresented[WARP_CP_COMMAND], tr.warp.cpCalls[WARP_CP_COMMAND],
			   tr.warp.cpPresented[WARP_CP_AFTER_DEPTH], tr.warp.cpCalls[WARP_CP_AFTER_DEPTH],
			   tr.warp.cpPresented[WARP_CP_AFTER_SSAO], tr.warp.cpCalls[WARP_CP_AFTER_SSAO],
			   tr.warp.cpPresented[WARP_CP_AFTER_OPAQUE], tr.warp.cpCalls[WARP_CP_AFTER_OPAQUE] );
	ri.Printf( PRINT_ALL, "warp latch: mode %d, calls %d, applied %d, skipped(small) %d, denied %d, noCapture %d\n",
			   r_frameWarp->integer, latchCalls, latchApplied, latchSkipped, latchDenied, latchNoCapture );

	ri.Printf( PRINT_ALL, "warp mesh: level %d, grid %dx%d step %d, velocity resets %d\n", r_frameWarpMesh->integer,
			   tr.warpGridW, tr.warpGridH, tr.warpGridStep, tr.warp.velResets );
	R_WarpPrintRing( "speed", "units/s", tr.warp.speedRing, meshSpeedSamples, 1.0f );
	R_WarpPrintRing( "dt", "ms", tr.warp.dtRing, meshDtSamples, 1000.0f );
	R_WarpPrintRing( "move", "units", tr.warp.moveRing, meshMoveSamples, 1.0f );
	if ( meshDtUnknown ) {
		ri.Printf( PRINT_ALL, "warp mesh: dt unknown for %d presents\n", meshDtUnknown );
	}

	if ( latchSamples > 0 ) {
		float tmp[WARP_LATCH_RING];
		int n = latchSamples < WARP_LATCH_RING ? latchSamples : WARP_LATCH_RING;
		int i, hi = ( n * 98 ) / 100;

		for ( i = 0; i < n; i++ ) {
			tmp[i] = latchRing[i];
		}
		qsort( tmp, n, sizeof( float ), WarpCmpFloat );
		ri.Printf( PRINT_ALL, "warp latch: applied delta (last %d) P50 %.3f P98 %.3f max %.3f deg\n", n,
				   tmp[( n - 1 ) / 2], tmp[hi >= n ? n - 1 : hi], tmp[n - 1] );
	}
}


/*
=============
RE_WarpSetRef

The client passes the view angles the frame is built with, before rendering. RB_WarpCapture stores them with
the capture so the late absolute angles can be turned into a delta.
=============
*/
void RE_WarpSetRef( const vec3_t viewangles ) {
	warpPendingRef[0] = viewangles[PITCH];
	warpPendingRef[1] = viewangles[YAW];
}

/*
=============
RB_WarpGpuWait

Callback for the common side: blocks until the GPU finished the work submitted so far (the post process).
Cheap enough to skip: it is only a fence placed after the post process commands.
=============
*/
typedef void *( APIENTRY *warpFenceSyncProc_t )( GLenum condition, GLbitfield flags );
typedef GLenum ( APIENTRY *warpClientWaitProc_t )( void *sync, GLbitfield flags, GLuint64 timeout );
typedef void ( APIENTRY *warpDeleteSyncProc_t )( void *sync );

void RB_WarpGpuWait( void ) {
	static qboolean initialized;
	static warpFenceSyncProc_t fenceSync;
	static warpClientWaitProc_t clientWait;
	static warpDeleteSyncProc_t deleteSync;

	if ( !initialized ) {
		initialized = qtrue;
		fenceSync = (warpFenceSyncProc_t)SDL_GL_GetProcAddress( "glFenceSync" );
		clientWait = (warpClientWaitProc_t)SDL_GL_GetProcAddress( "glClientWaitSync" );
		deleteSync = (warpDeleteSyncProc_t)SDL_GL_GetProcAddress( "glDeleteSync" );
	}

	if ( fenceSync && clientWait && deleteSync ) {
		// GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117, GL_SYNC_FLUSH_COMMANDS_BIT 1, timeout 100 ms
		void *sync = fenceSync( 0x9117, 0 );

		if ( sync ) {
			clientWait( sync, 1, 100000000ULL );
			deleteSync( sync );
			return;
		}
	}

	qglFinish();
}

/*
=============
RB_WarpLatch

Late-latch (r_frameWarp 1): right after the capture, asks the common side for the freshest absolute view
angles and re-projects the captured view by the difference to the angles the frame was built with.
=============
*/
void RB_WarpLatch( void ) {
	float pitch, yaw, dPitch, dYaw, mag;

	if ( ( r_frameWarp->integer != 1 && r_frameWarp->integer != 2 ) || r_warpDebug->integer || !tr.warpFbo ) {
		return;
	}

	latchCalls++;

	if ( !tr.warp.valid || tr.warp.frameCount != tr.frameCount ) {
		latchNoCapture++;
		return;
	}

	if ( !ri.WarpLatch || !ri.WarpLatch( RB_WarpGpuWait, &pitch, &yaw ) ) {
		latchDenied++;
		return;
	}

	dPitch = pitch - tr.warp.refAngles[0];
	dYaw = AngleNormalize180( yaw - tr.warp.refAngles[1] );

	if ( fabs( dPitch ) < 0.01f && fabs( dYaw ) < 0.01f ) {
		latchSkipped++;
		return;
	}

	if ( !RB_WarpApply( dPitch, dYaw ) ) {
		tr.warp.reasons[WARP_REASON_APPLY_FAIL]++;
		return;
	}

	mag = sqrt( dPitch * dPitch + dYaw * dYaw );
	latchRing[latchSamples++ % WARP_LATCH_RING] = mag;
	latchApplied++;
	RB_GpuMark( "warp_latch" );
}
