// cl_cin_avi.c -- MJPEG/AVI cinematic reader (see cl_cin_avi.h)

#include <setjmp.h>
#include <stdio.h>
#include <jpeglib.h>

#include "client.h"
#include "cl_cin_avi.h"

#define AVI_MAX_CHUNK ( 16 * 1024 * 1024 )

struct cinAvi_s {
	fileHandle_t f;
	char name[MAX_QPATH];
	long moviStart;
	long moviEnd;
	long pos;

	int width, height, fps;
	int audioRate, audioChannels;

	byte *chunk;
	int chunkAlloc;
	int chunkSize;
	qboolean chunkIsAudio;

	byte *rgba;
};

typedef struct {
	struct jpeg_error_mgr pub;
	jmp_buf jb;
} aviJpegErr_t;

static void AVI_JpegExit( j_common_ptr c ) {
	longjmp( ( (aviJpegErr_t *)c->err )->jb, 1 );
}

static void AVI_JpegMessage( j_common_ptr c ) {
}

static unsigned AVI_LE32( const byte *b ) {
	return b[0] | ( b[1] << 8 ) | ( b[2] << 16 ) | ( (unsigned)b[3] << 24 );
}

static unsigned AVI_LE16( const byte *b ) {
	return b[0] | ( b[1] << 8 );
}

static qboolean AVI_Read( cinAvi_t *a, void *buf, int len ) {
	if ( FS_Read( buf, len, a->f ) != len ) {
		return qfalse;
	}
	a->pos += len;
	return qtrue;
}

static qboolean AVI_Skip( cinAvi_t *a, long len ) {
	byte tmp[4096];

	while ( len > 0 ) {
		int n = len > (long)sizeof( tmp ) ? (int)sizeof( tmp ) : (int)len;
		if ( !AVI_Read( a, tmp, n ) ) {
			return qfalse;
		}
		len -= n;
	}
	return qtrue;
}

static qboolean AVI_IsPow2( int v ) {
	return v > 0 && ( v & ( v - 1 ) ) == 0;
}

// Parses an strl list (list payload size bytes, already past the 'strl' tag).
static qboolean AVI_ParseStrl( cinAvi_t *a, long size ) {
	byte hdr[8], buf[64];
	long end = a->pos + size;
	qboolean audio = qfalse;

	while ( a->pos + 8 <= end ) {
		unsigned id, csize;

		if ( !AVI_Read( a, hdr, 8 ) ) {
			return qfalse;
		}
		id = AVI_LE32( hdr );
		csize = AVI_LE32( hdr + 4 );
		if ( id == 0x68727473 ) {            // 'strh'
			if ( csize < 4 || !AVI_Read( a, buf, 4 ) ) {
				return qfalse;
			}
			audio = AVI_LE32( buf ) == 0x73647561;     // 'auds'
			if ( !AVI_Skip( a, csize - 4 + ( csize & 1 ) ) ) {
				return qfalse;
			}
		} else if ( id == 0x66727473 && audio && csize >= 16 ) {      // 'strf' WAVEFORMATEX
			if ( !AVI_Read( a, buf, 16 ) ) {
				return qfalse;
			}
			if ( AVI_LE16( buf ) == 1 && AVI_LE16( buf + 14 ) == 16 ) {     // PCM s16
				a->audioChannels = AVI_LE16( buf + 2 );
				a->audioRate = AVI_LE32( buf + 4 );
			}
			if ( !AVI_Skip( a, csize - 16 + ( csize & 1 ) ) ) {
				return qfalse;
			}
		} else if ( !AVI_Skip( a, csize + ( csize & 1 ) ) ) {
			return qfalse;
		}
	}
	return a->pos == end || AVI_Skip( a, end - a->pos );
}

static qboolean AVI_ParseHeader( cinAvi_t *a ) {
	byte hdr[12], buf[56];
	unsigned usPerFrame;

	if ( !AVI_Read( a, hdr, 12 ) || AVI_LE32( hdr ) != 0x46464952 || AVI_LE32( hdr + 8 ) != 0x20495641 ) {   // RIFF....AVI
		return qfalse;
	}
	for ( ;; ) {
		unsigned id, size, type;

		if ( !AVI_Read( a, hdr, 8 ) ) {
			return qfalse;
		}
		id = AVI_LE32( hdr );
		size = AVI_LE32( hdr + 4 );
		if ( id != 0x5453494c ) {                        // 'LIST'
			if ( !AVI_Skip( a, size + ( size & 1 ) ) ) {
				return qfalse;
			}
			continue;
		}
		if ( size < 4 || !AVI_Read( a, hdr, 4 ) ) {
			return qfalse;
		}
		type = AVI_LE32( hdr );
		if ( type == 0x6c726468 ) {                      // 'hdrl'
			long end = a->pos + size - 4;
			while ( a->pos + 8 <= end ) {
				unsigned sid, ssize;

				if ( !AVI_Read( a, hdr, 8 ) ) {
					return qfalse;
				}
				sid = AVI_LE32( hdr );
				ssize = AVI_LE32( hdr + 4 );
				if ( sid == 0x68697661 && ssize >= 56 ) {          // 'avih'
					if ( !AVI_Read( a, buf, 56 ) ) {
						return qfalse;
					}
					usPerFrame = AVI_LE32( buf );
					a->width = AVI_LE32( buf + 32 );
					a->height = AVI_LE32( buf + 36 );
					a->fps = usPerFrame ? (int)( 1000000.0 / usPerFrame + 0.5 ) : 0;
					if ( !AVI_Skip( a, ssize - 56 + ( ssize & 1 ) ) ) {
						return qfalse;
					}
				} else if ( sid == 0x5453494c && ssize >= 4 ) {    // nested 'LIST'
					if ( !AVI_Read( a, hdr, 4 ) ) {
						return qfalse;
					}
					if ( AVI_LE32( hdr ) == 0x6c727473 ) {         // 'strl'
						if ( !AVI_ParseStrl( a, ssize - 4 ) ) {
							return qfalse;
						}
					} else if ( !AVI_Skip( a, ssize - 4 + ( ssize & 1 ) ) ) {
						return qfalse;
					}
				} else if ( !AVI_Skip( a, ssize + ( ssize & 1 ) ) ) {
					return qfalse;
				}
			}
		} else if ( type == 0x69766f6d ) {               // 'movi'
			a->moviStart = a->pos;
			a->moviEnd = a->pos + size - 4;
			return a->width > 0 && a->height > 0 && a->fps > 0;
		} else if ( !AVI_Skip( a, size - 4 + ( size & 1 ) ) ) {
			return qfalse;
		}
	}
}

cinAvi_t *CIN_AVI_Open( const char *name ) {
	cinAvi_t *a;
	int len;
	fileHandle_t f;

	len = FS_FOpenFileRead( name, &f, qtrue );
	if ( len <= 0 ) {
		return NULL;
	}
	a = Z_Malloc( sizeof( *a ) );
	Com_Memset( a, 0, sizeof( *a ) );
	a->f = f;
	Q_strncpyz( a->name, name, sizeof( a->name ) );

	if ( !AVI_ParseHeader( a ) || !AVI_IsPow2( a->width ) || !AVI_IsPow2( a->height ) ) {
		Com_DPrintf( "CIN_AVI_Open: %s is not a usable MJPEG AVI\n", name );
		CIN_AVI_Close( a );
		return NULL;
	}
	a->rgba = Z_Malloc( a->width * a->height * 4 );
	Com_Memset( a->rgba, 0, a->width * a->height * 4 );
	return a;
}

void CIN_AVI_Close( cinAvi_t *a ) {
	if ( !a ) {
		return;
	}
	if ( a->f ) {
		FS_FCloseFile( a->f );
	}
	if ( a->chunk ) {
		Z_Free( a->chunk );
	}
	if ( a->rgba ) {
		Z_Free( a->rgba );
	}
	Z_Free( a );
}

void CIN_AVI_Rewind( cinAvi_t *a ) {
	FS_Seek( a->f, a->moviStart, FS_SEEK_SET );
	a->pos = a->moviStart;
}

static qboolean AVI_DecodeJpeg( cinAvi_t *a ) {
	struct jpeg_decompress_struct cinfo;
	aviJpegErr_t jerr;
	int y;

	cinfo.err = jpeg_std_error( &jerr.pub );
	jerr.pub.error_exit = AVI_JpegExit;
	jerr.pub.output_message = AVI_JpegMessage;
	if ( setjmp( jerr.jb ) ) {
		jpeg_destroy_decompress( &cinfo );
		return qfalse;
	}
	jpeg_create_decompress( &cinfo );
	jpeg_mem_src( &cinfo, a->chunk, a->chunkSize );
	jpeg_read_header( &cinfo, TRUE );
	cinfo.out_color_space = JCS_RGB;
	jpeg_start_decompress( &cinfo );
	if ( (int)cinfo.output_width != a->width || (int)cinfo.output_height != a->height || cinfo.output_components != 3 ) {
		jpeg_destroy_decompress( &cinfo );
		return qfalse;
	}
	for ( y = 0; y < a->height; y++ ) {
		byte *dst = a->rgba + y * a->width * 4;
		byte *src = dst + a->width;     // RGB row parked in the tail of its own RGBA row
		JSAMPROW rp = src;
		int x;

		jpeg_read_scanlines( &cinfo, &rp, 1 );
		// src (w*3 bytes at +w) overlaps dst (w*4 bytes at +0); expanding front to back never
		// overwrites a source pixel before it is read (write end 4x+3 < next read start w+3x+3).
		for ( x = 0; x < a->width; x++ ) {
			byte r = src[x * 3], g = src[x * 3 + 1], b = src[x * 3 + 2];
			dst[x * 4] = r;
			dst[x * 4 + 1] = g;
			dst[x * 4 + 2] = b;
			dst[x * 4 + 3] = 255;
		}
	}
	jpeg_finish_decompress( &cinfo );
	jpeg_destroy_decompress( &cinfo );
	return qtrue;
}

cinAviChunk_t CIN_AVI_Next( cinAvi_t *a ) {
	byte hdr[8];
	unsigned id, size;

	for ( ;; ) {
		if ( a->pos + 8 > a->moviEnd ) {
			return CIN_AVI_END;
		}
		if ( !AVI_Read( a, hdr, 8 ) ) {
			return CIN_AVI_END;
		}
		id = AVI_LE32( hdr );
		size = AVI_LE32( hdr + 4 );
		if ( size > AVI_MAX_CHUNK ) {
			return CIN_AVI_ERROR;
		}
		if ( (int)size > a->chunkAlloc ) {
			if ( a->chunk ) {
				Z_Free( a->chunk );
			}
			a->chunkAlloc = ( size + 4095 ) & ~4095;
			a->chunk = Z_Malloc( a->chunkAlloc );
		}
		if ( !AVI_Read( a, a->chunk, size ) || !AVI_Skip( a, size & 1 ) ) {
			return CIN_AVI_END;
		}
		a->chunkSize = size;

		// stream chunk ids: "NNdc"/"NNdb" video, "NNwb" audio
		switch ( id >> 16 ) {
		case 0x6364:        // 'dc'
		case 0x6264:        // 'db'
			if ( !size ) {
				continue;       // dropped frame: keep previous image
			}
			return AVI_DecodeJpeg( a ) ? CIN_AVI_VIDEO : CIN_AVI_ERROR;
		case 0x6277:        // 'wb'
			if ( a->audioChannels > 0 && size >= 2u * a->audioChannels ) {
				return CIN_AVI_AUDIO;
			}
			continue;
		default:
			continue;       // JUNK, idx1, nested lists
		}
	}
}

int CIN_AVI_Width( const cinAvi_t *a ) {
	return a->width;
}

int CIN_AVI_Height( const cinAvi_t *a ) {
	return a->height;
}

int CIN_AVI_FPS( const cinAvi_t *a ) {
	return a->fps;
}

byte *CIN_AVI_Frame( cinAvi_t *a ) {
	return a->rgba;
}

const byte *CIN_AVI_Audio( const cinAvi_t *a, int *samples, int *rate, int *channels ) {
	*samples = a->chunkSize / ( 2 * a->audioChannels );
	*rate = a->audioRate;
	*channels = a->audioChannels;
	return a->chunk;
}
