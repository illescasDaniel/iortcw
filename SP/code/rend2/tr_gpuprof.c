/*
===========================================================================
Return to Castle Wolfenstein single player GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.

This file is part of the Return to Castle Wolfenstein single player GPL Source Code (RTCW SP Source Code).

RTCW SP Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTCW SP Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTCW SP Source Code.  If not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/
// tr_gpuprof.c - codmod: per-pass GPU timings with timestamp queries (r_gpuProfile 1)
//
// RB_GpuMark( "name" ) charges the GPU time since the previous mark to "name". Results are
// read a few frames later without blocking, summed per name over all 3D frames, and printed
// by the "gpuprof" command and at renderer shutdown (so a bench run ends with a table).

#ifdef USE_LOCAL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#include "tr_local.h"
FBO_t *FBO_Create( const char *name, int width, int height );
qboolean R_CheckFBO( const FBO_t *fbo );

#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif

typedef void ( APIENTRY *queryCounterProc_t )( GLuint id, GLenum target );
typedef void ( APIENTRY *getQueryObjectui64vProc_t )( GLuint id, GLenum pname, GLuint64 *params );

#define GPUPROF_FRAMES  8
#define GPUPROF_MARKS   128
#define GPUPROF_NAMES   48
#define GPUPROF_PRESENTS 16384

typedef struct {
	GLuint		queries[GPUPROF_MARKS];
	const char	*names[GPUPROF_MARKS];
	int			numMarks;
	qboolean	has3D;
} gpuProfFrame_t;

typedef struct {
	const char	*name;
	double		totalMs;
	double		maxMs;
	double		frameMs;	// this frame, for maxMs
	qboolean	seen;		// marked this frame
	int			frames;		// frames that hit this mark
} gpuProfStat_t;

static struct {
	qboolean					initialized;
	qboolean					available;
	queryCounterProc_t			queryCounter;
	getQueryObjectui64vProc_t	getQueryObjectui64v;
	gpuProfFrame_t				frames[GPUPROF_FRAMES];
	int							frameIndex;
	gpuProfStat_t				stats[GPUPROF_NAMES];
	int							numStats;
	int							numFrames;
	double						totalMs;
	double						maxFrameMs;
	float						presentMs[GPUPROF_PRESENTS];	// one sample per compositor warp present (mesh warp + 2D tail replay)
	int							numPresents;		// total ever seen; the last GPUPROF_PRESENTS are kept
} gp;

static void R_GpuProfInit( void ) {
	int i;

	gp.initialized = qtrue;
	gp.queryCounter = (queryCounterProc_t)SDL_GL_GetProcAddress( "glQueryCounter" );
	gp.getQueryObjectui64v = (getQueryObjectui64vProc_t)SDL_GL_GetProcAddress( "glGetQueryObjectui64v" );
	gp.available = gp.queryCounter && gp.getQueryObjectui64v && qglGenQueries;
	if ( !gp.available ) {
		ri.Printf( PRINT_WARNING, "r_gpuProfile: timestamp queries not available\n" );
		return;
	}

	for ( i = 0; i < GPUPROF_FRAMES; i++ ) {
		qglGenQueries( GPUPROF_MARKS, gp.frames[i].queries );
		gp.frames[i].numMarks = 0;
	}
}

void R_GpuProfReset( void ) {
	gp.numStats = 0;
	gp.numFrames = 0;
	gp.totalMs = 0;
	gp.maxFrameMs = 0;
	gp.numPresents = 0;
}

static gpuProfStat_t *R_GpuProfStat( const char *name ) {
	int i;

	for ( i = 0; i < gp.numStats; i++ ) {
		if ( gp.stats[i].name == name || !strcmp( gp.stats[i].name, name ) ) {
			return &gp.stats[i];
		}
	}
	if ( gp.numStats == GPUPROF_NAMES ) {
		return NULL;
	}
	gp.stats[gp.numStats].name = name;
	gp.stats[gp.numStats].totalMs = 0;
	gp.stats[gp.numStats].maxMs = 0;
	gp.stats[gp.numStats].frameMs = 0;
	gp.stats[gp.numStats].seen = qfalse;
	gp.stats[gp.numStats].frames = 0;
	return &gp.stats[gp.numStats++];
}

static const char *const warpPresentName = "warp_present";

static int R_GpuProfCmpFloat( const void *a, const void *b ) {
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : ( x > y ? 1 : 0 );
}

// read a finished frame; returns qfalse if its results are not ready yet
static qboolean R_GpuProfHarvest( gpuProfFrame_t *f ) {
	GLint available = 0;
	GLuint64 t[GPUPROF_MARKS];
	double frameMs;
	int i;

	if ( f->numMarks < 2 ) {
		f->numMarks = 0;
		return qtrue;
	}

	qglGetQueryObjectiv( f->queries[f->numMarks - 1], GL_QUERY_RESULT_AVAILABLE, &available );
	if ( !available ) {
		return qfalse;
	}

	for ( i = 0; i < f->numMarks; i++ ) {
		gp.getQueryObjectui64v( f->queries[i], GL_QUERY_RESULT, &t[i] );
	}

	if ( f->has3D ) {
		for ( i = 0; i < gp.numStats; i++ ) {
			gp.stats[i].frameMs = 0;
			gp.stats[i].seen = qfalse;
		}
		for ( i = 1; i < f->numMarks; i++ ) {
			gpuProfStat_t *s = R_GpuProfStat( f->names[i] );
			double ms = ( t[i] - t[i - 1] ) / 1000000.0;

			if ( f->names[i] == warpPresentName ) {
				gp.presentMs[gp.numPresents++ % GPUPROF_PRESENTS] = (float)ms;
			}
			if ( s ) {
				s->frameMs += ms;
				s->totalMs += ms;
				s->seen = qtrue;
			}
		}
		for ( i = 0; i < gp.numStats; i++ ) {
			if ( gp.stats[i].frameMs > gp.stats[i].maxMs ) {
				gp.stats[i].maxMs = gp.stats[i].frameMs;
			}
			if ( gp.stats[i].seen ) {
				gp.stats[i].frames++;
			}
		}
		frameMs = ( t[f->numMarks - 1] - t[0] ) / 1000000.0;
		gp.totalMs += frameMs;
		if ( frameMs > gp.maxFrameMs ) {
			gp.maxFrameMs = frameMs;
		}
		gp.numFrames++;
	}

	f->numMarks = 0;
	f->has3D = qfalse;
	return qtrue;
}

/*
=============
RB_GpuMark

Charge the GPU time since the previous mark of this frame to name (a string literal).
=============
*/
void RB_GpuMark( const char *name ) {
	gpuProfFrame_t *f;

	if ( !r_gpuProfile->integer ) {
		return;
	}
	if ( !gp.initialized ) {
		R_GpuProfInit();
	}
	if ( !gp.available ) {
		return;
	}

	f = &gp.frames[gp.frameIndex];

	// everything drawn by a compositor warp present goes to one bucket; consecutive marks of it
	// collapse into one (the later timestamp replaces the earlier), so a present costs few slots
	if ( tr.warp.presenting ) {
		name = warpPresentName;
		if ( f->numMarks > 0 && f->names[f->numMarks - 1] == name ) {
			f->numMarks--;
		}
	}

	if ( f->numMarks == GPUPROF_MARKS ) {
		return;
	}

	// flush batched 2D/3D surfaces so their draws land before the timestamp
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	gp.queryCounter( f->queries[f->numMarks], GL_TIMESTAMP );
	f->names[f->numMarks] = name;
	f->numMarks++;
}

void RB_GpuMark3D( void ) {
	if ( r_gpuProfile->integer && gp.available ) {
		gp.frames[gp.frameIndex].has3D = qtrue;
	}
}

/*
=============
RB_GpuProfEndFrame

Called once per frame after the last mark (before the swap).
=============
*/
void RB_GpuProfEndFrame( void ) {
	int i;

	if ( !r_gpuProfile->integer || !gp.available ) {
		return;
	}

	gp.frameIndex = ( gp.frameIndex + 1 ) % GPUPROF_FRAMES;

	// harvest every older frame that is ready, oldest first; the slot about to be reused
	// is forced (it is GPUPROF_FRAMES-1 frames old, so normally long finished)
	for ( i = 1; i < GPUPROF_FRAMES; i++ ) {
		gpuProfFrame_t *f = &gp.frames[( gp.frameIndex + i ) % GPUPROF_FRAMES];
		R_GpuProfHarvest( f );
	}
	if ( gp.frames[gp.frameIndex].numMarks ) {
		gp.frames[gp.frameIndex].numMarks = 0;	// still not ready: drop it rather than stall
		gp.frames[gp.frameIndex].has3D = qfalse;
	}
}

void R_GpuProfPrint( void ) {
	int i;

	if ( !gp.numFrames ) {
		ri.Printf( PRINT_ALL, "gpuprof: no 3D frames recorded (set r_gpuProfile 1)\n" );
		return;
	}

	ri.Printf( PRINT_ALL, "codmod gpuprof: frames=%d gpu_avg_ms=%.3f gpu_max_ms=%.3f\n",
		gp.numFrames, gp.totalMs / gp.numFrames, gp.maxFrameMs );
	for ( i = 0; i < gp.numStats; i++ ) {
		gpuProfStat_t *s = &gp.stats[i];

		ri.Printf( PRINT_ALL, "codmod gpuprof: %-16s avg_ms=%6.3f  pct=%5.1f  max_ms=%6.3f  hit=%5.1f%%\n",
			s->name, s->totalMs / gp.numFrames, 100.0 * s->totalMs / gp.totalMs, s->maxMs,
			100.0 * s->frames / gp.numFrames );
	}
	if ( gp.numPresents ) {
		// per present (one warp + 2D tail replay each): the distribution the per-frame sum above hides
		int n = gp.numPresents < GPUPROF_PRESENTS ? gp.numPresents : GPUPROF_PRESENTS;

		qsort( gp.presentMs, n, sizeof( float ), R_GpuProfCmpFloat );
		ri.Printf( PRINT_ALL, "codmod gpuprof: warp_present per present: n=%d p50_ms=%.3f p98_ms=%.3f max_ms=%.3f\n",
			gp.numPresents, gp.presentMs[( n - 1 ) / 2], gp.presentMs[(int)( ( n - 1 ) * 0.98 + 0.5 )], gp.presentMs[n - 1] );
	}
}

void R_GpuProf_f( void ) {
	if ( ri.Cmd_Argc() > 1 && !Q_stricmp( ri.Cmd_Argv( 1 ), "reset" ) ) {
		R_GpuProfReset();
		return;
	}
	R_GpuProfPrint();
}

void R_ShutdownGpuProf( void ) {
	int i;

	if ( gp.numFrames ) {
		R_GpuProfPrint();
	}
	if ( gp.initialized && gp.available ) {
		for ( i = 0; i < GPUPROF_FRAMES; i++ ) {
			qglDeleteQueries( GPUPROF_MARKS, gp.frames[i].queries );
		}
	}
	Com_Memset( &gp, 0, sizeof( gp ) );
}

/*
=============
RB_GpuLoad

Dev-only synthetic GPU load (r_gpuLoad = microseconds per real frame): one full draw of an ALU-heavy shader into a
small private target that is never read. The iteration count self-calibrates from GL_TIME_ELAPSED queries read a few
frames later, so the added cost tracks the requested time. Called only from the world scene pass of a real frame
(never for a warp present, the 2D/HUD path or the present path). Restores the FBO (FBO_BlitFromTexture) and viewport.
=============
*/
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#define GPULOAD_SIZE	512
#define GPULOAD_SLOTS	4

static struct {
	GLuint		queries[GPULOAD_SLOTS];
	qboolean	pending[GPULOAD_SLOTS];
	int			slot;
	float		iters;
	qboolean	failed;
	int			lastFrame;
	getQueryObjectui64vProc_t	getQueryObjectui64v;
} gload;

void RB_GpuLoad( void ) {
	vec4_t color;
	image_t *img;
	int i;

	if ( gload.failed ) {
		return;
	}
	// the world scene can be drawn more than once per frame: charge the load once
	if ( gload.lastFrame == tr.frameCount ) {
		return;
	}
	gload.lastFrame = tr.frameCount;

	if ( !tr.gpuLoadFbo ) {
		// first use, or after a vid_restart (tr is cleared): create the target, shader and queries
		Com_Memset( &gload, 0, sizeof( gload ) );
		gload.getQueryObjectui64v = (getQueryObjectui64vProc_t)SDL_GL_GetProcAddress( "glGetQueryObjectui64v" );
		if ( !gload.getQueryObjectui64v || !qglGenQueries || !qglBeginQuery || !qglEndQuery || !GLSL_InitGpuLoadShader() ) {
			ri.Printf( PRINT_WARNING, "r_gpuLoad: not available\n" );
			gload.failed = qtrue;
			return;
		}
		img = R_CreateImage( "*gpuLoad", NULL, GPULOAD_SIZE, GPULOAD_SIZE, IMGTYPE_COLORALPHA,
							 IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA8 );
		tr.gpuLoadFbo = FBO_Create( "_gpuLoad", GPULOAD_SIZE, GPULOAD_SIZE );
		FBO_AttachImage( tr.gpuLoadFbo, img, GL_COLOR_ATTACHMENT0, 0 );
		if ( !R_CheckFBO( tr.gpuLoadFbo ) ) {
			gload.failed = qtrue;
			return;
		}
		qglGenQueries( GPULOAD_SLOTS, gload.queries );
		gload.iters = 64.0f;
	}

	// harvest finished timings (oldest first, never blocking) and steer the iteration count to the target
	for ( i = 1; i <= GPULOAD_SLOTS; i++ ) {
		int s = ( gload.slot + i ) % GPULOAD_SLOTS;
		GLint available = 0;
		GLuint64 ns = 0;

		if ( !gload.pending[s] ) {
			continue;
		}
		qglGetQueryObjectiv( gload.queries[s], GL_QUERY_RESULT_AVAILABLE, &available );
		if ( !available ) {
			continue;
		}
		gload.getQueryObjectui64v( gload.queries[s], GL_QUERY_RESULT, &ns );
		gload.pending[s] = qfalse;
		if ( ns > 20000 ) {
			float ratio = r_gpuLoad->value * 1000.0f / (float)ns;
			if ( ratio > 4.0f ) ratio = 4.0f;
			if ( ratio < 0.25f ) ratio = 0.25f;
			gload.iters *= 1.0f + 0.5f * ( ratio - 1.0f );
			if ( gload.iters < 1.0f ) gload.iters = 1.0f;
			if ( gload.iters > 4.0e6f ) gload.iters = 4.0e6f;
		}
	}

	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	gload.slot = ( gload.slot + 1 ) % GPULOAD_SLOTS;
	VectorSet4( color, (float)(int)gload.iters, (float)( tr.frameCount & 255 ) * 0.01f, 0, 1 );
	if ( !gload.pending[gload.slot] ) {
		qglBeginQuery( GL_TIME_ELAPSED, gload.queries[gload.slot] );
	}
	FBO_BlitFromTexture( tr.whiteImage, NULL, NULL, tr.gpuLoadFbo, NULL, &tr.gpuLoadShader, color, GLS_DEPTHTEST_DISABLE );
	if ( !gload.pending[gload.slot] ) {
		qglEndQuery( GL_TIME_ELAPSED );
		gload.pending[gload.slot] = qtrue;
	}

	// FBO_BlitFromTexture restores the FBO but leaves the small viewport/scissor behind
	qglViewport( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	qglScissor( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
}
