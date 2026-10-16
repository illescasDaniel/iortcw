// codmod: multilanguage support (string table, UTF-8 -> Latin-1, font glyph fallback), shared by ui and cgame.
//
// lang/<cod_textLang>/strings.txt maps the exact English string a draw function receives to its translation:
//   "Subtitles:" "Untertitel:"
// The file is parsed in place into one static buffer and indexed by a hash table, so lookups never allocate.

#include "ui_lang.h"

#define LANG_BUF_SIZE       ( 512 * 1024 )
#define LANG_MAX_STRINGS    4096
#define LANG_HASH_SIZE      8192            // power of two
#define LANG_MAX_KEYLEN     1000
#define LANG_CODE_LEN       16
#define LANG_FIX_SIZE       2048

// the Latin-1 loader only sees these from the module's own syscall layer (same signatures in ui and cgame)
void    trap_Cvar_Register( vmCvar_t *vmCvar, const char *varName, const char *defaultValue, int flags );
void    trap_Cvar_Update( vmCvar_t *vmCvar );
int     trap_FS_FOpenFile( const char *qpath, fileHandle_t *f, fsMode_t mode );
void    trap_FS_Read( void *buffer, int len, fileHandle_t f );
void    trap_FS_FCloseFile( fileHandle_t f );

typedef struct {
	const char *key;
	const char *val;
	int next;
} langEntry_t;

static char langBuf[LANG_BUF_SIZE];
static langEntry_t langEntries[LANG_MAX_STRINGS];
static int langHash[LANG_HASH_SIZE];
static int langNumEntries;

static vmCvar_t langTextCvar;
static char langCode[LANG_CODE_LEN] = "en";
static int langGeneration = 1;
static int langLastModCount = -1;
static qboolean langRegistered;

/*
=================
Lang_Utf8ToLatin1
=================
*/
void Lang_Utf8ToLatin1( char *s ) {
	const unsigned char *r = (const unsigned char *)s;
	unsigned char *w = (unsigned char *)s;

	while ( *r ) {
		unsigned int c = *r;
		int extra = 0;
		unsigned int cp = c;

		if ( c >= 0xC2 && c <= 0xDF ) {
			extra = 1; cp = c & 0x1F;
		} else if ( c >= 0xE0 && c <= 0xEF ) {
			extra = 2; cp = c & 0x0F;
		} else if ( c >= 0xF0 && c <= 0xF4 ) {
			extra = 3; cp = c & 0x07;
		}
		if ( extra ) {
			int i;
			for ( i = 1; i <= extra; i++ ) {
				if ( ( r[i] & 0xC0 ) != 0x80 ) {
					break;
				}
				cp = ( cp << 6 ) | ( r[i] & 0x3F );
			}
			if ( i <= extra ) {     // not valid UTF-8: keep the byte (already Latin-1)
				*w++ = *r++;
				continue;
			}
			r += extra + 1;
			if ( cp <= 0xFF ) {
				*w++ = (unsigned char)cp;
			} else if ( cp == 0x2018 || cp == 0x2019 || cp == 0x201A ) {
				*w++ = '\'';
			} else if ( cp == 0x201C || cp == 0x201D || cp == 0x201E ) {
				*w++ = '"';
			} else if ( cp == 0x2013 || cp == 0x2014 || cp == 0x2212 ) {
				*w++ = '-';
			} else if ( cp == 0x2026 ) {
				*w++ = '.'; *w++ = '.'; *w++ = '.';     // same 3 bytes as the UTF-8 sequence
			} else if ( cp == 0x20AC ) {
				*w++ = 'E'; *w++ = 'U'; *w++ = 'R';
			} else if ( cp == 0x0152 ) {
				*w++ = 'O'; *w++ = 'E';
			} else if ( cp == 0x0153 ) {
				*w++ = 'o'; *w++ = 'e';
			} else {
				*w++ = '?';
			}
			continue;
		}
		*w++ = *r++;
	}
	*w = 0;
}

/*
=================
String table
=================
*/
static unsigned int Lang_Hash( const char *s ) {
	unsigned int h = 2166136261u;

	while ( *s ) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h & ( LANG_HASH_SIZE - 1 );
}

// p points at an opening quote. Decodes in place (the output is never longer than the input) and returns the string.
static char *Lang_ParseQuoted( char **pp ) {
	char *r = *pp + 1;
	char *start = r;
	char *w = r;

	while ( *r && *r != '"' && *r != '\n' ) {
		if ( *r == '\\' && r[1] ) {
			r++;
			switch ( *r ) {
			case 'n': *w++ = '\n'; break;
			case 't': *w++ = '\t'; break;
			case '"': *w++ = '"'; break;
			case '\\': *w++ = '\\'; break;
			default: *w++ = '\\'; *w++ = *r; break;
			}
			r++;
		} else {
			*w++ = *r++;
		}
	}
	if ( *r == '"' ) {
		r++;
		*w = 0;                 // may overwrite the closing quote: already consumed
	} else {
		*w = 0;
		if ( *r ) {
			r++;
		}
	}
	*pp = r;
	Lang_Utf8ToLatin1( start );
	return start;
}

static void Lang_ClearTable( void ) {
	int i;

	langNumEntries = 0;
	for ( i = 0; i < LANG_HASH_SIZE; i++ ) {
		langHash[i] = -1;
	}
}

static void Lang_LoadTable( void ) {
	char path[MAX_QPATH];
	fileHandle_t f;
	int len;
	char *p;

	Lang_ClearTable();
	if ( !strcmp( langCode, "en" ) ) {
		return;
	}

	Com_sprintf( path, sizeof( path ), "lang/%s/strings.txt", langCode );
	len = trap_FS_FOpenFile( path, &f, FS_READ );
	if ( len <= 0 ) {
		if ( f ) {
			trap_FS_FCloseFile( f );
		}
		return;
	}
	if ( len >= LANG_BUF_SIZE ) {
		Com_Printf( S_COLOR_YELLOW "%s too big (%d bytes)\n", path, len );
		trap_FS_FCloseFile( f );
		return;
	}
	trap_FS_Read( langBuf, len, f );
	trap_FS_FCloseFile( f );
	langBuf[len] = 0;

	p = langBuf;
	if ( (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF ) {
		p += 3;
	}
	while ( *p ) {
		char *key, *val;

		while ( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ) {
			p++;
		}
		if ( !*p ) {
			break;
		}
		if ( *p != '"' ) {      // comment or junk: skip the line
			while ( *p && *p != '\n' ) {
				p++;
			}
			continue;
		}
		key = Lang_ParseQuoted( &p );
		while ( *p == ' ' || *p == '\t' ) {
			p++;
		}
		if ( *p != '"' ) {
			while ( *p && *p != '\n' ) {
				p++;
			}
			continue;
		}
		val = Lang_ParseQuoted( &p );
		if ( !key[0] || strlen( key ) > LANG_MAX_KEYLEN ) {
			continue;
		}
		if ( langNumEntries >= LANG_MAX_STRINGS ) {
			Com_Printf( S_COLOR_YELLOW "%s: more than %d strings, rest ignored\n", path, LANG_MAX_STRINGS );
			break;
		}
		{
			unsigned int h = Lang_Hash( key );
			langEntries[langNumEntries].key = key;
			langEntries[langNumEntries].val = val;
			langEntries[langNumEntries].next = langHash[h];     // newest entry first: later duplicates win
			langHash[h] = langNumEntries++;
		}
	}
	Com_Printf( "Language %s: %d strings\n", langCode, langNumEntries );
}

static void Lang_SetCode( const char *code ) {
	int i;
	qboolean ok = code[0] != 0;

	for ( i = 0; code[i]; i++ ) {
		if ( i >= LANG_CODE_LEN - 1 || !( ( code[i] >= 'a' && code[i] <= 'z' ) || ( code[i] >= 'A' && code[i] <= 'Z' ) ||
										  ( code[i] >= '0' && code[i] <= '9' ) || code[i] == '_' || code[i] == '-' ) ) {
			ok = qfalse;
			break;
		}
		langCode[i] = tolower( (unsigned char)code[i] );
	}
	if ( ok ) {
		langCode[i] = 0;
	} else {
		Q_strncpyz( langCode, "en", sizeof( langCode ) );
	}
}

void Lang_Init( void ) {
	trap_Cvar_Register( &langTextCvar, "cod_textLang", "en", CVAR_ARCHIVE );
	langRegistered = qtrue;
	langLastModCount = langTextCvar.modificationCount;
	Lang_SetCode( langTextCvar.string );
	langGeneration++;
	Lang_LoadTable();
}

qboolean Lang_Frame( void ) {
	if ( !langRegistered ) {
		return qfalse;
	}
	trap_Cvar_Update( &langTextCvar );
	if ( langTextCvar.modificationCount == langLastModCount ) {
		return qfalse;
	}
	langLastModCount = langTextCvar.modificationCount;
	Lang_SetCode( langTextCvar.string );
	langGeneration++;
	Lang_LoadTable();
	return qtrue;
}

const char *Lang_Code( void ) {
	return langCode;
}

int Lang_Generation( void ) {
	return langGeneration;
}

const char *Lang_Translate( const char *s ) {
	int i;

	if ( !langNumEntries || !s || !s[0] ) {
		return s;
	}
	for ( i = langHash[Lang_Hash( s )]; i >= 0; i = langEntries[i].next ) {
		if ( !strcmp( langEntries[i].key, s ) ) {
			return langEntries[i].val;
		}
	}
	return s;
}

const char *Lang_TextPath( const char *path, char *out, int outSize ) {
	fileHandle_t f;
	int len;

	if ( !langNumEntries && !strcmp( langCode, "en" ) ) {
		return path;
	}
	if ( !strcmp( langCode, "en" ) || Q_stricmpn( path, "text/EnglishUSA/", 16 ) ) {
		return path;
	}
	Com_sprintf( out, outSize, "lang/%s/text/%s", langCode, path + 16 );
	len = trap_FS_FOpenFile( out, &f, FS_READ );
	if ( len > 0 ) {
		trap_FS_FCloseFile( f );
		return out;
	}
	if ( f ) {
		trap_FS_FCloseFile( f );
	}
	return path;
}

/*
=================
Font glyph fallback

The retail glyph fonts (fonts/fontImage_<pt>.dat) cover Latin-1 except a few cells (0xA0, 0xFF; 0xD6 in the MP-only
20 pt font). A glyph is missing when its cell has no size (imageWidth/imageHeight 0, empty shaderName), which is
read straight from the registered font, so any font pack that adds glyphs is picked up automatically.
=================
*/
// unaccented replacement for Latin-1 0xA0..0xFF
static const char *const langFold[96] = {
	" ", "", "c", "L", "o", "Y", "|", "S", "", "(c)", "a", "\"", "-", "-", "(R)", "-",      // A0
	"o", "+/-", "2", "3", "'", "u", "P", ".", ",", "1", "o", "\"", "1/4", "1/2", "3/4", "",   // B0
	"A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",         // C0
	"D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",       // D0
	"a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",         // E0
	"d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y"          // F0
};

static qboolean Lang_GlyphMissing( const fontInfo_t *fnt, unsigned char c ) {
	const glyphInfo_t *g = &fnt->glyphs[c];

	return ( g->imageWidth <= 0 || g->imageHeight <= 0 ) ? qtrue : qfalse;
}

const char *Lang_Prepare( const fontInfo_t *fnt, const char *s ) {
	static char fixed[LANG_FIX_SIZE];
	const unsigned char *r;
	char *w;
	qboolean gotMissing = qfalse;

	if ( !s ) {
		return s;
	}
	if ( langNumEntries ) {
		s = Lang_Translate( s );
	}

	for ( r = (const unsigned char *)s; *r; r++ ) {         // fast path: pure ASCII needs nothing
		if ( *r >= 0xA0 ) {
			if ( Lang_GlyphMissing( fnt, *r ) ) {
				gotMissing = qtrue;
				break;
			}
		}
	}
	if ( !gotMissing || strlen( s ) >= LANG_FIX_SIZE / 4 ) {
		return s;
	}

	w = fixed;
	for ( r = (const unsigned char *)s; *r && w < fixed + LANG_FIX_SIZE - 8; r++ ) {
		if ( *r >= 0xA0 && Lang_GlyphMissing( fnt, *r ) ) {
			const char *f = langFold[*r - 0xA0];
			while ( *f ) {
				*w++ = *f++;
			}
		} else {
			*w++ = *r;
		}
	}
	*w = 0;
	return fixed;
}
