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
// tr_gpuprof.c - codmod: GPU/CPU render profiler (r_gpuProfile)
//
// r_gpuProfile 1: RB_GpuMark( "name" ) charges the GPU time since the previous mark to "name" (timestamp
//   queries, read a few frames later without blocking). CPU scopes (R_ProfNow / R_ProfCpuEnd) time the
//   front end and back end functions in microseconds. Per-frame rows are kept for the whole run.
// r_gpuProfile 2: additionally brackets every draw batch (RB_EndSurface -> stage iterator) with two
//   timestamps and charges it to its shader, entity kind and stage iterator function. A batch belongs to
//   the pass (mark interval) it ended in. This perturbs the frame rate: use it for the breakdown, not for FPS.
//
// Output: the "codmod gpuprof:" console table (kept byte-compatible, parsed by the rtcw MCP / bench.sh) printed
// by the "gpuprof" command and at renderer shutdown, plus CSV/JSON files written there and by "gpuprof dump"
// to <fs_homepath>/<fs_game>/profiling/ (passes.csv, shaders.csv, cpu_scopes.csv, counters.csv, cascades.csv,
// cvars.csv, frames.csv, run.json). frames.csv rows carry bench_window (1 inside the cl_benchDemo measurement window).
// scripts/bench.sh --profile collects them into profiling/<stamp>-<label>/ with a summary.

#ifdef USE_LOCAL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#include <stdlib.h>
#include <stddef.h>
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
#define GPUPROF_NAMES   96
#define GPUPROF_PRESENTS 16384
#define GPUPROF_DEEP    8192		// batches per frame in mode 2
#define GPUPROF_SHADERS 8192		// distinct (shader, pass, kind, iterator, depth) keys; power of two
#define GPUPROF_MAXROWS 60000

typedef enum {
	CPU_FE_SCENE,
	CPU_FE_VIEW,
	CPU_FE_WORLD,
	CPU_FE_POLYS,
	CPU_FE_ENTS,
	CPU_FE_SORT,
	CPU_BE_TOTAL,
	CPU_BE_DRAWSURFS,
	CPU_BE_LIST,
	CPU_BE_ENDSURFACE,
	CPU_BE_POST,
	CPU_BE_SWAP,
	CPU_MDS_BONES,			// MDS surface assembly: R_CalcBones, lod/index build, vertex skinning, tangents (color / _D depth passes)
	CPU_MDS_BONES_D,
	CPU_MDS_LOD,
	CPU_MDS_LOD_D,
	CPU_MDS_SKIN,
	CPU_MDS_SKIN_D,
	CPU_MDS_TANGENT,
	CPU_MDS_TANGENT_D,
	CPU_MDS_PREVPOS_D,		// SMAA velocity previous positions (depth pass only)
	CPU_BE_UPDATEVAO,		// RB_UpdateTessVao: dynamic vertex upload
	CPU_BE_DRAWELEMENTS,	// R_DrawElements: submit incl. driver time
	CPU_ASM_FIRST,			// surface assembly (rb_surfaceTable call) by kind x color/depth: CPU_ASM_FIRST + kind * 2 + depth
	CPU_NUM = CPU_ASM_FIRST + 12
} cpuScope_t;

// names are the function names; nesting: be_ExecuteRenderCommands > be_DrawSurfs > be_RenderDrawSurfList > be_EndSurface,
// fe_RE_RenderScene > fe_R_RenderView > fe_* (all inclusive, so they overlap). be_GLimp_EndFrame is charged to the next frame.
static const char *const cpuScopeNames[CPU_NUM] = {
	"fe_RE_RenderScene", "fe_R_RenderView", "fe_R_AddWorldSurfaces", "fe_R_AddPolygonSurfaces",
	"fe_R_AddEntitySurfaces", "fe_R_SortDrawSurfs", "be_RB_ExecuteRenderCommands", "be_RB_DrawSurfs",
	"be_RB_RenderDrawSurfList", "be_RB_EndSurface_stage_iterator", "be_RB_PostProcess", "be_GLimp_EndFrame",
	"mds_bones_color", "mds_bones_depth", "mds_lod_index_color", "mds_lod_index_depth", "mds_skin_color", "mds_skin_depth",
	"mds_tangent_color", "mds_tangent_depth", "mds_prevpos_depth", "be_RB_UpdateTessVao", "be_R_DrawElements",
	"assemble_world_color", "assemble_world_depth", "assemble_ent_mdv_color", "assemble_ent_mdv_depth",
	"assemble_ent_mds_color", "assemble_ent_mds_depth", "assemble_ent_mdc_color", "assemble_ent_mdc_depth",
	"assemble_ent_brush_color", "assemble_ent_brush_depth", "assemble_ent_other_color", "assemble_ent_other_depth"
};

// event counters (R_ProfCount), per frame
#define CNT_NUM 10
static const char *const cntNames[CNT_NUM] = {
	"mds_surfaces_color", "mds_surfaces_depth", "mds_verts_emitted_color", "mds_verts_emitted_depth",
	"mds_tris_emitted_color", "mds_tris_emitted_depth", "mds_tangent_verts_color", "mds_tangent_verts_depth",
	"mds_full_lod_surfaces_color", "mds_full_lod_surfaces_depth"
};

// sun cascade re-render reasons (R_ProfSunCascade); 0 = reused
static const char *const cascadeReasons[] = {
	"reused", "cache_invalid", "map_size_changed", "scheduled_refresh", "cache_too_old", "extent_changed",
	"light_axes_changed", "box_moved_past_margin", "not_cached_level"
};
#define CASC_REASONS 9

static const char *const kindNames[] = { "world", "ent_mdv", "ent_mds", "ent_mdc", "ent_brush", "ent_other", "none" };
#define KIND_NONE 6

static const char *const iterNames[] = { "Generic", "Sky", "other" };

typedef struct {
	int			shaderIndex;
	short		kind;
	short		iter;
	short		depthFill;
	short		cascade;		// sun shadow cascade 0..3 or -1
	int			verts;
	int			indexes;
	int			draws;
	int			subdraws;
	float		cpuMs;
} gpuDeepRec_t;

typedef struct {
	GLuint			queries[GPUPROF_MARKS];
	const char		*names[GPUPROF_MARKS];
	int				numMarks;
	qboolean		has3D;
	// mode 2
	GLuint			*deepQ;			// 2 per batch: begin, end
	gpuDeepRec_t	*deepRec;
	int				numDeep;
	int				deepDropped;
	int				deepDrawsStart;
	// per-frame context captured at the scene mark / frame end
	vec3_t			org;
	int				refTime;
	double			cpuMs[CPU_NUM];
	int				cpuCalls[CPU_NUM];
	int				cnt[CNT_NUM];
	int				cascState[4];		// 0 not seen, 1 rendered, 2 reused
	int				cascReason[4];
	int				cascMarkIdx[4];		// mark index closing each cascade's render
	int				cascMarkLevel[4];
	int				numCascMarks;
	float			yaw, pitch;
	int				bench;				// inside the cl_benchDemo measurement window
} gpuProfFrame_t;

typedef struct {
	const char	*name;
	double		totalMs;
	double		maxMs;
	double		frameMs;	// this frame, for maxMs
	qboolean	seen;		// marked this frame
	int			frames;		// frames that hit this mark
	double		batchMs;	// mode 2: time of batches that ended in this pass
	int			batches;
} gpuProfStat_t;

typedef struct {
	qboolean	used;
	int			shaderIndex;
	short		kind, iter, depthFill, pass;
	char		name[MAX_QPATH];
	double		gpuMs, cpuMs, maxMs;
	int			batches;
	int			frames;
	int			lastSeq;
	double		verts, indexes;
	double		draws, subdraws;
} gpuShaderStat_t;

typedef struct {
	int		seq;
	int		refTime;
	float	org[3];
	float	gpuMs;
	int		batches, draws, tris;
	int		subdraws;
	float	yaw, pitch;
	int		bench, benchFrame;
	int		cascState[4], cascReason[4];
	float	cascMs[4];
	float	cnt[CNT_NUM];
	float	cpuMs[CPU_NUM];
	float	passMs[GPUPROF_NAMES];
} gpuFrameRow_t;

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
	int							seq;			// frames ended (3D or not)
	int							droppedFrames;
	int							statsOverflow;
	double						totalMs;
	double						maxFrameMs;
	float						presentMs[GPUPROF_PRESENTS];	// one sample per compositor warp present (mesh warp + 2D tail replay)
	int							numPresents;		// total ever seen; the last GPUPROF_PRESENTS are kept
	// CPU scopes: accumulated since the last frame end
	double						cpuPending[CPU_NUM];
	int							cpuPendingCalls[CPU_NUM];
	double						cpuTotal[CPU_NUM];
	long long					cpuCalls[CPU_NUM];
	// mode 2
	qboolean					deepReady;
	int							deepFrames;
	long long					deepBatches;
	long long					deepDropped;
	gpuShaderStat_t				*shaders;		// GPUPROF_SHADERS, allocated on first mode 2 frame
	int							numShaders;
	int							shaderOverflow;
	// per-frame rows
	gpuFrameRow_t				*rows;
	int							numRows, capRows;
	int							rowsDropped;
	double						freq;
	int							benchOrdinal;
	// event counters and sun cascade state accumulated since the last frame end
	int							cntPending[CNT_NUM];
	int							cascPendingState[4];
	int							cascPendingReason[4];
	// mode 2: per sun cascade totals over the batches drawn into it
	struct {
		double	gpuMs, mdsGpuMs;
		double	batches, mdsBatches, tris, verts;
	}							cascDeep[4];
} gp;

/*
=============
CPU scopes
=============
*/

// returns 0 (and the matching R_ProfCpuEnd does nothing) while the profiler is off
unsigned long long R_ProfNow( void ) {
	if ( !r_gpuProfile || !r_gpuProfile->integer ) {
		return 0;
	}
	if ( !gp.freq ) {
		gp.freq = (double)SDL_GetPerformanceFrequency();
	}
	return SDL_GetPerformanceCounter();
}

static double R_ProfElapsedMs( unsigned long long t0 ) {
	return (double)( SDL_GetPerformanceCounter() - t0 ) * 1000.0 / gp.freq;
}

void R_ProfCpuEnd( int scope, unsigned long long t0 ) {
	if ( !t0 || scope < 0 || scope >= CPU_NUM ) {
		return;
	}
	gp.cpuPending[scope] += R_ProfElapsedMs( t0 );
	gp.cpuPendingCalls[scope]++;
}

void R_ProfCount( int id, int n ) {
	if ( id >= 0 && id < CNT_NUM && r_gpuProfile && r_gpuProfile->integer ) {
		gp.cntPending[id] += n;
	}
}

int RB_GpuDeepKindExt( void );
static int RB_GpuCascadeOfView( void );

// the surface call (rb_surfaceTable) just finished; the current entity decides the kind
void R_ProfAssembleEnd( unsigned long long t0 ) {
	if ( t0 ) {
		R_ProfCpuEnd( CPU_ASM_FIRST + RB_GpuDeepKindExt() * 2 + ( backEnd.depthFill ? 1 : 0 ), t0 );
	}
}

void R_ProfSunCascade( int level, int state, int reason ) {
	if ( r_gpuProfile && r_gpuProfile->integer && level >= 0 && level < 4 ) {
		gp.cascPendingState[level] = state;
		gp.cascPendingReason[level] = reason;
	}
}

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

// mode 2 pools, created on first use
static qboolean R_GpuDeepEnsure( void ) {
	int i;

	if ( gp.deepReady ) {
		return qtrue;
	}
	if ( !gp.available ) {
		return qfalse;
	}
	gp.shaders = malloc( sizeof( gpuShaderStat_t ) * GPUPROF_SHADERS );
	Com_Memset( gp.shaders, 0, sizeof( gpuShaderStat_t ) * GPUPROF_SHADERS );
	for ( i = 0; i < GPUPROF_FRAMES; i++ ) {
		gp.frames[i].deepQ = malloc( sizeof( GLuint ) * 2 * GPUPROF_DEEP );
		gp.frames[i].deepRec = malloc( sizeof( gpuDeepRec_t ) * GPUPROF_DEEP );
		qglGenQueries( 2 * GPUPROF_DEEP, gp.frames[i].deepQ );
		gp.frames[i].numDeep = 0;
	}
	gp.deepReady = qtrue;
	return qtrue;
}

void R_GpuProfReset( void ) {
	int i;

	gp.numStats = 0;
	gp.numFrames = 0;
	gp.totalMs = 0;
	gp.maxFrameMs = 0;
	gp.numPresents = 0;
	gp.numRows = 0;
	gp.rowsDropped = 0;
	gp.droppedFrames = 0;
	gp.statsOverflow = 0;
	gp.benchOrdinal = 0;
	Com_Memset( gp.cascDeep, 0, sizeof( gp.cascDeep ) );
	Com_Memset( gp.cpuTotal, 0, sizeof( gp.cpuTotal ) );
	Com_Memset( gp.cpuCalls, 0, sizeof( gp.cpuCalls ) );
	gp.deepFrames = 0;
	gp.deepBatches = 0;
	gp.deepDropped = 0;
	gp.numShaders = 0;
	gp.shaderOverflow = 0;
	if ( gp.shaders ) {
		Com_Memset( gp.shaders, 0, sizeof( gpuShaderStat_t ) * GPUPROF_SHADERS );
	}
	for ( i = 0; i < GPUPROF_FRAMES; i++ ) {
		gp.frames[i].numMarks = 0;
		gp.frames[i].numDeep = 0;
		gp.frames[i].numCascMarks = 0;
		gp.frames[i].has3D = qfalse;
	}
}

static gpuProfStat_t *R_GpuProfStat( const char *name ) {
	int i;

	for ( i = 0; i < gp.numStats; i++ ) {
		if ( gp.stats[i].name == name || !strcmp( gp.stats[i].name, name ) ) {
			return &gp.stats[i];
		}
	}
	if ( gp.numStats == GPUPROF_NAMES ) {
		gp.statsOverflow++;
		return NULL;
	}
	Com_Memset( &gp.stats[gp.numStats], 0, sizeof( gp.stats[0] ) );
	gp.stats[gp.numStats].name = name;
	return &gp.stats[gp.numStats++];
}

static const char *const warpPresentName = "warp_present";

static int R_GpuProfCmpFloat( const void *a, const void *b ) {
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : ( x > y ? 1 : 0 );
}

static gpuShaderStat_t *R_GpuShaderStat( const gpuDeepRec_t *r, int pass ) {
	unsigned h = (unsigned)r->shaderIndex * 2654435761u + (unsigned)pass * 40503u + (unsigned)r->kind * 131u
		+ (unsigned)r->iter * 17u + (unsigned)r->depthFill * 7u;
	int probe;

	h ^= h >> 15;
	for ( probe = 0; probe < GPUPROF_SHADERS; probe++ ) {
		gpuShaderStat_t *s = &gp.shaders[( h + probe ) & ( GPUPROF_SHADERS - 1 )];

		if ( !s->used ) {
			shader_t *sh = ( r->shaderIndex >= 0 && r->shaderIndex < tr.numShaders ) ? tr.shaders[r->shaderIndex] : NULL;

			Com_Memset( s, 0, sizeof( *s ) );
			s->used = qtrue;
			s->shaderIndex = r->shaderIndex;
			s->kind = r->kind;
			s->iter = r->iter;
			s->depthFill = r->depthFill;
			s->pass = pass;
			s->lastSeq = -1;
			Q_strncpyz( s->name, sh ? sh->name : "?", sizeof( s->name ) );
			gp.numShaders++;
			return s;
		}
		if ( s->shaderIndex == r->shaderIndex && s->pass == pass && s->kind == r->kind && s->iter == r->iter
			&& s->depthFill == r->depthFill ) {
			return s;
		}
	}
	gp.shaderOverflow++;
	return NULL;
}

// read a finished frame; returns qfalse if its results are not ready yet
static qboolean R_GpuProfHarvest( gpuProfFrame_t *f ) {
	GLint available = 0;
	GLuint64 t[GPUPROF_MARKS];
	double frameMs;
	int i;

	if ( f->numMarks < 2 ) {
		f->numMarks = 0;
		f->numDeep = 0;
		f->numCascMarks = 0;
		return qtrue;
	}

	qglGetQueryObjectiv( f->queries[f->numMarks - 1], GL_QUERY_RESULT_AVAILABLE, &available );
	if ( !available ) {
		return qfalse;
	}
	if ( f->numDeep ) {
		available = 0;
		qglGetQueryObjectiv( f->deepQ[2 * f->numDeep - 1], GL_QUERY_RESULT_AVAILABLE, &available );
		if ( !available ) {
			return qfalse;
		}
	}

	for ( i = 0; i < f->numMarks; i++ ) {
		gp.getQueryObjectui64v( f->queries[i], GL_QUERY_RESULT, &t[i] );
	}

	if ( f->has3D ) {
		gpuFrameRow_t *row = NULL;
		int passOf[GPUPROF_MARKS];

		for ( i = 0; i < gp.numStats; i++ ) {
			gp.stats[i].frameMs = 0;
			gp.stats[i].seen = qfalse;
		}
		for ( i = 1; i < f->numMarks; i++ ) {
			gpuProfStat_t *s = R_GpuProfStat( f->names[i] );
			double ms = ( t[i] - t[i - 1] ) / 1000000.0;

			passOf[i] = s ? (int)( s - gp.stats ) : -1;
			if ( f->names[i] == warpPresentName ) {
				gp.presentMs[gp.numPresents++ % GPUPROF_PRESENTS] = (float)ms;
			}
			if ( s ) {
				s->frameMs += ms;
				s->totalMs += ms;
				s->seen = qtrue;
			}
		}
		frameMs = ( t[f->numMarks - 1] - t[0] ) / 1000000.0;

		// per-frame row
		if ( gp.numRows == gp.capRows ) {
			if ( gp.capRows >= GPUPROF_MAXROWS ) {
				gp.rowsDropped++;
			} else {
				int newCap = gp.capRows ? gp.capRows * 2 : 4096;
				gpuFrameRow_t *n = malloc( sizeof( gpuFrameRow_t ) * newCap );

				if ( gp.rows ) {
					Com_Memcpy( n, gp.rows, sizeof( gpuFrameRow_t ) * gp.numRows );
					free( gp.rows );
				}
				gp.rows = n;
				gp.capRows = newCap;
			}
		}
		if ( gp.numRows < gp.capRows ) {
			row = &gp.rows[gp.numRows++];
			Com_Memset( row, 0, sizeof( *row ) );
			row->seq = gp.numFrames;
			row->refTime = f->refTime;
			VectorCopy( f->org, row->org );
			row->gpuMs = (float)frameMs;
			row->yaw = f->yaw;
			row->pitch = f->pitch;
			row->bench = f->bench;
			row->benchFrame = f->bench ? gp.benchOrdinal++ : -1;
			for ( i = 0; i < CPU_NUM; i++ ) {
				row->cpuMs[i] = (float)f->cpuMs[i];
				gp.cpuTotal[i] += f->cpuMs[i];
				gp.cpuCalls[i] += f->cpuCalls[i];
			}
			for ( i = 0; i < CNT_NUM; i++ ) {
				row->cnt[i] = (float)f->cnt[i];
			}
			for ( i = 0; i < 4; i++ ) {
				row->cascState[i] = f->cascState[i];
				row->cascReason[i] = f->cascReason[i];
			}
			for ( i = 0; i < f->numCascMarks; i++ ) {
				int mi = f->cascMarkIdx[i];

				if ( mi >= 1 && mi < f->numMarks ) {
					row->cascMs[f->cascMarkLevel[i]] += (float)( ( t[mi] - t[mi - 1] ) / 1000000.0 );
				}
			}
		}

		// mode 2: attribute each batch to the pass (mark interval) it ended in and to its shader
		if ( f->numDeep ) {
			int k, m = 1;

			gp.deepFrames++;
			gp.deepBatches += f->numDeep;
			gp.deepDropped += f->deepDropped;
			for ( k = 0; k < f->numDeep; k++ ) {
				GLuint64 b, e;
				double ms;
				gpuDeepRec_t *r = &f->deepRec[k];
				gpuShaderStat_t *ss;
				int pass;

				gp.getQueryObjectui64v( f->deepQ[2 * k], GL_QUERY_RESULT, &b );
				gp.getQueryObjectui64v( f->deepQ[2 * k + 1], GL_QUERY_RESULT, &e );
				ms = e > b ? ( e - b ) / 1000000.0 : 0.0;
				while ( m < f->numMarks - 1 && t[m] < e ) {
					m++;
				}
				pass = passOf[m];
				if ( pass >= 0 ) {
					gp.stats[pass].batchMs += ms;
					gp.stats[pass].batches++;
				}
				if ( row ) {
					row->batches++;
					row->draws += r->draws;
					row->subdraws += r->subdraws;
					row->tris += r->indexes / 3;
				}
				if ( r->cascade >= 0 ) {
					int cv = r->cascade;

					gp.cascDeep[cv].gpuMs += ms;
					gp.cascDeep[cv].batches++;
					gp.cascDeep[cv].tris += r->indexes / 3;
					gp.cascDeep[cv].verts += r->verts;
					if ( r->kind == 2 ) {
						gp.cascDeep[cv].mdsGpuMs += ms;
						gp.cascDeep[cv].mdsBatches++;
					}
				}
				ss = R_GpuShaderStat( r, pass < 0 ? 0 : pass );
				if ( ss ) {
					ss->gpuMs += ms;
					ss->cpuMs += r->cpuMs;
					if ( ms > ss->maxMs ) {
						ss->maxMs = ms;
					}
					ss->batches++;
					ss->verts += r->verts;
					ss->indexes += r->indexes;
					ss->draws += r->draws;
					ss->subdraws += r->subdraws;
					if ( ss->lastSeq != gp.numFrames ) {
						ss->lastSeq = gp.numFrames;
						ss->frames++;
					}
				}
			}
		}

		for ( i = 0; i < gp.numStats; i++ ) {
			if ( gp.stats[i].frameMs > gp.stats[i].maxMs ) {
				gp.stats[i].maxMs = gp.stats[i].frameMs;
			}
			if ( gp.stats[i].seen ) {
				gp.stats[i].frames++;
			}
			if ( row ) {
				row->passMs[i] = (float)gp.stats[i].frameMs;
			}
		}
		gp.totalMs += frameMs;
		if ( frameMs > gp.maxFrameMs ) {
			gp.maxFrameMs = frameMs;
		}
		gp.numFrames++;
	}

	f->numMarks = 0;
	f->numDeep = 0;
	f->numCascMarks = 0;
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
		gpuProfFrame_t *f = &gp.frames[gp.frameIndex];

		f->has3D = qtrue;
		VectorCopy( backEnd.refdef.vieworg, f->org );
		f->refTime = backEnd.refdef.time;
		f->yaw = atan2f( backEnd.refdef.viewaxis[0][1], backEnd.refdef.viewaxis[0][0] ) * ( 180.0f / (float)M_PI );
		f->pitch = -asinf( Com_Clamp( -1.0f, 1.0f, backEnd.refdef.viewaxis[0][2] ) ) * ( 180.0f / (float)M_PI );
		f->bench = ri.Cvar_VariableIntegerValue( "cl_benchMeasuring" );
	}
}

// remember which mark closed the sun shadow render of the current cascade (the "sunshadow" pass)
void RB_GpuMarkCascade( void ) {
	gpuProfFrame_t *f;
	int level;

	if ( !r_gpuProfile->integer || !gp.available ) {
		return;
	}
	f = &gp.frames[gp.frameIndex];
	level = RB_GpuCascadeOfView();
	if ( level < 0 || f->numMarks == 0 || f->numCascMarks >= 4 ) {
		return;
	}
	f->cascMarkIdx[f->numCascMarks] = f->numMarks - 1;
	f->cascMarkLevel[f->numCascMarks] = level;
	f->numCascMarks++;
}

/*
=============
RB_GpuDeepBegin / RB_GpuDeepEnd

Mode 2: bracket one draw batch (the stage iterator call in RB_EndSurface) with two timestamps. Returns the
record index or -1 (profiler off, not mode 2, a warp present, or the frame's pool is full). The caller passes
the R_ProfNow() value taken before Begin to End, which also charges the CPU submit time.
=============
*/
static int RB_GpuDrawCounter( void ) {
	return backEnd.pc.c_physDraws;
}

static int RB_GpuSubdrawCounter( void ) {
	return backEnd.pc.c_physSubdraws;
}

static int RB_GpuCascadeOfView( void ) {
	int i;

	if ( !( backEnd.viewParms.flags & VPF_DEPTHSHADOW ) || !( backEnd.viewParms.flags & VPF_ORTHOGRAPHIC ) ) {
		return -1;
	}
	for ( i = 0; i < 4; i++ ) {
		if ( backEnd.viewParms.targetFbo && backEnd.viewParms.targetFbo == tr.sunShadowFbo[i] ) {
			return i;
		}
	}
	return -1;
}

static int RB_GpuDeepIter( void ) {
	if ( tess.currentStageIteratorFunc == RB_StageIteratorGeneric ) return 0;
	if ( tess.currentStageIteratorFunc == RB_StageIteratorSky ) return 1;
	return 2;
}

static int RB_GpuDeepKind( void );
int RB_GpuDeepKindExt( void ) {
	return RB_GpuDeepKind();
}

static int RB_GpuDeepKind( void ) {
	const trRefEntity_t *e = backEnd.currentEntity;

	if ( !e || e == &tr.worldEntity ) {
		return 0;
	}
	if ( e->e.reType == RT_MODEL ) {
		model_t *m = R_GetModelByHandle( e->e.hModel );

		switch ( m->type ) {
		case MOD_MESH:	return 1;
		case MOD_MDS:	return 2;
		case MOD_MDC:	return 3;
		case MOD_BRUSH:	return 4;
		default:		break;
		}
	}
	return 5;
}

int RB_GpuDeepBegin( void ) {
	gpuProfFrame_t *f;
	gpuDeepRec_t *r;

	if ( r_gpuProfile->integer < 2 || tr.warp.presenting ) {
		return -1;
	}
	if ( !gp.initialized ) {
		R_GpuProfInit();
	}
	if ( !R_GpuDeepEnsure() ) {
		return -1;
	}
	f = &gp.frames[gp.frameIndex];
	if ( f->numDeep == GPUPROF_DEEP ) {
		f->deepDropped++;
		return -1;
	}
	r = &f->deepRec[f->numDeep];
	r->shaderIndex = tess.shader ? tess.shader->index : -1;
	r->kind = (short)RB_GpuDeepKind();
	r->iter = (short)RB_GpuDeepIter();
	r->depthFill = backEnd.depthFill ? 1 : 0;
	r->verts = tess.numVertexes;
	r->indexes = tess.numIndexes;
	r->draws = RB_GpuDrawCounter();
	r->subdraws = RB_GpuSubdrawCounter();
	r->cascade = (short)RB_GpuCascadeOfView();
	r->cpuMs = 0;
	gp.queryCounter( f->deepQ[2 * f->numDeep], GL_TIMESTAMP );
	return f->numDeep;
}

void RB_GpuDeepEnd( int idx, unsigned long long t0 ) {
	gpuProfFrame_t *f = &gp.frames[gp.frameIndex];

	if ( idx >= 0 ) {
		gpuDeepRec_t *r = &f->deepRec[idx];

		gp.queryCounter( f->deepQ[2 * idx + 1], GL_TIMESTAMP );
		r->draws = RB_GpuDrawCounter() - r->draws;
		r->subdraws = RB_GpuSubdrawCounter() - r->subdraws;
		if ( t0 ) {
			r->cpuMs = (float)R_ProfElapsedMs( t0 );
		}
		f->numDeep = idx + 1;
	}
	R_ProfCpuEnd( CPU_BE_ENDSURFACE, t0 );
}

/*
=============
RB_GpuProfEndFrame

Called once per frame after the last mark (before the swap).
=============
*/
void RB_GpuProfEndFrame( void ) {
	int i;
	gpuProfFrame_t *cur;

	if ( !r_gpuProfile->integer || !gp.available ) {
		return;
	}

	// hand the CPU scopes accumulated since the last frame end to this frame
	cur = &gp.frames[gp.frameIndex];
	for ( i = 0; i < CPU_NUM; i++ ) {
		cur->cpuMs[i] = gp.cpuPending[i];
		cur->cpuCalls[i] = gp.cpuPendingCalls[i];
		gp.cpuPending[i] = 0;
		gp.cpuPendingCalls[i] = 0;
	}
	for ( i = 0; i < CNT_NUM; i++ ) {
		cur->cnt[i] = gp.cntPending[i];
		gp.cntPending[i] = 0;
	}
	for ( i = 0; i < 4; i++ ) {
		cur->cascState[i] = gp.cascPendingState[i];
		cur->cascReason[i] = gp.cascPendingReason[i];
		gp.cascPendingState[i] = 0;
		gp.cascPendingReason[i] = 0;
	}
	gp.seq++;

	gp.frameIndex = ( gp.frameIndex + 1 ) % GPUPROF_FRAMES;

	// harvest every older frame that is ready, oldest first; the slot about to be reused
	// is forced (it is GPUPROF_FRAMES-1 frames old, so normally long finished)
	for ( i = 1; i < GPUPROF_FRAMES; i++ ) {
		gpuProfFrame_t *f = &gp.frames[( gp.frameIndex + i ) % GPUPROF_FRAMES];
		R_GpuProfHarvest( f );
	}
	cur = &gp.frames[gp.frameIndex];
	if ( cur->numMarks ) {
		cur->numMarks = 0;	// still not ready: drop it rather than stall
		cur->numCascMarks = 0;
		cur->has3D = qfalse;
		gp.droppedFrames++;
	}
	cur->numDeep = 0;
	cur->deepDropped = 0;
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

/*
=============
File output (profiling/ CSVs and run.json under the game's home directory)
=============
*/
static char *outBuf;
static int outLen, outCap;

static void Out_Printf( const char *fmt, ... ) {
	va_list ap;
	char tmp[2048];
	int n;

	va_start( ap, fmt );
	n = Q_vsnprintf( tmp, sizeof( tmp ), fmt, ap );
	va_end( ap );
	if ( n < 0 || n >= (int)sizeof( tmp ) ) {
		n = (int)strlen( tmp );
	}
	if ( outLen + n + 1 > outCap ) {
		int newCap = outCap ? outCap * 2 : 1 << 16;
		char *nb;

		while ( newCap < outLen + n + 1 ) {
			newCap *= 2;
		}
		nb = malloc( newCap );
		if ( outBuf ) {
			Com_Memcpy( nb, outBuf, outLen );
			free( outBuf );
		}
		outBuf = nb;
		outCap = newCap;
	}
	Com_Memcpy( outBuf + outLen, tmp, n );
	outLen += n;
}

static void Out_Write( const char *name ) {
	if ( outLen ) {
		ri.FS_WriteFile( va( "profiling/%s", name ), outBuf, outLen );
	}
	outLen = 0;
}

static void Out_Free( void ) {
	if ( outBuf ) {
		free( outBuf );
	}
	outBuf = NULL;
	outLen = outCap = 0;
}

static const char *R_GpuColName( const char *name, char *buf, int size ) {
	int i;

	Q_strncpyz( buf, name, size );
	for ( i = 0; buf[i]; i++ ) {
		if ( !( ( buf[i] >= 'a' && buf[i] <= 'z' ) || ( buf[i] >= 'A' && buf[i] <= 'Z' ) || ( buf[i] >= '0' && buf[i] <= '9' ) ) ) {
			buf[i] = '_';
		}
	}
	return buf;
}

typedef struct {
	float	p50, p95, p99, max;
} pctl_t;

// percentiles of one column of the frame rows (0 where the pass did not run)
static pctl_t R_GpuColumnPctl( float *scratch, int stride, int offsetFloats, int mode ) {
	pctl_t p;
	int i, n = gp.numRows;

	for ( i = 0; i < n; i++ ) {
		const float *base = (const float *)&gp.rows[i];

		scratch[i] = base[offsetFloats];
	}
	(void)stride;
	(void)mode;
	qsort( scratch, n, sizeof( float ), R_GpuProfCmpFloat );
	p.p50 = scratch[( n - 1 ) / 2];
	p.p95 = scratch[(int)( ( n - 1 ) * 0.95 + 0.5 )];
	p.p99 = scratch[(int)( ( n - 1 ) * 0.99 + 0.5 )];
	p.max = scratch[n - 1];
	return p;
}

static int R_GpuCmpShader( const void *a, const void *b ) {
	const gpuShaderStat_t *x = *(gpuShaderStat_t *const *)a, *y = *(gpuShaderStat_t *const *)b;

	return x->gpuMs < y->gpuMs ? 1 : ( x->gpuMs > y->gpuMs ? -1 : 0 );
}

void R_GpuProfDump( void ) {
	int i, n = gp.numRows;
	float *scratch;
	char col[80];

	if ( !gp.numFrames || !n ) {
		ri.Printf( PRINT_ALL, "gpuprof dump: no 3D frames recorded (set r_gpuProfile 1)\n" );
		return;
	}
	scratch = malloc( sizeof( float ) * n );

	// passes.csv
	Out_Printf( "pass,avg_ms_per_frame,share_pct,avg_ms_when_run,p50_ms,p95_ms,p99_ms,max_ms,hit_pct,"
		"batch_ms_per_frame,non_batch_ms_per_frame,batches_per_frame\n" );
	for ( i = 0; i < gp.numStats; i++ ) {
		gpuProfStat_t *s = &gp.stats[i];
		pctl_t p = R_GpuColumnPctl( scratch, 0, (int)( offsetof( gpuFrameRow_t, passMs ) / sizeof( float ) ) + i, 0 );
		double avg = s->totalMs / gp.numFrames;

		Out_Printf( "%s,%.4f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.1f,", s->name, avg, 100.0 * s->totalMs / gp.totalMs,
			s->frames ? s->totalMs / s->frames : 0.0, p.p50, p.p95, p.p99, p.max, 100.0 * s->frames / gp.numFrames );
		if ( gp.deepFrames ) {
			Out_Printf( "%.4f,%.4f,%.1f\n", s->batchMs / gp.deepFrames, avg - s->batchMs / gp.deepFrames,
				(double)s->batches / gp.deepFrames );
		} else {
			Out_Printf( ",,\n" );
		}
	}
	Out_Write( "passes.csv" );

	// cpu_scopes.csv
	Out_Printf( "scope,avg_ms_per_frame,p50_ms,p95_ms,p99_ms,max_ms,calls_per_frame,avg_us_per_call\n" );
	for ( i = 0; i < CPU_NUM; i++ ) {
		pctl_t p = R_GpuColumnPctl( scratch, 0, (int)( offsetof( gpuFrameRow_t, cpuMs ) / sizeof( float ) ) + i, 0 );

		Out_Printf( "%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%.3f\n", cpuScopeNames[i], gp.cpuTotal[i] / n, p.p50, p.p95, p.p99, p.max,
			(double)gp.cpuCalls[i] / n, gp.cpuCalls[i] ? gp.cpuTotal[i] * 1000.0 / gp.cpuCalls[i] : 0.0 );
	}
	Out_Write( "cpu_scopes.csv" );

	// counters.csv: per-frame event counters, over all frames and inside the bench window
	Out_Printf( "counter,avg_per_frame_all,avg_per_frame_bench_window\n" );
	for ( i = 0; i < CNT_NUM; i++ ) {
		double all = 0, win = 0;
		int k, nw = 0;

		for ( k = 0; k < n; k++ ) {
			all += gp.rows[k].cnt[i];
			if ( gp.rows[k].bench ) {
				win += gp.rows[k].cnt[i];
				nw++;
			}
		}
		Out_Printf( "%s,%.3f,%.3f\n", cntNames[i], all / n, nw ? win / nw : 0.0 );
	}
	Out_Write( "counters.csv" );

	// cascades.csv: sun shadow cascade render/reuse decisions, GPU interval of the render, mode 2 geometry
	Out_Printf( "cascade,frames_rendered,frames_reused,frames_unseen" );
	for ( i = 1; i < CASC_REASONS; i++ ) {
		Out_Printf( ",rerender_%s", cascadeReasons[i] );
	}
	Out_Printf( ",avg_ms_when_rendered,max_ms,batches_per_render,tris_per_render,verts_per_render,gpu_batch_ms_per_render,"
		"mds_batches_per_render,mds_gpu_batch_ms_per_render\n" );
	for ( i = 0; i < 4; i++ ) {
		int k, rendered = 0, reused = 0, unseen = 0, reason[CASC_REASONS] = { 0 };
		double ms = 0, mx = 0;

		for ( k = 0; k < n; k++ ) {
			const gpuFrameRow_t *r = &gp.rows[k];

			if ( r->cascState[i] == 1 ) {
				rendered++;
				if ( r->cascReason[i] >= 0 && r->cascReason[i] < CASC_REASONS ) {
					reason[r->cascReason[i]]++;
				}
				ms += r->cascMs[i];
				if ( r->cascMs[i] > mx ) {
					mx = r->cascMs[i];
				}
			} else if ( r->cascState[i] == 2 ) {
				reused++;
			} else {
				unseen++;
			}
		}
		Out_Printf( "%d,%d,%d,%d", i, rendered, reused, unseen );
		for ( k = 1; k < CASC_REASONS; k++ ) {
			Out_Printf( ",%d", reason[k] );
		}
		Out_Printf( ",%.4f,%.4f", rendered ? ms / rendered : 0.0, mx );
		if ( gp.deepFrames && rendered ) {
			Out_Printf( ",%.1f,%.0f,%.0f,%.4f,%.1f,%.4f\n", gp.cascDeep[i].batches / rendered, gp.cascDeep[i].tris / rendered,
				gp.cascDeep[i].verts / rendered, gp.cascDeep[i].gpuMs / rendered, gp.cascDeep[i].mdsBatches / rendered,
				gp.cascDeep[i].mdsGpuMs / rendered );
		} else {
			Out_Printf( ",,,,,,\n" );
		}
	}
	Out_Write( "cascades.csv" );

	// cvars.csv: the renderer/pacing settings this capture ran with (value strings as the engine holds them)
	{
		static const char *const names[] = {
			"r_frameWarp", "r_aa", "r_ao", "r_smaa", "r_smaaTemporal", "r_smaaVelocity", "r_ext_framebuffer_multisample", "r_depthPrepass",
			"r_sunShadows", "r_sunShadowStable", "r_sunShadowCascadeUpdate", "r_shadowMapSize", "r_shadowFilter", "r_shadowBlur",
			"r_shadowCascadeZNear", "r_shadowCascadeZFar", "r_shadowCascadeZBias", "r_flashlightShadow", "r_flashlightShadowSize",
			"r_normalMapping", "r_specularMapping", "r_deluxeMapping", "r_parallaxMapping", "r_parallaxMapShadows", "r_parallaxMaxSteps",
			"r_parallaxStepDensity", "r_parallaxShadowFade", "r_parallaxFadeDist", "r_cubeMapping", "r_dlightMode", "r_forceSun",
			"r_sunlightMode", "r_drawSunRays", "r_bloom", "r_grade", "r_toneMap", "r_autoExposure", "r_hdr", "r_postProcess",
			"r_ssao", "r_ssaoMode", "r_ssaoSamples", "r_ssaoSlices", "r_ssaoSteps", "r_ssaoDepthMips", "r_lodscale", "r_lodbias",
			"r_picmip", "r_textureMode", "r_ext_max_anisotropy", "r_mode", "r_customwidth", "r_customheight", "r_fullscreen",
			"r_swapInterval", "r_vsync", "r_finish", "r_swapFinish", "r_flares", "r_dynamiclight", "r_mergeLightmaps",
			"r_vaoCacheStats", "r_gpuProfile", "r_gpuLoad", "com_maxfps", "com_maxfpsMode", "com_maxfpsGpuSync", "cl_benchDemo",
			"cg_shadows", "cg_skybox"
		};
		int k;

		Out_Printf( "cvar,value\n" );
		for ( k = 0; k < (int)( sizeof( names ) / sizeof( names[0] ) ); k++ ) {
			char v[MAX_STRING_CHARS];
			int m;

			Q_strncpyz( v, ri.Cvar_Get( names[k], "", 0 )->string, sizeof( v ) );
			for ( m = 0; v[m]; m++ ) {
				if ( v[m] == ',' || v[m] == '\n' ) v[m] = ' ';
			}
			Out_Printf( "%s,%s\n", names[k], v );
		}
		Out_Write( "cvars.csv" );
	}

	// frames.csv
	Out_Printf( "frame,ref_time_ms,x,y,z,gpu_ms,batches,draws,tris,subdraws,yaw,pitch,bench_window,bench_frame" );
	for ( i = 0; i < 4; i++ ) {
		Out_Printf( ",casc%d_state,casc%d_reason,casc%d_ms", i, i, i );
	}
	for ( i = 0; i < CNT_NUM; i++ ) {
		Out_Printf( ",cnt_%s", cntNames[i] );
	}
	for ( i = 0; i < CPU_NUM; i++ ) {
		Out_Printf( ",cpu_%s_ms", cpuScopeNames[i] );
	}
	for ( i = 0; i < gp.numStats; i++ ) {
		Out_Printf( ",pass_%s_ms", R_GpuColName( gp.stats[i].name, col, sizeof( col ) ) );
	}
	Out_Printf( "\n" );
	for ( i = 0; i < n; i++ ) {
		const gpuFrameRow_t *r = &gp.rows[i];
		int k;

		Out_Printf( "%d,%d,%.1f,%.1f,%.1f,%.4f,%d,%d,%d,%d,%.1f,%.1f,%d,%d", r->seq, r->refTime, r->org[0], r->org[1], r->org[2], r->gpuMs,
			r->batches, r->draws, r->tris, r->subdraws, r->yaw, r->pitch, r->bench, r->benchFrame );
		for ( k = 0; k < 4; k++ ) {
			Out_Printf( ",%d,%d,%.4f", r->cascState[k], r->cascReason[k], r->cascMs[k] );
		}
		for ( k = 0; k < CNT_NUM; k++ ) {
			Out_Printf( ",%.0f", r->cnt[k] );
		}
		for ( k = 0; k < CPU_NUM; k++ ) {
			Out_Printf( ",%.4f", r->cpuMs[k] );
		}
		for ( k = 0; k < gp.numStats; k++ ) {
			Out_Printf( ",%.4f", r->passMs[k] );
		}
		Out_Printf( "\n" );
	}
	Out_Write( "frames.csv" );

	// shaders.csv (mode 2)
	if ( gp.deepFrames && gp.shaders ) {
		gpuShaderStat_t **list = malloc( sizeof( gpuShaderStat_t * ) * GPUPROF_SHADERS );
		int num = 0, rank;

		for ( i = 0; i < GPUPROF_SHADERS; i++ ) {
			if ( gp.shaders[i].used ) {
				list[num++] = &gp.shaders[i];
			}
		}
		qsort( list, num, sizeof( list[0] ), R_GpuCmpShader );
		Out_Printf( "rank,shader,pass,kind,iterator,depth_fill,gpu_ms_per_frame,gpu_pct_of_frame,batches_per_frame,"
			"gpu_us_per_batch,max_batch_us,cpu_ms_per_frame,verts_per_frame,tris_per_frame,draws_per_frame,subdraws_per_frame,hit_pct\n" );
		for ( rank = 0; rank < num; rank++ ) {
			const gpuShaderStat_t *s = list[rank];
			char nm[MAX_QPATH];
			int k;

			Q_strncpyz( nm, s->name, sizeof( nm ) );
			for ( k = 0; nm[k]; k++ ) {
				if ( nm[k] == ',' ) nm[k] = '_';
			}
			Out_Printf( "%d,%s,%s,%s,%s,%d,%.5f,%.3f,%.3f,%.2f,%.1f,%.5f,%.1f,%.1f,%.2f,%.2f,%.1f\n", rank + 1, nm,
				gp.stats[s->pass].name, kindNames[s->kind], iterNames[s->iter], s->depthFill,
				s->gpuMs / gp.deepFrames, 100.0 * s->gpuMs / ( gp.totalMs * gp.deepFrames / gp.numFrames ),
				(double)s->batches / gp.deepFrames, s->batches ? s->gpuMs * 1000.0 / s->batches : 0.0, s->maxMs * 1000.0,
				s->cpuMs / gp.deepFrames, s->verts / gp.deepFrames, s->indexes / 3.0 / gp.deepFrames,
				s->draws / gp.deepFrames, s->subdraws / gp.deepFrames, 100.0 * s->frames / gp.deepFrames );
		}
		Out_Write( "shaders.csv" );
		free( list );
	}

	// run.json
	Out_Printf( "{\n \"mode\": %d,\n \"frames_3d\": %d,\n \"frames_deep\": %d,\n \"frames_dropped_unready\": %d,\n"
		" \"rows_dropped_cap\": %d,\n \"pass_names_overflow\": %d,\n \"shader_keys\": %d,\n \"shader_key_overflow\": %d,\n"
		" \"batches_recorded\": %lld,\n \"batches_dropped_pool_full\": %lld,\n \"gpu_avg_ms\": %.4f,\n \"gpu_max_ms\": %.4f,\n"
		" \"resolution\": \"%dx%d\",\n \"gl_renderer\": \"%s\",\n \"gl_version\": \"%s\",\n"
		" \"r_frameWarp\": %d,\n \"r_aa\": %d,\n \"r_ao\": %d\n}\n",
		r_gpuProfile->integer, gp.numFrames, gp.deepFrames, gp.droppedFrames, gp.rowsDropped, gp.statsOverflow,
		gp.numShaders, gp.shaderOverflow, gp.deepBatches, gp.deepDropped, gp.totalMs / gp.numFrames, gp.maxFrameMs,
		glConfig.vidWidth, glConfig.vidHeight, glConfig.renderer_string, glConfig.version_string,
		ri.Cvar_VariableIntegerValue( "r_frameWarp" ), ri.Cvar_VariableIntegerValue( "r_aa" ),
		ri.Cvar_VariableIntegerValue( "r_ao" ) );
	Out_Write( "run.json" );

	Out_Free();
	free( scratch );
	ri.Printf( PRINT_ALL, "gpuprof: wrote profiling/{passes,cpu_scopes,counters,cascades,cvars,frames%s}.csv and run.json (%d frames)\n",
		gp.deepFrames ? ",shaders" : "", n );
}

void R_GpuProf_f( void ) {
	if ( ri.Cmd_Argc() > 1 && !Q_stricmp( ri.Cmd_Argv( 1 ), "reset" ) ) {
		R_GpuProfReset();
		return;
	}
	if ( ri.Cmd_Argc() > 1 && !Q_stricmp( ri.Cmd_Argv( 1 ), "dump" ) ) {
		R_GpuProfDump();
		return;
	}
	R_GpuProfPrint();
}

void R_ShutdownGpuProf( void ) {
	int i;

	if ( gp.numFrames ) {
		R_GpuProfPrint();
		R_GpuProfDump();
	}
	if ( gp.initialized && gp.available ) {
		for ( i = 0; i < GPUPROF_FRAMES; i++ ) {
			qglDeleteQueries( GPUPROF_MARKS, gp.frames[i].queries );
			if ( gp.deepReady ) {
				qglDeleteQueries( 2 * GPUPROF_DEEP, gp.frames[i].deepQ );
				free( gp.frames[i].deepQ );
				free( gp.frames[i].deepRec );
			}
		}
	}
	if ( gp.shaders ) {
		free( gp.shaders );
	}
	if ( gp.rows ) {
		free( gp.rows );
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
