// codmod: subtitles for voice lines.
//
// text/subtitles.txt (optional, from a private pack built by tools/subtitles) maps a sound path to its text:
//   sound/scenaric/cs1/cs01_dir_02.wav 2.7 "Well, Jack, have we heard anything?"
// With cod_textLang != en, lang/<code>/subtitles.txt (same format, UTF-8, any order) is used first and the English
// table fills in the paths it lacks. Sound events call CG_SubtitleSound(); CG_DrawSubtitles() draws the active
// lines above the bottom edge. cod_subtitles (archived, game menu) turns it off, cod_subtitleScale (0.5..2.5)
// scales text, spacing and wrap width.

#include "cg_local.h"
#include "../ui/ui_lang.h"

#define SUB_MAX_ENTRIES     4096
#define SUB_POOL_SIZE       ( 384 * 1024 )
#define SUB_LANG_POOL_SIZE  ( 512 * 1024 )
#define SUB_MAX_ACTIVE      3
#define SUB_LINE_CHARS      64          // wrap width at scale 1 (512 virtual pixels)
#define SUB_MAX_LINE_CHARS  160         // at the smallest scale
#define SUB_MAX_LINES       40
#define SUB_LINES_PER_SUB   12
#define SUB_MAX_DISTANCE    1200.0f
#define SUB_FADE_MS         300
#define SUB_MARGIN_TOP      40
#define SUB_MARGIN_BOTTOM   52

typedef struct {
	const char *path;
	const char *text;
	int durationMs;
} subEntry_t;

typedef struct {
	const char *text;
	int endTime;
} subActive_t;

typedef struct {
	subEntry_t *entries;
	int count;
} subSet_t;

static char subPool[SUB_POOL_SIZE];
static char subLangPool[SUB_LANG_POOL_SIZE];
static subEntry_t subTable[SUB_MAX_ENTRIES];
static subEntry_t subLangTable[SUB_MAX_ENTRIES];
static subSet_t subEnglish = { subTable, 0 };
static subSet_t subLang = { subLangTable, 0 };
static subActive_t subActive[SUB_MAX_ACTIVE];
static int subNumActive;

static vmCvar_t cod_subtitles;
static vmCvar_t cod_subtitleScale;

static int CG_SubtitleCompare( const void *a, const void *b ) {
	return strcmp( ( (const subEntry_t *)a )->path, ( (const subEntry_t *)b )->path );
}

// Reads `file` into `pool` and parses it in place: each line is `path duration "text"`. Paths are lowercased, the text
// is converted from UTF-8 to Latin-1, and the entries are sorted by path (the file may be unsorted).
static void CG_SubtitleLoadFile( const char *file, char *pool, int poolSize, subSet_t *set ) {
	fileHandle_t f;
	int len;
	char *buf, *line, *next;

	set->count = 0;
	len = trap_FS_FOpenFile( file, &f, FS_READ );
	if ( len <= 0 || len >= poolSize ) {
		if ( len > 0 ) {
			CG_Printf( S_COLOR_YELLOW "%s too big (%d bytes)\n", file, len );
			trap_FS_FCloseFile( f );
		} else if ( f ) {
			trap_FS_FCloseFile( f );
		}
		return;
	}
	buf = pool;
	trap_FS_Read( buf, len, f );
	trap_FS_FCloseFile( f );
	buf[len] = 0;
	if ( (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF ) {
		buf += 3;
	}

	for ( line = buf; line && *line && set->count < SUB_MAX_ENTRIES; line = next ) {
		char *sp, *q, *c;

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
		for ( c = line; *c; c++ ) {
			*c = *c == '\\' ? '/' : tolower( (unsigned char)*c );
		}
		set->entries[set->count].durationMs = (int)( atof( sp ) * 1000.0f );
		set->entries[set->count].path = line;
		set->entries[set->count].text = q + 1;
		len = strlen( q + 1 );
		if ( len > 0 && q[len] == '"' ) {
			q[len] = 0;
		}
		Lang_Utf8ToLatin1( q + 1 );
		set->count++;
	}
	qsort( set->entries, set->count, sizeof( set->entries[0] ), CG_SubtitleCompare );
}

static void CG_SubtitlesLoadLang( void ) {
	subLang.count = 0;
	subNumActive = 0;
	if ( strcmp( Lang_Code(), "en" ) ) {
		CG_SubtitleLoadFile( va( "lang/%s/subtitles.txt", Lang_Code() ), subLangPool, sizeof( subLangPool ), &subLang );
	}
	CG_Printf( "Subtitles: %d lines (%s: %d)\n", subEnglish.count, Lang_Code(), subLang.count );
}

void CG_SubtitlesInit( void ) {
	subNumActive = 0;
	trap_Cvar_Register( &cod_subtitles, "cod_subtitles", "1", CVAR_ARCHIVE );
	trap_Cvar_Register( &cod_subtitleScale, "cod_subtitleScale", "1.0", CVAR_ARCHIVE );

	CG_SubtitleLoadFile( "text/subtitles.txt", subPool, sizeof( subPool ), &subEnglish );
	CG_SubtitlesLoadLang();
}

static const subEntry_t *CG_SubtitleFindIn( const subSet_t *set, const char *key ) {
	int lo = 0, hi = set->count - 1;

	while ( lo <= hi ) {
		int mid = ( lo + hi ) / 2;
		int c = strcmp( key, set->entries[mid].path );
		if ( !c ) {
			return &set->entries[mid];
		}
		if ( c < 0 ) {
			hi = mid - 1;
		} else {
			lo = mid + 1;
		}
	}
	return NULL;
}

static const subEntry_t *CG_SubtitleFind( const char *path ) {
	char key[MAX_QPATH];
	const subEntry_t *e;
	int i;

	for ( i = 0; path[i] && i < MAX_QPATH - 1; i++ ) {
		key[i] = path[i] == '\\' ? '/' : tolower( (unsigned char)path[i] );
	}
	key[i] = 0;

	e = CG_SubtitleFindIn( &subLang, key );
	return e ? e : CG_SubtitleFindIn( &subEnglish, key );
}

// entnum < 0: play from the listener (no distance cull)
void CG_SubtitleSound( const char *path, int entnum ) {
	const subEntry_t *e;
	int i, ms;

	if ( ( !subEnglish.count && !subLang.count ) || !cod_subtitles.integer || !path ) {
		return;
	}
	trap_Cvar_Update( &cod_subtitles );
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

// word-wrap `text` into lines of at most maxChars; returns the line count
static int CG_SubtitleWrap( const char *text, char lines[][SUB_MAX_LINE_CHARS + 1], int maxLines, int maxChars ) {
	int n = 0;

	while ( *text && n < maxLines ) {
		int len, cut;

		while ( *text == ' ' ) {
			text++;
		}
		len = strlen( text );
		if ( len <= maxChars ) {
			cut = len;
		} else {
			for ( cut = maxChars; cut > 0 && text[cut] != ' '; cut-- ) {}
			if ( !cut ) {
				cut = maxChars;
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
	char lines[SUB_MAX_LINES][SUB_MAX_LINE_CHARS + 1];
	int owner[SUB_MAX_LINES];
	vec4_t white = { 1, 1, 1, 1 }, box = { 0, 0, 0, 0.55f };
	int i, j, total = 0, kept = 0, y, cw, ch, lh, maxChars, maxLines;
	float scale;

	trap_Cvar_Update( &cod_subtitles );
	trap_Cvar_Update( &cod_subtitleScale );
	if ( Lang_Frame() ) {                       // cod_textLang changed: reload
		CG_SubtitlesLoadLang();
	}
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

	scale = cod_subtitleScale.value;
	if ( scale < 0.5f ) {
		scale = 0.5f;
	} else if ( scale > 2.5f ) {
		scale = 2.5f;
	}
	cw = (int)( 8 * scale + 0.5f );
	ch = (int)( 12 * scale + 0.5f );
	lh = (int)( 14 * scale + 0.5f );
	maxChars = (int)( ( SUB_LINE_CHARS * 8 ) / (float)cw );
	if ( maxChars > SUB_MAX_LINE_CHARS ) {
		maxChars = SUB_MAX_LINE_CHARS;
	}
	if ( maxChars * cw > SCREEN_WIDTH - 24 ) {
		maxChars = ( SCREEN_WIDTH - 24 ) / cw;
	}
	maxLines = ( SCREEN_HEIGHT - SUB_MARGIN_TOP - SUB_MARGIN_BOTTOM ) / lh;
	if ( maxLines > SUB_MAX_LINES ) {
		maxLines = SUB_MAX_LINES;
	}

	for ( i = 0; i < subNumActive && total < maxLines; i++ ) {
		int n = CG_SubtitleWrap( subActive[i].text, &lines[total], maxLines - total, maxChars );
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
	y = SCREEN_HEIGHT - SUB_MARGIN_BOTTOM - total * lh;
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
