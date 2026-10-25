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

// defined in tr_fbo.c, not in a header
FBO_t *FBO_Create( const char *name, int width, int height );
qboolean R_CheckFBO( const FBO_t *fbo );

// view angles (pitch, yaw) the client built the frame with, set by RE_WarpSetRef before rendering
static float warpPendingRef[2];

#define WARP_LATCH_RING 128
static float latchRing[WARP_LATCH_RING];
static int latchSamples, latchCalls, latchApplied, latchSkipped, latchDenied, latchNoCapture;

// the 2D commands that followed the captured view, kept so a later chunk can replay them over a warped frame
static void *warpTailBuf[( MAX_RENDER_COMMANDS + sizeof( void * ) - 1 ) / sizeof( void * )];

#define WARP_TAIL_SIZE ( (int)sizeof( warpTailBuf ) )

/*
=============
R_WarpInitTargets

Called after FBO_Init: creates the capture target when the feature (or its debug preview) is requested.
=============
*/
void R_WarpInitTargets( void ) {
	tr.warpImage = NULL;
	tr.warpFbo = NULL;

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
RB_WarpApply

Draws the captured main view into tr.renderFbo as seen from the captured direction plus (dPitch, dYaw)
degrees. Returns qfalse when there is nothing valid to warp. Not exported yet; chunk 2 uses it for presents.
=============
*/
qboolean RB_WarpApply( float dPitch, float dYaw ) {
	vec3_t angles, newAxis[3];
	vec4_t box, params;
	mat4_t m;
	float maxDeg = r_frameWarpMaxDeg->value;

	if ( !tr.warpFbo || !tr.warp.valid || !tr.renderFbo ) {
		tr.warp.reasons[WARP_REASON_STALE]++;
		return qfalse;
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
	FBO_BlitFromTexture( tr.warpImage, NULL, NULL, tr.renderFbo, NULL, &tr.warpShader, NULL, GLS_DEPTHTEST_DISABLE );
	return qtrue;
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
		RB_WarpApply( dPitch, dYaw );
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

	if ( !tr.warp.valid || tr.warp.frameCount != tr.frameCount ) {
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

static int WarpCmpFloat( const void *a, const void *b ) {
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : ( x > y ? 1 : 0 );
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
	ri.Printf( PRINT_ALL, "warp latch: mode %d, calls %d, applied %d, skipped(small) %d, denied %d, noCapture %d\n",
			   r_frameWarp->integer, latchCalls, latchApplied, latchSkipped, latchDenied, latchNoCapture );

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

	if ( r_frameWarp->integer != 1 || r_warpDebug->integer || !tr.warpFbo ) {
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
