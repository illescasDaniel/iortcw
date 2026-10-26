/*
===========================================================================

Return to Castle Wolfenstein single player GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company. 

This file is part of the Return to Castle Wolfenstein single player GPL Source Code (RTCW SP Source Code).  

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

In addition, the RTCW SP Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the RTCW SP Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

// common.c -- misc functions used in client and server

#include "q_shared.h"
#include "qcommon.h"
#include <setjmp.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/stat.h> // umask
#else
#include <winsock.h>
#endif

int demo_protocols[] =
{ 59, 58, 57, 0 };

#define MAXPRINTMSG 4096

#define MAX_NUM_ARGVS   50

#define MIN_DEDICATED_COMHUNKMEGS 1
#define MIN_COMHUNKMEGS		128
#define DEF_COMHUNKMEGS 	256
#define DEF_COMZONEMEGS		32
#define DEF_COMHUNKMEGS_S	XSTRING(DEF_COMHUNKMEGS)
#define DEF_COMZONEMEGS_S	XSTRING(DEF_COMZONEMEGS)

int com_argc;
char    *com_argv[MAX_NUM_ARGVS + 1];


jmp_buf abortframe;     // an ERR_DROP occured, exit the entire frame


FILE *debuglogfile;
static fileHandle_t pipefile;
static fileHandle_t logfile;
fileHandle_t com_journalFile;               // events are written here
fileHandle_t com_journalDataFile;           // config files are written here

cvar_t  *com_speeds;
cvar_t  *com_developer;
cvar_t  *com_dedicated;
cvar_t  *com_timescale;
cvar_t  *com_fixedtime;
cvar_t  *com_journal;
cvar_t  *com_maxfps;
cvar_t	*com_altivec;
cvar_t  *com_timedemo;
cvar_t  *com_sv_running;
cvar_t  *com_cl_running;
cvar_t  *com_logfile;       // 1 = buffer log, 2 = flush after each print
cvar_t	*com_pipefile;
cvar_t  *com_showtrace;
cvar_t  *com_version;
cvar_t  *com_blood;
cvar_t  *com_buildScript;   // for automated data building scripts
#ifdef CINEMATICS_INTRO
cvar_t  *com_introPlayed;
cvar_t  *cod_skipIntro;
#endif
cvar_t  *cl_paused;
cvar_t  *sv_paused;
cvar_t  *cl_packetdelay;
cvar_t  *sv_packetdelay;
cvar_t  *com_cameraMode;
cvar_t  *com_recommendedSet;
cvar_t	*com_ansiColor;
cvar_t	*com_unfocused;
cvar_t	*com_maxfpsUnfocused;
cvar_t	*com_minimized;
cvar_t	*com_maxfpsMinimized;
cvar_t	*com_abnormalExit;
cvar_t	*com_standalone;
cvar_t	*com_gamename;
cvar_t	*com_protocol;
#ifdef LEGACY_PROTOCOL
cvar_t	*com_legacyprotocol;
#endif
cvar_t	*com_basegame;
cvar_t  *com_homepath;
cvar_t	*com_busyWait;
cvar_t	*com_maxfpsMenu;
cvar_t	*com_maxfpsPrecise;
cvar_t	*com_maxfpsSpinUs;
cvar_t	*com_maxfpsMode;
cvar_t	*com_maxfpsGpuSync;
static cvar_t	*com_maxfpsGpuQueue;   // com_maxfpsGpuSync 2: frames the GPU may be behind (0..3)
static cvar_t	*com_vsync;
static cvar_t	*com_displayRefresh;
static cvar_t	*r_frameWarp;       // owned by the renderer; registered here too so the pacer can read it
static cvar_t	*com_latchSync;     // r_frameWarpLatchSync: late-latch wait for the present deadline (0 never, 1 when paced, 2 always)
static cvar_t	*com_frameWarpHz;   // r_frameWarpHz: refresh rate override (0 = measured/auto)
int com_frameFracUs;
long long com_frameClockUs;   // smooth frame clock (paced schedule, or real time uncapped)
long long com_lastPresentUs;
static cvar_t	*com_loadProfile;
#ifndef DEDICATED
cvar_t  *con_autochat;
#endif

#if idx64
	int (*Q_VMftol)(void);
#elif id386
	long (QDECL *Q_ftol)(float f);
	int (QDECL *Q_VMftol)(void);
	void (QDECL *Q_SnapVector)(vec3_t vec);
#endif

// Rafael Notebook
cvar_t  *cl_notebook;

cvar_t  *com_hunkused;      // Ridah

// com_speeds times
int time_game;
int time_frontend;          // renderer frontend time
int time_backend;           // renderer backend time

int com_frameTime;
int com_frameNumber;

qboolean	com_errorEntered = qfalse;
qboolean	com_fullyInitialized = qfalse;
qboolean	com_gameRestarting = qfalse;
qboolean	com_gameClientRestarting = qfalse;

char com_errorMessage[MAXPRINTMSG];

void Com_WriteConfig_f( void );
void CIN_CloseAllVideos( void );

//============================================================================

static char *rd_buffer;
static int rd_buffersize;
static void ( *rd_flush )( char *buffer );

void Com_BeginRedirect( char *buffer, int buffersize, void ( *flush )( char *) ) {
	if ( !buffer || !buffersize || !flush ) {
		return;
	}
	rd_buffer = buffer;
	rd_buffersize = buffersize;
	rd_flush = flush;

	*rd_buffer = 0;
}

void Com_EndRedirect( void ) {
	if ( rd_flush ) {
		rd_flush( rd_buffer );
	}

	rd_buffer = NULL;
	rd_buffersize = 0;
	rd_flush = NULL;
}

/*
=============
Com_Printf

Both client and server can use this, and it will output
to the apropriate place.

A raw string should NEVER be passed as fmt, because of "%f" type crashers.
=============
*/
void QDECL Com_Printf( const char *fmt, ... ) {
	va_list argptr;
	char msg[MAXPRINTMSG];
	static qboolean opening_qconsole = qfalse;

	va_start( argptr,fmt );
	Q_vsnprintf( msg, sizeof (msg), fmt, argptr );
	va_end( argptr );

	if ( rd_buffer ) {
		if ( ( strlen( msg ) + strlen( rd_buffer ) ) > ( rd_buffersize - 1 ) ) {
			rd_flush( rd_buffer );
			*rd_buffer = 0;
		}
		Q_strcat( rd_buffer, rd_buffersize, msg );
		// show_bug.cgi?id=51
		// only flush the rcon buffer when it's necessary, avoid fragmenting
		//rd_flush(rd_buffer);
		//*rd_buffer = 0;
		return;
	}

#ifndef DEDICATED
	CL_ConsolePrint( msg );
#endif

	// echo to dedicated console and early console
	Sys_Print( msg );

	// logfile
	if ( com_logfile && com_logfile->integer ) {
		// TTimo: only open the qconsole.log if the filesystem is in an initialized state
		//   also, avoid recursing in the qconsole.log opening (i.e. if fs_debug is on)
		if ( !logfile && FS_Initialized() && !opening_qconsole ) {
			struct tm *newtime;
			time_t aclock;

			opening_qconsole = qtrue;

			time( &aclock );
			newtime = localtime( &aclock );

			logfile = FS_FOpenFileWrite( "rtcwconsole.log" );    //----(SA)	changed name for Wolf
			if(logfile)
			{
				Com_Printf( "logfile opened on %s\n", asctime( newtime ) );
			
				if ( com_logfile->integer > 1 )
				{
					// force it to not buffer so we get valid
					// data even if we are crashing
					FS_ForceFlush(logfile);
				}
			}
			else
			{
				Com_Printf("Opening rtcwconsole.log failed!\n");
				Cvar_SetValue("logfile", 0);
			}

			opening_qconsole = qfalse;
		}
		if ( logfile && FS_Initialized() ) {
			FS_Write( msg, strlen( msg ), logfile );
		}
	}
}


/*
================
Com_DPrintf

A Com_Printf that only shows up if the "developer" cvar is set
================
*/
void QDECL Com_DPrintf( const char *fmt, ... ) {
	va_list argptr;
	char msg[MAXPRINTMSG];

	if ( !com_developer || !com_developer->integer ) {
		return;         // don't confuse non-developers with techie stuff...
	}

	va_start( argptr,fmt );
	Q_vsnprintf( msg, sizeof ( msg ), fmt, argptr );
	va_end( argptr );

	Com_Printf( "%s", msg );
}

/*
=============
Com_Error

Both client and server can use this, and it will
do the appropriate thing.
=============
*/
void QDECL Com_Error( int code, const char *fmt, ... ) {
	va_list argptr;
	static int lastErrorTime;
	static int errorCount;
	int currentTime;
	qboolean restartClient;

	if ( Com_OffloadIsWorker() ) {
		// raised inside an offloaded job: never longjmp across threads, the main thread re-raises it
		char workerMsg[MAXPRINTMSG];

		va_start( argptr, fmt );
		Q_vsnprintf( workerMsg, sizeof( workerMsg ), fmt, argptr );
		va_end( argptr );
		Com_OffloadWorkerError( code, workerMsg );  // does not return
	}

	if(com_errorEntered)
		Sys_Error("recursive error after: %s", com_errorMessage);

	com_errorEntered = qtrue;

	Cvar_Set("com_errorCode", va("%i", code));

	// when we are running automated scripts, make sure we
	// know if anything failed
	if ( com_buildScript && com_buildScript->integer ) {

		// ERR_ENDGAME is not really an error, don't die if building a script
		if ( code != ERR_ENDGAME ) {
			code = ERR_FATAL;
		}
	}

	// if we are getting a solid stream of ERR_DROP, do an ERR_FATAL
	currentTime = Sys_Milliseconds();
	if ( currentTime - lastErrorTime < 100 ) {
		if ( ++errorCount > 3 ) {
			code = ERR_FATAL;
		}
	} else {
		errorCount = 0;
	}
	lastErrorTime = currentTime;

	va_start( argptr,fmt );
	Q_vsnprintf (com_errorMessage, sizeof(com_errorMessage),fmt,argptr);
	va_end( argptr );

	if ( code != ERR_DISCONNECT && code != ERR_NEED_CD && code != ERR_ENDGAME ) {
		Cvar_Set( "com_errorMessage", com_errorMessage );
	}

	restartClient = com_gameClientRestarting && !( com_cl_running && com_cl_running->integer );

	com_gameRestarting = qfalse;
	com_gameClientRestarting = qfalse;

	if (code == ERR_DISCONNECT || code == ERR_SERVERDISCONNECT) {
		VM_Forced_Unload_Start();
		SV_Shutdown( "Server disconnected" );
		if ( restartClient ) {
			CL_Init();
		}
		CL_Disconnect( qtrue );
		CL_FlushMemory();
		VM_Forced_Unload_Done();
		// make sure we can get at our local stuff
		FS_PureServerSetLoadedPaks("", "");
		com_errorEntered = qfalse;
		longjmp( abortframe, -1 );
	} else if ( code == ERR_ENDGAME ) {  //----(SA)	added
		VM_Forced_Unload_Start();
		SV_Shutdown( "endgame" );
		if ( restartClient ) {
			CL_Init();
		}
		if ( com_cl_running && com_cl_running->integer ) {
			CL_Disconnect( qtrue );
			CL_FlushMemory();
			VM_Forced_Unload_Done();
#ifndef DEDICATED
			CL_EndgameMenu();
#endif
		}

		FS_PureServerSetLoadedPaks("", "");

		com_errorEntered = qfalse;
		longjmp( abortframe, -1 );
	} else if (code == ERR_DROP) {
		Com_Printf( "********************\nERROR: %s\n********************\n", com_errorMessage );
		VM_Forced_Unload_Start();
		SV_Shutdown (va("Server crashed: %s",  com_errorMessage));
		if ( restartClient ) {
			CL_Init();
		}
		CL_Disconnect( qtrue );
		CL_FlushMemory();
		VM_Forced_Unload_Done();
		FS_PureServerSetLoadedPaks("", "");
		com_errorEntered = qfalse;
		longjmp( abortframe, -1 );
	} else if ( code == ERR_NEED_CD ) {
		VM_Forced_Unload_Start();
		SV_Shutdown( "Server didn't have CD" );
		if ( restartClient ) {
			CL_Init();
		}
		if ( com_cl_running && com_cl_running->integer ) {
			CL_Disconnect( qtrue );
			CL_FlushMemory();
			VM_Forced_Unload_Done();
			CL_CDDialog();
		} else {
			Com_Printf( "Server didn't have CD\n" );
			VM_Forced_Unload_Done();
		}

		FS_PureServerSetLoadedPaks("", "");

		com_errorEntered = qfalse;
		longjmp( abortframe, -1 );
	} else {
		VM_Forced_Unload_Start();
		CL_Shutdown(va("Client fatal crashed: %s", com_errorMessage), qtrue, qtrue);
		SV_Shutdown(va("Server fatal crashed: %s", com_errorMessage));
		VM_Forced_Unload_Done();
	}

	Com_Shutdown();

	Sys_Error( "%s", com_errorMessage );
}


/*
=============
Com_Quit_f

Both client and server can use this, and it will
do the apropriate things.
=============
*/
void Com_Quit_f( void ) {
	// don't try to shutdown if we are in a recursive error
	char *p = Cmd_Args( );
	if ( !com_errorEntered ) {
		// Some VMs might execute "quit" command directly,
		// which would trigger an unload of active VM error.
		// Sys_Quit will kill this process anyways, so
		// a corrupt call stack makes no difference
		VM_Forced_Unload_Start();
		SV_Shutdown(p[0] ? p : "Server quit");
		CL_Shutdown(p[0] ? p : "Client quit", qtrue, qtrue);
		VM_Forced_Unload_Done();
		Com_Shutdown();
		FS_Shutdown( qtrue );
	}
	Sys_Quit();
}



/*
============================================================================

COMMAND LINE FUNCTIONS

+ characters seperate the commandLine string into multiple console
command lines.

All of these are valid:

quake3 +set test blah +map test
quake3 set test blah+map test
quake3 set test blah + map test

============================================================================
*/

#define MAX_CONSOLE_LINES   64
int com_numConsoleLines;
char    *com_consoleLines[MAX_CONSOLE_LINES];

/*
==================
Com_ParseCommandLine

Break it up into multiple console lines
==================
*/
void Com_ParseCommandLine( char *commandLine ) {
	com_consoleLines[0] = commandLine;
	com_numConsoleLines = 1;

	while ( *commandLine ) {
		// look for a + separating character
		// if commandLine came from a file, we might have real line seperators
		if ( *commandLine == '+' || *commandLine == '\n' ) {
			if ( com_numConsoleLines == MAX_CONSOLE_LINES ) {
				return;
			}
			com_consoleLines[com_numConsoleLines] = commandLine + 1;
			com_numConsoleLines++;
			*commandLine = 0;
		}
		commandLine++;
	}
}


/*
===================
Com_SafeMode

Check for "safe" on the command line, which will
skip loading of wolfconfig.cfg
===================
*/
qboolean Com_SafeMode( void ) {
	int i;

	for ( i = 0 ; i < com_numConsoleLines ; i++ ) {
		Cmd_TokenizeString( com_consoleLines[i] );
		if ( !Q_stricmp( Cmd_Argv( 0 ), "safe" )
			 || !Q_stricmp( Cmd_Argv( 0 ), "cvar_restart" ) ) {
			com_consoleLines[i][0] = 0;
			return qtrue;
		}
	}
	return qfalse;
}


/*
===============
Com_StartupVariable

Searches for command line parameters that are set commands.
If match is not NULL, only that cvar will be looked for.
That is necessary because cddir and basedir need to be set
before the filesystem is started, but all other sets should
be after execing the config and default.
===============
*/
void Com_StartupVariable( const char *match ) {
	int i;
	char    *s;

	for ( i = 0 ; i < com_numConsoleLines ; i++ ) {
		Cmd_TokenizeString( com_consoleLines[i] );
		if ( strcmp( Cmd_Argv( 0 ), "set" ) ) {
			continue;
		}

		s = Cmd_Argv( 1 );

		if(!match || !strcmp(s, match))
		{
			if(Cvar_Flags(s) == CVAR_NONEXISTENT)
				Cvar_Get(s, Cmd_ArgsFrom(2), CVAR_USER_CREATED);
			else
				Cvar_Set2(s, Cmd_ArgsFrom(2), qfalse);
		}
	}
}


/*
=================
Com_AddStartupCommands

Adds command line parameters as script statements
Commands are seperated by + signs

Returns qtrue if any late commands were added, which
will keep the demoloop from immediately starting
=================
*/
static qboolean Com_StartupHasGameCommand( void ) {
	int i;

	for ( i = 0 ; i < com_numConsoleLines ; i++ ) {
		const char *l = com_consoleLines[i];

		if ( !l || !l[0] || !Q_stricmpn( l, "set ", 4 ) || !Q_stricmpn( l, "seta ", 5 ) || !Q_stricmpn( l, "exec ", 5 ) ) {
			continue;
		}
		return qtrue;
	}
	return qfalse;
}

qboolean Com_AddStartupCommands( void ) {
	int i;
	qboolean added;

	added = qfalse;
	// quote every token, so args with semicolons can work
	for ( i = 0 ; i < com_numConsoleLines ; i++ ) {
		if ( !com_consoleLines[i] || !com_consoleLines[i][0] ) {
			continue;
		}

		// set commands already added with Com_StartupVariable
		if ( !Q_stricmpn( com_consoleLines[i], "set ", 4 ) ) {
			continue;
		}

		added = qtrue;
		Cbuf_AddText( com_consoleLines[i] );
		Cbuf_AddText( "\n" );
	}

	return added;
}


//============================================================================

void Info_Print( const char *s ) {
	char	key[BIG_INFO_KEY];
	char	value[BIG_INFO_VALUE];
	char    *o;
	int l;

	if ( *s == '\\' ) {
		s++;
	}
	while ( *s )
	{
		o = key;
		while ( *s && *s != '\\' )
			*o++ = *s++;

		l = o - key;
		if ( l < 20 ) {
			memset( o, ' ', 20 - l );
			key[20] = 0;
		} else {
			*o = 0;
		}
		Com_Printf( "%s ", key );

		if ( !*s ) {
			Com_Printf( "MISSING VALUE\n" );
			return;
		}

		o = value;
		s++;
		while ( *s && *s != '\\' )
			*o++ = *s++;
		*o = 0;

		if ( *s ) {
			s++;
		}
		Com_Printf( "%s\n", value );
	}
}

/*
============
Com_StringContains
============
*/
char *Com_StringContains( char *str1, char *str2, int casesensitive ) {
	int len, i, j;

	len = strlen( str1 ) - strlen( str2 );
	for ( i = 0; i <= len; i++, str1++ ) {
		for ( j = 0; str2[j]; j++ ) {
			if ( casesensitive ) {
				if ( str1[j] != str2[j] ) {
					break;
				}
			} else {
				if ( toupper( str1[j] ) != toupper( str2[j] ) ) {
					break;
				}
			}
		}
		if ( !str2[j] ) {
			return str1;
		}
	}
	return NULL;
}

/*
============
Com_Filter
============
*/
int Com_Filter( char *filter, char *name, int casesensitive ) {
	char buf[MAX_TOKEN_CHARS];
	char *ptr;
	int i, found;

	while ( *filter ) {
		if ( *filter == '*' ) {
			filter++;
			for ( i = 0; *filter; i++ ) {
				if ( *filter == '*' || *filter == '?' ) {
					break;
				}
				buf[i] = *filter;
				filter++;
			}
			buf[i] = '\0';
			if ( strlen( buf ) ) {
				ptr = Com_StringContains( name, buf, casesensitive );
				if ( !ptr ) {
					return qfalse;
				}
				name = ptr + strlen( buf );
			}
		} else if ( *filter == '?' )      {
			filter++;
			name++;
		} else if ( *filter == '[' && *( filter + 1 ) == '[' )           {
			filter++;
		} else if ( *filter == '[' )      {
			filter++;
			found = qfalse;
			while ( *filter && !found ) {
				if ( *filter == ']' && *( filter + 1 ) != ']' ) {
					break;
				}
				if ( *( filter + 1 ) == '-' && *( filter + 2 ) && ( *( filter + 2 ) != ']' || *( filter + 3 ) == ']' ) ) {
					if ( casesensitive ) {
						if ( *name >= *filter && *name <= *( filter + 2 ) ) {
							found = qtrue;
						}
					} else {
						if ( toupper( *name ) >= toupper( *filter ) &&
							 toupper( *name ) <= toupper( *( filter + 2 ) ) ) {
							found = qtrue;
						}
					}
					filter += 3;
				} else {
					if ( casesensitive ) {
						if ( *filter == *name ) {
							found = qtrue;
						}
					} else {
						if ( toupper( *filter ) == toupper( *name ) ) {
							found = qtrue;
						}
					}
					filter++;
				}
			}
			if ( !found ) {
				return qfalse;
			}
			while ( *filter ) {
				if ( *filter == ']' && *( filter + 1 ) != ']' ) {
					break;
				}
				filter++;
			}
			filter++;
			name++;
		} else {
			if ( casesensitive ) {
				if ( *filter != *name ) {
					return qfalse;
				}
			} else {
				if ( toupper( *filter ) != toupper( *name ) ) {
					return qfalse;
				}
			}
			filter++;
			name++;
		}
	}
	return qtrue;
}

/*
============
Com_FilterPath
============
*/
int Com_FilterPath( char *filter, char *name, int casesensitive ) {
	int i;
	char new_filter[MAX_QPATH];
	char new_name[MAX_QPATH];

	for ( i = 0; i < MAX_QPATH - 1 && filter[i]; i++ ) {
		if ( filter[i] == '\\' || filter[i] == ':' ) {
			new_filter[i] = '/';
		} else {
			new_filter[i] = filter[i];
		}
	}
	new_filter[i] = '\0';
	for ( i = 0; i < MAX_QPATH - 1 && name[i]; i++ ) {
		if ( name[i] == '\\' || name[i] == ':' ) {
			new_name[i] = '/';
		} else {
			new_name[i] = name[i];
		}
	}
	new_name[i] = '\0';
	return Com_Filter( new_filter, new_name, casesensitive );
}

/*
================
Com_RealTime
================
*/
int Com_RealTime( qtime_t *qtime ) {
	time_t t;
	struct tm *tms;

	t = time( NULL );
	if ( !qtime ) {
		return t;
	}
	tms = localtime( &t );
	if ( tms ) {
		qtime->tm_sec = tms->tm_sec;
		qtime->tm_min = tms->tm_min;
		qtime->tm_hour = tms->tm_hour;
		qtime->tm_mday = tms->tm_mday;
		qtime->tm_mon = tms->tm_mon;
		qtime->tm_year = tms->tm_year;
		qtime->tm_wday = tms->tm_wday;
		qtime->tm_yday = tms->tm_yday;
		qtime->tm_isdst = tms->tm_isdst;
	}
	return t;
}


/*
==============================================================================

						ZONE MEMORY ALLOCATION

==============================================================================

  The old zone is gone, mallocs replaced it. To keep the widespread code changes down to a bare minimum
  Z_Malloc and Z_Free still work.
*/


/*
========================
Z_Free
========================
*/
void Z_Free( void *ptr ) {
	free( ptr );
}


/*
================
Z_Malloc
================
*/
void *Z_Malloc( int size ) {
	void *buf = malloc( size );
	Com_Memset( buf, 0, size );
	return buf;
}

#if 0
/*
================
Z_TagMalloc
================
*/
void *Z_TagMalloc( int size, int tag ) {

	if ( tag != TAG_RENDERER ) {
		assert( 0 );
	}

	if ( g_numTaggedAllocs < MAX_TAG_ALLOCS ) {
		void *ptr = Z_Malloc( size );
		g_taggedAllocations[g_numTaggedAllocs++] = ptr;
		return ptr;
	} else {
		Com_Error( ERR_FATAL, "Z_TagMalloc: out of tagged allocation space\n" );
	}
	return NULL;
}

/*
================
Z_FreeTags
================
*/
void Z_FreeTags( int tag ) {
	int i;

	if ( tag != TAG_RENDERER ) {
		assert( 0 );
	}

	for ( i = 0; i < g_numTaggedAllocs; i++ ) {
		free( g_taggedAllocations[i] );
	}

	g_numTaggedAllocs = 0;
}

#endif

/*
========================
CopyString

 NOTE:	never write over the memory CopyString returns because
		memory from a memstatic_t might be returned
========================
*/
char *CopyString( const char *in ) {
	char    *out;

	out = Z_Malloc( strlen( in ) + 1 );
	strcpy( out, in );
	return out;
}

/*
==============================================================================

Goals:
	reproducable without history effects -- no out of memory errors on weird map to map changes
	allow restarting of the client without fragmentation
	minimize total pages in use at run time
	minimize total pages needed during load time

  Single block of memory with stack allocators coming from both ends towards the middle.

  One side is designated the temporary memory allocator.

  Temporary memory can be allocated and freed in any order.

  A highwater mark is kept of the most in use at any time.

  When there is no temporary memory allocated, the permanent and temp sides
  can be switched, allowing the already touched temp memory to be used for
  permanent storage.

  Temp memory must never be allocated on two ends at once, or fragmentation
  could occur.

  If we have any in-use temp memory, additional temp allocations must come from
  that side.

  If not, we can choose to make either side the new temp side and push future
  permanent allocations to the other side.  Permanent allocations should be
  kept on the side that has the current greatest wasted highwater mark.

==============================================================================
*/


#define HUNK_MAGIC  0x89537892
#define HUNK_FREE_MAGIC 0x89537893

typedef struct {
	int magic;
	int size;
} hunkHeader_t;

typedef struct {
	int mark;
	int permanent;
	int temp;
	int tempHighwater;
} hunkUsed_t;

typedef struct hunkblock_s {
	int size;
	byte printed;
	struct hunkblock_s *next;
	char *label;
	char *file;
	int line;
} hunkblock_t;

static hunkblock_t *hunkblocks;

static hunkUsed_t hunk_low, hunk_high;
static hunkUsed_t  *hunk_permanent, *hunk_temp;

static byte    *s_hunkData = NULL;
static int s_hunkTotal;

static int s_zoneTotal;
//static	int		s_smallZoneTotal; // TTimo: unused


/*
=================
Com_Meminfo_f
=================
*/
void Com_Meminfo_f( void ) {
	int unused;

	Com_Printf( "%8i bytes total hunk\n", s_hunkTotal );
	Com_Printf( "%8i bytes total zone\n", s_zoneTotal );
	Com_Printf( "\n" );
	Com_Printf( "%8i low mark\n", hunk_low.mark );
	Com_Printf( "%8i low permanent\n", hunk_low.permanent );
	if ( hunk_low.temp != hunk_low.permanent ) {
		Com_Printf( "%8i low temp\n", hunk_low.temp );
	}
	Com_Printf( "%8i low tempHighwater\n", hunk_low.tempHighwater );
	Com_Printf( "\n" );
	Com_Printf( "%8i high mark\n", hunk_high.mark );
	Com_Printf( "%8i high permanent\n", hunk_high.permanent );
	if ( hunk_high.temp != hunk_high.permanent ) {
		Com_Printf( "%8i high temp\n", hunk_high.temp );
	}
	Com_Printf( "%8i high tempHighwater\n", hunk_high.tempHighwater );
	Com_Printf( "\n" );
	Com_Printf( "%8i total hunk in use\n", hunk_low.permanent + hunk_high.permanent );
	unused = 0;
	if ( hunk_low.tempHighwater > hunk_low.permanent ) {
		unused += hunk_low.tempHighwater - hunk_low.permanent;
	}
	if ( hunk_high.tempHighwater > hunk_high.permanent ) {
		unused += hunk_high.tempHighwater - hunk_high.permanent;
	}
	Com_Printf( "%8i unused highwater\n", unused );
	Com_Printf( "\n" );

	//Com_Printf( "        %i number of tagged renderer allocations\n", g_numTaggedAllocs);
}

/*
===============
Com_TouchMemory

Touch all known used data to make sure it is paged in
===============
*/
void Com_TouchMemory( void ) {
	int start, end;
	int i, j;
	unsigned sum;

	start = Sys_Milliseconds();

	sum = 0;

	j = hunk_low.permanent >> 2;
	for ( i = 0 ; i < j ; i += 64 ) {         // only need to touch each page
		sum += ( (int *)s_hunkData )[i];
		if ( ( i & 0xfffff ) < 64 ) {
			CL_LoadingReplayTick();     // no-op outside a local map load
		}
	}

	i = ( s_hunkTotal - hunk_high.permanent ) >> 2;
	j = hunk_high.permanent >> 2;
	for (  ; i < j ; i += 64 ) {          // only need to touch each page
		sum += ( (int *)s_hunkData )[i];
		if ( ( i & 0xfffff ) < 64 ) {
			CL_LoadingReplayTick();
		}
	}

	end = Sys_Milliseconds();

	Com_Printf( "Com_TouchMemory: %i msec\n", end - start );
}




void Com_InitZoneMemory( void ) {
	//memset(g_taggedAllocations, 0, sizeof(g_taggedAllocations));
	//g_numTaggedAllocs = 0;
}

/*
=================
Hunk_Log
=================
*/
void Hunk_Log( void ) {
	hunkblock_t *block;
	char buf[4096];
	int size, numBlocks;

	if ( !logfile || !FS_Initialized() ) {
		return;
	}
	size = 0;
	numBlocks = 0;
	Com_sprintf( buf, sizeof( buf ), "\r\n================\r\nHunk log\r\n================\r\n" );
	FS_Write( buf, strlen( buf ), logfile );
	for ( block = hunkblocks ; block; block = block->next ) {
#ifdef HUNK_DEBUG
		Com_sprintf( buf, sizeof( buf ), "size = %8d: %s, line: %d (%s)\r\n", block->size, block->file, block->line, block->label );
		FS_Write( buf, strlen( buf ), logfile );
#endif
		size += block->size;
		numBlocks++;
	}
	Com_sprintf( buf, sizeof( buf ), "%d Hunk memory\r\n", size );
	FS_Write( buf, strlen( buf ), logfile );
	Com_sprintf( buf, sizeof( buf ), "%d hunk blocks\r\n", numBlocks );
	FS_Write( buf, strlen( buf ), logfile );
}

/*
=================
Hunk_SmallLog
=================
*/
void Hunk_SmallLog( void ) {
	hunkblock_t *block, *block2;
	char buf[4096];
	int size, locsize, numBlocks;

	if ( !logfile || !FS_Initialized() ) {
		return;
	}
	for ( block = hunkblocks ; block; block = block->next ) {
		block->printed = qfalse;
	}
	size = 0;
	numBlocks = 0;
	Com_sprintf( buf, sizeof( buf ), "\r\n================\r\nHunk Small log\r\n================\r\n" );
	FS_Write( buf, strlen( buf ), logfile );
	for ( block = hunkblocks; block; block = block->next ) {
		if ( block->printed ) {
			continue;
		}
		locsize = block->size;
		for ( block2 = block->next; block2; block2 = block2->next ) {
			if ( block->line != block2->line ) {
				continue;
			}
			if ( Q_stricmp( block->file, block2->file ) ) {
				continue;
			}
			size += block2->size;
			locsize += block2->size;
			block2->printed = qtrue;
		}
#ifdef HUNK_DEBUG
		Com_sprintf( buf, sizeof( buf ), "size = %8d: %s, line: %d (%s)\r\n", locsize, block->file, block->line, block->label );
		FS_Write( buf, strlen( buf ), logfile );
#endif
		size += block->size;
		numBlocks++;
	}
	Com_sprintf( buf, sizeof( buf ), "%d Hunk memory\r\n", size );
	FS_Write( buf, strlen( buf ), logfile );
	Com_sprintf( buf, sizeof( buf ), "%d hunk blocks\r\n", numBlocks );
	FS_Write( buf, strlen( buf ), logfile );
}

/*
=================
Com_InitHunkMemory
=================
*/
void Com_InitHunkMemory( void ) {
	cvar_t  *cv;
	int nMinAlloc;
	char *pMsg = NULL;

	// make sure the file system has allocated and "not" freed any temp blocks
	// this allows the config and product id files ( journal files too ) to be loaded
	// by the file system without redunant routines in the file system utilizing different
	// memory systems
	if ( FS_LoadStack() != 0 ) {
		Com_Error( ERR_FATAL, "Hunk initialization failed. File system load stack not zero" );
	}

	// allocate the stack based hunk allocator
	cv = Cvar_Get( "com_hunkMegs", DEF_COMHUNKMEGS_S, CVAR_LATCH | CVAR_ARCHIVE );

	// if we are not dedicated min allocation is 56, otherwise min is 1
	if ( com_dedicated && com_dedicated->integer ) {
		nMinAlloc = MIN_DEDICATED_COMHUNKMEGS;
		pMsg = "Minimum com_hunkMegs for a dedicated server is %i, allocating %i megs.\n";
	} else {
		nMinAlloc = MIN_COMHUNKMEGS;
		pMsg = "Minimum com_hunkMegs is %i, allocating %i megs.\n";
	}

	if ( cv->integer < nMinAlloc ) {
		s_hunkTotal = 1024 * 1024 * nMinAlloc;
		Com_Printf( pMsg, nMinAlloc, s_hunkTotal / ( 1024 * 1024 ) );
	} else {
		s_hunkTotal = cv->integer * 1024 * 1024;
	}


	s_hunkData = malloc( s_hunkTotal + 31 );
	if ( !s_hunkData ) {
		Com_Error( ERR_FATAL, "Hunk data failed to allocate %i megs", s_hunkTotal / ( 1024 * 1024 ) );
	}
	// cacheline align
	s_hunkData = (byte *) ( ( (intptr_t)s_hunkData + 31 ) & ~31 );
	Hunk_Clear();

	Cmd_AddCommand( "meminfo", Com_Meminfo_f );
#ifdef HUNK_DEBUG
	Cmd_AddCommand( "hunklog", Hunk_Log );
	Cmd_AddCommand( "hunksmalllog", Hunk_SmallLog );
#endif
}

/*
====================
Hunk_MemoryRemaining
====================
*/
int Hunk_MemoryRemaining( void ) {
	int low, high;

	low = hunk_low.permanent > hunk_low.temp ? hunk_low.permanent : hunk_low.temp;
	high = hunk_high.permanent > hunk_high.temp ? hunk_high.permanent : hunk_high.temp;

	return s_hunkTotal - ( low + high );
}

/*
===================
Hunk_SetMark

The server calls this after the level and game VM have been loaded
===================
*/
void Hunk_SetMark( void ) {
	hunk_low.mark = hunk_low.permanent;
	hunk_high.mark = hunk_high.permanent;
}

/*
=================
Hunk_ClearToMark

The client calls this before starting a vid_restart or snd_restart
=================
*/
void Hunk_ClearToMark( void ) {
	hunk_low.permanent = hunk_low.temp = hunk_low.mark;
	hunk_high.permanent = hunk_high.temp = hunk_high.mark;
}

/*
=================
Hunk_CheckMark
=================
*/
qboolean Hunk_CheckMark( void ) {
	if ( hunk_low.mark || hunk_high.mark ) {
		return qtrue;
	}
	return qfalse;
}

void CL_ShutdownCGame( void );
void CL_ShutdownUI( void );
void SV_ShutdownGameProgs( void );

/*
=================
Hunk_Clear

The server calls this before shutting down or loading a new map
=================
*/
void Hunk_Clear( void ) {

#ifndef DEDICATED
	CL_ShutdownCGame();
	CL_ShutdownUI();
#endif
	SV_ShutdownGameProgs();
#ifndef DEDICATED
	CIN_CloseAllVideos();
#endif
	hunk_low.mark = 0;
	hunk_low.permanent = 0;
	hunk_low.temp = 0;
	hunk_low.tempHighwater = 0;

	hunk_high.mark = 0;
	hunk_high.permanent = 0;
	hunk_high.temp = 0;
	hunk_high.tempHighwater = 0;

	hunk_permanent = &hunk_low;
	hunk_temp = &hunk_high;

	Cvar_Set( "com_hunkused", va( "%i", hunk_low.permanent + hunk_high.permanent ) );
	Com_Printf( "Hunk_Clear: reset the hunk ok\n" );
	VM_Clear(); // (SA) FIXME:TODO: was commented out in wolf
#ifdef HUNK_DEBUG
	hunkblocks = NULL;
#endif
}

static void Hunk_SwapBanks( void ) {
	hunkUsed_t  *swap;

	// can't swap banks if there is any temp already allocated
	if ( hunk_temp->temp != hunk_temp->permanent ) {
		return;
	}

	// if we have a larger highwater mark on this side, start making
	// our permanent allocations here and use the other side for temp
	if ( hunk_temp->tempHighwater - hunk_temp->permanent >
		 hunk_permanent->tempHighwater - hunk_permanent->permanent ) {
		swap = hunk_temp;
		hunk_temp = hunk_permanent;
		hunk_permanent = swap;
	}
}

/*
=================
Hunk_Alloc

Allocate permanent (until the hunk is cleared) memory
=================
*/
#ifdef HUNK_DEBUG
void *Hunk_AllocDebug( int size, ha_pref preference, char *label, char *file, int line ) {
#else
void *Hunk_Alloc( int size, ha_pref preference ) {
#endif
	void    *buf;

	if ( s_hunkData == NULL ) {
		Com_Error( ERR_FATAL, "Hunk_Alloc: Hunk memory system not initialized" );
	}

	Hunk_SwapBanks();

#ifdef HUNK_DEBUG
	size += sizeof( hunkblock_t );
#endif

	// round to cacheline
	size = ( size + 31 ) & ~31;

	if ( hunk_low.temp + hunk_high.temp + size > s_hunkTotal ) {
#ifdef HUNK_DEBUG
		Hunk_Log();
		Hunk_SmallLog();

		Com_Error(ERR_DROP, "Hunk_Alloc failed on %i: %s, line: %d (%s)", size, file, line, label);
#else
		Com_Error(ERR_DROP, "Hunk_Alloc failed on %i", size);
#endif
	}

	if ( hunk_permanent == &hunk_low ) {
		buf = ( void * )( s_hunkData + hunk_permanent->permanent );
		hunk_permanent->permanent += size;
	} else {
		hunk_permanent->permanent += size;
		buf = ( void * )( s_hunkData + s_hunkTotal - hunk_permanent->permanent );
	}

	hunk_permanent->temp = hunk_permanent->permanent;

	memset( buf, 0, size );

#ifdef HUNK_DEBUG
	{
		hunkblock_t *block;

		block = (hunkblock_t *) buf;
		block->size = size - sizeof( hunkblock_t );
		block->file = file;
		block->label = label;
		block->line = line;
		block->next = hunkblocks;
		hunkblocks = block;
		buf = ( (byte *) buf ) + sizeof( hunkblock_t );
	}
#endif
	// Ridah, update the com_hunkused cvar in increments, so we don't update it too often, since this cvar call isn't very efficent
	if ( ( hunk_low.permanent + hunk_high.permanent ) > com_hunkused->integer + 10000 ) {
		Cvar_Set( "com_hunkused", va( "%i", hunk_low.permanent + hunk_high.permanent ) );
	}

	return buf;
}

/*
=================
Hunk_AllocateTempMemory

This is used by the file loading system.
Multiple files can be loaded in temporary memory.
When the files-in-use count reaches zero, all temp memory will be deleted
=================
*/
void *Hunk_AllocateTempMemory( int size ) {
	void        *buf;
	hunkHeader_t    *hdr;

	// return a Z_Malloc'd block if the hunk has not been initialized
	// this allows the config and product id files ( journal files too ) to be loaded
	// by the file system without redunant routines in the file system utilizing different
	// memory systems
	if ( s_hunkData == NULL ) {
		return Z_Malloc( size );
	}

	Hunk_SwapBanks();

	size = PAD(size, sizeof(intptr_t)) + sizeof( hunkHeader_t );

	if ( hunk_temp->temp + hunk_permanent->permanent + size > s_hunkTotal ) {
		Com_Error( ERR_DROP, "Hunk_AllocateTempMemory: failed on %i", size );
	}

	if ( hunk_temp == &hunk_low ) {
		buf = ( void * )( s_hunkData + hunk_temp->temp );
		hunk_temp->temp += size;
	} else {
		hunk_temp->temp += size;
		buf = ( void * )( s_hunkData + s_hunkTotal - hunk_temp->temp );
	}

	if ( hunk_temp->temp > hunk_temp->tempHighwater ) {
		hunk_temp->tempHighwater = hunk_temp->temp;
	}

	hdr = (hunkHeader_t *)buf;
	buf = ( void * )( hdr + 1 );

	hdr->magic = HUNK_MAGIC;
	hdr->size = size;

	// don't bother clearing, because we are going to load a file over it
	return buf;
}


/*
==================
Hunk_FreeTempMemory
==================
*/
void Hunk_FreeTempMemory( void *buf ) {
	hunkHeader_t    *hdr;

	// free with Z_Free if the hunk has not been initialized
	// this allows the config and product id files ( journal files too ) to be loaded
	// by the file system without redunant routines in the file system utilizing different
	// memory systems
	if ( s_hunkData == NULL ) {
		Z_Free( buf );
		return;
	}


	hdr = ( (hunkHeader_t *)buf ) - 1;
	if ( hdr->magic != HUNK_MAGIC ) {
		Com_Error( ERR_FATAL, "Hunk_FreeTempMemory: bad magic" );
	}

	hdr->magic = HUNK_FREE_MAGIC;

	// this only works if the files are freed in stack order,
	// otherwise the memory will stay around until Hunk_ClearTempMemory
	if ( hunk_temp == &hunk_low ) {
		if ( hdr == ( void * )( s_hunkData + hunk_temp->temp - hdr->size ) ) {
			hunk_temp->temp -= hdr->size;
		} else {
			Com_Printf( "Hunk_FreeTempMemory: not the final block\n" );
		}
	} else {
		if ( hdr == ( void * )( s_hunkData + s_hunkTotal - hunk_temp->temp ) ) {
			hunk_temp->temp -= hdr->size;
		} else {
			Com_Printf( "Hunk_FreeTempMemory: not the final block\n" );
		}
	}
}


/*
=================
Hunk_ClearTempMemory

The temp space is no longer needed.  If we have left more
touched but unused memory on this side, have future
permanent allocs use this side.
=================
*/
void Hunk_ClearTempMemory( void ) {
	if ( s_hunkData != NULL ) {
		hunk_temp->temp = hunk_temp->permanent;
	}
}

/*
===================================================================

EVENTS AND JOURNALING

In addition to these events, .cfg files are also copied to the
journaled file
===================================================================
*/

#define MAX_PUSHED_EVENTS              1024 
static int com_pushedEventsHead = 0;
static int com_pushedEventsTail = 0;
static sysEvent_t com_pushedEvents[MAX_PUSHED_EVENTS];

/*
=================
Com_InitJournaling
=================
*/
void Com_InitJournaling( void ) {
	Com_StartupVariable( "journal" );
	com_journal = Cvar_Get( "journal", "0", CVAR_INIT );
	if ( !com_journal->integer ) {
		return;
	}

	if ( com_journal->integer == 1 ) {
		Com_Printf( "Journaling events\n" );
		com_journalFile = FS_FOpenFileWrite( "journal.dat" );
		com_journalDataFile = FS_FOpenFileWrite( "journaldata.dat" );
	} else if ( com_journal->integer == 2 ) {
		Com_Printf( "Replaying journaled events\n" );
		FS_FOpenFileRead( "journal.dat", &com_journalFile, qtrue );
		FS_FOpenFileRead( "journaldata.dat", &com_journalDataFile, qtrue );
	}

	if ( !com_journalFile || !com_journalDataFile ) {
		Cvar_Set( "com_journal", "0" );
		com_journalFile = 0;
		com_journalDataFile = 0;
		Com_Printf( "Couldn't open journal files\n" );
	}
}

/*
========================================================================

EVENT LOOP

========================================================================
*/

#define MAX_QUEUED_EVENTS  256
#define MASK_QUEUED_EVENTS ( MAX_QUEUED_EVENTS - 1 )

static sysEvent_t  eventQueue[ MAX_QUEUED_EVENTS ];
static int         eventHead = 0;
static int         eventTail = 0;

/*
================
Com_QueueEvent

A time of 0 will get the current time
Ptr should either be null, or point to a block of data that can
be freed by the game later.
================
*/
void Com_QueueEvent( int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr )
{
	sysEvent_t  *ev;

	// combine mouse movement with previous mouse event
	if ( type == SE_MOUSE && eventHead != eventTail )
	{
		ev = &eventQueue[ ( eventHead + MAX_QUEUED_EVENTS - 1 ) & MASK_QUEUED_EVENTS ];

		if ( ev->evType == SE_MOUSE )
		{
			ev->evValue += value;
			ev->evValue2 += value2;
			return;
		}
	}

	ev = &eventQueue[ eventHead & MASK_QUEUED_EVENTS ];

	if ( eventHead - eventTail >= MAX_QUEUED_EVENTS )
	{
		Com_Printf("Com_QueueEvent: overflow\n");
		// we are discarding an event, but don't leak memory
		if ( ev->evPtr )
		{
			Z_Free( ev->evPtr );
		}
		eventTail++;
	}

	eventHead++;

	if ( time == 0 )
	{
		time = Sys_Milliseconds();
	}

	ev->evTime = time;
	ev->evType = type;
	ev->evValue = value;
	ev->evValue2 = value2;
	ev->evPtrLength = ptrLength;
	ev->evPtr = ptr;
}

/*
================
Com_PeekMouseEvents

Sums the queued, not yet consumed mouse deltas without dequeuing them.
================
*/
void Com_PeekMouseEvents( int *dx, int *dy )
{
	int i;

	*dx = 0;
	*dy = 0;
	for ( i = eventTail; i < eventHead; i++ )
	{
		const sysEvent_t *ev = &eventQueue[ i & MASK_QUEUED_EVENTS ];

		if ( ev->evType == SE_MOUSE )
		{
			*dx += ev->evValue;
			*dy += ev->evValue2;
		}
	}
}

/*
================
Com_GetSystemEvent

================
*/
sysEvent_t Com_GetSystemEvent( void )
{
	sysEvent_t  ev;
	char        *s;

	// return if we have data
	if ( eventHead > eventTail )
	{
		eventTail++;
		return eventQueue[ ( eventTail - 1 ) & MASK_QUEUED_EVENTS ];
	}

	// check for console commands
	s = Sys_ConsoleInput();
	if ( s )
	{
		char  *b;
		int   len;

		len = strlen( s ) + 1;
		b = Z_Malloc( len );
		strcpy( b, s );
		Com_QueueEvent( 0, SE_CONSOLE, 0, 0, len, b );
	}

	// return if we have data
	if ( eventHead > eventTail )
	{
		eventTail++;
		return eventQueue[ ( eventTail - 1 ) & MASK_QUEUED_EVENTS ];
	}

	// create an empty event to return
	memset( &ev, 0, sizeof( ev ) );
	ev.evTime = Sys_Milliseconds();

	return ev;
}

/*
=================
Com_GetRealEvent
=================
*/
sysEvent_t  Com_GetRealEvent( void ) {
	int r;
	sysEvent_t ev;

	// either get an event from the system or the journal file
	if ( com_journal->integer == 2 ) {
		r = FS_Read( &ev, sizeof( ev ), com_journalFile );
		if ( r != sizeof( ev ) ) {
			Com_Error( ERR_FATAL, "Error reading from journal file" );
		}
		if ( ev.evPtrLength ) {
			ev.evPtr = Z_Malloc( ev.evPtrLength );
			r = FS_Read( ev.evPtr, ev.evPtrLength, com_journalFile );
			if ( r != ev.evPtrLength ) {
				Com_Error( ERR_FATAL, "Error reading from journal file" );
			}
		}
	} else {
		ev = Com_GetSystemEvent();

		// write the journal value out if needed
		if ( com_journal->integer == 1 ) {
			r = FS_Write( &ev, sizeof( ev ), com_journalFile );
			if ( r != sizeof( ev ) ) {
				Com_Error( ERR_FATAL, "Error writing to journal file" );
			}
			if ( ev.evPtrLength ) {
				r = FS_Write( ev.evPtr, ev.evPtrLength, com_journalFile );
				if ( r != ev.evPtrLength ) {
					Com_Error( ERR_FATAL, "Error writing to journal file" );
				}
			}
		}
	}

	return ev;
}


/*
=================
Com_InitPushEvent
=================
*/
// bk001129 - added
void Com_InitPushEvent( void ) {
	// clear the static buffer array
	// this requires SE_NONE to be accepted as a valid but NOP event
	memset( com_pushedEvents, 0, sizeof( com_pushedEvents ) );
	// reset counters while we are at it
	// beware: GetEvent might still return an SE_NONE from the buffer
	com_pushedEventsHead = 0;
	com_pushedEventsTail = 0;
}


/*
=================
Com_PushEvent
=================
*/
void Com_PushEvent( sysEvent_t *event ) {
	sysEvent_t      *ev;
	static int printedWarning = 0;

	ev = &com_pushedEvents[ com_pushedEventsHead & ( MAX_PUSHED_EVENTS - 1 ) ];

	if ( com_pushedEventsHead - com_pushedEventsTail >= MAX_PUSHED_EVENTS ) {

		// don't print the warning constantly, or it can give time for more...
		if ( !printedWarning ) {
			printedWarning = qtrue;
			Com_Printf( "WARNING: Com_PushEvent overflow\n" );
		}

		if ( ev->evPtr ) {
			Z_Free( ev->evPtr );
		}
		com_pushedEventsTail++;
	} else {
		printedWarning = qfalse;
	}

	*ev = *event;
	com_pushedEventsHead++;
}

/*
=================
Com_GetEvent
=================
*/
sysEvent_t  Com_GetEvent( void ) {
	if ( com_pushedEventsHead > com_pushedEventsTail ) {
		com_pushedEventsTail++;
		return com_pushedEvents[ ( com_pushedEventsTail - 1 ) & ( MAX_PUSHED_EVENTS - 1 ) ];
	}
	return Com_GetRealEvent();
}

/*
=================
Com_RunAndTimeServerPacket
=================
*/
void Com_RunAndTimeServerPacket( netadr_t *evFrom, msg_t *buf ) {
	int t1, t2, msec;

	t1 = 0;

	if ( com_speeds->integer ) {
		t1 = Sys_Milliseconds();
	}

	SV_PacketEvent( *evFrom, buf );

	if ( com_speeds->integer ) {
		t2 = Sys_Milliseconds();
		msec = t2 - t1;
		if ( com_speeds->integer == 3 ) {
			Com_Printf( "SV_PacketEvent time: %i\n", msec );
		}
	}
}

/*
=================
Com_EventLoop

Returns last event time
=================
*/
int Com_EventLoop( void ) {
	sysEvent_t ev;
	netadr_t evFrom;
	byte bufData[MAX_MSGLEN];
	msg_t buf;

	MSG_Init( &buf, bufData, sizeof( bufData ) );

	while ( 1 ) {
		ev = Com_GetEvent();

		// if no more events are available
		if ( ev.evType == SE_NONE ) {
			// manually send packet events for the loopback channel
			while ( NET_GetLoopPacket( NS_CLIENT, &evFrom, &buf ) ) {
				CL_PacketEvent( evFrom, &buf );
			}

			while ( NET_GetLoopPacket( NS_SERVER, &evFrom, &buf ) ) {
				// if the server just shut down, flush the events
				if ( com_sv_running->integer ) {
					Com_RunAndTimeServerPacket( &evFrom, &buf );
				}
			}

			return ev.evTime;
		}


		switch ( ev.evType ) {
		case SE_KEY:
			CL_KeyEvent( ev.evValue, ev.evValue2, ev.evTime );
			break;
		case SE_CHAR:
			CL_CharEvent( ev.evValue );
			break;
		case SE_MOUSE:
			CL_MouseEvent( ev.evValue, ev.evValue2, ev.evTime );
			break;
		case SE_JOYSTICK_AXIS:
			CL_JoystickEvent( ev.evValue, ev.evValue2, ev.evTime );
			break;
		case SE_CONSOLE:
			Cbuf_AddText( (char *)ev.evPtr );
			Cbuf_AddText( "\n" );
			break;
		default:
			Com_Error( ERR_FATAL, "Com_EventLoop: bad event type %i", ev.evType );
			break;
		}

		// free any block data
		if ( ev.evPtr ) {
			Z_Free( ev.evPtr );
		}
	}

	return 0;   // never reached
}

/*
================
Com_Milliseconds

Can be used for profiling, but will be journaled accurately
================
*/
int Com_Milliseconds( void ) {
	sysEvent_t ev;

	// get events and push them until we get a null event with the current time
	do {

		ev = Com_GetRealEvent();
		if ( ev.evType != SE_NONE ) {
			Com_PushEvent( &ev );
		}
	} while ( ev.evType != SE_NONE );

	return ev.evTime;
}

//============================================================================

/*
=============
Com_Error_f

Just throw a fatal error to
test error shutdown procedures
=============
*/
static void __attribute__((__noreturn__)) Com_Error_f (void) {
	if ( Cmd_Argc() > 1 ) {
		Com_Error( ERR_DROP, "Testing drop error" );
	} else {
		Com_Error( ERR_FATAL, "Testing fatal error" );
	}
}


/*
=============
Com_Freeze_f

Just freeze in place for a given number of seconds to test
error recovery
=============
*/
static void Com_Freeze_f( void ) {
	float s;
	int start, now;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "freeze <seconds>\n" );
		return;
	}
	s = atof( Cmd_Argv( 1 ) );

	start = Com_Milliseconds();

	while ( 1 ) {
		now = Com_Milliseconds();
		if ( ( now - start ) * 0.001 > s ) {
			break;
		}
	}
}

/*
=================
Com_Crash_f

A way to force a bus error for development reasons
=================
*/
static void Com_Crash_f( void ) {
	* ( volatile int * ) 0 = 0x12345678;
}

/*
==================
Com_Setenv_f

For controlling environment variables
==================
*/
void Com_Setenv_f(void)
{
	int argc = Cmd_Argc();
	char *arg1 = Cmd_Argv(1);

	if(argc > 2)
	{
		char *arg2 = Cmd_ArgsFrom(2);
		
		Sys_SetEnv(arg1, arg2);
	}
	else if(argc == 2)
	{
		char *env = getenv(arg1);
		
		if(env)
			Com_Printf("%s=%s\n", arg1, env);
		else
			Com_Printf("%s undefined\n", arg1);
        }
}

/*
==================
Com_ExecModCfg

codmod: generic mod config hooks. Executes "<fs_game><suffix>.cfg" (e.g. codmod_defaults.cfg, codmod.cfg)
when a mod is loaded (fs_game set and not the base game). Missing files are skipped silently.
==================
*/
static void Com_ExecModCfg( const char *suffix ) {
	char game[MAX_QPATH];
	char name[MAX_QPATH];
	const char *p;

	Q_strncpyz( game, Cvar_VariableString( "fs_game" ), sizeof( game ) );
	if ( !game[0] || !Q_stricmp( game, BASEGAME ) || strlen( game ) > 32 ) {
		return;
	}
	// plain directory names only, so the name cannot inject commands or leave the mod directory
	for ( p = game; *p; p++ ) {
		if ( !( ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) || ( *p >= '0' && *p <= '9' ) || *p == '_' || *p == '-' ) ) {
			return;
		}
	}

	Com_sprintf( name, sizeof( name ), "%s%s.cfg", game, suffix );
	if ( FS_ReadFile( name, NULL ) > 0 ) {
		Cbuf_ExecuteText( EXEC_NOW, va( "exec %s\n", name ) );
		Cbuf_Execute();
	}
}

/*
==================
Com_ExecModCfg_f

codmod: "execmodcfg" re-runs the mod config hooks (<fs_game>_defaults.cfg, then <fs_game>.cfg). Used by the
menu Reset action after cvar_restart, so a mod's defaults and pinned values come back.
==================
*/
static void Com_ExecModCfg_f( void ) {
	Com_ExecModCfg( "_defaults" );
	Com_ExecModCfg( "" );
}

/*
==================
Com_ExecuteCfg

For controlling environment variables
==================
*/

void Com_ExecuteCfg(void)
{
	Cbuf_ExecuteText(EXEC_NOW, "exec default.cfg\n");
	if ( FS_ReadFile( "language.cfg", NULL ) > 0 ) {
		Cbuf_ExecuteText(EXEC_APPEND, "exec language.cfg\n");
	} else if ( FS_ReadFile( "Language.cfg", NULL ) > 0 ) {
		Cbuf_ExecuteText(EXEC_APPEND, "exec Language.cfg\n");
	}
	Cbuf_Execute(); // Always execute after exec to prevent text buffer overflowing

	// codmod: mod defaults (first launch values and menu Reset), before the user config so saved choices win.
	// Mod data, not user data, so it also runs in safe mode.
	Com_ExecModCfg( "_defaults" );

	if(!Com_SafeMode())
	{
		// skip the wolfconfig.cfg and autoexec.cfg if "safe" is on the command line
		Cbuf_ExecuteText(EXEC_NOW, "exec " Q3CONFIG_CFG "\n");
		Cbuf_Execute();
		Cbuf_ExecuteText(EXEC_NOW, "exec autoexec.cfg\n");
		Cbuf_Execute();
	}

	// codmod: mod pinned/internal config, after the user config so it always applies (also in safe mode)
	Com_ExecModCfg( "" );
}

/*
==================
Com_GameRestart

Change to a new mod properly with cleaning up cvars before switching.
==================
*/

void Com_GameRestart(int checksumFeed, qboolean disconnect)
{
	// make sure no recursion can be triggered
	if(!com_gameRestarting && com_fullyInitialized)
	{
		com_gameRestarting = qtrue;
		com_gameClientRestarting = com_cl_running->integer;

		// Kill server if we have one
		if(com_sv_running->integer)
			SV_Shutdown("Game directory changed");

		if(com_gameClientRestarting)
		{
			if(disconnect)
				CL_Disconnect(qfalse);
				
			CL_Shutdown("Game directory changed", disconnect, qfalse);
		}

		FS_Restart(checksumFeed);
	
		// Clean out any user and VM created cvars
		Cvar_Restart(qtrue);
		Com_ExecuteCfg();

		if(disconnect)
		{
			// We don't want to change any network settings if gamedir
			// change was triggered by a connect to server because the
			// new network settings might make the connection fail.
			NET_Restart_f();
		}

		if(com_gameClientRestarting)
		{
			CL_Init();
			CL_StartHunkUsers(qfalse);
		}
		
		com_gameRestarting = qfalse;
		com_gameClientRestarting = qfalse;
	}
}

/*
==================
Com_GameRestart_f

Expose possibility to change current running mod to the user
==================
*/

void Com_GameRestart_f(void)
{
	Cvar_Set("fs_game", Cmd_Argv(1));

	Com_GameRestart(0, qtrue);
}

#ifndef STANDALONE

qboolean CL_CDKeyValidate( const char *key, const char *checksum );

// TTimo: centralizing the cl_cdkey stuff after I discovered a buffer overflow problem with the dedicated server version
//   not sure it's necessary to have different defaults for regular and dedicated, but I don't want to take the risk
#ifndef DEDICATED
char cl_cdkey[34] = "                                ";
#else
char cl_cdkey[34] = "123456789";
#endif

/*
=================
Com_ReadCDKey
=================
*/
void Com_ReadCDKey( const char *filename ) {
	fileHandle_t f;
	char buffer[33];
	char fbuffer[MAX_OSPATH];

	Com_sprintf(fbuffer, sizeof(fbuffer), "%s/rtcwkey", filename);

	FS_SV_FOpenFileRead( fbuffer, &f );
	if ( !f ) {
		Com_Memset( cl_cdkey, '\0', 17 );
		return;
	}

	Com_Memset( buffer, 0, sizeof( buffer ) );

	FS_Read( buffer, 16, f );
	FS_FCloseFile( f );

	if ( CL_CDKeyValidate( buffer, NULL ) ) {
		Q_strncpyz( cl_cdkey, buffer, 17 );
	} else {
		Com_Memset( cl_cdkey, '\0', 17 );
	}
}

/*
=================
Com_AppendCDKey
=================
*/
void Com_AppendCDKey( const char *filename ) {
	fileHandle_t f;
	char buffer[33];
	char fbuffer[MAX_OSPATH];

	Com_sprintf(fbuffer, sizeof(fbuffer), "%s/rtcwkey", filename);

	FS_SV_FOpenFileRead( fbuffer, &f );
	if ( !f ) {
		Com_Memset( &cl_cdkey[16], '\0', 17 );
		return;
	}

	Com_Memset( buffer, 0, sizeof( buffer ) );

	FS_Read( buffer, 16, f );
	FS_FCloseFile( f );

	if ( CL_CDKeyValidate( buffer, NULL ) ) {
		strcat( &cl_cdkey[16], buffer );
	} else {
		Com_Memset( &cl_cdkey[16], '\0', 17 );
	}
}

#ifndef DEDICATED
/*
=================
Com_WriteCDKey
=================
*/
static void Com_WriteCDKey( const char *filename, const char *ikey ) {
	fileHandle_t f;
	char fbuffer[MAX_OSPATH];
	char key[17];
#ifndef _WIN32
	mode_t			savedumask;
#endif

	Com_sprintf(fbuffer, sizeof(fbuffer), "%s/rtcwkey", filename);


	Q_strncpyz( key, ikey, 17 );

	if ( !CL_CDKeyValidate( key, NULL ) ) {
		return;
	}

#ifndef _WIN32
	savedumask = umask(0077);
#endif
	f = FS_SV_FOpenFileWrite( fbuffer );
	if ( !f ) {
		Com_Printf ("Couldn't write CD key to %s.\n", fbuffer );
		goto out;
	}

	FS_Write( key, 16, f );

	FS_Printf( f, "\n// generated by RTCW, do not modify\r\n" );
	FS_Printf( f, "// Do not give this file to ANYONE.\r\n" );
#ifdef __APPLE__ // TTimo
	FS_Printf( f, "// Aspyr will NOT ask you to send this file to them.\r\n" );
#else
	FS_Printf( f, "// id Software and Activision will NOT ask you to send this file to them.\r\n" );
#endif
	FS_FCloseFile( f );
out:
#ifndef _WIN32
	umask(savedumask);
#else
	;
#endif
}
#endif

#endif // STANDALONE

static void Com_DetectAltivec(void)
{
	// Only detect if user hasn't forcibly disabled it.
	if (com_altivec->integer) {
		static qboolean altivec = qfalse;
		static qboolean detected = qfalse;
		if (!detected) {
			altivec = ( Sys_GetProcessorFeatures( ) & CF_ALTIVEC );
			detected = qtrue;
		}

		if (!altivec) {
			Cvar_Set( "com_altivec", "0" );  // we don't have it! Disable support!
		}
	}
}

void Com_SetRecommended( qboolean vidrestart ) {
	cvar_t *cv;
	qboolean goodVideo;
	qboolean goodCPU;
	qboolean lowMemory;
	// will use this for recommended settings as well.. do i outside the lower check so it gets done even with command line stuff
	cv = Cvar_Get( "r_highQualityVideo", "1", CVAR_ARCHIVE );
	goodVideo = ( cv && cv->integer );
	goodCPU = Sys_GetHighQualityCPU();
	lowMemory = Sys_LowPhysicalMemory();

	if ( goodVideo && goodCPU ) {
		Com_Printf( "Found high quality video and CPU\n" );
		Cbuf_AddText( "exec highVidhighCPU.cfg\n" );
	} else if ( goodVideo && !goodCPU ) {
		Cbuf_AddText( "exec highVidlowCPU.cfg\n" );
		Com_Printf( "Found high quality video and low quality CPU\n" );
	} else if ( !goodVideo && goodCPU ) {
		Cbuf_AddText( "exec lowVidhighCPU.cfg\n" );
		Com_Printf( "Found low quality video and high quality CPU\n" );
	} else {
		Cbuf_AddText( "exec lowVidlowCPU.cfg\n" );
		Com_Printf( "Found low quality video and low quality CPU\n" );
	}

// (SA) set the cvar so the menu will reflect this on first run
	Cvar_Set( "ui_glCustom", "999" );   // 'recommended'


	if ( lowMemory ) {
		Com_Printf( "Found minimum memory requirement\n" );
		Cvar_Set( "s_khz", "11" );
		if ( !goodVideo ) {
			Cvar_Set( "r_lowMemTextureSize", "256" );
			Cvar_Set( "r_lowMemTextureThreshold", "40.0" );
		}
	}
	if ( vidrestart ) {
		Cbuf_AddText( "vid_restart\n" );
	}
}

/*
=================
Com_DetectSSE
Find out whether we have SSE support for Q_ftol function
=================
*/

#if id386 || idx64

static void Com_DetectSSE(void)
{
#if !idx64
	cpuFeatures_t feat;
	
	feat = Sys_GetProcessorFeatures();

	if(feat & CF_SSE)
	{
		if(feat & CF_SSE2)
			Q_SnapVector = qsnapvectorsse;
		else
			Q_SnapVector = qsnapvectorx87;

		Q_ftol = qftolsse;
#endif
		Q_VMftol = qvmftolsse;

		Com_Printf("SSE instruction set enabled\n");
#if !idx64
	}
	else
	{
		Q_ftol = qftolx87;
		Q_VMftol = qvmftolx87;
		Q_SnapVector = qsnapvectorx87;

		Com_Printf("SSE instruction set not available\n");
	}
#endif
}

#else

#define Com_DetectSSE()

#endif

/*
=================
Com_InitRand
Seed the random number generator, if possible with an OS supplied random seed.
=================
*/
static void Com_InitRand(void)
{
	unsigned int seed;

	if(Sys_RandomBytes((byte *) &seed, sizeof(seed)))
		srand(seed);
	else
		srand(time(NULL));
}

/*
=================
Com_Init
=================
*/
static void Com_WarpStats_f( void );
static void Com_PaceStats_f( void );

void Com_Init( char *commandLine ) {
	char    *s;
	int	qport;

	Com_Printf( "%s %s %s\n", Q3_VERSION, PLATFORM_STRING, PRODUCT_DATE );

	if ( setjmp( abortframe ) ) {
		Sys_Error( "Error during initialization" );
	}

	// Clear queues
	Com_Memset( &eventQueue[ 0 ], 0, MAX_QUEUED_EVENTS * sizeof( sysEvent_t ) );

	// initialize the weak pseudo-random number generator for use later.
	Com_InitRand();

	// do this before anything else decides to push events
	Com_InitPushEvent();

	Cvar_Init();

	// prepare enough of the subsystems to handle
	// cvar and command buffer management
	Com_ParseCommandLine( commandLine );

//	Swap_Init();
	Cbuf_Init();

	Com_DetectSSE();

	// override anything from the config files with command line args
	Com_StartupVariable( NULL );

	Com_InitZoneMemory();
	Cmd_Init ();

	// get the developer cvar set as early as possible
	com_developer = Cvar_Get("developer", "0", CVAR_TEMP);

	// done early so bind command exists
	CL_InitKeyCommands();

	com_standalone = Cvar_Get("com_standalone", "0", CVAR_ROM);
	com_basegame = Cvar_Get("com_basegame", BASEGAME, CVAR_INIT);
	com_homepath = Cvar_Get("com_homepath", "", CVAR_INIT|CVAR_PROTECTED);

	FS_InitFilesystem();

	Com_InitJournaling();

	// Add some commands here already so users can use them from config files
	Cmd_AddCommand ("setenv", Com_Setenv_f);
	if (com_developer && com_developer->integer)
	{
		Cmd_AddCommand ("error", Com_Error_f);
		Cmd_AddCommand ("crash", Com_Crash_f);
		Cmd_AddCommand ("freeze", Com_Freeze_f);
 	}
	Cmd_AddCommand ("quit", Com_Quit_f);
	Cmd_AddCommand ("changeVectors", MSG_ReportChangeVectors_f );
	Cmd_AddCommand ("writeconfig", Com_WriteConfig_f );
	Cmd_SetCommandCompletionFunc( "writeconfig", Cmd_CompleteCfgName );
	Cmd_AddCommand("game_restart", Com_GameRestart_f);
	Cmd_AddCommand( "execmodcfg", Com_ExecModCfg_f );

	Com_ExecuteCfg();

	// override anything from the config files with command line args
	Com_StartupVariable( NULL );

	// get dedicated here for proper hunk megs initialization
#ifdef DEDICATED
	com_dedicated = Cvar_Get ("dedicated", "1", CVAR_INIT);
	Cvar_CheckRange( com_dedicated, 1, 2, qtrue );
#else
	com_dedicated = Cvar_Get( "dedicated", "0", CVAR_LATCH );
	Cvar_CheckRange( com_dedicated, 0, 2, qtrue );
#endif
	// allocate the stack based hunk allocator
	Com_InitHunkMemory();

	// if any archived cvars are modified after this, we will trigger a writing
	// of the config file
	cvar_modifiedFlags &= ~CVAR_ARCHIVE;

	//
	// init commands and vars
	//
	com_altivec = Cvar_Get ("com_altivec", "1", CVAR_ARCHIVE);
	com_loadProfile = Cvar_Get( "com_loadProfile", "0", CVAR_TEMP );
	com_maxfps = Cvar_Get( "com_maxfps", "76", CVAR_ARCHIVE );
	com_blood = Cvar_Get( "com_blood", "1", CVAR_ARCHIVE );

	com_logfile = Cvar_Get( "logfile", "0", CVAR_TEMP );

	com_timescale = Cvar_Get( "timescale", "1", CVAR_CHEAT | CVAR_SYSTEMINFO );
	com_fixedtime = Cvar_Get( "fixedtime", "0", CVAR_CHEAT );
	com_showtrace = Cvar_Get( "com_showtrace", "0", CVAR_CHEAT );
	com_speeds = Cvar_Get( "com_speeds", "0", 0 );
	com_timedemo = Cvar_Get( "timedemo", "0", CVAR_CHEAT );
	com_cameraMode = Cvar_Get( "com_cameraMode", "0", CVAR_CHEAT );

	cl_paused = Cvar_Get( "cl_paused", "0", CVAR_ROM );
	sv_paused = Cvar_Get( "sv_paused", "0", CVAR_ROM );
	cl_packetdelay = Cvar_Get ("cl_packetdelay", "0", CVAR_CHEAT);
	sv_packetdelay = Cvar_Get ("sv_packetdelay", "0", CVAR_CHEAT);
	com_sv_running = Cvar_Get( "sv_running", "0", CVAR_ROM );
	com_cl_running = Cvar_Get( "cl_running", "0", CVAR_ROM );
	com_buildScript = Cvar_Get( "com_buildScript", "0", 0 );
	com_ansiColor = Cvar_Get( "com_ansiColor", "0", CVAR_ARCHIVE );

	com_unfocused = Cvar_Get( "com_unfocused", "0", CVAR_ROM );
	com_maxfpsUnfocused = Cvar_Get( "com_maxfpsUnfocused", "0", CVAR_ARCHIVE );
	com_minimized = Cvar_Get( "com_minimized", "0", CVAR_ROM );
	com_maxfpsMinimized = Cvar_Get( "com_maxfpsMinimized", "0", CVAR_ARCHIVE );
	com_abnormalExit = Cvar_Get( "com_abnormalExit", "0", CVAR_ROM );
	com_busyWait = Cvar_Get("com_busyWait", "0", CVAR_ARCHIVE);
	// absolute-deadline frame limiter (float fps, hybrid sleep+spin); 0 = legacy integer-ms limiter
	// FPS cap for menus/cinematics only: -1 monitor refresh, 0 off, else fps
	com_maxfpsMenu = Cvar_Get( "com_maxfpsMenu", "-1", CVAR_ARCHIVE );
	com_maxfpsPrecise = Cvar_Get( "com_maxfpsPrecise", "0", CVAR_ARCHIVE );
	// spin margin in us before each deadline: -1 auto, 0 sleep only
	com_maxfpsSpinUs = Cvar_Get( "com_maxfpsSpinUs", "-1", CVAR_ARCHIVE );
	// where the cap waits: 0 start of frame, 1 right before present, 2 predictive (start late, present on time)
	com_maxfpsMode = Cvar_Get( "com_maxfpsMode", "2", CVAR_ARCHIVE );
	// GPU pacing: 0 none (CPU timing only), 1 glFinish before the present wait / after the swap so frame timing sees
	// the GPU, 2 fence pipeline (never blocks the GPU: timestamps + fences, com_maxfpsGpuQueue frames in flight)
	com_maxfpsGpuSync = Cvar_Get( "com_maxfpsGpuSync", "1", CVAR_ARCHIVE );
	com_maxfpsGpuQueue = Cvar_Get( "com_maxfpsGpuQueue", "1", 0 );
	// owned by the renderer (sdl_glimp.c); registered here too so the pacer can read it
	com_vsync = Cvar_Get( "r_vsync", "0", CVAR_ARCHIVE );
	com_displayRefresh = Cvar_Get( "r_displayRefreshActual", "0", CVAR_TEMP );
	r_frameWarp = Cvar_Get( "r_frameWarp", "0", CVAR_ARCHIVE | CVAR_LATCH );
	com_frameWarpHz = Cvar_Get( "r_frameWarpHz", "0", 0 );
	com_latchSync = Cvar_Get( "r_frameWarpLatchSync", "1", 0 );
	Cmd_AddCommand( "warpstats", Com_WarpStats_f );
	Cmd_AddCommand( "pacestats", Com_PaceStats_f );
	Cvar_Get("com_errorMessage", "", CVAR_ROM | CVAR_NORESTART);

#ifdef CINEMATICS_INTRO
	com_introPlayed = Cvar_Get( "com_introplayed", "0", CVAR_ARCHIVE );
	cod_skipIntro = Cvar_Get( "cod_skipIntro", "0", CVAR_ARCHIVE );
	Cvar_Get( "cl_introAlways", "0", 0 ); // codmod: 1 = intro plays on every start (ignores com_introplayed, and +set/+exec launch args do not suppress it); 0 = vanilla
	Cvar_Get( "cod_subtitles", "0", CVAR_ARCHIVE ); // codmod: Game menu toggle, read by cgame (cg_subtitles.c)
	// codmod: language packs (docs/languages.md), read by ui, cgame and the sound code; reset values in codmod_binds.cfg
	Cvar_Get( "cod_textLang", "en", CVAR_ARCHIVE );
	Cvar_Get( "cod_audioLang", "en", CVAR_ARCHIVE );
	Cvar_Get( "cod_subtitleScale", "1.0", CVAR_ARCHIVE );
#endif
	com_recommendedSet = Cvar_Get( "com_recommendedSet", "0", CVAR_ARCHIVE );

	Cvar_Get( "savegame_loading", "0", CVAR_ROM );

	s = va("%s %s %s", Q3_VERSION, PLATFORM_STRING, PRODUCT_DATE );
	com_version = Cvar_Get ("version", s, CVAR_ROM | CVAR_SERVERINFO );
	com_gamename = Cvar_Get("com_gamename", GAMENAME_FOR_MASTER, CVAR_SERVERINFO | CVAR_INIT);
	com_protocol = Cvar_Get("com_protocol", va("%i", PROTOCOL_VERSION), CVAR_SERVERINFO | CVAR_INIT);
#ifdef LEGACY_PROTOCOL
	com_legacyprotocol = Cvar_Get("com_legacyprotocol", va("%i", PROTOCOL_LEGACY_VERSION), CVAR_INIT);

	// Keep for compatibility with old mods / mods that haven't updated yet.
	if(com_legacyprotocol->integer > 0)
		Cvar_Get("protocol", com_legacyprotocol->string, CVAR_ROM);
	else
#endif
		Cvar_Get("protocol", com_protocol->string, CVAR_ROM);

#ifndef DEDICATED
	con_autochat = Cvar_Get("con_autochat", "1", CVAR_ARCHIVE);
#endif

	com_hunkused = Cvar_Get( "com_hunkused", "0", 0 );

	Sys_Init();

	Sys_InitPIDFile( FS_GetCurrentGameDir() );

	// Pick a random port value
	Com_RandomBytes( (byte*)&qport, sizeof(int) );
	Netchan_Init( qport & 0xffff );

	VM_Init();
	SV_Init();

	com_dedicated->modified = qfalse;

#ifndef DEDICATED
	CL_Init();
#endif

	// set com_frameTime so that if a map is started on the
	// command line it will still be able to count on com_frameTime
	// being random enough for a serverid
	com_frameTime = Com_Milliseconds();

	// add + commands from command line
	// codmod: launchers pass +set/+exec only, which must not suppress the intro; any other command
	// (map, loadgame, cinematic, ...) still does.
	// codmod: cl_introAlways 0 = vanilla (any startup command suppresses the demoloop/intro)
	if ( !Com_AddStartupCommands() || ( Cvar_VariableIntegerValue( "cl_introAlways" ) && !Com_StartupHasGameCommand() ) ) {
		// if the user didn't give any commands, run default action
		if ( !com_dedicated->integer ) {
#ifdef CINEMATICS_LOGO
			//Cbuf_AddText ("cinematic " CINEMATICS_LOGO "\n");
#endif
#ifdef CINEMATICS_INTRO
			// codmod: vanilla plays the intro once (com_introplayed); cl_introAlways 1 plays it on every start
			// unless the Skip Intro setting (cod_skipIntro) is on, ignoring the archived com_introplayed
			if ( Cvar_VariableIntegerValue( "cl_introAlways" ) ? !cod_skipIntro->integer : ( !com_introPlayed->integer && !cod_skipIntro->integer ) ) {
				//Cvar_Set( com_introPlayed->name, "1" );		//----(SA)	force this to get played every time (but leave cvar for override)
				Cbuf_AddText( "cinematic " CINEMATICS_INTRO " 3\n" );
				//Cvar_Set( "nextmap", "cinematic " CINEMATICS_INTRO );
			}
#endif
		}
	}

	// start in full screen ui mode
	Cvar_Set( "r_uiFullScreen", "1" );

	CL_StartHunkUsers( qfalse );

	if ( !com_recommendedSet->integer ) {
		Com_SetRecommended( qtrue );
		Cvar_Set( "com_recommendedSet", "1" );
	}

	com_fullyInitialized = qtrue;

	// always set the cvar, but only print the info if it makes sense.
	Com_DetectAltivec();
#if idppc
	Com_Printf ("Altivec support is %s\n", com_altivec->integer ? "enabled" : "disabled");
#endif

	com_pipefile = Cvar_Get( "com_pipefile", "", CVAR_ARCHIVE|CVAR_LATCH );
	if( com_pipefile->string[0] )
	{
		pipefile = FS_FCreateOpenPipeFile( com_pipefile->string );
	}

	Com_Printf ("--- Common Initialization Complete ---\n");
}

/*
===============
Com_ReadFromPipe

Read whatever is in com_pipefile, if anything, and execute it
===============
*/
void Com_ReadFromPipe( void )
{
	static char buf[MAX_STRING_CHARS];
	static int accu = 0;
	int read;

	if( !pipefile )
		return;

	while( ( read = FS_Read( buf + accu, sizeof( buf ) - accu - 1, pipefile ) ) > 0 )
	{
		char *brk = NULL;
		int i;

		for( i = accu; i < accu + read; ++i )
		{
			if( buf[ i ] == '\0' )
				buf[ i ] = '\n';
			if( buf[ i ] == '\n' || buf[ i ] == '\r' )
				brk = &buf[ i + 1 ];
		}
		buf[ accu + read ] = '\0';

		accu += read;

		if( brk )
		{
			char tmp = *brk;
			*brk = '\0';
			Cbuf_ExecuteText( EXEC_APPEND, buf );
			*brk = tmp;

			accu -= brk - buf;
			memmove( buf, brk, accu + 1 );
		}
		else if( accu >= sizeof( buf ) - 1 ) // full
		{
			Cbuf_ExecuteText( EXEC_APPEND, buf );
			accu = 0;
		}
	}
}

//==================================================================

void Com_WriteConfigToFile( const char *filename ) {
	fileHandle_t f;

	f = FS_FOpenFileWrite( filename );
	if ( !f ) {
		Com_Printf( "Couldn't write %s.\n", filename );
		return;
	}

	FS_Printf( f, "// generated by RTCW, do not modify\n" );
	Key_WriteBindings( f );
	Cvar_WriteVariables( f );
	FS_FCloseFile( f );
}


/*
===============
Com_WriteConfiguration

Writes key bindings and archived cvars to config file if modified
===============
*/
void Com_WriteConfiguration( void ) {
	// if we are quiting without fully initializing, make sure
	// we don't write out anything
	if ( !com_fullyInitialized ) {
		return;
	}

	if ( !( cvar_modifiedFlags & CVAR_ARCHIVE ) ) {
		return;
	}
	cvar_modifiedFlags &= ~CVAR_ARCHIVE;

	Com_WriteConfigToFile( Q3CONFIG_CFG );

	// not needed for dedicated or standalone
#if !defined(DEDICATED) && !defined(STANDALONE)
	if(!com_standalone->integer)
	{
		const char *gamedir;
		gamedir = Cvar_VariableString( "fs_game" );
		if ( UI_usesUniqueCDKey() && gamedir[0] != 0 ) {
			Com_WriteCDKey( gamedir, &cl_cdkey[16] );
		} else {
			Com_WriteCDKey( BASEGAME, cl_cdkey );
		}
	}
#endif
}


/*
===============
Com_WriteConfig_f

Write the config file to a specific name
===============
*/
void Com_WriteConfig_f( void ) {
	char filename[MAX_QPATH];

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "Usage: writeconfig <filename>\n" );
		return;
	}

	Q_strncpyz( filename, Cmd_Argv( 1 ), sizeof( filename ) );
	COM_DefaultExtension( filename, sizeof( filename ), ".cfg" );

	if (!COM_CompareExtension(filename, ".cfg"))
	{
		Com_Printf("Com_WriteConfig_f: Only the \".cfg\" extension is supported by this command!\n");
		return;
	}

	Com_Printf( "Writing %s.\n", filename );
	Com_WriteConfigToFile( filename );
}

/*
================
Com_ModifyMsec
================
*/
int Com_ModifyMsec( int msec ) {
	int clampTime;

	//
	// modify time for debugging values
	//
	if ( com_fixedtime->integer ) {
		msec = com_fixedtime->integer;
	} else if ( com_timescale->value ) {
		msec *= com_timescale->value;
//	} else if (com_cameraMode->integer) {
//		msec *= com_timescale->value;
	}

	// don't let it scale below 1 msec
	if ( msec < 1 && com_timescale->value ) {
		msec = 1;
	}

	if ( com_dedicated->integer ) {
		// dedicated servers don't want to clamp for a much longer
		// period, because it would mess up all the client's views
		// of time.
		if (com_sv_running->integer && msec > 500)
			Com_Printf( "Hitch warning: %i msec frame time\n", msec );

		clampTime = 5000;
	} else
	if ( !com_sv_running->integer ) {
		// clients of remote servers do not want to clamp time, because
		// it would skew their view of the server's time temporarily
		clampTime = 5000;
	} else {
		// for local single player gaming
		// we may want to clamp the time to prevent players from
		// flying off edges when something hitches.
		clampTime = 200;
	}

	if ( msec > clampTime ) {
		msec = clampTime;
	}

	return msec;
}

/*
=================
Com_TimeVal
=================
*/

int Com_TimeVal(int minMsec)
{
	int timeVal;

	timeVal = Sys_Milliseconds() - com_frameTime;

	if(timeVal >= minMsec)
		timeVal = 0;
	else
		timeVal = minMsec - timeVal;

	return timeVal;
}

/*
=================
Frame pacer

Absolute-deadline frame limiter on the monotonic microsecond clock. Each frame
has a present deadline D that advances by exactly one (fractional) period; a
frame that misses by more than half a period re-anchors the schedule instead
of bursting to catch up.

  mode 0  wait for D at the top of the frame (lowest latency, present jitters)
  mode 1  run the frame at once, wait for D right before the swap
  mode 2  predictive: start the frame at D - (recent P95 frame cost), wait for D
          before the swap (flat present cadence and low latency)

r_vsync 4 (low latency vsync) uses mode 2 phase-locked to the observed vblank
instead of a fixed schedule: the swap blocks, we time when to start the frame
so it finishes just before it.
=================
*/
#define PACE_RING 64

typedef struct {
	qboolean  active;           // pacer controls this frame
	qboolean  late;             // previous frame was re-anchored (game time follows the real clock)
	qboolean  vblankLock;       // r_vsync 4 phase-locked scheduling
	int       mode;
	double    periodUs;
	double    deadlineUs;       // present deadline of the current frame
	long long frameStartUs;     // timestamp after the top-of-frame wait
	long long lastTopUs;
	int       presentedFrame;   // com_frameNumber the readiness was recorded for
	int       ring[PACE_RING];
	int       ringPos;
	long long lastVblankUs;
	double    vblankPeriodUs;   // measured swap-to-swap interval (vsync lock)
	int       vblankGood;       // consecutive swap intervals that looked like a real vblank
	qboolean  gpuBound;         // GPU sync suspended: the GPU cannot hold the cap
	int       gpuMissed;        // consecutive synced frames that finished after their deadline
	int       gpuSynced;        // synced frames since the last (re)start of the GPU sync
	int       gpuBoundFrames;   // frames spent with the sync suspended
	int       gpuRetryFrames;   // when to try the sync again (doubles after each failed try)
	qboolean  inWarp;           // a frame-warp frame is being drawn (see Com_WarpPresentWait)
	long long warpDeadlineUs;   // present time of that warp frame
	long long warpStartUs;      // when it started
	qboolean  readyRecorded;    // late-latch already wrote this frame's ring sample at the GPU fence (see Com_WarpLatch)
	long long readyUs;          // time of that fence
	qboolean  fence;            // this frame is paced by the fence pipeline (com_maxfpsGpuSync 2), see Com_PaceFenceOk
	int       recIdx;           // index of this frame's record in paceRecs (-1 none)
	int       leadUs;           // lead used by the top-of-frame wait (fence mode)
	int       markedFrame;      // com_frameNumber + 1 the GPU mark was taken for
} pacer_t;

static pacer_t pacer;

// late-latch (r_frameWarp 1) statistics and the wait lead
#define LATCH_RING 128
typedef struct {
	int       count;                // latches that returned angles
	int       denied;               // latches refused (client denied / re.WarpFrame missing)
	int       waited;               // latches that waited for the present deadline (LL-1)
	int       late;                 // LL-1 latches whose target time had already passed
	long long sampleUs;             // time of the last sample, 0 once its swap-return was recorded
	int       cmdLat[LATCH_RING];   // sample time - cl.lastCmdUs, us
	int       cmdPos;
	int       swapLat[LATCH_RING];  // sample time -> swap return, us
	int       swapPos;
	int       leadLat[LATCH_RING];  // sample time -> GPU finished the warp + 2D tail (Com_PresentWait), us
	int       leadPos;
} latch_t;

static latch_t latch;

static int Com_PaceCmpInt( const void *a, const void *b ) {
	return *(const int *)a - *(const int *)b;
}

// ~P98 (second worst of the last 64 frames) of the recent "frame start -> ready to present" cost, in us
static int Com_PaceLead( void ) {
	int tmp[PACE_RING];

	Com_Memcpy( tmp, pacer.ring, sizeof( tmp ) );
	qsort( tmp, PACE_RING, sizeof( int ), Com_PaceCmpInt );
	return tmp[PACE_RING - 2];
}


/*
=================
GPU fence pipeline (com_maxfpsGpuSync 2)

The renderer registers timestamp/fence ops (Com_PaceGpuRegister). Every paced frame gets a record
(start time on the CPU clock and the GPU clock, deadline); the renderer marks it after the frame's last
command (fence + timestamp). Completed records are harvested without blocking: the GPU completion time
(mapped to the CPU clock) feeds the lead ring and the statistics. The next frame only waits (sleep-poll,
100 ms timeout) while more than com_maxfpsGpuQueue earlier frames are still unfinished.
=================
*/
#define PACE_RECS 16

typedef struct {
	int       seq;              // renderer fence/timestamp id, 0 = not marked
	int       frameTag;         // com_frameNumber + 1 the record belongs to
	long long startUs;
	long long startGpuNs;
	double    deadlineUs;       // 0 = uncapped frame
	double    periodUs;
	qboolean  latched;          // late-latch wrote the ring sample (LL-1)
	qboolean  active;           // pacer controlled this frame (feeds the ring)
} paceRec_t;

static const paceGpu_t *paceOps;
static paceRec_t paceRecs[PACE_RECS];
static int paceRecCount, paceRecDone;   // records created / harvested (monotonic)

#define PSTAT_N 16384
static struct {
	int       s2g[PSTAT_N], s2gN;       // input -> GPU done, non-latched frames, us
	int       giv[PSTAT_N], givN;       // GPU completion intervals, us
	int       siv[PSTAT_N], sivN;       // swap-return intervals, us
	int       bl[PSTAT_N], blN;         // backlog wait, us
	double    capSum, holdSum;
	int       capN, holdN;
	int       frames, framesD, miss, latched, timeouts, nowMax;
	long long lastG, lastSwapUs;
} ps;

void Com_PaceStatsReset( void ) {
	Com_Memset( &ps, 0, sizeof( ps ) );
}

static void Com_PaceStatPush( int *arr, int *n, int v ) {
	arr[(*n)++ % PSTAT_N] = v;
}

void Com_PaceGpuRegister( const paceGpu_t *ops ) {
	paceOps = ops;
	Com_Memset( paceRecs, 0, sizeof( paceRecs ) );
	paceRecCount = paceRecDone = 0;
	pacer.recIdx = -1;
	ps.lastG = 0;
}

static qboolean Com_PaceFenceOk( void ) {
	return com_maxfpsGpuSync->integer == 2 && paceOps && paceOps->mark && paceOps->poll &&
		   paceOps->doneNs && paceOps->nowNs;
}

static int Com_PaceQueue( void ) {
	int q = com_maxfpsGpuQueue ? com_maxfpsGpuQueue->integer : 1;

	return q < 0 ? 0 : q > 3 ? 3 : q;
}

// records that were marked and are not harvested yet
static int Com_PacePending( void ) {
	int i, n = 0;

	for ( i = paceRecDone; i < paceRecCount; i++ ) {
		if ( paceRecs[i % PACE_RECS].seq ) {
			n++;
		}
	}
	return n;
}

// resolve finished records in order, without blocking
static void Com_PaceHarvest( void ) {
	while ( paceRecDone < paceRecCount ) {
		paceRec_t *r = &paceRecs[paceRecDone % PACE_RECS];
		long long gpuNs;

		if ( r->seq == 0 ) {
			paceRecDone++;      // never marked (frame without a present)
			continue;
		}
		if ( paceOps->doneNs( r->seq, &gpuNs ) ) {
			double g = (double)r->startUs + (double)( gpuNs - r->startGpuNs ) / 1000.0;
			long long gUs = (long long)g;

			ps.frames++;
			if ( r->latched ) {
				ps.latched++;
			} else {
				int cost = (int)( gUs - r->startUs );

				Com_PaceStatPush( ps.s2g, &ps.s2gN, cost );
				if ( r->active ) {
					pacer.ring[pacer.ringPos++ % PACE_RING] = cost;
				}
			}
			if ( ps.lastG && gUs > ps.lastG ) {
				Com_PaceStatPush( ps.giv, &ps.givN, (int)( gUs - ps.lastG ) );
			}
			ps.lastG = gUs;
			if ( r->deadlineUs > 0 ) {
				ps.framesD++;
				if ( g > r->deadlineUs + 0.25 * r->periodUs ) {
					ps.miss++;
				}
			}
		} else if ( !paceOps->poll( r->seq ) ) {
			break;
		}
		r->seq = 0;
		paceRecDone++;
	}
}

// create this frame's record (fence mode only)
static void Com_PaceRecBegin( long long startUs, long long startGpuNs, qboolean active ) {
	paceRec_t *r;

	if ( paceRecCount - paceRecDone >= PACE_RECS ) {
		paceRecDone++;
	}
	pacer.recIdx = paceRecCount++;
	r = &paceRecs[pacer.recIdx % PACE_RECS];
	Com_Memset( r, 0, sizeof( *r ) );
	r->frameTag = com_frameNumber + 1;
	r->startUs = startUs;
	r->startGpuNs = startGpuNs;
	r->deadlineUs = active ? pacer.deadlineUs : 0;
	r->periodUs = pacer.periodUs;
	r->active = active;
}

static paceRec_t *Com_PaceRecCur( void ) {
	paceRec_t *r;

	if ( pacer.recIdx < 0 || pacer.recIdx < paceRecDone ) {
		return NULL;
	}
	r = &paceRecs[pacer.recIdx % PACE_RECS];
	return r->frameTag == com_frameNumber + 1 ? r : NULL;
}

static long long Com_PaceNowNs( void ) {
	long long t0 = Sys_Microseconds(), ns = paceOps->nowNs();
	int us = (int)( Sys_Microseconds() - t0 );

	if ( us > ps.nowMax ) {
		ps.nowMax = us;
	}
	return ns;
}

// once per Com_Frame: fence + timestamp after the frame's last command
static void Com_PaceMark( void ) {
	paceRec_t *r = Com_PaceRecCur();

	if ( !r || r->seq || pacer.markedFrame == com_frameNumber + 1 ) {
		return;
	}
	pacer.markedFrame = com_frameNumber + 1;
	r->seq = paceOps->mark();
}

// P98 of "late-latch sample -> GPU finished the warp + 2D tail", us (1500 until enough samples)
static int Com_LatchLeadP98( void ) {
	int n = latch.leadPos < LATCH_RING ? latch.leadPos : LATCH_RING;
	int tmp[LATCH_RING];

	if ( n < 16 ) {
		return 1500;
	}
	Com_Memcpy( tmp, latch.leadLat, sizeof( tmp ) );
	qsort( tmp, n, sizeof( int ), Com_PaceCmpInt );
	return tmp[(int)( ( n - 1 ) * 0.98 + 0.5 )];
}

// copy the last samples of a stats ring into a sorted scratch array; returns the count
static int Com_PaceSorted( const int *arr, int total, int *out ) {
	int n = total < PSTAT_N ? total : PSTAT_N;

	if ( n ) {
		Com_Memcpy( out, arr, n * sizeof( int ) );     // wrapped order does not matter for percentiles
		qsort( out, n, sizeof( int ), Com_PaceCmpInt );
	}
	return n;
}

static void Com_PaceMeanSd( const int *arr, int total, double *mean, double *sd ) {
	int i, n = total < PSTAT_N ? total : PSTAT_N;
	double sum = 0, var = 0;

	*mean = *sd = 0;
	if ( !n ) {
		return;
	}
	for ( i = 0; i < n; i++ ) {
		sum += arr[i];
	}
	*mean = sum / n;
	for ( i = 0; i < n; i++ ) {
		var += ( arr[i] - *mean ) * ( arr[i] - *mean );
	}
	*sd = sqrt( var / n );
}

static int Com_PacePct( const int *arr, int total, double p ) {
	static int tmp[PSTAT_N];
	int n = Com_PaceSorted( arr, total, tmp );

	return n ? tmp[(int)( ( n - 1 ) * p + 0.5 )] : 0;
}

void Com_PaceStatsPrint( void ) {
	double gm, gs, sm, ss;
	double T = pacer.periodUs;

	Com_PaceMeanSd( ps.giv, ps.givN, &gm, &gs );
	Com_PaceMeanSd( ps.siv, ps.sivN, &sm, &ss );
	Com_Printf( "codmod pace: mode=%d gsync=%d q=%d T=%.0f frames=%d s2g_p50=%d s2g_p98=%d latched=%d "
				"giv_mean=%.0f giv_sd=%.0f giv_p99=%d siv_sd=%.0f miss%%=%.2f wait_cap=%.0f "
				"wait_backlog_p98=%d hold=%.0f fence_timeouts=%d nowns_max_us=%d\n",
				com_maxfpsMode->integer, com_maxfpsGpuSync->integer, Com_PaceQueue(), T, ps.frames,
				Com_PacePct( ps.s2g, ps.s2gN, 0.5 ), Com_PacePct( ps.s2g, ps.s2gN, 0.98 ), ps.latched,
				gm, gs, Com_PacePct( ps.giv, ps.givN, 0.99 ), ss,
				ps.framesD ? 100.0 * ps.miss / ps.framesD : 0.0,
				ps.capN ? ps.capSum / ps.capN : 0.0, Com_PacePct( ps.bl, ps.blN, 0.98 ),
				ps.holdN ? ps.holdSum / ps.holdN : 0.0, ps.timeouts, ps.nowMax );
}

static void Com_PaceStats_f( void ) {
	if ( Cmd_Argc() > 1 && !Q_stricmp( Cmd_Argv( 1 ), "reset" ) ) {
		Com_PaceStatsReset();
		Com_Printf( "pacestats reset\n" );
		return;
	}
	Com_PaceStatsPrint();
}

/*
=================
Com_ResolveFps

-1 = monitor refresh rate (60 if the renderer has not reported it yet).
=================
*/
static float Com_ResolveFps( float fps ) {
	if ( fps < 0 ) {
		return com_displayRefresh->value > 1 ? com_displayRefresh->value : 60;
	}
	return fps;
}

/*
=================
Com_EffectiveFps

The in-game cap (com_maxfps), lowered to com_maxfpsMenu while only a menu or
cinematic is showing. Unfocused/minimized caps are handled by the callers.
=================
*/
static float Com_EffectiveFps( void ) {
	float fps = Com_ResolveFps( com_maxfps->value );

	if ( com_maxfpsMenu->value != 0 && CL_InMenuOnly() ) {
		float menuFps = Com_ResolveFps( com_maxfpsMenu->value );

		if ( fps <= 0 || menuFps < fps ) {
			fps = menuFps;
		}
	}
	return fps;
}

/*
=================
Frame warp (async reprojection) pacer

With r_frameWarp the engine renders a real frame every k refresh periods T and
presents k-1 re-projected warp frames (CL_WarpFrame) in the slots between
them, so a game that renders at about half the display rate still presents at
the display rate. Everything runs on the main thread, in sequence: the warp
slots are served from the top-of-frame wait of the next real frame, which
therefore has to fit inside one T (otherwise warp suspends itself).

  scheduled   slot i = previous real deadline + i*T; real deadline = + k*T
  vsync lock  slot i = last observed vblank + (i - last presented slot)*T,
              the swap blocks at the vblank (r_vsync 4 with a good vblank lock)

Game time steps by k*T per real frame (pacer.periodUs).
=================
*/
#define WARP_RING 32

typedef struct {
	qboolean  suspended;
	int       suspFrames;       // frames spent suspended
	int       retryFrames;      // when to try again (doubles after each early failure, 64..2048)
	int       activeFrames;     // active frames since the last (re)start
	int       missed;           // consecutive real frames that missed their deadline
	int       failStreak;       // consecutive failed warp slots
	qboolean  wasActive;        // previous frame was a warp frame
	int       k;
	double    periodUs;         // T
	int       cost[WARP_RING];  // warp frame start -> ready to present, us
	int       costPos;
	int       costN;
	int       presents, skips, fails, suspensions;
	int       skipEarly, skipLate;  // skipped slots: next real frame too close at entry / slot time already past
	int       realFrames;        // real frames run while warp was active
	int       denied;            // CL_WarpFrame refused (CL_WarpDeny)
	int       rejected;          // re.WarpFrame returned qfalse
	const char *lastDeny;
	int       suspByReason[3];   // fit / missed deadlines / failing
	int       suspFrame[4], suspPos;  // com_frameNumber of the last suspensions
	int       waitTopCalls, nearT, lastLeadUs;  // diagnostics: active top-of-frame calls, last Com_PaceLead() + 500
	long long lastPresentUs;    // com_lastPresentUs at the previous interval sample
	int       ivN;
	double    ivMean, ivM2;     // present-to-present interval (Welford), us
	const char *lastSuspend;
} warp_t;

static warp_t warp;

// refresh period T in us (0 = unknown)
static double Com_WarpPeriodUs( void ) {
	double nominal = com_displayRefresh->value > 1 ? 1000000.0 / com_displayRefresh->value : 0;

	if ( com_frameWarpHz->value > 1 ) {
		return 1000000.0 / com_frameWarpHz->value;
	}
	if ( pacer.vblankGood >= 4 && pacer.vblankPeriodUs > 0 ) {
		return pacer.vblankPeriodUs < nominal ? nominal : pacer.vblankPeriodUs;
	}
	return nominal;
}

// lead of a warp frame: P98 of its recent cost + 500 us, clamped to [500, T/2]
static int Com_WarpLeadUs( double T ) {
	int tmp[WARP_RING];
	int n = warp.costN < WARP_RING ? warp.costN : WARP_RING;
	int lead;

	if ( n < 8 ) {
		return (int)( T / 3 );
	}
	Com_Memcpy( tmp, warp.cost, sizeof( tmp ) );
	qsort( tmp, n, sizeof( int ), Com_PaceCmpInt );
	lead = tmp[(int)( ( n - 1 ) * 0.98 + 0.5 )] + 500;
	if ( lead > T / 2 ) {
		lead = (int)( T / 2 );
	}
	if ( lead < 500 ) {
		lead = 500;
	}
	return lead;
}

static void Com_WarpSuspend( const char *why ) {
	if ( warp.activeFrames < 64 && warp.retryFrames ) {
		warp.retryFrames = warp.retryFrames * 2 < 2048 ? warp.retryFrames * 2 : 2048;
	} else {
		warp.retryFrames = 64;
	}
	warp.suspended = qtrue;
	warp.suspFrames = 0;
	warp.activeFrames = 0;
	warp.missed = 0;
	warp.failStreak = 0;
	warp.suspensions++;
	warp.suspFrame[warp.suspPos++ & 3] = com_frameNumber;
	warp.lastSuspend = why;
}

// NULL when warp can run this frame, else why not; fills warp.k / warp.periodUs
static const char *Com_WarpWhyNot( void ) {
	double T, eff;
	int k;

	if ( r_frameWarp->integer != 3 ) {
		return "r_frameWarp is not 3 (fixed-k pacer)";
	}
	if ( !com_maxfpsPrecise->integer ) {
		return "com_maxfpsPrecise 0";
	}
	if ( !com_maxfpsGpuSync->integer ) {
		return "com_maxfpsGpuSync 0";
	}
	if ( com_minimized->integer || com_unfocused->integer ) {
		return "window unfocused/minimized";
	}
	if ( com_dedicated->integer || com_timedemo->integer ) {
		return "dedicated/timedemo";
	}
	if ( !CL_WarpAllowed() ) {
		return "not allowed by client (menu/loading/demo)";
	}
	if ( warp.suspended ) {
		return warp.lastSuspend ? warp.lastSuspend : "suspended";
	}
	T = Com_WarpPeriodUs();
	if ( T <= 0 ) {
		return "refresh rate unknown (set r_frameWarpHz)";
	}
	eff = Com_EffectiveFps();
	k = eff > 0 ? (int)ceil( 1000000.0 / T / eff - 0.05 ) : 2;
	if ( k < 2 ) {
		k = 2;
	}
	if ( k > 16 ) {
		k = 16;
	}
	warp.k = k;
	warp.periodUs = T;
	return NULL;
}

static qboolean Com_WarpActive( void ) {
	if ( r_frameWarp->integer != 3 ) {
		return qfalse;
	}
	if ( warp.wasActive ) {
		warp.missed = pacer.late ? warp.missed + 1 : 0;
		warp.wasActive = qfalse;
	}
	if ( warp.suspended && ++warp.suspFrames >= warp.retryFrames ) {
		warp.suspended = qfalse;
		warp.activeFrames = 0;
	}
	if ( Com_WarpWhyNot() ) {
		return qfalse;
	}
	if ( warp.activeFrames == 0 ) {
		// (re)start: the ring is only fed by paced frames, so when uncapped it still holds the values that
		// caused the last suspension (or the loading hitches), which would re-suspend at once. Start over;
		// two slow frames suspend again.
		Com_Memset( pacer.ring, 0, sizeof( pacer.ring ) );
	}
	if ( warp.missed >= 4 ) {
		warp.suspByReason[1]++;
		Com_WarpSuspend( "suspended: real frames miss their deadline" );
		return qfalse;
	}
	if ( warp.failStreak >= 8 ) {
		warp.suspByReason[2]++;
		Com_WarpSuspend( "suspended: warp frames keep failing" );
		return qfalse;
	}
	// the warp slot (work in [D0 + T - warpLead, D0 + T]) and the next real frame (work in [D1 - lead, D1],
	// D1 = D0 + 2T) only have to be disjoint, which is lead < T; the warp lead itself is not part of it
	if ( pacer.presentedFrame && Com_PaceLead() + 500 > warp.periodUs ) {
		warp.lastLeadUs = Com_PaceLead() + 500;
		warp.suspByReason[0]++;
		Com_WarpSuspend( "suspended: real frame does not fit in one refresh period" );
		return qfalse;
	}
	return qtrue;
}

// present-to-present interval statistics, sampled once per distinct present
static void Com_WarpNotePresent( void ) {
	double d;

	if ( com_lastPresentUs == warp.lastPresentUs ) {
		return;
	}
	if ( warp.lastPresentUs && com_lastPresentUs - warp.lastPresentUs < 4 * warp.periodUs ) {
		double delta;

		d = (double)( com_lastPresentUs - warp.lastPresentUs );
		warp.ivN++;
		delta = d - warp.ivMean;
		warp.ivMean += delta / warp.ivN;
		warp.ivM2 += delta * ( d - warp.ivMean );
	}
	warp.lastPresentUs = com_lastPresentUs;
}

// one warp slot: 1 presented, 0 skipped (too late), -1 failed
static int Com_WarpSlot( long long slotUs ) {
	long long now = Sys_Microseconds();
	int lead = Com_WarpLeadUs( warp.periodUs );
	const char *deny;
	qboolean ok;

	if ( now > slotUs - lead ) {
		warp.skips++;
		warp.skipLate++;
		return 0;
	}
	Sys_WaitUntil( slotUs - lead, com_maxfpsSpinUs->integer );
	pacer.warpDeadlineUs = slotUs;
	pacer.warpStartUs = Sys_Microseconds();
	deny = CL_WarpDeny();
	pacer.inWarp = qtrue;
	ok = deny ? qfalse : CL_WarpFrame();
	pacer.inWarp = qfalse;
	if ( !ok ) {
		if ( deny ) {
			warp.denied++;
			warp.lastDeny = deny;
		} else {
			warp.rejected++;
		}
		warp.fails++;
		warp.failStreak++;
		return -1;
	}
	warp.presents++;
	warp.failStreak = 0;
	Com_WarpNotePresent();
	return 1;
}

static void Com_WarpFrameWaitTop( void ) {
	double T = warp.periodUs;
	int k = warp.k;
	long long now = Sys_Microseconds();
	qboolean lock;
	double prevD = 0, D;
	int i, lastIdx = 0;
	int lead;

	pacer.active = qtrue;
	pacer.mode = 2;                 // real frames always gpu-sync and present on a deadline
	pacer.gpuBound = qfalse;
	pacer.gpuBoundFrames = 0;
	pacer.gpuMissed = 0;
	pacer.late = qfalse;
	warp.wasActive = qtrue;
	warp.activeFrames++;
	warp.realFrames++;
	Com_WarpNotePresent();

	lock = com_vsync->integer == 4 && com_frameWarpHz->value <= 1 && pacer.vblankGood >= 4 &&
		   pacer.lastVblankUs && now - pacer.lastVblankUs < ( k + 2 ) * T;
	pacer.vblankLock = lock;
	pacer.periodUs = k * T;

	if ( lock ) {
		for ( i = 1; i < k; i++ ) {
			if ( Com_WarpSlot( pacer.lastVblankUs + (long long)( ( i - lastIdx ) * T ) ) > 0 ) {
				lastIdx = i;
			}
		}
		D = (double)pacer.lastVblankUs + ( k - lastIdx ) * T;
		lead = pacer.presentedFrame ? Com_PaceLead() : (int)( T * 0.5 );
		if ( lead > T * 0.85 ) {
			lead = (int)( T * 0.85 );
		}
		pacer.deadlineUs = D;
		now = Sys_Microseconds();
		if ( now < D - lead - 300 ) {
			Sys_WaitUntil( (long long)( D - lead - 300 ), com_maxfpsSpinUs->integer );
		} else {
			pacer.late = qtrue;
		}
	} else {
		prevD = pacer.deadlineUs;
		D = prevD + k * T;
		if ( prevD > 0 && (double)com_lastPresentUs > prevD + T * 0.5 ) {
			// the previous real frame presented late: schedule from when it really did
			prevD = (double)com_lastPresentUs;
			D = prevD + k * T;
			pacer.late = qtrue;
		}
		if ( prevD <= 0 || D < now - k * T * 0.5 || D > now + 2 * k * T ) {
			D = (double)now;            // first frame, hitch, or cap changed: no slots
			pacer.late = qtrue;
		} else {
			for ( i = 1; i < k; i++ ) {
				Com_WarpSlot( (long long)( prevD + i * T ) );
			}
		}
		pacer.deadlineUs = D;
		lead = Com_PaceLead() + 500;
		if ( pacer.presentedFrame == 0 ) {
			lead = (int)( pacer.periodUs * 0.5 );
		}
		if ( lead > pacer.periodUs * 0.9 ) {
			lead = (int)( pacer.periodUs * 0.9 );
		}
		Sys_WaitUntil( (long long)D - lead, com_maxfpsSpinUs->integer );
	}
	pacer.frameStartUs = Sys_Microseconds();
	pacer.lastTopUs = pacer.frameStartUs;
}

// the renderer's present wait while a warp frame is drawn (never touches presentedFrame or the main ring)
static void Com_WarpPresentWait( void ( *finish )( void ) ) {
	long long ready;

	if ( finish ) {
		finish();
	}
	ready = Sys_Microseconds();
	warp.cost[warp.costPos++ % WARP_RING] = (int)( ready - pacer.warpStartUs );
	warp.costN++;
	if ( !pacer.vblankLock ) {
		Sys_WaitUntil( pacer.warpDeadlineUs, com_maxfpsSpinUs->integer );
	}
}

// P50 / P98 (ms) of the filled part of a latch sample ring
static void Com_LatchPct( const int *ring, int pos, float *p50, float *p98, int *n ) {
	int tmp[LATCH_RING];
	int cnt = pos < LATCH_RING ? pos : LATCH_RING;

	*n = cnt;
	*p50 = *p98 = 0;
	if ( !cnt ) {
		return;
	}
	Com_Memcpy( tmp, ring, sizeof( tmp ) );
	qsort( tmp, cnt, sizeof( int ), Com_PaceCmpInt );
	*p50 = tmp[cnt / 2] * 0.001f;
	*p98 = tmp[(int)( ( cnt - 1 ) * 0.98 + 0.5 )] * 0.001f;
}

static void Com_WarpLatchStats( void ) {
	float a50, a98, b50, b98, c50, c98;
	int na, nb, nc;

	Com_LatchPct( latch.cmdLat, latch.cmdPos, &a50, &a98, &na );
	Com_LatchPct( latch.swapLat, latch.swapPos, &b50, &b98, &nb );
	Com_LatchPct( latch.leadLat, latch.leadPos, &c50, &c98, &nc );
	Com_Printf( "latch: mode %d sync %d, latches %d, denied %d, waited %d (target already past %d)\n",
				r_frameWarp->integer, com_latchSync->integer, latch.count, latch.denied, latch.waited, latch.late );
	Com_Printf( "latch: cmd->sample P50 %.2f P98 %.2f ms (%d); sample->swap-return P50 %.2f P98 %.2f ms (%d); sample->warp+2D done P50 %.2f P98 %.2f ms (%d)\n",
				a50, a98, na, b50, b98, nb, c50, c98, nc );
}

static void Com_WarpStats_f( void ) {
	int tmp[WARP_RING];
	int n = warp.costN < WARP_RING ? warp.costN : WARP_RING;
	const char *why = Com_WarpWhyNot();
	int p50 = 0, p98 = 0;

	if ( n ) {
		Com_Memcpy( tmp, warp.cost, sizeof( tmp ) );
		qsort( tmp, n, sizeof( int ), Com_PaceCmpInt );
		p50 = tmp[n / 2];
		p98 = tmp[(int)( ( n - 1 ) * 0.98 + 0.5 )];
	}
	Com_Printf( "warp: %s%s%s\n", why ? "inactive: " : "active", why ? why : "",
				warp.suspended && !why ? " (suspended)" : "" );
	Com_Printf( "warp: k %d, T %.1f us (%.1f Hz), real frames %d, presents %d, skips %d, failures %d, suspensions %d\n",
				warp.k, warp.periodUs, warp.periodUs > 0 ? 1000000.0 / warp.periodUs : 0.0,
				warp.realFrames, warp.presents, warp.skips, warp.fails, warp.suspensions );
	Com_Printf( "warp: slots skipped (slot time already past) %d, client denied %d (last: %s), renderer rejected %d; last real-frame lead %d us\n",
				warp.skipLate, warp.denied, warp.lastDeny ? warp.lastDeny : "-", warp.rejected, warp.lastLeadUs );
	Com_Printf( "warp: suspensions: lead>T %d, missed deadlines %d, failing %d; at frames %d %d %d %d (now %d)\n",
				warp.suspByReason[0], warp.suspByReason[1], warp.suspByReason[2],
				warp.suspFrame[0], warp.suspFrame[1], warp.suspFrame[2], warp.suspFrame[3], com_frameNumber );
	{
		int rt[PACE_RING];

		Com_Memcpy( rt, pacer.ring, sizeof( rt ) );
		qsort( rt, PACE_RING, sizeof( int ), Com_PaceCmpInt );
		Com_Printf( "warp: real frame top->ready: min %d, P50 %d, max %d us (last %d); gpuBound %d\n",
					rt[0], rt[PACE_RING / 2], rt[PACE_RING - 1], PACE_RING, pacer.gpuBound );
	}
	Cmd_ExecuteString( "warprstats" );
	Com_WarpLatchStats();
	Com_Printf( "warp: cost P50 %d us, P98 %d us (last %d), lead %d us; present interval mean %.1f us, sd %.1f us (%d samples)\n",
				p50, p98, n, Com_WarpLeadUs( warp.periodUs > 0 ? warp.periodUs : 4000.0 ), warp.ivMean,
				warp.ivN > 1 ? sqrt( warp.ivM2 / ( warp.ivN - 1 ) ) : 0.0, warp.ivN );
}

/*
=================
Com_FrameWaitTopFence

Capped frame in fence mode (com_maxfpsGpuSync 2); the deadline is already advanced. The cap wait is as in
the other modes, then the frame waits (sleep-poll, never a blocking GPU call) until at most
com_maxfpsGpuQueue earlier frames are unfinished on the GPU.
=================
*/
static void Com_FrameWaitTopFence( long long arrivedUs ) {
	double T = pacer.periodUs, planned;
	int lead = 0, q = Com_PaceQueue();
	long long t0, now;
	qboolean ll1;

	pacer.fence = qtrue;
	Com_PaceHarvest();

	ll1 = r_frameWarp->integer == 1 && com_latchSync && com_latchSync->integer != 0 && !pacer.gpuBound;
	if ( pacer.mode == 2 ) {
		lead = Com_PaceLead() + 500;
		if ( pacer.presentedFrame == 0 ) {
			lead = (int)( T * 0.5 );
		}
		if ( ll1 ) {
			if ( lead > T * 0.9 ) {
				lead = (int)( T * 0.9 );
			}
		} else if ( lead > ( q + 1 ) * T ) {
			lead = (int)( ( q + 1 ) * T );
		}
	}
	pacer.leadUs = lead;

	planned = pacer.mode == 0 ? pacer.deadlineUs : pacer.mode == 2 ? pacer.deadlineUs - lead : (double)arrivedUs;
	t0 = Sys_Microseconds();
	if ( pacer.mode == 0 ) {
		Sys_WaitUntil( (long long)pacer.deadlineUs, com_maxfpsSpinUs->integer );
	} else if ( pacer.mode == 2 ) {
		Sys_WaitUntil( (long long)pacer.deadlineUs - lead, com_maxfpsSpinUs->integer );
	}
	now = Sys_Microseconds();
	ps.capSum += (double)( now - t0 );
	ps.capN++;

	// backlog wait
	t0 = now;
	Com_PaceHarvest();
	while ( Com_PacePending() > q ) {
		if ( now - t0 > 100000 ) {
			ps.timeouts++;
			paceRecDone = paceRecCount;
			break;
		}
		Sys_WaitUntil( now + 100, 0 );
		now = Sys_Microseconds();
		Com_PaceHarvest();
	}
	now = Sys_Microseconds();
	Com_PaceStatPush( ps.bl, &ps.blN, (int)( now - t0 ) );

	if ( now > planned + 0.25 * T ) {
		pacer.deadlineUs = pacer.mode == 2 ? (double)now + lead : (double)now;
		pacer.late = qtrue;
	}
	pacer.frameStartUs = now;
	Com_PaceRecBegin( now, Com_PaceNowNs(), qtrue );
}

static void Com_FrameWaitTop( void ) {
	float fps;
	long long now;
	int vsync = com_vsync->integer;
	qboolean llEligible, fence;
	int lead;
	double target;

	pacer.active = qfalse;
	pacer.vblankLock = qfalse;
	pacer.readyRecorded = qfalse;
	pacer.fence = qfalse;
	pacer.recIdx = -1;

	if ( com_minimized->integer && com_maxfpsMinimized->value > 0 ) {
		fps = com_maxfpsMinimized->value;
	} else if ( com_unfocused->integer && com_maxfpsUnfocused->value > 0 ) {
		fps = com_maxfpsUnfocused->value;
	} else {
		fps = Com_EffectiveFps();
	}

	now = Sys_Microseconds();
	pacer.mode = com_maxfpsMode->integer;
	if ( pacer.mode < 0 || pacer.mode > 2 ) {
		pacer.mode = 2;
	}

	if ( Com_WarpActive() ) { Com_WarpFrameWaitTop(); return; }

	// low latency vsync: lock to the display unless the user capped below it
	llEligible = vsync == 4 && com_displayRefresh->value > 1 &&
				 ( fps <= 0 || fps >= com_displayRefresh->value - 0.5f );
	if ( llEligible && pacer.vblankGood < 4 ) {
		// the swap is not (yet) behaving like a vblank-locked flip: pace at the refresh rate instead
		fps = com_displayRefresh->value;
	} else if ( llEligible ) {
		pacer.vblankLock = qtrue;
		pacer.periodUs = pacer.vblankPeriodUs > 0 ? pacer.vblankPeriodUs : 1000000.0 / com_displayRefresh->value;
		// a swap that does not block at the vblank (compositor/driver quirks) must not schedule faster than the display
		if ( pacer.periodUs < 1000000.0 / com_displayRefresh->value ) {
			pacer.periodUs = 1000000.0 / com_displayRefresh->value;
		}
		pacer.active = qtrue;
		pacer.late = qfalse;
		lead = pacer.presentedFrame ? Com_PaceLead() : (int)( pacer.periodUs * 0.5 );
		if ( lead > pacer.periodUs * 0.85 ) {
			lead = (int)( pacer.periodUs * 0.85 );
		}
		if ( pacer.lastVblankUs && now - pacer.lastVblankUs < 6 * pacer.periodUs ) {
			// phase-locked: schedule from the last observed vblank, never the wall clock
			target = (double)pacer.lastVblankUs + pacer.periodUs - lead - 300;
			if ( now < target ) {
				Sys_WaitUntil( (long long)target, com_maxfpsSpinUs->integer );
			} else {
				pacer.late = qtrue;
			}
		}
		pacer.deadlineUs = (double)pacer.lastVblankUs + pacer.periodUs;
		pacer.frameStartUs = Sys_Microseconds();
		pacer.lastTopUs = pacer.frameStartUs;
		return;
	}

	fence = Com_PaceFenceOk();
	if ( !fence ) {
		paceRecDone = paceRecCount;     // mode switched away: drop what is in flight
	}

	if ( fps <= 0 ) {
		pacer.deadlineUs = 0;       // uncapped; re-anchor when a cap comes back
		pacer.lastTopUs = now;
		if ( fence ) {
			// stats only: no waits
			pacer.fence = qtrue;
			Com_PaceHarvest();
			pacer.frameStartUs = Sys_Microseconds();
			Com_PaceRecBegin( pacer.frameStartUs, Com_PaceNowNs(), qfalse );
		}
		return;
	}

	pacer.active = qtrue;
	pacer.periodUs = 1000000.0 / fps;
	pacer.deadlineUs += pacer.periodUs;
	pacer.late = qfalse;
	if ( pacer.deadlineUs < now - pacer.periodUs * 0.5 || pacer.deadlineUs > now + 2 * pacer.periodUs ) {
		pacer.deadlineUs = (double)now;     // first frame, hitch, or cap changed
		pacer.late = qtrue;
	}

	if ( fence ) {
		Com_FrameWaitTopFence( now );
		return;
	}

	if ( pacer.mode == 0 ) {
		Sys_WaitUntil( (long long)pacer.deadlineUs, com_maxfpsSpinUs->integer );
	} else if ( pacer.mode == 2 ) {
		lead = Com_PaceLead() + 500;
		if ( pacer.presentedFrame == 0 ) {
			lead = (int)( pacer.periodUs * 0.5 );
		}
		if ( lead > pacer.periodUs * 0.9 ) {
			lead = (int)( pacer.periodUs * 0.9 );
		}
		Sys_WaitUntil( (long long)pacer.deadlineUs - lead, com_maxfpsSpinUs->integer );
	}
	pacer.frameStartUs = Sys_Microseconds();
}

/*
=================
Com_PresentWait

Called by the renderer right before it swaps buffers (finish = glFinish).
=================
*/
void Com_PresentWait( void ( *finish )( void ) ) {
	long long ready, t0;
	qboolean sync, fenceMode;
	double holdTo;

	if ( pacer.inWarp ) { Com_WarpPresentWait( finish ); return; }

	if ( pacer.fence ) {
		Com_PaceMark();     // before the early returns: also uncapped frames get a GPU timestamp
	}
	fenceMode = pacer.fence && pacer.active && !pacer.vblankLock;

	if ( !pacer.active || pacer.presentedFrame == com_frameNumber + 1 ) {
		return;     // uncapped, or an extra EndFrame within the same Com_Frame (loading screens)
	}
	pacer.presentedFrame = com_frameNumber + 1;

	sync = finish && com_maxfpsGpuSync->integer && ( pacer.mode > 0 || pacer.vblankLock );
	if ( fenceMode && !pacer.readyRecorded ) {
		sync = qfalse;      // no glFinish: the GPU is observed through fences (Com_PaceHarvest)
	}
	if ( fenceMode && pacer.gpuBound ) {
		// LL-1 gave up (the GPU cannot hold the cap); retry only when it would fit
		if ( ++pacer.gpuBoundFrames >= pacer.gpuRetryFrames &&
			 Com_PaceLead() + Com_LatchLeadP98() + 500 < pacer.periodUs * 0.9 ) {
			pacer.gpuBound = qfalse;
			pacer.gpuMissed = 0;
			pacer.gpuSynced = 0;
		}
	}

	// glFinish here serializes the CPU and the GPU: fine while the frame fits the cap (it
	// lowers latency), but when the GPU alone cannot hold the cap it idles the GPU during the
	// next frame's CPU work (240 cap: 211 FPS vs 239 without the sync, on a GPU that does
	// ~250 uncapped). After a few frames that miss their deadline the sync is suspended so the
	// GPU stays busy, and retried later, with a doubling interval while it keeps failing.
	if ( sync && !pacer.vblankLock && pacer.gpuBound ) {
		sync = qfalse;
		if ( ++pacer.gpuBoundFrames >= pacer.gpuRetryFrames ) {
			pacer.gpuBound = qfalse;
			pacer.gpuMissed = 0;
			pacer.gpuSynced = 0;
		}
	}
	if ( pacer.readyRecorded ) {
		// late-latch: the ring sample and the fence time were taken when the post-process fence signalled.
		// The finish below only measures how long the warp + 2D tail need after the sample (the latch lead).
		pacer.readyRecorded = qfalse;
		if ( Com_PaceRecCur() ) {
			Com_PaceRecCur()->latched = qtrue;
		}
		if ( finish ) {
			finish();
		}
		if ( latch.sampleUs ) {
			latch.leadLat[latch.leadPos++ % LATCH_RING] = (int)( Sys_Microseconds() - latch.sampleUs );
		}
		ready = pacer.readyUs;
		sync = qtrue;
	} else {
		if ( sync ) {
			finish();
		}
		ready = Sys_Microseconds();
		if ( !fenceMode && ( sync || !pacer.gpuBound ) ) {
			pacer.ring[pacer.ringPos++ % PACE_RING] = (int)( ready - pacer.frameStartUs );
		}
	}

	if ( sync && !pacer.vblankLock ) {
		pacer.gpuSynced++;
		pacer.gpuMissed = ready > pacer.deadlineUs ? pacer.gpuMissed + 1 : 0;
		if ( pacer.gpuMissed >= 4 ) {
			// a retry that fails right away backs off; one that held for a while starts over
			if ( pacer.gpuSynced < 64 && pacer.gpuRetryFrames ) {
				pacer.gpuRetryFrames = pacer.gpuRetryFrames * 2 < 2048 ? pacer.gpuRetryFrames * 2 : 2048;
			} else {
				pacer.gpuRetryFrames = 64;
			}
			pacer.gpuBound = qtrue;
			pacer.gpuBoundFrames = 0;
		}
	}

	if ( pacer.vblankLock || pacer.mode == 0 ) {
		return;
	}
	if ( ready > pacer.deadlineUs + pacer.periodUs * 0.5 ) {
		pacer.deadlineUs = (double)ready;       // missed: re-anchor, game time follows the real clock
		pacer.late = qtrue;
		if ( Com_PaceRecCur() ) {
			Com_PaceRecCur()->deadlineUs = pacer.deadlineUs;
		}
		return;
	}
	holdTo = pacer.deadlineUs;
	if ( fenceMode && pacer.mode == 2 && pacer.deadlineUs + pacer.periodUs - pacer.leadUs < holdTo ) {
		holdTo = pacer.deadlineUs + pacer.periodUs - pacer.leadUs;
	}
	t0 = Sys_Microseconds();
	Sys_WaitUntil( (long long)holdTo, com_maxfpsSpinUs->integer );
	ps.holdSum += (double)( Sys_Microseconds() - t0 );
	ps.holdN++;
}

/*
=================
Load profiler (com_loadProfile 1 = summary, 2 = also list every mark)

Fixed static storage only: the hunk is cleared mid-load. A gap is the time
between two consecutive presents, attributed to the last mark set before the
ending present (the most recently completed stage; the stage that blocked is
the one after it, see the mark list at level 2).
=================
*/
#define LP_MAXGAPS  8192
#define LP_MAXMARKS 192
#define LP_NAMELEN  40
#define LP_HINTLEN  36

static struct {
	qboolean active;
	char why[32];
	long long beginUs, lastUs;
	int cur;                    // index of last mark, -1 if none
	int numMarks, numGaps;
	long long markUs[LP_MAXMARKS];
	char markName[LP_MAXMARKS][LP_NAMELEN];
	int gapUs[LP_MAXGAPS];
	int gapMark[LP_MAXGAPS];
	int gapAtMs[LP_MAXGAPS];
	char gapHint[LP_MAXGAPS][LP_HINTLEN];
	char hint[LP_HINTLEN];      // asset most recently announced by Com_LoadProfileHint
} lp;

void Com_LoadProfileBegin( const char *why ) {
	if ( !com_loadProfile || !com_loadProfile->integer ) {
		return;
	}
	Com_Memset( &lp, 0, sizeof( lp ) );
	Q_strncpyz( lp.why, why, sizeof( lp.why ) );
	lp.active = qtrue;
	lp.beginUs = lp.lastUs = Sys_Microseconds();
	lp.cur = -1;
}

static int lpFrameBudget;

void Com_LoadProfileMark( const char *stage ) {
	if ( Com_OffloadIsWorker() ) {
		return;     // the profiler state belongs to the main thread
	}
	if ( !com_loadProfile || !com_loadProfile->integer || !lp.active || lp.numMarks >= LP_MAXMARKS ) {
		return;
	}
	lp.markUs[lp.numMarks] = Sys_Microseconds();
	Q_strncpyz( lp.markName[lp.numMarks], stage, LP_NAMELEN );
	lp.cur = lp.numMarks++;
	if ( !strcmp( stage, "sv:end" ) ) {
		lpFrameBudget = 100;     // Com_LoadProfileFrameMark: the first Com_Frame passes after the server spawn
	}
}

/*
Marks inside Com_Frame / the gamestate path, only for the first frames after sv:end (a fixed budget, so the
slots are not flooded). Shows what runs between the last server frame and the client's gamestate parse.
*/
void Com_LoadProfileFrameMark( const char *stage ) {
	if ( lpFrameBudget <= 0 ) {
		return;
	}
	if ( !Com_OffloadIsWorker() && com_loadProfile && com_loadProfile->integer && lp.active && lp.numMarks < LP_MAXMARKS ) {
		// only stretches of 4 ms or more are worth a slot (the pacer waits and idle passes would flood them)
		if ( lp.numMarks > 0 && Sys_Microseconds() - lp.markUs[lp.numMarks - 1] < 4000 ) {
			return;
		}
		lpFrameBudget--;
		Com_LoadProfileMark( stage );
	}
}

/*
Cheap: copies the asset name into a fixed buffer, only while profiling. Not a mark (no slot used).
A gap line carries the latest hint set before the ending present, i.e. the asset whose load blocked.
*/
void Com_LoadProfileHint( const char *what ) {
	if ( !lp.active || !what || Com_OffloadIsWorker() ) {
		return;
	}
	Q_strncpyz( lp.hint, what, LP_HINTLEN );
}

static void Com_LoadProfilePresent( long long now ) {
	if ( lp.numGaps < LP_MAXGAPS ) {
		lp.gapUs[lp.numGaps] = (int)( now - lp.lastUs );
		lp.gapMark[lp.numGaps] = lp.cur;
		lp.gapAtMs[lp.numGaps] = (int)( ( now - lp.beginUs ) / 1000 );
		Q_strncpyz( lp.gapHint[lp.numGaps], lp.hint, LP_HINTLEN );
		lp.numGaps++;
	}
	lp.lastUs = now;
}

static int Com_LoadProfileCmp( const void *a, const void *b ) {
	return *(const int *)a - *(const int *)b;
}

void Com_LoadProfileEnd( void ) {
	static int sorted[LP_MAXGAPS];
	int i, max = 0, maxI = -1, over = 0, p95 = 0;

	if ( !com_loadProfile || !lp.active ) {
		return;
	}
	lp.active = qfalse;
	for ( i = 0; i < lp.numGaps; i++ ) {
		sorted[i] = lp.gapUs[i];
		if ( lp.gapUs[i] > max ) {
			max = lp.gapUs[i];
			maxI = i;
		}
		if ( lp.gapUs[i] > 33000 ) {
			over++;
		}
	}
	if ( lp.numGaps ) {
		qsort( sorted, lp.numGaps, sizeof( int ), Com_LoadProfileCmp );
		p95 = sorted[( lp.numGaps * 95 ) / 100 < lp.numGaps ? ( lp.numGaps * 95 ) / 100 : lp.numGaps - 1];
	}
	Com_Printf( "LOADPROFILE %s total=%dms frames=%d maxgap=%dms@%s p95=%dms over33=%d\n", lp.why,
		(int)( ( Sys_Microseconds() - lp.beginUs ) / 1000 ), lp.numGaps, max / 1000,
		maxI >= 0 && lp.gapMark[maxI] >= 0 ? lp.markName[lp.gapMark[maxI]] : "-", p95 / 1000, over );
	for ( i = 0; i < lp.numGaps; i++ ) {
		if ( lp.gapUs[i] > 33000 ) {
			Com_Printf( "LOADPROFILE_GAP %dms @%s t=%d hint=%s\n", lp.gapUs[i] / 1000,
				lp.gapMark[i] >= 0 ? lp.markName[lp.gapMark[i]] : "-", lp.gapAtMs[i],
				lp.gapHint[i][0] ? lp.gapHint[i] : "-" );
		}
	}
	if ( com_loadProfile->integer >= 2 ) {
		for ( i = 0; i < lp.numMarks; i++ ) {
			Com_Printf( "LOADPROFILE_MARK %d %s\n", (int)( ( lp.markUs[i] - lp.beginUs ) / 1000 ), lp.markName[i] );
		}
	}
}

/*
=================
Com_PresentDone

Called by the renderer right after the buffer swap. With vsync lock the swap
returns at the vblank, which is what the next frame is scheduled from.
=================
*/
void Com_PresentDone( void ( *finish )( void ) ) {
	long long now;

	com_lastPresentUs = Sys_Microseconds();
	if ( ps.lastSwapUs && com_lastPresentUs - ps.lastSwapUs < 500000 ) {
		Com_PaceStatPush( ps.siv, &ps.sivN, (int)( com_lastPresentUs - ps.lastSwapUs ) );
	}
	ps.lastSwapUs = com_lastPresentUs;
	if ( latch.sampleUs ) {
		latch.swapLat[latch.swapPos++ % LATCH_RING] = (int)( com_lastPresentUs - latch.sampleUs );
		latch.sampleUs = 0;
	}
	if ( lp.active ) {
		Com_LoadProfilePresent( com_lastPresentUs );
	}
	if ( com_vsync->integer != 4 || com_displayRefresh->value <= 1 ) {
		pacer.vblankGood = 0;
		return;
	}
	if ( finish ) {
		finish();   // queue depth 1: swap returned means the flip happened
	}
	now = Sys_Microseconds();
	if ( pacer.lastVblankUs ) {
		double d = (double)( now - pacer.lastVblankUs );
		double nominal = 1000000.0 / com_displayRefresh->value;

		if ( d > nominal * 0.88 && d < nominal * 1.12 ) {
			pacer.vblankPeriodUs = pacer.vblankPeriodUs > 0 ? pacer.vblankPeriodUs * 0.95 + d * 0.05 : d;
			if ( pacer.vblankGood < 8 ) {
				pacer.vblankGood++;
			}
		} else {
			pacer.vblankGood = 0;
		}
	}
	pacer.lastVblankUs = now;
}

/*
=================
Com_PacedMsec

Game-time step for paced frames: the scheduled period, not the wake-up time.
The sub-millisecond remainder goes to cgame (com_frameFracUs) so visuals can
interpolate between integer-ms game ticks. A late frame, or more than 3 ms of
drift from the real clock, follows the real clock instead.
=================
*/
static int Com_PacedMsec( int eventMsec ) {
	static double gameClockUs;
	static long long originUs, prevStartUs;
	static int lastMs;
	static qboolean synced;
	double real, step;
	int msec;

	if ( !pacer.active || com_dedicated->integer || com_timedemo->integer ) {
		synced = qfalse;
		com_frameFracUs = 0;
		com_frameClockUs = Sys_Microseconds();
		return eventMsec;
	}
	if ( !synced ) {
		synced = qtrue;
		originUs = pacer.frameStartUs;
		prevStartUs = pacer.frameStartUs;
		gameClockUs = 0;
		lastMs = 0;
		com_frameFracUs = 0;
		com_frameClockUs = pacer.frameStartUs;
		return eventMsec;
	}

	real = (double)( pacer.frameStartUs - originUs );
	step = pacer.late ? (double)( pacer.frameStartUs - prevStartUs ) : pacer.periodUs;
	prevStartUs = pacer.frameStartUs;
	gameClockUs += step;
	if ( fabs( gameClockUs - real ) > 3000.0 ) {
		gameClockUs = real;
	}
	msec = (int)( gameClockUs / 1000.0 ) - lastMs;
	if ( msec < 0 ) {
		msec = 0;
	}
	lastMs += msec;
	com_frameFracUs = (int)( gameClockUs - lastMs * 1000.0 );
	if ( com_frameFracUs < 0 ) {
		com_frameFracUs = 0;
	}
	com_frameClockUs = originUs + (long long)gameClockUs;
	return msec;
}

/*
=================
Com_Frame
=================
*/
void Com_Frame( void ) {

	int msec, minMsec;
	int		timeVal, timeValSV;
	static int	lastTime = 0, bias = 0;

	int timeBeforeFirstEvents;
	int timeBeforeServer;
	int timeBeforeEvents;
	int timeBeforeClient;
	int timeAfter;





	if ( setjmp( abortframe ) ) {
		return;         // an ERR_DROP was thrown
	}

	timeBeforeFirstEvents = 0;
	timeBeforeServer = 0;
	timeBeforeEvents = 0;
	timeBeforeClient = 0;
	timeAfter = 0;


	// write config file if anything changed
	Com_WriteConfiguration();
	Com_LoadProfileFrameMark( "f:writecfg" );

	//
	// main event loop
	//
	if ( com_speeds->integer ) {
		timeBeforeFirstEvents = Sys_Milliseconds();
	}

	// Figure out how much time we have
	if(!com_timedemo->integer)
	{
		if(com_dedicated->integer)
			minMsec = SV_FrameMsec();
		else
		{
			if(com_minimized->integer && com_maxfpsMinimized->integer > 0)
				minMsec = 1000 / com_maxfpsMinimized->integer;
			else if(com_unfocused->integer && com_maxfpsUnfocused->integer > 0)
				minMsec = 1000 / com_maxfpsUnfocused->integer;
			else if(Com_EffectiveFps() >= 1)
				minMsec = 1000 / (int)Com_EffectiveFps();
			else
				minMsec = 1;
			
			timeVal = com_frameTime - lastTime;
			bias += timeVal - minMsec;
			
			if(bias > minMsec)
				bias = minMsec;
			
			// Adjust minMsec if previous frame took too long to render so
			// that framerate is stable at the requested value.
			minMsec -= bias;

			if ( com_maxfpsPrecise->integer ) {
				Com_FrameWaitTop();
				minMsec = 0;    // legacy loop below runs once (network poll only)
			} else {
				pacer.active = qfalse;
				pacer.fence = qfalse;
			}
		}
	}
	else
		minMsec = 1;

	do
	{
		if(com_sv_running->integer)
		{
			timeValSV = SV_SendQueuedPackets();
			
			timeVal = Com_TimeVal(minMsec);

			if(timeValSV < timeVal)
				timeVal = timeValSV;
		}
		else
			timeVal = Com_TimeVal(minMsec);
		
		if(com_busyWait->integer || timeVal < 1)
			NET_Sleep(0);
		else
			NET_Sleep(timeVal - 1);
	} while(Com_TimeVal(minMsec));

	CL_LoadingReplayTick();     // idle frames of the local connect handshake keep the loading screen alive (no-op outside a load)
	Com_LoadProfileFrameMark( "f:wait" );
	IN_Frame();
	
	lastTime = com_frameTime;
	com_frameTime = Com_EventLoop();
	
	msec = com_frameTime - lastTime;
	msec = Com_PacedMsec( msec );
	Com_LoadProfileFrameMark( "f:events" );

	Cbuf_Execute();
	Com_LoadProfileFrameMark( "f:cbuf" );

	if (com_altivec->modified)
	{
		Com_DetectAltivec();
		com_altivec->modified = qfalse;
	}

	// mess with msec if needed
	msec = Com_ModifyMsec(msec);

	//
	// server side
	//
	if ( com_speeds->integer ) {
		timeBeforeServer = Sys_Milliseconds();
	}

	SV_Frame( msec );
	Com_LoadProfileFrameMark( "f:sv" );

	// if "dedicated" has been modified, start up
	// or shut down the client system.
	// Do this after the server may have started,
	// but before the client tries to auto-connect
	if ( com_dedicated->modified ) {
		// get the latched value
		Cvar_Get( "dedicated", "0", 0 );
		com_dedicated->modified = qfalse;
		if ( !com_dedicated->integer ) {
			SV_Shutdown( "dedicated set to 0" );
			CL_FlushMemory();
		}
	}

#ifndef DEDICATED
	//
	// client system
	//

	//
	// run event loop a second time to get server to client packets
	// without a frame of latency
	//
	if ( com_speeds->integer ) {
		timeBeforeEvents = Sys_Milliseconds ();
	}
	Com_EventLoop();
	Com_LoadProfileFrameMark( "f:events2" );
	Cbuf_Execute ();
	Com_LoadProfileFrameMark( "f:cbuf2" );

	//
	// client side
	//
	if ( com_speeds->integer ) {
		timeBeforeClient = Sys_Milliseconds ();
	}

	CL_Frame( msec );
	Com_LoadProfileFrameMark( "f:cl" );

	if ( com_speeds->integer ) {
		timeAfter = Sys_Milliseconds ();
	}
#else
	if ( com_speeds->integer ) {
		timeAfter = Sys_Milliseconds ();
		timeBeforeEvents = timeAfter;
		timeBeforeClient = timeAfter;
	}
#endif

	NET_FlushPacketQueue();

	//
	// report timing information
	//
	if ( com_speeds->integer ) {
		int all, sv, ev, cl;

		all = timeAfter - timeBeforeServer;
		sv = timeBeforeEvents - timeBeforeServer;
		ev = timeBeforeServer - timeBeforeFirstEvents + timeBeforeClient - timeBeforeEvents;
		cl = timeAfter - timeBeforeClient;
		sv -= time_game;
		cl -= time_frontend + time_backend;

		Com_Printf( "frame:%i all:%3i sv:%3i ev:%3i cl:%3i gm:%3i rf:%3i bk:%3i\n",
					com_frameNumber, all, sv, ev, cl, time_game, time_frontend, time_backend );
	}

	//
	// trace optimization tracking
	//
	if ( com_showtrace->integer ) {

		extern int c_traces, c_brush_traces, c_patch_traces;
		extern int c_pointcontents;

		Com_Printf( "%4i traces  (%ib %ip) %4i points\n", c_traces,
					c_brush_traces, c_patch_traces, c_pointcontents );
		c_traces = 0;
		c_brush_traces = 0;
		c_patch_traces = 0;
		c_pointcontents = 0;
	}

	Com_ReadFromPipe( );

	com_frameNumber++;
}

/*
=================
Com_Shutdown
=================
*/
void Com_Shutdown( void ) {

	// write config file if anything changed
	Com_WriteConfiguration();

	if ( logfile ) {
		FS_FCloseFile( logfile );
		logfile = 0;
	}

	if ( com_journalFile ) {
		FS_FCloseFile( com_journalFile );
		com_journalFile = 0;
	}

	if( pipefile ) {
		FS_FCloseFile( pipefile );
		FS_HomeRemove( com_pipefile->string );
	}

}



/*
===========================================
command line completion
===========================================
*/

/*
==================
Field_Clear
==================
*/
void Field_Clear( field_t *edit ) {
	memset( edit->buffer, 0, MAX_EDIT_LINE );
	edit->cursor = 0;
	edit->scroll = 0;
}

static const char *completionString;
static char shortestMatch[MAX_TOKEN_CHARS];
static int matchCount;
// field we are working on, passed to Field_AutoComplete(&g_consoleCommand for instance)
static field_t *completionField;

/*
===============
FindMatches

===============
*/
static void FindMatches( const char *s ) {
	int i;

	if ( Q_stricmpn( s, completionString, strlen( completionString ) ) ) {
		return;
	}
	matchCount++;
	if ( matchCount == 1 ) {
		Q_strncpyz( shortestMatch, s, sizeof( shortestMatch ) );
		return;
	}

	// cut shortestMatch to the amount common with s
	for ( i = 0 ; shortestMatch[i] ; i++ ) {
		if ( i >= strlen( s ) ) {
			shortestMatch[i] = 0;
			break;
		}

		if ( tolower( shortestMatch[i] ) != tolower( s[i] ) ) {
			shortestMatch[i] = 0;
		}
	}
}

/*
===============
PrintMatches

===============
*/
static void PrintMatches( const char *s ) {
	if ( !Q_stricmpn( s, shortestMatch, strlen( shortestMatch ) ) ) {
		Com_Printf( "    %s\n", s );
	}
}

/*
===============
PrintCvarMatches

===============
*/
static void PrintCvarMatches( const char *s ) {
	char value[ TRUNCATE_LENGTH ];

	if ( !Q_stricmpn( s, shortestMatch, strlen( shortestMatch ) ) ) {
		Com_TruncateLongString( value, Cvar_VariableString( s ) );
		Com_Printf( "    %s = \"%s\"\n", s, value );
	}
}

/*
===============
Field_FindFirstSeparator
===============
*/
static char *Field_FindFirstSeparator( char *s )
{
	int i;

	for( i = 0; i < strlen( s ); i++ )
	{
		if( s[ i ] == ';' )
			return &s[ i ];
	}

	return NULL;
}

/*
===============
Field_Complete
===============
*/
static qboolean Field_Complete( void )
{
	int completionOffset;

	if( matchCount == 0 )
		return qtrue;

	completionOffset = strlen( completionField->buffer ) - strlen( completionString );

	Q_strncpyz( &completionField->buffer[ completionOffset ], shortestMatch,
		sizeof( completionField->buffer ) - completionOffset );

	completionField->cursor = strlen( completionField->buffer );

	if( matchCount == 1 )
	{
		Q_strcat( completionField->buffer, sizeof( completionField->buffer ), " " );
		completionField->cursor++;
		return qtrue;
	}

	Com_Printf( "]%s\n", completionField->buffer );

	return qfalse;
}

#ifndef DEDICATED
/*
===============
Field_CompleteKeyname
===============
*/
void Field_CompleteKeyname( void )
{
	matchCount = 0;
	shortestMatch[ 0 ] = 0;

	Key_KeynameCompletion( FindMatches );

	if( !Field_Complete( ) )
		Key_KeynameCompletion( PrintMatches );
}
#endif

/*
===============
Field_CompleteFilename
===============
*/
void Field_CompleteFilename( const char *dir,
		const char *ext, qboolean stripExt, qboolean allowNonPureFilesOnDisk )
{
	matchCount = 0;
	shortestMatch[ 0 ] = 0;

	FS_FilenameCompletion( dir, ext, stripExt, FindMatches, allowNonPureFilesOnDisk );

	if( !Field_Complete( ) )
		FS_FilenameCompletion( dir, ext, stripExt, PrintMatches, allowNonPureFilesOnDisk );
}

/*
===============
Field_CompleteCommand
===============
*/
void Field_CompleteCommand( char *cmd,
		qboolean doCommands, qboolean doCvars )
{
	int		completionArgument = 0;

	// Skip leading whitespace and quotes
	cmd = Com_SkipCharset( cmd, " \"" );

	Cmd_TokenizeStringIgnoreQuotes( cmd );
	completionArgument = Cmd_Argc( );

	// If there is trailing whitespace on the cmd
	if( *( cmd + strlen( cmd ) - 1 ) == ' ' )
	{
		completionString = "";
		completionArgument++;
	}
	else
		completionString = Cmd_Argv( completionArgument - 1 );

#ifndef DEDICATED
	// add a '\' to the start of the buffer if it might be sent as chat otherwise
	if( con_autochat->integer && completionField->buffer[ 0 ] &&
			completionField->buffer[ 0 ] != '\\' )
	{
		if( completionField->buffer[ 0 ] != '/' )
		{
			// Buffer is full, refuse to complete
			if( strlen( completionField->buffer ) + 1 >=
				sizeof( completionField->buffer ) )
				return;

			memmove( &completionField->buffer[ 1 ],
				&completionField->buffer[ 0 ],
				strlen( completionField->buffer ) + 1 );
			completionField->cursor++;
		}

		completionField->buffer[ 0 ] = '\\';
	}
#endif

	if( completionArgument > 1 )
	{
		const char *baseCmd = Cmd_Argv( 0 );
		char *p;

#ifndef DEDICATED
		// This should always be true
		if( baseCmd[ 0 ] == '\\' || baseCmd[ 0 ] == '/' )
			baseCmd++;
#endif

		if( ( p = Field_FindFirstSeparator( cmd ) ) )
			Field_CompleteCommand( p + 1, qtrue, qtrue ); // Compound command
		else
			Cmd_CompleteArgument( baseCmd, cmd, completionArgument ); 
	}
	else
	{
		if( completionString[0] == '\\' || completionString[0] == '/' )
			completionString++;

		matchCount = 0;
		shortestMatch[ 0 ] = 0;

		if( strlen( completionString ) == 0 )
			return;

		if( doCommands )
			Cmd_CommandCompletion( FindMatches );

		if( doCvars )
			Cvar_CommandCompletion( FindMatches );

		if( !Field_Complete( ) )
		{
			// run through again, printing matches
			if( doCommands )
				Cmd_CommandCompletion( PrintMatches );

			if( doCvars )
				Cvar_CommandCompletion( PrintCvarMatches );
		}
	}
}

/*
===============
Field_AutoComplete

Perform Tab expansion
===============
*/
void Field_AutoComplete( field_t *field )
{
	completionField = field;

	Field_CompleteCommand( completionField->buffer, qtrue, qtrue );
}

/*
==================
Com_RandomBytes

fills string array with len random bytes, peferably from the OS randomizer
==================
*/
void Com_RandomBytes( byte *string, int len )
{
	int i;

	if( Sys_RandomBytes( string, len ) )
		return;

	Com_Printf( "Com_RandomBytes: using weak randomization\n" );
	for( i = 0; i < len; i++ )
		string[i] = (unsigned char)( rand() % 256 );
}


/*
==================
Com_IsVoipTarget

Returns non-zero if given clientNum is enabled in voipTargets, zero otherwise.
If clientNum is negative return if any bit is set.
==================
*/
qboolean Com_IsVoipTarget(uint8_t *voipTargets, int voipTargetsSize, int clientNum)
{
	int index;
	if(clientNum < 0)
	{
		for(index = 0; index < voipTargetsSize; index++)
		{
			if(voipTargets[index])
				return qtrue;
		}
		
		return qfalse;
	}

	index = clientNum >> 3;
	
	if(index < voipTargetsSize)
		return (voipTargets[index] & (1 << (clientNum & 0x07)));

	return qfalse;
}


/*
=================
Com_WarpLatch

Late-latch (r_frameWarp 1), called by the renderer right after post-process of a real frame:
returns the freshest absolute view angles (degrees) to warp the finished frame to.
LL-0 (uncapped, or r_frameWarpLatchSync 0): sample at once. LL-1 (paced): wait for the GPU fence
of the post-process (gpuWait), record the pacer ring sample there (not after the deliberate wait,
or Com_PaceLead would grow every frame), then wait until presentDeadline - latchLead and sample.
=================
*/
qboolean Com_WarpLatch( void ( *gpuWait )( void ), float *pitch, float *yaw ) {
	long long now, target;
	qboolean paced;
	int sync = com_latchSync ? com_latchSync->integer : 0;

	if ( r_frameWarp->integer != 1 ) {
		return qfalse;
	}
	if ( CL_WarpDeny() ) {
		latch.denied++;
		return qfalse;
	}

	paced = pacer.active && ( pacer.mode > 0 || pacer.vblankLock ) && pacer.deadlineUs > 0 &&
			pacer.presentedFrame != com_frameNumber + 1 && !pacer.gpuBound && gpuWait;
	if ( sync != 0 && paced ) {     // LL-1; an unpaced frame has no deadline to wait for, even with sync 2
		int n = latch.leadPos < LATCH_RING ? latch.leadPos : LATCH_RING;
		int lead = 1500;

		gpuWait();
		now = Sys_Microseconds();
		pacer.ring[pacer.ringPos++ % PACE_RING] = (int)( now - pacer.frameStartUs );
		pacer.readyUs = now;
		pacer.readyRecorded = qtrue;

		if ( n >= 16 ) {
			int tmp[LATCH_RING];

			Com_Memcpy( tmp, latch.leadLat, sizeof( tmp ) );
			qsort( tmp, n, sizeof( int ), Com_PaceCmpInt );
			lead = tmp[(int)( ( n - 1 ) * 0.98 + 0.5 )] + 300;
		}
		if ( lead > pacer.periodUs * 0.9 ) {
			lead = (int)( pacer.periodUs * 0.9 );
		}
		target = (long long)pacer.deadlineUs - lead;
		latch.waited++;
		if ( now >= target ) {
			latch.late++;
		} else {
			Sys_WaitUntil( target, com_maxfpsSpinUs->integer );
		}
	}

	latch.sampleUs = Sys_Microseconds();
	if ( !CL_WarpLatchView( pitch, yaw ) ) {
		latch.denied++;
		latch.sampleUs = 0;
		return qfalse;
	}
	latch.count++;
	latch.cmdLat[latch.cmdPos++ % LATCH_RING] = (int)( latch.sampleUs - CL_WarpLastCmdUs() );
	return qtrue;
}
