// codmod: subtitles for voice lines.
//
// text/subtitles.txt (optional, from a private pack built by tools/subtitles) maps a sound path to its text:
//   sound/scenaric/cs1/cs01_dir_02.wav 2.7 "Well, Jack, have we heard anything?"
// Lines are sorted by lowercase path. Sound events call CG_SubtitleSound(); CG_DrawSubtitles() draws the active
// lines above the bottom edge. cod_subtitles (archived, game menu) turns it off.

#include "cg_local.h"

#define SUB_MAX_ENTRIES     4096
#define SUB_POOL_SIZE       ( 384 * 1024 )
#define SUB_MAX_ACTIVE      3
#define SUB_LINE_CHARS      64
#define SUB_MAX_DISTANCE    1200.0f
#define SUB_FADE_MS         300

typedef struct {
	const char *path;
	const char *text;
	int durationMs;
} subEntry_t;

typedef struct {
	const char *text;
	int endTime;
} subActive_t;

static char subPool[SUB_POOL_SIZE];
static subEntry_t subTable[SUB_MAX_ENTRIES];
static int subCount;
static subActive_t subActive[SUB_MAX_ACTIVE];
static int subNumActive;

static vmCvar_t cod_subtitles;

void CG_SubtitlesInit( void ) {
	fileHandle_t f;
	int len, poolUsed = 0;
	char *buf, *line, *next;

	subCount = subNumActive = 0;
	trap_Cvar_Register( &cod_subtitles, "cod_subtitles", "1", CVAR_ARCHIVE );

	len = trap_FS_FOpenFile( "text/subtitles.txt", &f, FS_READ );
	if ( len <= 0 || len >= SUB_POOL_SIZE ) {
		if ( len > 0 ) {
			CG_Printf( S_COLOR_YELLOW "subtitles.txt too big (%d bytes)\n", len );
			trap_FS_FCloseFile( f );
		}
		return;
	}
	buf = subPool;
	trap_FS_Read( buf, len, f );
	trap_FS_FCloseFile( f );
	buf[len] = 0;
	poolUsed = len + 1;
	( void )poolUsed;

	// parse in place: each line is `path duration "text"`
	for ( line = buf; line && *line && subCount < SUB_MAX_ENTRIES; line = next ) {
		char *sp, *q;

		next = strchr( line, '\n' );
		if ( next ) {
			*next++ = 0;
		}
		if ( ( q = strchr( line, '\r' ) ) ) {
			*q = 0;
		}
		sp = strchr( line, ' ' );
		if ( !sp ) {
			continue;
		}
		*sp++ = 0;
		q = strchr( sp, '"' );
		if ( !q ) {
			continue;
		}
		subTable[subCount].durationMs = (int)( atof( sp ) * 1000.0f );
		subTable[subCount].path = line;
		subTable[subCount].text = q + 1;
		len = strlen( q + 1 );
		if ( len > 0 && q[len] == '"' ) {
			q[len] = 0;
		}
		subCount++;
	}
	CG_Printf( "Subtitles: %d lines\n", subCount );
}

static const subEntry_t *CG_SubtitleFind( const char *path ) {
	char key[MAX_QPATH];
	int lo = 0, hi = subCount - 1, i;

	for ( i = 0; path[i] && i < MAX_QPATH - 1; i++ ) {
		key[i] = path[i] == '\\' ? '/' : tolower( (unsigned char)path[i] );
	}
	key[i] = 0;

	while ( lo <= hi ) {
		int mid = ( lo + hi ) / 2;
		int c = strcmp( key, subTable[mid].path );
		if ( !c ) {
			return &subTable[mid];
		}
		if ( c < 0 ) {
			hi = mid - 1;
		} else {
			lo = mid + 1;
		}
	}
	return NULL;
}

// entnum < 0: play from the listener (no distance cull)
void CG_SubtitleSound( const char *path, int entnum ) {
	const subEntry_t *e;
	int i, ms;

	if ( !subCount || !cod_subtitles.integer || !path ) {
		return;
	}
	e = CG_SubtitleFind( path );
	if ( !e ) {
		return;
	}

	if ( entnum >= 0 && entnum < MAX_GENTITIES && !cg.cameraMode && cg.snap && entnum != cg.snap->ps.clientNum ) {
		if ( Distance( cg_entities[entnum].lerpOrigin, cg.refdef.vieworg ) > SUB_MAX_DISTANCE ) {
			return;
		}
	}

	for ( i = 0; i < subNumActive; i++ ) {      // same line already up
		if ( subActive[i].text == e->text ) {
			return;
		}
	}
	if ( subNumActive == SUB_MAX_ACTIVE ) {     // drop the oldest
		memmove( &subActive[0], &subActive[1], sizeof( subActive[0] ) * ( SUB_MAX_ACTIVE - 1 ) );
		subNumActive--;
	}
	ms = e->durationMs + 400;
	if ( ms < 1800 ) {
		ms = 1800;
	}
	subActive[subNumActive].text = e->text;
	subActive[subNumActive].endTime = cg.time + ms;
	subNumActive++;
}

// word-wrap `text` into lines of at most SUB_LINE_CHARS; returns the line count
static int CG_SubtitleWrap( const char *text, char lines[][SUB_LINE_CHARS + 1], int maxLines ) {
	int n = 0;

	while ( *text && n < maxLines ) {
		int len = strlen( text ), cut;

		while ( *text == ' ' ) {
			text++;
		}
		len = strlen( text );
		if ( len <= SUB_LINE_CHARS ) {
			cut = len;
		} else {
			for ( cut = SUB_LINE_CHARS; cut > 0 && text[cut] != ' '; cut-- ) {}
			if ( !cut ) {
				cut = SUB_LINE_CHARS;
			}
		}
		memcpy( lines[n], text, cut );
		lines[n][cut] = 0;
		n++;
		text += cut;
	}
	return n;
}

void CG_DrawSubtitles( void ) {
	char lines[SUB_MAX_ACTIVE * 3][SUB_LINE_CHARS + 1];
	vec4_t white = { 1, 1, 1, 1 }, box = { 0, 0, 0, 0.55f };
	int i, j, total = 0, kept = 0, y, cw = 8, ch = 12, lh = 14;
	int owner[SUB_MAX_ACTIVE * 3];

	if ( !subNumActive ) {
		return;
	}
	for ( i = 0; i < subNumActive; i++ ) {      // expire
		if ( cg.time < subActive[i].endTime ) {
			subActive[kept++] = subActive[i];
		}
	}
	subNumActive = kept;
	if ( !cod_subtitles.integer ) {
		subNumActive = 0;
		return;
	}

	for ( i = 0; i < subNumActive && total < SUB_MAX_ACTIVE * 3; i++ ) {
		int n = CG_SubtitleWrap( subActive[i].text, &lines[total], SUB_MAX_ACTIVE * 3 - total );
		for ( j = 0; j < n; j++ ) {
			owner[total + j] = i;
		}
		total += n;
	}
	if ( !total ) {
		return;
	}

	if ( cg_fixedAspect.integer ) {
		CG_SetScreenPlacement( PLACE_CENTER, PLACE_CENTER );
	}
	y = SCREEN_HEIGHT - 52 - total * lh;
	for ( i = 0; i < total; i++, y += lh ) {
		int w = CG_DrawStrlen( lines[i] ) * cw;
		int left = subActive[owner[i]].endTime - cg.time;
		float a = left < SUB_FADE_MS ? (float)left / SUB_FADE_MS : 1.0f;
		float x = ( SCREEN_WIDTH - w ) / 2;

		box[3] = 0.55f * a;
		CG_FillRect( x - 4, y - 1, w + 8, lh, box );
		white[3] = a;
		CG_DrawStringExt( x, y, lines[i], white, qfalse, qtrue, cw, ch, 0 );
	}
}
