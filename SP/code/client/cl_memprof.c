/*
===========================================================================
Memory profiler, engine half (RAM). Opt-in: cvar com_memProfile 1.

A census, not allocation hooks: process RSS is broken down from /proc/self/status (cheap, per sample) and
/proc/self/smaps (full parse, only at a dump), plus hunk stats, mallinfo2 and OpenAL buffer sizes.
The renderer half (rend2/tr_memprof.c) writes the VRAM files; this one writes, under profiling/ in the game dir:
  mem_ram.csv           section,item,bytes,note
  mem_ram_timeline.csv  epoch_ms,t_ms,event,rss,anon,file,shmem,hunk_used,hunk_temp_hw,malloc_inuse,malloc_free,sound_bytes
Both timelines are stamped with CLOCK_REALTIME ms so they can be merged. Cost when off: one branch per frame
and per load mark (the callers test com_memProfile). Linux only; other platforms get empty stubs.
Plan: docs/memory-profiler-plan.md.
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#ifndef __linux__

void CL_MemProfEvent( const char *stage ) { }
void CL_MemProfFrame( void ) { }
void CL_MemProfDump( const char *why ) { }
void CL_MemProfInit( void ) { }

#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/mman.h>

#ifdef USE_OPENAL
void S_AL_MemStats( int *count, long long *bytes );     // snd_openal.c
#endif

// mallinfo2 needs glibc 2.33; older glibc (or another libc) gets zeros
#if defined( __GLIBC__ ) && ( __GLIBC__ > 2 || ( __GLIBC__ == 2 && __GLIBC_MINOR__ >= 33 ) )
#define MP_HAVE_MALLINFO2 1
#endif

#define MP_MAX_ROWS     100000      // timeline cap (about 8 MB), further rows are dropped
#define MP_NAMELEN      32
#define MP_MIN_INTERVAL 20          // ms, lower bound for com_memProfileInterval

typedef struct {
	long long epochMs;
	int tMs;
	char event[MP_NAMELEN];
	long long rss, anon, file, shmem;
	long long hunkUsed, hunkTempHw;
	long long mallocInuse, mallocFree;
	long long soundBytes;
} mpRow_t;

typedef struct {
	long long rss, anon, file, shmem;           // /proc/self/status
	long long hunkUsed, hunkTempHw;
	long long mallocInuse, mallocFree, mallocMmapped;
	long long soundBytes;
	int soundCount;
} mpSample_t;

// hunk deltas between two load marks (plan "Hunk attribution"); the hunk is cleared per map load
typedef struct {
	const char *name;
	const char *from, *to;
	long long begin;            // hunk bytes in use at the "from" mark, -1 = not seen
	long long delta;            // -1 = not measured yet
} mpPhase_t;

static mpPhase_t mpPhases[] = {
	{ "collision",          "sv:fs_restart",  "sv:cm_loadmap", -1, -1 },
	{ "game_aas",           "sv:cm_loadmap",  "sv:gameprogs",  -1, -1 },
	{ "renderer_bsp",       "cg:world_begin", "cg:world_end",  -1, -1 },
	{ "cgame_registration", "cg:init_begin",  "cg:init_end",   -1, -1 },
};
#define MP_NUMPHASES ( (int)( sizeof( mpPhases ) / sizeof( mpPhases[0] ) ) )

static mpRow_t *mpRows;
static int mpNumRows, mpCapRows;
static int mpLastMs;            // Sys_Milliseconds of the last periodic row
static int mpPrevBench;         // cl_benchMeasuring at the last look, to emit rows on its edges

/*
=================
text buffer (growable, for the csv writers)
=================
*/
typedef struct {
	char *p;
	int len, cap;
} mpText_t;

static void MP_Printf( mpText_t *t, const char *fmt, ... ) __attribute__ ( ( format( printf, 2, 3 ) ) );
static void MP_Printf( mpText_t *t, const char *fmt, ... ) {
	va_list ap;
	char line[1024];
	int n;

	va_start( ap, fmt );
	n = vsnprintf( line, sizeof( line ), fmt, ap );
	va_end( ap );
	if ( n < 0 ) {
		return;
	}
	if ( n >= (int)sizeof( line ) ) {
		n = sizeof( line ) - 1;
	}
	if ( t->len + n + 1 > t->cap ) {
		int cap = t->cap ? t->cap * 2 : 4096;
		char *np;
		while ( cap < t->len + n + 1 ) {
			cap *= 2;
		}
		np = realloc( t->p, cap );
		if ( !np ) {
			return;
		}
		t->p = np;
		t->cap = cap;
	}
	memcpy( t->p + t->len, line, n );
	t->len += n;
	t->p[t->len] = 0;
}

static void MP_Flush( mpText_t *t, const char *qpath ) {
	if ( t->p ) {
		FS_WriteFile( qpath, t->p, t->len );
		free( t->p );
	}
	t->p = NULL;
	t->len = t->cap = 0;
}

static long long MP_EpochMs( void ) {
	struct timespec ts;

	clock_gettime( CLOCK_REALTIME, &ts );
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
=================
MP_ReadStatus

Rss/anon/file/shmem in bytes from /proc/self/status (RssAnon, RssFile, RssShmem, VmRSS; kB lines).
=================
*/
static void MP_ReadStatus( mpSample_t *s ) {
	FILE *f;
	char line[256];
	long long v;

	s->rss = s->anon = s->file = s->shmem = 0;
	f = fopen( "/proc/self/status", "r" );
	if ( !f ) {
		return;
	}
	while ( fgets( line, sizeof( line ), f ) ) {
		if ( sscanf( line, "VmRSS: %lld", &v ) == 1 ) {
			s->rss = v * 1024;
		} else if ( sscanf( line, "RssAnon: %lld", &v ) == 1 ) {
			s->anon = v * 1024;
		} else if ( sscanf( line, "RssFile: %lld", &v ) == 1 ) {
			s->file = v * 1024;
		} else if ( sscanf( line, "RssShmem: %lld", &v ) == 1 ) {
			s->shmem = v * 1024;
		}
	}
	fclose( f );
}

/*
=================
MP_Sample

Everything cheap enough for the periodic timeline.
=================
*/
static void MP_Sample( mpSample_t *s ) {
	hunkMemStats_t hs;
	long long t;

	memset( s, 0, sizeof( *s ) );
	MP_ReadStatus( s );

	Hunk_GetMemStats( &hs );
	s->hunkUsed = (long long)hs.lowPermanent + hs.highPermanent;
	t = 0;
	if ( hs.lowTempHighwater > hs.lowPermanent ) {
		t += hs.lowTempHighwater - hs.lowPermanent;
	}
	if ( hs.highTempHighwater > hs.highPermanent ) {
		t += hs.highTempHighwater - hs.highPermanent;
	}
	s->hunkTempHw = t;

#ifdef MP_HAVE_MALLINFO2
	{
		struct mallinfo2 mi = mallinfo2();
		long long inuse = (long long)mi.uordblks + (long long)mi.hblkhd;

		// the hunk is one malloc() far above the mmap threshold, so it sits in hblkhd: take it out
		if ( hs.base ) {
			inuse -= (long long)hs.total + 31;
		}
		s->mallocInuse = inuse > 0 ? inuse : 0;
		s->mallocFree = mi.fordblks;
		s->mallocMmapped = mi.hblkhd;
	}
#endif

#ifdef USE_OPENAL
	S_AL_MemStats( &s->soundCount, &s->soundBytes );
#endif
}

static void MP_AddRow( const char *event, const mpSample_t *s ) {
	mpRow_t *r;

	if ( mpNumRows >= MP_MAX_ROWS ) {
		return;
	}
	if ( mpNumRows >= mpCapRows ) {
		int cap = mpCapRows ? mpCapRows * 2 : 1024;
		mpRow_t *np;

		if ( cap > MP_MAX_ROWS ) {
			cap = MP_MAX_ROWS;
		}
		np = realloc( mpRows, cap * sizeof( mpRow_t ) );
		if ( !np ) {
			return;
		}
		mpRows = np;
		mpCapRows = cap;
	}
	r = &mpRows[mpNumRows++];
	r->epochMs = MP_EpochMs();
	r->tMs = Sys_Milliseconds();
	Q_strncpyz( r->event, event, sizeof( r->event ) );
	r->rss = s->rss;
	r->anon = s->anon;
	r->file = s->file;
	r->shmem = s->shmem;
	r->hunkUsed = s->hunkUsed;
	r->hunkTempHw = s->hunkTempHw;
	r->mallocInuse = s->mallocInuse;
	r->mallocFree = s->mallocFree;
	r->soundBytes = s->soundBytes;
}

static void MP_SampleRow( const char *event ) {
	mpSample_t s;

	MP_Sample( &s );
	MP_AddRow( event, &s );
	mpLastMs = Sys_Milliseconds();
}

/*
=================
CL_MemProfEvent

Called from Com_LoadProfileMark (main thread, only when com_memProfile is set): one timeline row per load stage
and the hunk phase deltas.
=================
*/
void CL_MemProfEvent( const char *stage ) {
	hunkMemStats_t hs;
	long long used;
	int i;

	// cl:first_frame_begin is marked every frame while active: keep one row per interval
	if ( !strcmp( stage, "cl:first_frame_begin" ) ) {
		static int lastMs;
		int now = Sys_Milliseconds();

		if ( now - lastMs < ( com_memProfileInterval ? com_memProfileInterval->integer : 500 ) ) {
			return;
		}
		lastMs = now;
	}

	MP_SampleRow( stage );

	Hunk_GetMemStats( &hs );
	used = (long long)hs.lowPermanent + hs.highPermanent;
	for ( i = 0; i < MP_NUMPHASES; i++ ) {
		mpPhase_t *ph = &mpPhases[i];

		if ( !strcmp( stage, ph->from ) ) {
			ph->begin = used;
		} else if ( !strcmp( stage, ph->to ) && ph->begin >= 0 ) {
			ph->delta = used >= ph->begin ? used - ph->begin : -1;
			ph->begin = -1;
		}
	}
}

/*
=================
CL_MemProfFrame

Per client frame: a rate-limited periodic row, plus a row on each edge of cl_benchMeasuring.
=================
*/
void CL_MemProfFrame( void ) {
	int now = Sys_Milliseconds();
	int bench = Cvar_VariableIntegerValue( "cl_benchMeasuring" );
	int interval;

	if ( bench != mpPrevBench ) {
		mpPrevBench = bench;
		MP_SampleRow( bench ? "bench_start" : "bench_end" );
		return;
	}
	interval = com_memProfileInterval ? com_memProfileInterval->integer : 500;
	if ( interval < MP_MIN_INTERVAL ) {
		interval = MP_MIN_INTERVAL;
	}
	if ( now - mpLastMs >= interval ) {
		MP_SampleRow( "periodic" );
	}
}

/*
=================
smaps classification (dump time only)
=================
*/
typedef struct {
	char name[96];
	long long rssKb, sizeKb;
} mpFile_t;

#define MP_MAXFILES 512

typedef struct {
	// anonymous Rss by bucket, bytes
	long long hunk, heap, soBss, anonOther, stack;
	mpFile_t *files, *devs;
	int numFiles, numDevs;
} mpMaps_t;

static void MP_AddFile( mpFile_t *tab, int *num, const char *name, long long rssKb, long long sizeKb ) {
	int i;

	for ( i = 0; i < *num; i++ ) {
		if ( !strcmp( tab[i].name, name ) ) {
			tab[i].rssKb += rssKb;
			tab[i].sizeKb += sizeKb;
			return;
		}
	}
	if ( *num < MP_MAXFILES ) {
		Q_strncpyz( tab[*num].name, name, sizeof( tab[*num].name ) );
		tab[*num].rssKb = rssKb;
		tab[*num].sizeKb = sizeKb;
		( *num )++;
	}
}

static int QDECL MP_FileCmp( const void *a, const void *b ) {
	long long d = ( (const mpFile_t *)b )->rssKb - ( (const mpFile_t *)a )->rssKb;
	return d > 0 ? 1 : ( d < 0 ? -1 : 0 );
}

// bytes of [base, base+total) that are resident (mincore), 0 on failure
static long long MP_HunkResident( const hunkMemStats_t *hs ) {
	long pg = sysconf( _SC_PAGESIZE );
	uintptr_t start, end;
	size_t pages, i;
	unsigned char *vec;
	long long res = 0;

	if ( !hs->base || hs->total <= 0 || pg <= 0 ) {
		return 0;
	}
	start = (uintptr_t)hs->base & ~( (uintptr_t)pg - 1 );
	end = ( (uintptr_t)hs->base + hs->total + pg - 1 ) & ~( (uintptr_t)pg - 1 );
	pages = ( end - start ) / pg;
	vec = malloc( pages );
	if ( !vec ) {
		return 0;
	}
	if ( mincore( (void *)start, end - start, vec ) == 0 ) {
		for ( i = 0; i < pages; i++ ) {
			if ( vec[i] & 1 ) {
				res += pg;
			}
		}
	}
	free( vec );
	return res;
}

static void MP_ParseSmaps( mpMaps_t *m, const hunkMemStats_t *hs ) {
	FILE *f = fopen( "/proc/self/smaps", "r" );
	char line[1024], path[512];
	unsigned long long vs = 0, ve = 0, prevEnd = 0;
	long long rssKb = 0, v;
	int have = 0, prevIsSo = 0, isSo = 0;
	long long hunkRes = MP_HunkResident( hs );
	unsigned long long hb = (unsigned long long)(uintptr_t)hs->base;

	if ( !f ) {
		return;
	}
	path[0] = 0;

	for ( ;; ) {
		int atEnd = !fgets( line, sizeof( line ), f );
		unsigned long long s, e, off;
		char perms[16];
		int n = 0;

		if ( !atEnd ) {
			if ( sscanf( line, "%llx-%llx %15s %llx %*s %*s %n", &s, &e, perms, &off, &n ) < 4 ) {
				if ( sscanf( line, "Rss: %lld", &v ) == 1 ) {
					rssKb = v;
				}
				continue;
			}
		}

		// a header (or EOF) closes the previous mapping
		if ( have ) {
			long long bytes = rssKb * 1024;
			long long sizeKb = (long long)( ve - vs ) / 1024;
			const char *base;

			if ( path[0] == '/' || !strncmp( path, "memfd:", 6 ) ) {
				base = strrchr( path, '/' );
				base = base ? base + 1 : path;
				if ( !strncmp( path, "/dev/nvidia", 11 ) || !strncmp( path, "memfd:", 6 ) ) {
					MP_AddFile( m->devs, &m->numDevs, path, rssKb, sizeKb );
				} else {
					MP_AddFile( m->files, &m->numFiles, base, rssKb, sizeKb );
				}
				isSo = strstr( path, ".so" ) != NULL;
			} else {
				isSo = 0;
				if ( !strcmp( path, "[heap]" ) ) {
					m->heap += bytes;
				} else if ( !strcmp( path, "[stack]" ) ) {
					m->stack += bytes;
				} else if ( path[0] == '[' && strncmp( path, "[anon", 5 ) ) {
					// [vdso], [vvar], [vsyscall]: not interesting
				} else {
					long long b = bytes;

					if ( hb >= vs && hb < ve ) {
						// the hunk malloc lives in this mapping (possibly merged with neighbours): split off its resident part
						long long h = hunkRes < b ? hunkRes : b;
						m->hunk += h;
						b -= h;
					}
					if ( path[0] == 0 && vs == prevEnd && prevIsSo ) {
						m->soBss += b;
					} else {
						m->anonOther += b;
					}
				}
			}
			prevEnd = ve;
			prevIsSo = isSo;
		}
		if ( atEnd ) {
			break;
		}

		vs = s;
		ve = e;
		rssKb = 0;
		have = 1;
		Q_strncpyz( path, line + n, sizeof( path ) );
		{
			int l = strlen( path );
			while ( l > 0 && ( path[l - 1] == '\n' || path[l - 1] == ' ' ) ) {
				path[--l] = 0;
			}
		}
	}
	fclose( f );
}

/*
=================
CL_MemProfDump

why: label of the final timeline row (e.g. "bench_end"). Overwrites the two files each time.
=================
*/
void CL_MemProfDump( const char *why ) {
	mpSample_t s;
	hunkMemStats_t hs;
	mpMaps_t m;
	mpText_t t = { NULL, 0, 0 };
	long long anonSum, overhead, hunkResident;
	int i, shown;

	MP_Sample( &s );
	MP_AddRow( why && why[0] ? why : "dump", &s );
	mpLastMs = Sys_Milliseconds();
	mpPrevBench = Cvar_VariableIntegerValue( "cl_benchMeasuring" );    // the row above is the edge row

	Hunk_GetMemStats( &hs );
	hunkResident = MP_HunkResident( &hs );

	memset( &m, 0, sizeof( m ) );
	m.files = malloc( MP_MAXFILES * sizeof( mpFile_t ) );
	m.devs = malloc( MP_MAXFILES * sizeof( mpFile_t ) );
	if ( m.files && m.devs ) {
		MP_ParseSmaps( &m, &hs );
	}

	anonSum = m.hunk + m.heap + m.soBss + m.anonOther + m.stack;
	overhead = (long long)mpCapRows * sizeof( mpRow_t );

	MP_Printf( &t, "section,item,bytes,note\n" );
	MP_Printf( &t, "rollup,rss,%lld,\n", s.rss );
	MP_Printf( &t, "rollup,anon,%lld,\n", s.anon );
	MP_Printf( &t, "rollup,file,%lld,\n", s.file );
	MP_Printf( &t, "rollup,shmem,%lld,\n", s.shmem );

	MP_Printf( &t, "smaps_anon,hunk,%lld,resident pages of the hunk malloc (mincore)\n", m.hunk );
	MP_Printf( &t, "smaps_anon,[heap],%lld,\n", m.heap );
	MP_Printf( &t, "smaps_anon,.so bss,%lld,anonymous mapping right after a shared object\n", m.soBss );
	MP_Printf( &t, "smaps_anon,anon_other,%lld,mmap arenas and large mallocs\n", m.anonOther );
	MP_Printf( &t, "smaps_anon,[stack],%lld,\n", m.stack );

	if ( m.files ) {
		qsort( m.files, m.numFiles, sizeof( mpFile_t ), MP_FileCmp );
		shown = m.numFiles < 15 ? m.numFiles : 15;
		for ( i = 0; i < shown; i++ ) {
			MP_Printf( &t, "smaps_file,%s,%lld,\n", m.files[i].name, m.files[i].rssKb * 1024 );
		}
	}
	if ( m.devs ) {
		qsort( m.devs, m.numDevs, sizeof( mpFile_t ), MP_FileCmp );
		for ( i = 0; i < m.numDevs; i++ ) {
			MP_Printf( &t, "smaps_dev,%s,%lld,size_kb=%lld\n", m.devs[i].name, m.devs[i].rssKb * 1024, m.devs[i].sizeKb );
		}
	}

	MP_Printf( &t, "hunk,reserved,%d,com_hunkMegs\n", hs.total );
	MP_Printf( &t, "hunk,perm_low,%d,\n", hs.lowPermanent );
	MP_Printf( &t, "hunk,perm_high,%d,\n", hs.highPermanent );
	MP_Printf( &t, "hunk,temp_highwater_low,%d,absolute mark\n", hs.lowTempHighwater );
	MP_Printf( &t, "hunk,temp_highwater_high,%d,absolute mark\n", hs.highTempHighwater );
	MP_Printf( &t, "hunk,resident,%lld,touched pages (mincore)\n", hunkResident );
	for ( i = 0; i < MP_NUMPHASES; i++ ) {
		if ( mpPhases[i].delta >= 0 ) {
			MP_Printf( &t, "hunk_phase,%s,%lld,%s -> %s (last load)\n", mpPhases[i].name, mpPhases[i].delta,
					   mpPhases[i].from, mpPhases[i].to );
		}
	}

#ifdef MP_HAVE_MALLINFO2
	MP_Printf( &t, "malloc,in_use,%lld,nested; uordblks + hblkhd minus the hunk\n", s.mallocInuse );
	MP_Printf( &t, "malloc,free_retained,%lld,nested; fordblks\n", s.mallocFree );
	MP_Printf( &t, "malloc,mmapped,%lld,nested; hblkhd incl. the hunk\n", s.mallocMmapped );
#else
	MP_Printf( &t, "malloc,in_use,0,nested; mallinfo2 unavailable\n" );
#endif
	MP_Printf( &t, "sound,al_buffers,%d,nested\n", s.soundCount );
	MP_Printf( &t, "sound,al_bytes,%lld,nested\n", s.soundBytes );

	MP_Printf( &t, "derived,unattributed_anon,%lld,anon from status minus smaps_anon sum\n", s.anon - anonSum );
	MP_Printf( &t, "derived,profiler_overhead,%lld,timeline array (%d rows)\n", overhead, mpNumRows );
	MP_Flush( &t, "profiling/mem_ram.csv" );

	MP_Printf( &t, "epoch_ms,t_ms,event,rss,anon,file,shmem,hunk_used,hunk_temp_hw,malloc_inuse,malloc_free,sound_bytes\n" );
	for ( i = 0; i < mpNumRows; i++ ) {
		const mpRow_t *r = &mpRows[i];

		MP_Printf( &t, "%lld,%d,%s,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", r->epochMs, r->tMs, r->event,
				   r->rss, r->anon, r->file, r->shmem, r->hunkUsed, r->hunkTempHw,
				   r->mallocInuse, r->mallocFree, r->soundBytes );
	}
	MP_Flush( &t, "profiling/mem_ram_timeline.csv" );

	free( m.files );
	free( m.devs );
	Com_Printf( "memprof: profiling/mem_ram.csv + mem_ram_timeline.csv written (%d rows)\n", mpNumRows );
}

/*
=================
memprof [why]

RAM dump now, then the renderer's VRAM census.
=================
*/
static void CL_MemProf_f( void ) {
	const char *why = Cmd_Argc() > 1 ? Cmd_Argv( 1 ) : "memprof";

	CL_MemProfDump( why );
	Cbuf_ExecuteText( EXEC_NOW, va( "vramprof dump %s\n", why ) );
}

void CL_MemProfInit( void ) {
	Cmd_AddCommand( "memprof", CL_MemProf_f );
}

#endif  // __linux__
