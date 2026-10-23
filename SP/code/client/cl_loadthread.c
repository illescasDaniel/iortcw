/*
===========================================================================
Loading worker thread: "offload with keepalive" (codmod).

Com_RunOffloaded( fn, arg ) hands one CPU-only job (HD texture decode, big WAV decode,
CM_LoadMap) to a single persistent worker thread while the main thread keeps the loading
screen alive by re-presenting the last loading frame (CL_LoadingReplayTick, rate limited
to com_loadingFps) until the job is done.

It is a SYNCHRONOUS hand-off: the main thread never runs engine code while the worker
does (it only waits on a semaphore and runs the replay presenter, which uses raw GL and
touches no Z/Hunk/FS/Cvar state), so the engine's non-thread-safe allocators, file system,
cvar and print code need no locking. The semaphores provide the memory barriers.

Rules for offloaded code: no GL, no OpenAL, no VM calls, no screen updates. Loading ticks
called from inside a job are no-ops (CL_LoadingTick / CL_LoadingReplayTick check
Com_OffloadIsWorker). An error raised on the worker (Com_Error / Sys_Error) would longjmp
across threads, so Com_Error hands it to Com_OffloadWorkerError: the message is stored, the
main thread is woken and the worker parks forever. The main thread then abandons (detaches)
that worker and re-raises the error itself with Com_Error; a new worker is created on the
next job.

Everything is gated on com_loadingThread > 0 (pinned in codmod.cfg, code default 0 =
vanilla), com_loadingFps > 0 and a local map load being on screen; otherwise fn( arg ) is
called directly.
===========================================================================
*/

#include "client.h"

#ifdef USE_LOCAL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#define LT_WAIT_MS 4        // max time between replay ticks while waiting for the worker

typedef struct {
	SDL_Thread *thread;
	SDL_sem *startSem;          // main -> worker: a job is ready (or quit)
	SDL_sem *doneSem;           // worker -> main: job finished (or failed)
	SDL_sem *parkSem;           // never posted: a failed worker blocks here forever
	void ( *fn )( void * );
	void *arg;
	volatile int quit;
	volatile int failed;
	int errCode;
	char errMsg[MAXPRINTMSG];
} ltWorker_t;

static ltWorker_t *lt_worker;           // current worker; only changed by the main thread, never during a job
static volatile SDL_threadID lt_workerId;   // thread id of lt_worker (0 = none)
static qboolean lt_inJob;               // main thread: a job is running

/*
==================
Com_OffloadIsWorker
==================
*/
qboolean Com_OffloadIsWorker( void ) {
	SDL_threadID id = lt_workerId;

	return id != 0 && SDL_ThreadID() == id;
}

/*
==================
LT_WorkerMain
==================
*/
static int LT_WorkerMain( void *data ) {
	ltWorker_t *w = (ltWorker_t *)data;

	for ( ;; ) {
		SDL_SemWait( w->startSem );
		if ( w->quit ) {
			return 0;
		}
		w->fn( w->arg );
		SDL_SemPost( w->doneSem );
	}
}

/*
==================
Com_OffloadWorkerError

Called on the worker from Com_Error / Sys_Error. Never returns: the worker parks until the
process exits (the main thread detaches it).
==================
*/
void Com_OffloadWorkerError( int code, const char *msg ) {
	ltWorker_t *w = lt_worker;

	if ( !w ) {
		for ( ;; ) {
			SDL_Delay( 1000 );      // cannot happen (a worker exists while lt_workerId is set)
		}
	}
	w->errCode = code;
	Q_strncpyz( w->errMsg, msg, sizeof( w->errMsg ) );
	w->failed = 1;
	SDL_SemPost( w->doneSem );
	for ( ;; ) {
		SDL_SemWait( w->parkSem );
	}
}

/*
==================
LT_Wanted

Main thread only.
==================
*/
static qboolean LT_Wanted( void ) {
	if ( lt_inJob || Com_OffloadIsWorker() ) {
		return qfalse;
	}
	if ( Cvar_VariableIntegerValue( "com_loadingThread" ) <= 0 ) {
		return qfalse;
	}
	return CL_LoadingFrameWanted() && re.PresentLoadingSnapshot != NULL;
}

/*
==================
LT_Create

Returns qfalse if the worker cannot be created (the caller then runs the job inline).
==================
*/
static qboolean LT_Create( void ) {
	ltWorker_t *w = (ltWorker_t *)calloc( 1, sizeof( *w ) );

	if ( !w ) {
		return qfalse;
	}
	w->startSem = SDL_CreateSemaphore( 0 );
	w->doneSem = SDL_CreateSemaphore( 0 );
	w->parkSem = SDL_CreateSemaphore( 0 );
	if ( w->startSem && w->doneSem && w->parkSem ) {
		w->thread = SDL_CreateThread( LT_WorkerMain, "loadworker", w );
	}
	if ( !w->thread ) {
		if ( w->startSem ) {
			SDL_DestroySemaphore( w->startSem );
		}
		if ( w->doneSem ) {
			SDL_DestroySemaphore( w->doneSem );
		}
		if ( w->parkSem ) {
			SDL_DestroySemaphore( w->parkSem );
		}
		free( w );
		return qfalse;
	}
	lt_worker = w;
	lt_workerId = SDL_GetThreadID( w->thread );
	return qtrue;
}

/*
==================
Com_RunOffloaded
==================
*/
void Com_RunOffloaded( void ( *fn )( void * ), void *arg ) {
	ltWorker_t *w;

	if ( !LT_Wanted() ) {
		fn( arg );
		return;
	}
	if ( !lt_worker && !LT_Create() ) {
		fn( arg );
		return;
	}

	w = lt_worker;
	w->fn = fn;
	w->arg = arg;
	w->failed = 0;
	lt_inJob = qtrue;
	SDL_SemPost( w->startSem );

	// Wait for the job. This loop allocates nothing from Z/Hunk and calls no file system, cvar or print
	// code: CL_LoadingReplayTick reads a few cvar values and client state, then raw GL + SDL swap/pump.
	for ( ;; ) {
		int r = SDL_SemWaitTimeout( w->doneSem, LT_WAIT_MS );

		if ( r == 0 ) {
			break;
		}
		if ( r < 0 ) {
			SDL_Delay( LT_WAIT_MS );        // semaphore error: poll instead of spinning
		}
		CL_LoadingReplayTick();
	}
	lt_inJob = qfalse;

	if ( w->failed ) {
		char msg[MAXPRINTMSG];
		int code = w->errCode;

		Q_strncpyz( msg, w->errMsg, sizeof( msg ) );
		// the failed worker is parked forever; leak its small control block (it still references it)
		SDL_DetachThread( w->thread );
		lt_worker = NULL;
		lt_workerId = 0;
		Com_Error( code, "%s", msg );
	}
}

/*
==================
Com_OffloadShutdown

Idle worker: ask it to quit and join it. A parked (failed) worker was already detached.
==================
*/
void Com_OffloadShutdown( void ) {
	ltWorker_t *w = lt_worker;

	if ( !w || lt_inJob ) {
		return;
	}
	lt_worker = NULL;
	lt_workerId = 0;
	w->quit = 1;
	SDL_SemPost( w->startSem );
	SDL_WaitThread( w->thread, NULL );
	SDL_DestroySemaphore( w->startSem );
	SDL_DestroySemaphore( w->doneSem );
	SDL_DestroySemaphore( w->parkSem );
	free( w );
}
