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
// tr_pacegpu.c - codmod: fence + timestamp ops for the GPU-synced frame pacer (com_maxfpsGpuSync 2)
//
// mark() drops a GPU timestamp and a fence after the frame's last command and flushes; the
// engine polls the fence without blocking and reads the timestamp once its result is ready.

#ifdef USE_LOCAL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#include "tr_local.h"

#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif

#define PACE_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define PACE_ALREADY_SIGNALED           0x911A
#define PACE_CONDITION_SATISFIED        0x911C

typedef void *( APIENTRY *paceFenceSyncProc_t )( GLenum condition, GLbitfield flags );
typedef GLenum ( APIENTRY *paceClientWaitProc_t )( void *sync, GLbitfield flags, GLuint64 timeout );
typedef void ( APIENTRY *paceDeleteSyncProc_t )( void *sync );
typedef void ( APIENTRY *paceQueryCounterProc_t )( GLuint id, GLenum target );
typedef void ( APIENTRY *paceGetQueryui64vProc_t )( GLuint id, GLenum pname, GLuint64 *params );
typedef void ( APIENTRY *paceGetInteger64vProc_t )( GLenum pname, GLint64 *data );

#define PACE_SLOTS 8

typedef struct {
	void	*sync;
	GLuint	query;
	int		seq;
} paceSlot_t;

static struct {
	qboolean					loaded;
	paceFenceSyncProc_t			fenceSync;
	paceClientWaitProc_t		clientWait;
	paceDeleteSyncProc_t		deleteSync;
	paceQueryCounterProc_t		queryCounter;
	paceGetQueryui64vProc_t		getQueryui64v;
	paceGetInteger64vProc_t		getInteger64v;
	paceSlot_t					slots[PACE_SLOTS];
	int							seq;
} pg;

static int R_PaceGpuMark( void ) {
	paceSlot_t *s = &pg.slots[( pg.seq + 1 ) % PACE_SLOTS];

	if ( s->sync ) {
		pg.deleteSync( s->sync );
		s->sync = NULL;
	}
	pg.queryCounter( s->query, GL_TIMESTAMP );
	s->sync = pg.fenceSync( PACE_SYNC_GPU_COMMANDS_COMPLETE, 0 );
	s->seq = ++pg.seq;
	qglFlush();
	return s->seq;
}

static paceSlot_t *R_PaceGpuSlot( int seq ) {
	paceSlot_t *s;

	if ( seq <= 0 ) {
		return NULL;
	}
	s = &pg.slots[seq % PACE_SLOTS];
	return ( s->seq == seq && s->sync ) ? s : NULL;
}

static int R_PaceGpuPoll( int seq ) {
	paceSlot_t *s = R_PaceGpuSlot( seq );
	GLenum r;

	if ( !s ) {
		return 1;	// evicted (or never marked): treat as done
	}
	r = pg.clientWait( s->sync, 0, 0 );
	return ( r == PACE_ALREADY_SIGNALED || r == PACE_CONDITION_SATISFIED ) ? 1 : 0;
}

static qboolean R_PaceGpuDoneNs( int seq, long long *gpuNs ) {
	paceSlot_t *s = R_PaceGpuSlot( seq );
	GLint available = 0;
	GLuint64 ns = 0;

	if ( !s ) {
		return qfalse;
	}
	qglGetQueryObjectiv( s->query, GL_QUERY_RESULT_AVAILABLE, &available );
	if ( !available ) {
		return qfalse;
	}
	pg.getQueryui64v( s->query, GL_QUERY_RESULT, &ns );
	*gpuNs = (long long)ns;
	return qtrue;
}

static long long R_PaceGpuNowNs( void ) {
	GLint64 t = 0;

	pg.getInteger64v( GL_TIMESTAMP, &t );
	return (long long)t;
}

static const paceGpu_t paceOps = {
	R_PaceGpuMark,
	R_PaceGpuPoll,
	R_PaceGpuDoneNs,
	R_PaceGpuNowNs
};

void R_PaceGpuInit( void ) {
	int i;

	if ( !ri.PaceGpuRegister ) {
		return;
	}
	Com_Memset( &pg, 0, sizeof( pg ) );
	pg.fenceSync = (paceFenceSyncProc_t)SDL_GL_GetProcAddress( "glFenceSync" );
	pg.clientWait = (paceClientWaitProc_t)SDL_GL_GetProcAddress( "glClientWaitSync" );
	pg.deleteSync = (paceDeleteSyncProc_t)SDL_GL_GetProcAddress( "glDeleteSync" );
	pg.queryCounter = (paceQueryCounterProc_t)SDL_GL_GetProcAddress( "glQueryCounter" );
	pg.getQueryui64v = (paceGetQueryui64vProc_t)SDL_GL_GetProcAddress( "glGetQueryObjectui64v" );
	pg.getInteger64v = (paceGetInteger64vProc_t)SDL_GL_GetProcAddress( "glGetInteger64v" );
	if ( !pg.fenceSync || !pg.clientWait || !pg.deleteSync || !pg.queryCounter ||
		 !pg.getQueryui64v || !pg.getInteger64v || !qglGenQueries || !qglGetQueryObjectiv ) {
		ri.Printf( PRINT_DEVELOPER, "GPU pacer: fence/timestamp procs not available\n" );
		return;
	}
	for ( i = 0; i < PACE_SLOTS; i++ ) {
		qglGenQueries( 1, &pg.slots[i].query );
	}
	pg.loaded = qtrue;
	ri.PaceGpuRegister( &paceOps );
}

void R_PaceGpuShutdown( void ) {
	int i;

	if ( !pg.loaded ) {
		return;
	}
	ri.PaceGpuRegister( NULL );
	for ( i = 0; i < PACE_SLOTS; i++ ) {
		if ( pg.slots[i].sync ) {
			pg.deleteSync( pg.slots[i].sync );
		}
		qglDeleteQueries( 1, &pg.slots[i].query );
	}
	Com_Memset( &pg, 0, sizeof( pg ) );
}
