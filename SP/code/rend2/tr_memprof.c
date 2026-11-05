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
// tr_memprof.c - codmod: VRAM census (com_memProfile)
//
// A census, not allocation hooks: it walks tr.images[], tr.fbos[] and tr.vaos[] at sample points and computes
// the bytes analytically from width, height, mip count, faces and internal format. It never binds a texture or
// changes GL state (that would corrupt the glState cache); the only GL calls are glGetIntegerv for the
// GL_NVX_gpu_memory_info counters. The RAM half lives in client/cl_memprof.c. Design: docs/memory-profiler-plan.md.
//
// Output (<fs_homepath>/<fs_game>/profiling/): mem_vram_objects.csv, mem_vram_categories.csv,
// mem_vram_timeline.csv, mem_meta.json. Written by "vramprof dump [why]", by the engine at the end of a bench
// demo, and as a fallback at renderer shutdown when nothing was dumped.
//
// Cost when com_memProfile is 0: one cvar read per frame and per image load, nothing else.

#include <stdlib.h>
#include <time.h>
#include "tr_local.h"

#define MEMPROF_MAX_ROWS		200000	// timeline rows kept (about 11 MB)
#define MEMPROF_MAX_PAKS		128
#define MEMPROF_MAX_CATS		96
#define MEMPROF_MAX_RT			192

//===========================================================================

// bytes per texel of the uncompressed formats this renderer can create
typedef struct {
	int fmt;
	int bpp;
	const char *name;
} memFmt_t;

static const memFmt_t memFmts[] = {
	{ GL_RGBA8, 4, "RGBA8" },
	{ GL_SRGB8_ALPHA8_EXT, 4, "SRGB8_ALPHA8" },
	{ GL_RGB8, 4, "RGB8" },
	{ GL_SRGB8_EXT, 4, "SRGB8" },
	{ GL_RGBA, 4, "RGBA" },
	{ GL_RGB, 4, "RGB" },
	{ GL_RGB10_A2, 4, "RGB10_A2" },
	{ GL_R11F_G11F_B10F, 4, "R11F_G11F_B10F" },
	{ GL_RG16F, 4, "RG16F" },
	{ GL_R32F, 4, "R32F" },
	{ GL_DEPTH_COMPONENT, 4, "DEPTH" },
	{ GL_DEPTH_COMPONENT24_ARB, 4, "DEPTH24" },
	{ GL_DEPTH_COMPONENT32_ARB, 4, "DEPTH32" },
	{ GL_DEPTH_COMPONENT32F, 4, "DEPTH32F" },
	{ GL_DEPTH24_STENCIL8, 4, "DEPTH24_STENCIL8" },
	{ GL_DEPTH_STENCIL, 4, "DEPTH_STENCIL" },
	{ GL_RGBA16F_ARB, 8, "RGBA16F" },
	{ GL_RGB16F_ARB, 8, "RGB16F" },
	{ GL_RGBA16, 8, "RGBA16" },
	{ GL_RG32F, 8, "RG32F" },
	{ GL_RGBA32F_ARB, 16, "RGBA32F" },
	{ GL_RGB32F_ARB, 16, "RGB32F" },
	{ GL_RG8, 2, "RG8" },
	{ GL_LUMINANCE8_ALPHA8, 2, "LUMINANCE8_ALPHA8" },
	{ GL_SLUMINANCE8_ALPHA8_EXT, 2, "SLUMINANCE8_ALPHA8" },
	{ GL_R16F, 2, "R16F" },
	{ GL_DEPTH_COMPONENT16_ARB, 2, "DEPTH16" },
	{ GL_RGBA4, 2, "RGBA4" },
	{ GL_RGB5, 2, "RGB5" },
	{ GL_STENCIL_INDEX16, 2, "STENCIL16" },
	{ GL_R8, 1, "R8" },
	{ GL_LUMINANCE8, 1, "LUMINANCE8" },
	{ GL_SLUMINANCE8_EXT, 1, "SLUMINANCE8" },
	{ GL_STENCIL_INDEX8, 1, "STENCIL8" },
	{ GL_STENCIL_INDEX4, 1, "STENCIL4" },
	{ GL_STENCIL_INDEX1, 1, "STENCIL1" },
};

// bytes per 4x4 block of the compressed formats (mirrors CalculateMipSize in tr_image.c)
typedef struct {
	int fmt;
	int blockBytes;
	const char *name;
} memBlockFmt_t;

static const memBlockFmt_t memBlockFmts[] = {
	{ GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 8, "DXT1" },
	{ GL_COMPRESSED_SRGB_S3TC_DXT1_EXT, 8, "SRGB_DXT1" },
	{ GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 8, "RGBA_DXT1" },
	{ GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, 8, "SRGB_ALPHA_DXT1" },
	{ GL_COMPRESSED_RED_RGTC1, 8, "RGTC1" },
	{ GL_COMPRESSED_SIGNED_RED_RGTC1, 8, "SIGNED_RGTC1" },
	{ GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 16, "DXT3" },
	{ GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, 16, "SRGB_ALPHA_DXT3" },
	{ GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 16, "DXT5" },
	{ GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, 16, "SRGB_ALPHA_DXT5" },
	{ GL_COMPRESSED_RG_RGTC2, 16, "RGTC2" },
	{ GL_COMPRESSED_SIGNED_RG_RGTC2, 16, "SIGNED_RGTC2" },
	{ GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_ARB, 16, "BPTC_UFLOAT" },
	{ GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ARB, 16, "BPTC_SFLOAT" },
	{ GL_COMPRESSED_RGBA_BPTC_UNORM_ARB, 16, "BPTC" },
	{ GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB, 16, "SRGB_BPTC" },
};

static const memFmt_t *MemProf_FindFmt( int fmt ) {
	int i;

	for ( i = 0; i < (int)ARRAY_LEN( memFmts ); i++ ) {
		if ( memFmts[i].fmt == fmt ) {
			return &memFmts[i];
		}
	}
	return NULL;
}

static const memBlockFmt_t *MemProf_FindBlockFmt( int fmt ) {
	int i;

	for ( i = 0; i < (int)ARRAY_LEN( memBlockFmts ); i++ ) {
		if ( memBlockFmts[i].fmt == fmt ) {
			return &memBlockFmts[i];
		}
	}
	return NULL;
}

static void MemProf_FormatName( int fmt, char *out, int size ) {
	const memFmt_t *f = MemProf_FindFmt( fmt );
	const memBlockFmt_t *b;

	if ( f ) {
		Q_strncpyz( out, f->name, size );
	} else if ( ( b = MemProf_FindBlockFmt( fmt ) ) != NULL ) {
		Q_strncpyz( out, b->name, size );
	} else {
		Com_sprintf( out, size, "0x%x", fmt );
	}
}

// bytes of one mip level, or -1 for a format this file does not know
static long long MemProf_LevelBytes( int fmt, int w, int h ) {
	const memFmt_t *f = MemProf_FindFmt( fmt );
	const memBlockFmt_t *b;

	if ( f ) {
		return (long long)w * h * f->bpp;
	}
	b = MemProf_FindBlockFmt( fmt );
	if ( b ) {
		return (long long)( ( w + 3 ) / 4 ) * ( ( h + 3 ) / 4 ) * b->blockBytes;
	}
	return -1;
}

//===========================================================================

typedef struct {
	const char *kind;		// texture | renderbuffer | buffer | estimate
	const char *category;
	const char *map;
	int pak;
	char name[MAX_QPATH];
	int width, height, levels, faces, samples;
	char format[32];
	long long bytes;		// -1 = unknown format (counted in the summary, never silently 0)
	int used;				// 1/0, -1 = bench window not seen
	const char *note;
} memObj_t;

typedef struct {
	long long tex, rt, rb, buf, est;
	int numUnknown;
	int numObjs;
} memTotals_t;

typedef struct {
	long long epochMs;
	long long tex, rt, rb, buf, est;
	int tMs;
	int frame, bench, numImages;
	char event[24];
	int nvxAvailKb, nvxEvictCount, nvxEvictedKb;
} memRow_t;

typedef struct {
	const char *name;
	int count;
	long long bytes;
	long long bytesUsed;
} memCat_t;

typedef struct {
	const image_t *img;
	const char *cat;
} memRt_t;

static struct {
	cvar_t *on;
	cvar_t *interval;
	qboolean dumped;
	int benchStartFrame;	// tr.frameCount at the 0 -> 1 edge of cl_benchMeasuring, 0 = not seen
	int lastBench;
	int lastSampleMs;
	memRow_t *rows;
	int numRows, capRows, droppedRows;
	char paks[MEMPROF_MAX_PAKS][64];
	int numPaks;
	int unknownTotal;
} mp;

static memRt_t rtTable[MEMPROF_MAX_RT];
static int numRt;

//===========================================================================

/*
===============
R_MemProfOn

Lazily registers the cvars so the order of registration against the engine does not matter.
Main thread only.
===============
*/
qboolean R_MemProfOn( void ) {
	if ( !mp.on ) {
		mp.on = ri.Cvar_Get( "com_memProfile", "0", CVAR_TEMP );
		mp.interval = ri.Cvar_Get( "com_memProfileInterval", "500", CVAR_TEMP );
	}
	return mp.on->integer != 0;
}

/*
===============
R_MemProfPakIndex

Index (1 based) of a pak name in the renderer's string table, 0 for none. Stored in image_t.memPak.
===============
*/
int R_MemProfPakIndex( const char *pak ) {
	int i;

	if ( !pak || !pak[0] ) {
		return 0;
	}
	for ( i = 0; i < mp.numPaks; i++ ) {
		if ( !Q_stricmp( mp.paks[i], pak ) ) {
			return i + 1;
		}
	}
	if ( mp.numPaks >= MEMPROF_MAX_PAKS ) {
		return 0;
	}
	Q_strncpyz( mp.paks[mp.numPaks], pak, sizeof( mp.paks[0] ) );
	mp.numPaks++;
	return mp.numPaks;
}

static const char *MemProf_PakName( int idx ) {
	if ( idx < 1 || idx > mp.numPaks ) {
		return "-";
	}
	return mp.paks[idx - 1];
}

static long long MemProf_EpochMs( void ) {
#ifdef _WIN32
	return (long long)time( NULL ) * 1000;
#else
	struct timespec ts;

	clock_gettime( CLOCK_REALTIME, &ts );
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

// GL_NVX_gpu_memory_info counters, -1 where the extension is absent. glGetIntegerv only: no state change.
static void MemProf_Nvx( int *availKb, int *evictCount, int *evictedKb ) {
	*availKb = *evictCount = *evictedKb = -1;
	if ( glRefConfig.memInfo == MI_NVX && qglGetIntegerv ) {
		GLint v = 0;

		qglGetIntegerv( GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &v );
		*availKb = v;
		qglGetIntegerv( GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX, &v );
		*evictCount = v;
		qglGetIntegerv( GL_GPU_MEMORY_INFO_EVICTED_MEMORY_NVX, &v );
		*evictedKb = v;
	}
}

//===========================================================================
// categories

static void MemProf_RtAdd( const image_t *img, const char *cat ) {
	if ( img && numRt < MEMPROF_MAX_RT ) {
		rtTable[numRt].img = img;
		rtTable[numRt].cat = cat;
		numRt++;
	}
}

#define RT_ARRAY( arr, cat ) \
	for ( i = 0; i < (int)ARRAY_LEN( arr ); i++ ) { MemProf_RtAdd( ( arr )[i], cat ); }

// the exact render-target pointer table
static void MemProf_BuildRtTable( void ) {
	int i;

	numRt = 0;
	MemProf_RtAdd( tr.renderImage, "rt_scene" );
	MemProf_RtAdd( tr.renderDepthImage, "rt_scene" );
	MemProf_RtAdd( tr.worldImage, "rt_scene" );
	MemProf_RtAdd( tr.worldDepthImage, "rt_scene" );
	MemProf_RtAdd( tr.upscaleImage, "rt_scene" );
	MemProf_RtAdd( tr.dlsImage, "rt_scene" );
	MemProf_RtAdd( tr.screenScratchImage, "rt_scene" );
	MemProf_RtAdd( tr.hdrDepthImage, "rt_scene" );

	MemProf_RtAdd( tr.ssaoDepthImage, "rt_ssao" );
	MemProf_RtAdd( tr.aoImage, "rt_ssao" );
	MemProf_RtAdd( tr.screenSsaoImage, "rt_ssao" );

	MemProf_RtAdd( tr.smaaEdgesImage, "rt_smaa" );
	MemProf_RtAdd( tr.smaaBlendImage, "rt_smaa" );
	MemProf_RtAdd( tr.smaaHistoryImage, "rt_smaa" );
	MemProf_RtAdd( tr.smaaHistoryImageB, "rt_smaa" );
	MemProf_RtAdd( tr.velocityImage, "rt_smaa" );
	MemProf_RtAdd( tr.smaaAreaImage, "rt_smaa" );
	MemProf_RtAdd( tr.smaaSearchImage, "rt_smaa" );

	RT_ARRAY( tr.bloomImage, "rt_bloom" );
	RT_ARRAY( tr.quarterImage, "rt_bloom" );

	MemProf_RtAdd( tr.sunRaysImage, "rt_sunrays" );

	RT_ARRAY( tr.sunShadowDepthImage, "rt_shadow_sun" );
	MemProf_RtAdd( tr.screenShadowImage, "rt_shadow_sun" );

	MemProf_RtAdd( tr.flashShadowDepthImage, "rt_shadow_flash" );
	MemProf_RtAdd( tr.flashShadowMaskImage, "rt_shadow_flash" );

	RT_ARRAY( tr.pshadowMaps, "rt_shadow_pshadow" );
	MemProf_RtAdd( tr.textureDepthImage, "rt_shadow_pshadow" );
	RT_ARRAY( tr.shadowCubemaps, "rt_shadow_pshadow" );

	MemProf_RtAdd( tr.warpDepthImage, "rt_warp" );
	MemProf_RtAdd( tr.warpMotionImage, "rt_warp" );
	MemProf_RtAdd( tr.warpImage, "rt_warp" );
	MemProf_RtAdd( tr.warpOutImage, "rt_warp" );

	MemProf_RtAdd( tr.renderCubeImage, "rt_cuberender" );

	MemProf_RtAdd( tr.calcLevelsImage, "rt_tonemap" );
	MemProf_RtAdd( tr.targetLevelsImage, "rt_tonemap" );
	MemProf_RtAdd( tr.fixedLevelsImage, "rt_tonemap" );
	RT_ARRAY( tr.textureScratchImage, "rt_tonemap" );
}

static const char *MemProf_RtCategory( const image_t *img ) {
	int i;

	// asset textures always have a path; skip the scan for them
	if ( strchr( img->imgName, '/' ) ) {
		return NULL;
	}
	for ( i = 0; i < numRt; i++ ) {
		if ( rtTable[i].img == img ) {
			return rtTable[i].cat;
		}
	}
	return NULL;
}

static qboolean MemProf_Prefix( const char *s, const char *prefix ) {
	return Q_stricmpn( s, prefix, (int)strlen( prefix ) ) == 0;
}

static const char *MemProf_AssetCategory( const image_t *img ) {
	const char *n = img->imgName;

	if ( MemProf_Prefix( n, "*cubeMap" ) ) {
		return "tex_cubemap_probes";
	}
	if ( strstr( n, "deluxe" ) ) {
		return "tex_deluxemap";
	}
	if ( strstr( n, "lightmap" ) ) {
		return "tex_lightmap";
	}
	if ( !Q_stricmp( n, "*scratch" ) ) {
		return "tex_cinematic";
	}
	if ( MemProf_Prefix( n, "env/" ) || strstr( n, "/sky" ) || strstr( n, "skies/" ) ) {
		return "tex_sky";
	}
	if ( MemProf_Prefix( n, "textures/" ) ) {
		return "tex_world";
	}
	if ( MemProf_Prefix( n, "models/players/" ) ) {
		return "tex_players";
	}
	if ( MemProf_Prefix( n, "models/weapons" ) ) {
		return "tex_weapons";
	}
	if ( MemProf_Prefix( n, "models/" ) ) {
		return "tex_models";
	}
	if ( MemProf_Prefix( n, "gfx/" ) || MemProf_Prefix( n, "ui/" ) || MemProf_Prefix( n, "menu/" ) ||
		 MemProf_Prefix( n, "fonts/" ) || MemProf_Prefix( n, "sprites/" ) ) {
		return "tex_ui_fx";
	}
	if ( n[0] == '*' || n[0] == '_' ) {
		return "tex_builtin";
	}
	return "tex_other";
}

// normal | spec | diffuse for asset textures, "-" for built-in and render-target images
static const char *MemProf_ImageMap( const image_t *img ) {
	const char *n = img->imgName;
	const char *dot;
	int len;

	if ( n[0] == '*' || n[0] == '_' || !strchr( n, '/' ) ) {
		return "-";
	}
	if ( img->type == IMGTYPE_NORMAL || img->type == IMGTYPE_NORMALHEIGHT ) {
		return "normal";
	}
	dot = strrchr( n, '.' );
	len = dot ? (int)( dot - n ) : (int)strlen( n );
	if ( len >= 2 && n[len - 2] == '_' && ( n[len - 1] == 's' || n[len - 1] == 'S' ) ) {
		return "spec";
	}
	return "diffuse";
}

static const char *MemProf_VaoCategory( const vao_t *vao ) {
	const char *n = vao->name;

	if ( MemProf_Prefix( n, "VaoCache" ) ) {
		return "buf_vaocache";
	}
	if ( MemProf_Prefix( n, "tessVertexArray_VAO" ) ) {
		return "buf_tess";
	}
	if ( MemProf_Prefix( n, "staticMD3Mesh_VAO" ) || MemProf_Prefix( n, "staticIQMMesh_VAO" ) ) {
		return "buf_models";
	}
	if ( MemProf_Prefix( n, "warpMesh_VAO" ) ) {
		return "buf_warp";
	}
	return "buf_other";
}

static const char *MemProf_FboCategory( const FBO_t *fbo, qboolean dummyColor ) {
	const char *n = fbo->name;

	if ( dummyColor && ( MemProf_Prefix( n, "_shadowmap" ) || MemProf_Prefix( n, "_sunshadowmap" ) ||
						 MemProf_Prefix( n, "_flashshadowmap" ) ) ) {
		return "rb_shadow_dummy_color";
	}
	if ( MemProf_Prefix( n, "_render" ) && !MemProf_Prefix( n, "_renderCube" ) ) {
		return "rb_scene";
	}
	if ( MemProf_Prefix( n, "smaa" ) ) {
		return "rb_smaa";
	}
	if ( MemProf_Prefix( n, "_renderCube" ) ) {
		return "rb_cuberender";
	}
	return "rb_other";
}

//===========================================================================
// census

static memObj_t *MemProf_NewObj( memObj_t *objs, int *n, int max, memTotals_t *t, const char *kind, const char *cat, const char *name ) {
	memObj_t *o;

	if ( !objs ) {
		return NULL;
	}
	if ( *n >= max ) {
		return NULL;
	}
	o = &objs[( *n )++];
	Com_Memset( o, 0, sizeof( *o ) );
	o->kind = kind;
	o->category = cat;
	o->map = "-";
	o->used = 1;
	o->faces = 1;
	Q_strncpyz( o->name, name, sizeof( o->name ) );
	return o;
}

static void MemProf_Account( memTotals_t *t, char cls, long long bytes ) {
	if ( bytes < 0 ) {
		t->numUnknown++;
		return;
	}
	switch ( cls ) {
	case 't': t->tex += bytes; break;
	case 'r': t->rt += bytes; break;
	case 'b': t->rb += bytes; break;
	case 'v': t->buf += bytes; break;
	default:  t->est += bytes; break;
	}
}

/*
===============
MemProf_Census

Walks images, framebuffers and buffers. objs may be NULL (totals only, no copies: the periodic sample).
===============
*/
static void MemProf_Census( memObj_t *objs, int *numObjs, int maxObjs, memTotals_t *t ) {
	int i, j, n = 0;

	Com_Memset( t, 0, sizeof( *t ) );
	MemProf_BuildRtTable();

	// textures (render targets included: rt_* categories)
	for ( i = 0; i < tr.numImages; i++ ) {
		const image_t *img = tr.images[i];
		const char *cat;
		qboolean isRt, mipmap, scratch;
		int fmt, w, h, levels, faces, l;
		long long bytes = 0, lb;
		memObj_t *o;

		if ( !img ) {
			continue;
		}
		cat = MemProf_RtCategory( img );
		isRt = cat != NULL;
		if ( !cat ) {
			cat = MemProf_AssetCategory( img );
		}

		w = img->uploadWidth ? img->uploadWidth : img->width;
		h = img->uploadHeight ? img->uploadHeight : img->height;
		fmt = img->internalFormat;
		scratch = !Q_stricmp( img->imgName, "*scratch" );
		if ( scratch ) {
			// RE_UploadCinematic re-specs the storage as RGB8 but leaves internalFormat stale: 4 B/px
			fmt = GL_RGBA8;
		}
		mipmap = ( img->flags & IMGFLAG_MIPMAP ) != 0;
		faces = ( img->flags & IMGFLAG_CUBEMAP ) ? 6 : 1;

		levels = 0;
		{
			int mw = w, mh = h;

			if ( img == tr.ssaoDepthImage ) {
				// the mip chain is allocated outside R_CreateImage (R_CreateBuiltinImages)
				for ( l = 0; l <= tr.ssaoDepthMips; l++ ) {
					lb = MemProf_LevelBytes( fmt, mw, mh );
					if ( lb < 0 ) {
						bytes = -1;
						break;
					}
					bytes += lb;
					levels++;
					mw = MAX( 1, mw >> 1 );
					mh = MAX( 1, mh >> 1 );
				}
			} else {
				for ( ;; ) {
					lb = MemProf_LevelBytes( fmt, mw, mh );
					if ( lb < 0 ) {
						bytes = -1;
						break;
					}
					bytes += lb;
					levels++;
					if ( !mipmap || ( mw == 1 && mh == 1 ) ) {
						break;
					}
					mw = MAX( 1, mw >> 1 );
					mh = MAX( 1, mh >> 1 );
				}
			}
		}
		if ( bytes > 0 ) {
			bytes *= faces;
		}

		MemProf_Account( t, isRt ? 'r' : 't', bytes );
		t->numObjs++;

		o = MemProf_NewObj( objs, &n, maxObjs, t, "texture", cat, img->imgName );
		if ( o ) {
			o->map = isRt ? "-" : MemProf_ImageMap( img );
			o->pak = img->memPak;
			o->width = w;
			o->height = h;
			o->levels = levels;
			o->faces = faces;
			MemProf_FormatName( fmt, o->format, sizeof( o->format ) );
			o->bytes = bytes;
			if ( bytes < 0 ) {
				o->note = "unknown_format";
			} else if ( scratch ) {
				o->note = "cinematic_4Bpx";
			}
			// textures never bound since the bench started are resident but unused
			o->used = mp.benchStartFrame > 0 ? ( img->frameUsed >= mp.benchStartFrame ) : -1;
		}
	}

	// renderbuffers; attached textures are already counted above
	for ( i = 0; i < tr.numFBOs; i++ ) {
		const FBO_t *fbo = tr.fbos[i];
		int samples = 0;

		if ( !fbo ) {
			continue;
		}
		if ( fbo == tr.renderFbo && tr.msaaResolveFbo && r_ext_framebuffer_multisample->integer >= 2 ) {
			samples = r_ext_framebuffer_multisample->integer;
		}

		for ( j = 0; j < 4 + 16; j++ ) {
			unsigned int buf;
			int fmt;
			qboolean dummy = qfalse;
			const char *what;
			char nm[MAX_QPATH + 16];
			long long bytes;
			memObj_t *o;
			const memFmt_t *f;

			if ( j < 16 ) {
				buf = fbo->colorBuffers[j];
				fmt = fbo->colorFormat;
				what = "color";
				// shadow FBOs carry an RGBA8 colour buffer nothing reads (FIXMEs in FBO_Init)
				dummy = qtrue;
			} else if ( j == 16 ) {
				buf = fbo->depthBuffer;
				fmt = fbo->depthFormat;
				what = "depth";
			} else if ( j == 17 ) {
				buf = fbo->stencilBuffer;
				fmt = fbo->stencilFormat;
				what = "stencil";
			} else if ( j == 18 ) {
				buf = fbo->packedDepthStencilBuffer;
				fmt = fbo->packedDepthStencilFormat;
				what = "depthstencil";
			} else {
				break;
			}
			if ( !buf ) {
				continue;
			}

			f = MemProf_FindFmt( fmt );
			if ( f ) {
				bytes = (long long)fbo->width * fbo->height * f->bpp * MAX( 1, samples );
			} else {
				bytes = -1;
			}
			MemProf_Account( t, 'b', bytes );
			t->numObjs++;

			if ( j < 16 ) {
				Com_sprintf( nm, sizeof( nm ), "%s.%s%d", fbo->name, what, j );
			} else {
				Com_sprintf( nm, sizeof( nm ), "%s.%s", fbo->name, what );
			}
			o = MemProf_NewObj( objs, &n, maxObjs, t, "renderbuffer", MemProf_FboCategory( fbo, dummy ), nm );
			if ( o ) {
				o->width = fbo->width;
				o->height = fbo->height;
				o->levels = 1;
				o->samples = samples;
				MemProf_FormatName( fmt, o->format, sizeof( o->format ) );
				o->bytes = bytes;
				if ( bytes < 0 ) {
					o->note = "unknown_format";
				}
			}
		}
	}

	// vertex and index buffers
	for ( i = 0; i < tr.numVaos; i++ ) {
		const vao_t *vao = tr.vaos[i];
		long long bytes;
		memObj_t *o;

		if ( !vao ) {
			continue;
		}
		bytes = (long long)vao->vertexesSize + vao->indexesSize;
		MemProf_Account( t, 'v', bytes );
		t->numObjs++;

		o = MemProf_NewObj( objs, &n, maxObjs, t, "buffer", MemProf_VaoCategory( vao ), vao->name );
		if ( o ) {
			Q_strncpyz( o->format, "buffer", sizeof( o->format ) );
			o->bytes = bytes;
		}
	}

	// estimates: objects this file cannot see in the tr tables
	{
		int lsW = 0, lsH = 0;
		long long bytes;
		memObj_t *o;

		R_LoadScreenGetSize( &lsW, &lsH );
		if ( lsW > 0 && lsH > 0 ) {
			bytes = (long long)lsW * lsH * 4;
			MemProf_Account( t, 'e', bytes );
			t->numObjs++;
			o = MemProf_NewObj( objs, &n, maxObjs, t, "estimate", "est_loadscreen", "loadscreen_renderbuffer" );
			if ( o ) {
				o->width = lsW;
				o->height = lsH;
				o->levels = 1;
				Q_strncpyz( o->format, "RGBA8", sizeof( o->format ) );
				o->bytes = bytes;
				o->note = "estimate";
			}
		}

		if ( glConfig.vidWidth > 0 && glConfig.vidHeight > 0 ) {
			int dsBits = glConfig.depthBits + glConfig.stencilBits;
			int dsBytes = dsBits > 16 ? 4 : ( dsBits > 0 ? 2 : 0 );

			// double buffered 32 bit colour, plus the depth/stencil the SDL context asked for
			bytes = (long long)glConfig.vidWidth * glConfig.vidHeight * 4 * 2;
			MemProf_Account( t, 'e', bytes );
			t->numObjs++;
			o = MemProf_NewObj( objs, &n, maxObjs, t, "estimate", "est_default_fb", "default_framebuffer_color" );
			if ( o ) {
				o->width = glConfig.vidWidth;
				o->height = glConfig.vidHeight;
				o->levels = 1;
				Q_strncpyz( o->format, "RGBA8x2", sizeof( o->format ) );
				o->bytes = bytes;
				o->note = "estimate";
			}
			if ( dsBytes ) {
				bytes = (long long)glConfig.vidWidth * glConfig.vidHeight * dsBytes;
				MemProf_Account( t, 'e', bytes );
				t->numObjs++;
				o = MemProf_NewObj( objs, &n, maxObjs, t, "estimate", "est_default_fb", "default_framebuffer_depth" );
				if ( o ) {
					o->width = glConfig.vidWidth;
					o->height = glConfig.vidHeight;
					o->levels = 1;
					Q_strncpyz( o->format, dsBytes == 4 ? "DEPTH24_STENCIL8" : "DEPTH16", sizeof( o->format ) );
					o->bytes = bytes;
					o->note = "estimate";
				}
			}
		}
	}

	if ( numObjs ) {
		*numObjs = n;
	}
}

//===========================================================================
// timeline

static void MemProf_AddRow( const char *event ) {
	memTotals_t t;
	memRow_t *r;

	if ( mp.numRows >= MEMPROF_MAX_ROWS ) {
		mp.droppedRows++;
		return;
	}
	if ( mp.numRows >= mp.capRows ) {
		int newCap = mp.capRows ? mp.capRows * 2 : 512;
		memRow_t *nr;

		if ( newCap > MEMPROF_MAX_ROWS ) {
			newCap = MEMPROF_MAX_ROWS;
		}
		nr = malloc( newCap * sizeof( memRow_t ) );
		if ( !nr ) {
			mp.droppedRows++;
			return;
		}
		if ( mp.rows ) {
			Com_Memcpy( nr, mp.rows, mp.numRows * sizeof( memRow_t ) );
			free( mp.rows );
		}
		mp.rows = nr;
		mp.capRows = newCap;
	}

	MemProf_Census( NULL, NULL, 0, &t );

	r = &mp.rows[mp.numRows++];
	Com_Memset( r, 0, sizeof( *r ) );
	r->epochMs = MemProf_EpochMs();
	r->tMs = ri.Milliseconds();
	Q_strncpyz( r->event, event, sizeof( r->event ) );
	r->frame = tr.frameCount;
	r->bench = ri.Cvar_VariableIntegerValue( "cl_benchMeasuring" );
	r->tex = t.tex;
	r->rt = t.rt;
	r->rb = t.rb;
	r->buf = t.buf;
	r->est = t.est;
	r->numImages = tr.numImages;
	MemProf_Nvx( &r->nvxAvailKb, &r->nvxEvictCount, &r->nvxEvictedKb );
}

/*
===============
R_MemProfEvent

A labelled timeline row (load stages). No-op when com_memProfile is 0.
===============
*/
void R_MemProfEvent( const char *event ) {
	if ( !R_MemProfOn() ) {
		return;
	}
	MemProf_AddRow( event );
}

/*
===============
R_MemProfFrame

Once per frame from RE_BeginFrame: bench window edges and the periodic timeline rate.
===============
*/
void R_MemProfFrame( void ) {
	int bench, now, interval;

	if ( !R_MemProfOn() ) {
		return;
	}

	bench = ri.Cvar_VariableIntegerValue( "cl_benchMeasuring" ) != 0;
	if ( bench && !mp.lastBench ) {
		mp.benchStartFrame = tr.frameCount;
		MemProf_AddRow( "bench_start" );
	} else if ( !bench && mp.lastBench ) {
		MemProf_AddRow( "bench_stop" );
	}
	mp.lastBench = bench;

	interval = mp.interval->integer;
	if ( interval < 50 ) {
		interval = 50;
	}
	now = ri.Milliseconds();
	if ( now - mp.lastSampleMs >= interval ) {
		mp.lastSampleMs = now;
		MemProf_AddRow( "periodic" );
	}
}

//===========================================================================
// output

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
		if ( !nb ) {
			return;
		}
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

// csv/json safe copy of a name
static const char *MemProf_Clean( const char *s, char *buf, int size ) {
	int i;

	Q_strncpyz( buf, s, size );
	for ( i = 0; buf[i]; i++ ) {
		if ( buf[i] == ',' || buf[i] == '"' || buf[i] == '\\' || buf[i] == '\n' || buf[i] == '\r' ) {
			buf[i] = '_';
		}
	}
	return buf;
}

static int MemProf_CmpObj( const void *a, const void *b ) {
	const memObj_t *oa = (const memObj_t *)a;
	const memObj_t *ob = (const memObj_t *)b;

	if ( oa->bytes != ob->bytes ) {
		return oa->bytes < ob->bytes ? 1 : -1;
	}
	return strcmp( oa->name, ob->name );
}

static int MemProf_CmpCat( const void *a, const void *b ) {
	const memCat_t *ca = (const memCat_t *)a;
	const memCat_t *cb = (const memCat_t *)b;

	if ( ca->bytes != cb->bytes ) {
		return ca->bytes < cb->bytes ? 1 : -1;
	}
	return strcmp( ca->name, cb->name );
}

static int MemProf_BuildCats( const memObj_t *objs, int n, memCat_t *cats ) {
	int numCats = 0, i, j;

	for ( i = 0; i < n; i++ ) {
		const memObj_t *o = &objs[i];
		long long b = o->bytes > 0 ? o->bytes : 0;

		for ( j = 0; j < numCats; j++ ) {
			if ( !strcmp( cats[j].name, o->category ) ) {
				break;
			}
		}
		if ( j == numCats ) {
			if ( numCats >= MEMPROF_MAX_CATS ) {
				continue;
			}
			cats[numCats].name = o->category;
			cats[numCats].count = 0;
			cats[numCats].bytes = 0;
			cats[numCats].bytesUsed = 0;
			numCats++;
		}
		cats[j].count++;
		cats[j].bytes += b;
		// objects other than textures are resident all the time; textures count when bound in the bench window
		// (all of them while the bench window was never seen)
		if ( o->used != 0 ) {
			cats[j].bytesUsed += b;
		}
	}
	qsort( cats, numCats, sizeof( cats[0] ), MemProf_CmpCat );
	return numCats;
}

static memObj_t *MemProf_CensusAlloc( int *max ) {
	// images + up to 20 renderbuffers per fbo + vaos + 3 estimates
	*max = tr.numImages + tr.numFBOs * 20 + tr.numVaos + 8;
	return malloc( *max * sizeof( memObj_t ) );
}

static void MemProf_JsonStr( const char *key, const char *val, qboolean comma ) {
	char buf[256];

	MemProf_Clean( val, buf, sizeof( buf ) );
	Out_Printf( "  \"%s\": \"%s\"%s\n", key, buf, comma ? "," : "" );
}

/*
===============
R_MemProfDump

Census now, then write the objects, categories, timeline and meta files.
===============
*/
void R_MemProfDump( const char *why ) {
	memObj_t *objs;
	memCat_t cats[MEMPROF_MAX_CATS];
	memTotals_t t;
	int max, n = 0, numCats, i;
	long long total, totalUsed = 0;
	char nameBuf[MAX_QPATH + 16];
	int dedKb = -1;

	if ( !why || !why[0] ) {
		why = "dump";
	}
	if ( !tr.registered ) {
		ri.Printf( PRINT_ALL, "vramprof: renderer not registered, nothing to dump\n" );
		return;
	}
	if ( R_MemProfOn() ) {
		MemProf_AddRow( why );
	}

	objs = MemProf_CensusAlloc( &max );
	if ( !objs ) {
		ri.Printf( PRINT_WARNING, "vramprof: out of memory\n" );
		return;
	}
	MemProf_Census( objs, &n, max, &t );
	qsort( objs, n, sizeof( objs[0] ), MemProf_CmpObj );
	numCats = MemProf_BuildCats( objs, n, cats );
	total = t.tex + t.rt + t.rb + t.buf + t.est;
	for ( i = 0; i < numCats; i++ ) {
		totalUsed += cats[i].bytesUsed;
	}
	mp.unknownTotal = t.numUnknown;

	// objects
	Out_Printf( "kind,category,map,pak,name,width,height,levels,faces,samples,format,bytes,used_in_bench,note\n" );
	for ( i = 0; i < n; i++ ) {
		const memObj_t *o = &objs[i];
		char pak[64];

		Out_Printf( "%s,%s,%s,%s,%s,%d,%d,%d,%d,%d,%s,%lld,%d,%s\n",
					o->kind, o->category, o->map, MemProf_Clean( MemProf_PakName( o->pak ), pak, sizeof( pak ) ),
					MemProf_Clean( o->name, nameBuf, sizeof( nameBuf ) ),
					o->width, o->height, o->levels, o->faces, o->samples, o->format, o->bytes, o->used,
					o->note ? o->note : "" );
	}
	Out_Write( "mem_vram_objects.csv" );

	// categories
	Out_Printf( "category,count,bytes,mb,pct,bytes_used_in_bench\n" );
	for ( i = 0; i < numCats; i++ ) {
		Out_Printf( "%s,%d,%lld,%.2f,%.2f,%lld\n", cats[i].name, cats[i].count, cats[i].bytes,
					cats[i].bytes / 1048576.0, total > 0 ? 100.0 * cats[i].bytes / total : 0.0, cats[i].bytesUsed );
	}
	if ( t.numUnknown ) {
		Out_Printf( "unknown_format,%d,0,0.00,0.00,0\n", t.numUnknown );
	}
	Out_Write( "mem_vram_categories.csv" );

	// timeline
	Out_Printf( "epoch_ms,t_ms,event,frame,bench,tex,rt,rb,buf,est,total,num_images,nvx_avail_kb,nvx_evict_count,nvx_evicted_kb\n" );
	for ( i = 0; i < mp.numRows; i++ ) {
		const memRow_t *r = &mp.rows[i];

		Out_Printf( "%lld,%d,%s,%d,%d,%lld,%lld,%lld,%lld,%lld,%lld,%d,%d,%d,%d\n", r->epochMs, r->tMs, r->event,
					r->frame, r->bench, r->tex, r->rt, r->rb, r->buf, r->est,
					r->tex + r->rt + r->rb + r->buf + r->est, r->numImages, r->nvxAvailKb, r->nvxEvictCount,
					r->nvxEvictedKb );
	}
	Out_Write( "mem_vram_timeline.csv" );

	// meta
	if ( glRefConfig.memInfo == MI_NVX && qglGetIntegerv ) {
		GLint v = 0;

		qglGetIntegerv( GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &v );
		dedKb = v;
	}
	Out_Printf( "{\n" );
	MemProf_JsonStr( "dump_reason", why, qtrue );
	Out_Printf( "  \"width\": %d,\n  \"height\": %d,\n", glConfig.vidWidth, glConfig.vidHeight );
	MemProf_JsonStr( "gl_renderer", glConfig.renderer_string, qtrue );
	Out_Printf( "  \"nvx_dedicated_kb\": %d,\n", dedKb );
	Out_Printf( "  \"bench_start_frame\": %d,\n", mp.benchStartFrame );
	Out_Printf( "  \"frame\": %d,\n", tr.frameCount );
	Out_Printf( "  \"num_images\": %d,\n  \"num_fbos\": %d,\n  \"num_vaos\": %d,\n", tr.numImages, tr.numFBOs, tr.numVaos );
	Out_Printf( "  \"unknown_format_objects\": %d,\n", t.numUnknown );
	Out_Printf( "  \"accounted_bytes\": %lld,\n  \"accounted_bytes_used_in_bench\": %lld,\n", total, totalUsed );
	Out_Printf( "  \"dropped_timeline_rows\": %d,\n", mp.droppedRows );
	Out_Printf( "  \"profiler_overhead_bytes\": %lld,\n", (long long)mp.capRows * (long long)sizeof( memRow_t ) );
	Out_Printf( "  \"cvars\": {\n" );
	Out_Printf( "    \"r_ext_compressed_textures\": \"%d\",\n", r_ext_compressed_textures->integer );
	Out_Printf( "    \"r_picmip\": \"%d\",\n", r_picmip->integer );
	Out_Printf( "    \"r_cubemapSize\": \"%d\",\n", r_cubemapSize->integer );
	Out_Printf( "    \"r_shadowMapSize\": \"%d\",\n", r_shadowMapSize->integer );
	Out_Printf( "    \"r_renderScale\": \"%s\",\n", r_renderScale->string );
	Out_Printf( "    \"r_ext_framebuffer_multisample\": \"%d\",\n", r_ext_framebuffer_multisample->integer );
	Out_Printf( "    \"r_smaa\": \"%d\",\n", r_smaa->integer );
	Out_Printf( "    \"r_ssao\": \"%d\",\n", r_ssao->integer );
	Out_Printf( "    \"r_hdr\": \"%d\",\n", r_hdr->integer );
	Out_Printf( "    \"r_sunShadows\": \"%d\",\n", r_sunShadows->integer );
	Out_Printf( "    \"r_dlightMode\": \"%d\",\n", r_dlightMode->integer );
	{
		char hm[64];

		MemProf_Clean( ri.Cvar_Get( "com_hunkMegs", "", 0 )->string, hm, sizeof( hm ) );
		Out_Printf( "    \"com_hunkMegs\": \"%s\"\n", hm );
	}
	Out_Printf( "  }\n}\n" );
	Out_Write( "mem_meta.json" );

	Out_Free();
	free( objs );
	mp.dumped = qtrue;

	ri.Printf( PRINT_ALL, "vramprof: wrote profiling/mem_vram_*.csv, mem_meta.json (%s): %.1f MB accounted, %d objects, %d unknown format\n",
			   why, total / 1048576.0, n, t.numUnknown );
}

/*
===============
R_MemProfPrint

Console summary: categories and the top objects.
===============
*/
static void R_MemProfPrint( void ) {
	memObj_t *objs;
	memCat_t cats[MEMPROF_MAX_CATS];
	memTotals_t t;
	int max, n = 0, numCats, i;
	long long total;

	if ( !tr.registered ) {
		ri.Printf( PRINT_ALL, "vramprof: renderer not registered\n" );
		return;
	}
	objs = MemProf_CensusAlloc( &max );
	if ( !objs ) {
		return;
	}
	MemProf_Census( objs, &n, max, &t );
	qsort( objs, n, sizeof( objs[0] ), MemProf_CmpObj );
	numCats = MemProf_BuildCats( objs, n, cats );
	total = t.tex + t.rt + t.rb + t.buf + t.est;

	ri.Printf( PRINT_ALL, "vramprof: %.1f MB accounted (tex %.1f, rt %.1f, rb %.1f, buf %.1f, est %.1f), %d objects, %d unknown format\n",
			   total / 1048576.0, t.tex / 1048576.0, t.rt / 1048576.0, t.rb / 1048576.0, t.buf / 1048576.0,
			   t.est / 1048576.0, n, t.numUnknown );
	ri.Printf( PRINT_ALL, "%-24s %6s %10s %6s\n", "category", "count", "MB", "pct" );
	for ( i = 0; i < numCats; i++ ) {
		ri.Printf( PRINT_ALL, "%-24s %6d %10.2f %5.1f%%\n", cats[i].name, cats[i].count, cats[i].bytes / 1048576.0,
				   total > 0 ? 100.0 * cats[i].bytes / total : 0.0 );
	}
	ri.Printf( PRINT_ALL, "top objects:\n" );
	for ( i = 0; i < n && i < 20; i++ ) {
		ri.Printf( PRINT_ALL, "%9.2f MB  %-20s %5dx%-5d %-14s %s\n", objs[i].bytes / 1048576.0, objs[i].category,
				   objs[i].width, objs[i].height, objs[i].format, objs[i].name );
	}
	free( objs );
}

/*
===============
R_MemProf_f

vramprof [print|dump [why]]
===============
*/
void R_MemProf_f( void ) {
	const char *sub = ri.Cmd_Argc() > 1 ? ri.Cmd_Argv( 1 ) : "print";

	if ( !Q_stricmp( sub, "dump" ) ) {
		R_MemProfDump( ri.Cmd_Argc() > 2 ? ri.Cmd_Argv( 2 ) : "dump" );
	} else if ( !Q_stricmp( sub, "print" ) ) {
		R_MemProfPrint();
	} else {
		ri.Printf( PRINT_ALL, "usage: vramprof [print|dump [why]]\n" );
	}
}

/*
===============
R_ShutdownMemProf

Fallback dump at the final renderer shutdown when nothing was dumped yet (call before the FBOs and textures
are deleted), then frees the timeline. A map-change shutdown keeps the timeline: it spans loads.
===============
*/
void R_ShutdownMemProf( qboolean destroyWindow ) {
	if ( !destroyWindow ) {
		return;
	}
	if ( R_MemProfOn() && !mp.dumped ) {
		R_MemProfDump( "shutdown" );
	}
	if ( mp.rows ) {
		free( mp.rows );
	}
	mp.rows = NULL;
	mp.numRows = mp.capRows = 0;
}
