// codmod: multilanguage support shared by the ui module and cgame (ui_lang.c is linked into both, like ui_shared.c).
// Contract: docs/languages.md.

#ifndef __UI_LANG_H
#define __UI_LANG_H

#include "ui_shared.h"

// Registers cod_textLang and loads lang/<code>/strings.txt (no table for "en"). Call once per module init.
void Lang_Init( void );

// Call once per frame: reloads the table when cod_textLang changed. Returns qtrue on the frame it changed.
qboolean Lang_Frame( void );

// Current text language code ("en" when the cvar is empty or invalid) and a counter bumped on every change.
const char *Lang_Code( void );
int Lang_Generation( void );

// Exact-match lookup in the string table; returns s itself when there is no entry. No allocation.
const char *Lang_Translate( const char *s );

// Translate and, for glyph-based fonts, replace Latin-1 characters the font has no glyph for with an unaccented
// fallback (e->e, ss for the sharp s, etc). Returns s or a static buffer (valid until the next call).
const char *Lang_Prepare( const fontInfo_t *fnt, const char *s );

// In place UTF-8 -> Latin-1. Invalid UTF-8 bytes are kept as they are, so Latin-1 files pass through unchanged.
void Lang_Utf8ToLatin1( char *s );

// Maps "text/EnglishUSA/<file>" to "lang/<code>/text/<file>" when a text pack is active and the file exists.
// Returns the path to open (path itself when no redirect applies; out is used otherwise).
const char *Lang_TextPath( const char *path, char *out, int outSize );

#endif
