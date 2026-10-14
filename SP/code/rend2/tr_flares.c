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

// tr_flares.c

#include "tr_local.h"

/*
=============================================================================

LIGHT FLARES

A light flare is an effect that takes place inside the eye when bright light
sources are visible.  The size of the flare relative to the screen is nearly
constant, irrespective of distance, but the intensity should be proportional to the
projected area of the light source.

A surface that has been flagged as having a light flare will calculate the depth
buffer value that its midpoint should have when the surface is added.

After all opaque surfaces have been rendered, the depth buffer is read back for
each flare in view.  If the point has not been obscured by a closer surface, the
flare should be drawn.

Surfaces that have a repeated texture should never be flagged as flaring, because
there will only be a single flare added at the midpoint of the polygon.

To prevent abrupt popping, the intensity of the flare is interpolated up and
down as it changes visibility.  This involves scene to scene state, unlike almost
all other aspects of the renderer, and is complicated by the fact that a single
frame may have multiple scenes.

RB_RenderFlares() will be called once per view (twice in a mirrored scene, potentially
up to five or more times in a frame with 3D status bar icons).

=============================================================================
*/


// flare states maintain visibility over multiple frames for fading
// layers: view, mirror, menu
typedef struct flare_s {
	struct      flare_s *next;      // for active chain

	int addedFrame;

	qboolean inPortal;                  // true if in a portal view of the scene
	int frameSceneNum;
	void        *surface;
	int fogNum;

	int fadeTime;

	int flags;
	// for coronas, the client determines current visibility, but it's still inserted so it will fade out properly

	qboolean visible;               // state of last test
	float drawIntensity;            // may be non 0 even if !visible due to fading

	int windowX, windowY;
	float eyeZ;

	vec3_t origin;
	vec3_t color;
	float scale;

	int id;
} flare_t;

#define     MAX_FLARES      256

flare_t r_flareStructs[MAX_FLARES];
flare_t     *r_activeFlares, *r_inactiveFlares;

/*
Flare visibility via occlusion queries (r_flaresAsync 1).

The stock test reads the depth buffer back with one glReadPixels per flare, which makes
the GPU drain its pipeline to resolve the depth surface. Measured 2026-10-14: ~3 tests a
frame cost 10% FPS and left the GPU ~10% idle, even when read asynchronously into a pixel
pack buffer. Instead each test draws a one-pixel quad, depth-tested at the flare's depth
pulled 24 units toward the viewer (the stock tolerance), inside a GL_SAMPLES_PASSED query.
Results are read only once available, so visibility lags a frame or two (the fade hides it).
Results are keyed by flare identity, not flare_t slot, because a hidden flare's slot is
freed every frame and a slot-keyed result would never reach the next one.
*/
#define FLARE_QUERIES   ( MAX_FLARES * 2 )
#define FLARE_VIS_MAX_AGE 8     // frames; older results count as missing

typedef struct {
	int id, sceneNum;
	qboolean inPortal;
} flareKey_t;

typedef struct {
	GLuint query;
	qboolean pending;
	flareKey_t key;
} flareQuery_t;

typedef struct {
	flareKey_t key;
	qboolean visible;
	int frame;      // backEnd.viewParms.frameCount the result arrived
} flareVis_t;

static struct {
	qboolean init;
	flareQuery_t q[FLARE_QUERIES];
	int next;
	flareVis_t vis[MAX_FLARES];
	int numVis;
	qboolean active;    // queries in use for the view being tested
} flareOcc;

static qboolean FlareKeyMatch( const flareKey_t *k, int id, int sceneNum, qboolean inPortal ) {
	return k->id == id && k->sceneNum == sceneNum && k->inPortal == inPortal;
}

static void R_StoreFlareVis( const flareKey_t *key, qboolean visible ) {
	flareVis_t *v, *oldest = &flareOcc.vis[0];
	int i;

	for ( i = 0; i < flareOcc.numVis; i++ ) {
		v = &flareOcc.vis[i];
		if ( FlareKeyMatch( &v->key, key->id, key->sceneNum, key->inPortal ) ) {
			break;
		}
		if ( v->frame < oldest->frame ) {
			oldest = v;
		}
	}
	if ( i < flareOcc.numVis ) {
		v = &flareOcc.vis[i];
	} else if ( flareOcc.numVis < MAX_FLARES ) {
		v = &flareOcc.vis[flareOcc.numVis++];
	} else {
		v = oldest;
	}
	v->key = *key;
	v->visible = visible;
	v->frame = backEnd.viewParms.frameCount;
}

// newest finished result for this flare; qfalse (hidden) until one has arrived
static qboolean R_FlareOccVisible( const flare_t *f ) {
	int i;

	for ( i = 0; i < flareOcc.numVis; i++ ) {
		flareVis_t *v = &flareOcc.vis[i];

		if ( FlareKeyMatch( &v->key, f->id, f->frameSceneNum, f->inPortal ) ) {
			return v->visible && backEnd.viewParms.frameCount - v->frame <= FLARE_VIS_MAX_AGE;
		}
	}
	return qfalse;
}

static void RB_HarvestFlareQueries( void ) {
	int i;

	for ( i = 0; i < FLARE_QUERIES; i++ ) {
		flareQuery_t *q = &flareOcc.q[( flareOcc.next + i ) % FLARE_QUERIES];     // oldest first
		GLint available = 0;
		GLuint samples = 0;

		if ( !q->pending ) {
			continue;
		}
		qglGetQueryObjectiv( q->query, GL_QUERY_RESULT_AVAILABLE, &available );
		if ( !available ) {
			continue;
		}
		qglGetQueryObjectuiv( q->query, GL_QUERY_RESULT, &samples );
		q->pending = qfalse;
		R_StoreFlareVis( &q->key, samples > 0 );
	}
}

/*
==================
R_ShutdownFlareQueries

Called before the GL context goes away (RE_Shutdown).
==================
*/
void R_ShutdownFlareQueries( void ) {
	int i;

	if ( flareOcc.init ) {
		for ( i = 0; i < FLARE_QUERIES; i++ ) {
			qglDeleteQueries( 1, &flareOcc.q[i].query );
		}
	}
	Com_Memset( &flareOcc, 0, sizeof( flareOcc ) );
}

// issue the occlusion query for one flare; the view's projection is in backEnd.viewParms
static void RB_QueryFlare( flare_t *f ) {
	flareQuery_t *q = &flareOcc.q[flareOcc.next];
	const float *p = backEnd.viewParms.projectionMatrix;
	float x, y, dx, dy, z, zEye;
	vec4_t quad[4];

	if ( q->pending ) {
		return;     // GPU is far behind; keep the last result for this flare
	}
	flareOcc.next = ( flareOcc.next + 1 ) % FLARE_QUERIES;

	// same tolerance as the readback test: hidden only when the depth buffer is more than
	// 24 units in front of the flare
	zEye = f->eyeZ + 24;
	if ( zEye > -r_znear->value ) {
		z = -1.0f;
	} else {
		z = ( p[10] * zEye + p[14] ) / ( p[11] * zEye );
	}
	x = ( f->windowX + 0.5f - backEnd.viewParms.viewportX ) * 2.0f / backEnd.viewParms.viewportWidth - 1.0f;
	y = ( f->windowY + 0.5f - backEnd.viewParms.viewportY ) * 2.0f / backEnd.viewParms.viewportHeight - 1.0f;
	dx = 1.0f / backEnd.viewParms.viewportWidth;    // half a pixel: covers exactly this pixel's center
	dy = 1.0f / backEnd.viewParms.viewportHeight;
	VectorSet4( quad[0], x - dx, y - dy, z, 1 );
	VectorSet4( quad[1], x + dx, y - dy, z, 1 );
	VectorSet4( quad[2], x + dx, y + dy, z, 1 );
	VectorSet4( quad[3], x - dx, y + dy, z, 1 );

	q->key.id = f->id;
	q->key.sceneNum = f->frameSceneNum;
	q->key.inPortal = f->inPortal;
	q->pending = qtrue;
	qglBeginQuery( GL_SAMPLES_PASSED, q->query );
	RB_InstantQuad( quad );
	qglEndQuery( GL_SAMPLES_PASSED );
}

static void RB_BeginFlareQueries( mat4_t oldprojection, mat4_t oldmodelview ) {
	mat4_t identity;
	int i;

	if ( !flareOcc.init ) {
		for ( i = 0; i < FLARE_QUERIES; i++ ) {
			qglGenQueries( 1, &flareOcc.q[i].query );
		}
		flareOcc.init = qtrue;
	}
	RB_HarvestFlareQueries();

	if ( tess.numIndexes ) {
		RB_EndSurface();
	}
	Mat4Copy( glState.projection, oldprojection );
	Mat4Copy( glState.modelview, oldmodelview );
	Mat4Identity( identity );
	GL_SetModelviewMatrix( identity );
	GL_SetProjectionMatrix( identity );     // quads are given in NDC
	GL_BindToTMU( tr.whiteImage, TB_COLORMAP );
	GL_State( 0 );      // depth test LEQUAL, no depth write, no blend
	GL_Cull( CT_TWO_SIDED );
	qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	flareOcc.active = qtrue;
}

static void RB_EndFlareQueries( mat4_t oldprojection, mat4_t oldmodelview ) {
	qglColorMask( !backEnd.colorMask[0], !backEnd.colorMask[1], !backEnd.colorMask[2], !backEnd.colorMask[3] );
	GL_SetProjectionMatrix( oldprojection );
	GL_SetModelviewMatrix( oldmodelview );
	flareOcc.active = qfalse;
}

int flareCoeff;

/*
==================
R_SetFlareCoeff
==================
*/
static void R_SetFlareCoeff( void ) {

	if(r_flareCoeff->value == 0.0f)
		flareCoeff = atof(FLARE_STDCOEFF);
	else
		flareCoeff = r_flareCoeff->value;
}

/*
==================
R_ClearFlares
==================
*/
void R_ClearFlares( void ) {
	int i;

	memset( r_flareStructs, 0, sizeof( r_flareStructs ) );
	r_activeFlares = NULL;
	r_inactiveFlares = NULL;

	for ( i = 0 ; i < MAX_FLARES ; i++ ) {
		r_flareStructs[i].next = r_inactiveFlares;
		r_inactiveFlares = &r_flareStructs[i];
	}

	R_SetFlareCoeff();
}


/*
==================
RB_AddFlare

This is called at surface tesselation time
==================
*/
void RB_AddFlare( void *surface, int fogNum, vec3_t point, vec3_t color, float scale, vec3_t normal, int id, int flags ) {  //----(SA)	added scale. added id.  added visible
	int i;
	flare_t         *f;
	vec3_t local;
	float d = 1;
	vec4_t eye, clip, normalized, window;

	backEnd.pc.c_flareAdds++;

	if(normal && (normal[0] || normal[1] || normal[2]))
	{
		VectorSubtract( backEnd.viewParms.or.origin, point, local );
		VectorNormalizeFast(local);
		d = DotProduct(local, normal);

		// If the viewer is behind the flare don't add it.
		if(d < 0)
			return;
	}

	// if the point is off the screen, don't bother adding it
	// calculate screen coordinates and depth
	R_TransformModelToClip( point, backEnd.or.modelMatrix,
							backEnd.viewParms.projectionMatrix, eye, clip );

	//ri.Printf(PRINT_ALL, "src:  %f  %f  %f  \n", point[0], point[1], point[2]);
	//ri.Printf(PRINT_ALL, "eye:  %f  %f  %f  %f\n", eye[0], eye[1], eye[2], eye[3]);

	// check to see if the point is completely off screen
	for ( i = 0 ; i < 3 ; i++ ) {
		if ( clip[i] >= clip[3] || clip[i] <= -clip[3] ) {
			return;
		}
	}

	R_TransformClipToWindow( clip, &backEnd.viewParms, normalized, window );

	//ri.Printf(PRINT_ALL, "window:  %f  %f  %f  \n", window[0], window[1], window[2]);

	if ( window[0] < 0 || window[0] >= backEnd.viewParms.viewportWidth
		 || window[1] < 0 || window[1] >= backEnd.viewParms.viewportHeight ) {
		return; // shouldn't happen, since we check the clip[] above, except for FP rounding
	}

	// see if a flare with a matching surface, scene, and view exists
	for ( f = r_activeFlares ; f ; f = f->next ) {
//		if ( f->surface == surface && f->frameSceneNum == backEnd.viewParms.frameSceneNum && f->inPortal == backEnd.viewParms.isPortal ) {

		// (SA) added back in more checks for different scenes
		if ( f->id == id && f->frameSceneNum == backEnd.viewParms.frameSceneNum && f->inPortal == backEnd.viewParms.isPortal ) {
			break;
		}
	}

	// allocate a new one
	if ( !f ) {
		if ( !r_inactiveFlares ) {
			// the list is completely full
			return;
		}
		f = r_inactiveFlares;
		r_inactiveFlares = r_inactiveFlares->next;
		f->next = r_activeFlares;
		r_activeFlares = f;

		f->surface = surface;
		f->frameSceneNum = backEnd.viewParms.frameSceneNum;
		f->inPortal = backEnd.viewParms.isPortal;
		f->addedFrame = -1;
		f->id = id;
	}

	f->flags = flags;

	if ( f->addedFrame != backEnd.viewParms.frameCount - 1 ) {
		f->visible = qfalse;
		f->fadeTime = backEnd.refdef.time - 2000;
	}

	f->addedFrame = backEnd.viewParms.frameCount;
	f->fogNum = fogNum;

	VectorCopy(point, f->origin);
	VectorCopy( color, f->color );

	f->scale = scale;   //----(SA)

	// fade the intensity of the flare down as the
	// light surface turns away from the viewer
	VectorScale( f->color, d, f->color );

	// save info needed to test
	f->windowX = backEnd.viewParms.viewportX + window[0];
	f->windowY = backEnd.viewParms.viewportY + window[1];

	f->eyeZ = eye[2];
}

/*
==================
RB_AddDlightFlares
==================
*/
void RB_AddDlightFlares( void ) {
	dlight_t        *l;
	int i, j, k;
	int id = 0;
	fog_t           *fog = NULL;

	if ( r_flares->integer < 2 ) {
		return;
	}

	l = backEnd.refdef.dlights;

	if(tr.world)
		fog = tr.world->fogs;

	for ( i = 0 ; i < backEnd.refdef.num_dlights ; i++, l++ ) {

		// find which fog volume the light is in
		if(fog)
		{
			// find which fog volume the light is in 
			for ( j = 1 ; j < tr.world->numfogs ; j++ ) {
				fog = &tr.world->fogs[j];
				for ( k = 0 ; k < 3 ; k++ ) {
					if ( l->origin[k] < fog->bounds[0][k] || l->origin[k] > fog->bounds[1][k] ) {
						break;
					}
				}
				if ( k == 3 ) {
					break;
				}
			}
			if ( j == tr.world->numfogs ) {
				j = 0;
			}
		}
		else
			j = 0;

		RB_AddFlare( (void *)l, j, l->origin, l->color, 1.0f, NULL, id++, qtrue );  //----(SA)	also set scale
	}
}


/*
==============
RB_AddCoronaFlares
==============
*/
void RB_AddCoronaFlares( void ) {
	corona_t        *cor;
	int i, j, k;
	fog_t           *fog;

	if ( r_flares->integer != 1 && r_flares->integer != 3 ) {
		return;
	}

	if ( !( tr.world ) ) { // (SA) possible currently at the player model selection menu
		return;
	}

	cor = backEnd.refdef.coronas;

	for ( i = 0 ; i < backEnd.refdef.num_coronas ; i++, cor++ ) {

		// find which fog volume the corona is in
		for ( j = 1 ; j < tr.world->numfogs ; j++ ) {
			fog = &tr.world->fogs[j];
			for ( k = 0 ; k < 3 ; k++ ) {
				if ( cor->origin[k] < fog->bounds[0][k] || cor->origin[k] > fog->bounds[1][k] ) {
					break;
				}
			}
			if ( k == 3 ) {
				break;
			}
		}
		if ( j == tr.world->numfogs ) {
			j = 0;
		}
		RB_AddFlare( (void *)cor, j, cor->origin, cor->color, cor->scale, NULL, cor->id, cor->flags );
	}
}

/*
===============================================================================

FLARE BACK END

===============================================================================
*/

/*
==================
RB_TestFlare
==================
*/
void RB_TestFlare( flare_t *f ) {
	float			depth;
	qboolean		visible;
	float			fade;
	float			screenZ;
	FBO_t           *oldFbo;

	backEnd.pc.c_flareTests++;

	visible = (qboolean)( f->flags & 1 );

	if ( flareOcc.active ) {
		RB_QueryFlare( f );
		if ( !R_FlareOccVisible( f ) ) {
			visible = qfalse;
		}
	} else {
		// doing a readpixels is as good as doing a glFinish(), so
		// don't bother with another sync
		glState.finishCalled = qfalse;

		// if we're doing multisample rendering, read from the correct FBO
		oldFbo = glState.currentFBO;
		if (tr.msaaResolveFbo)
		{
			FBO_Bind(tr.msaaResolveFbo);
		}

		// read back the z buffer contents
		qglReadPixels( f->windowX, f->windowY, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth );

		// if we're doing multisample rendering, switch to the old FBO
		if (tr.msaaResolveFbo)
		{
			FBO_Bind(oldFbo);
		}

		screenZ = backEnd.viewParms.projectionMatrix[14] /
			( ( 2*depth - 1 ) * backEnd.viewParms.projectionMatrix[11] - backEnd.viewParms.projectionMatrix[10] );

		if ( -f->eyeZ - -screenZ  > 24 )
			visible = qfalse;
	}

	if ( visible ) {
		if ( !f->visible ) {
			f->visible = qtrue;
			f->fadeTime = backEnd.refdef.time - 1;
		}
		fade = ( ( backEnd.refdef.time - f->fadeTime ) /1000.0f ) * r_flareFade->value;
	} else {
		if ( f->visible ) {
			f->visible = qfalse;
			f->fadeTime = backEnd.refdef.time - 1;
		}
		fade = 1.0f - ( ( backEnd.refdef.time - f->fadeTime ) / 1000.0f ) * r_flareFade->value;
	}

	if ( fade < 0 ) {
		fade = 0;
	}
	if ( fade > 1 ) {
		fade = 1;
	}

	f->drawIntensity = fade;
}


/*
==================
RB_RenderFlare
==================
*/
void RB_RenderFlare( flare_t *f ) {
	float size;
	vec3_t color;
	int iColor[3];
	float distance, intensity, factor;
	byte fogFactors[3] = {255, 255, 255};

	backEnd.pc.c_flareRenders++;

	// We don't want too big values anyways when dividing by distance.
	if(f->eyeZ > -1.0f)
		distance = 1.0f;
	else
		distance = -f->eyeZ;

	// calculate the flare size..
	size = backEnd.viewParms.viewportWidth * ( r_flareSize->value/640.0f + 8 / distance );

/*
 * This is an alternative to intensity scaling. It changes the size of the flare on screen instead
 * with growing distance. See in the description at the top why this is not the way to go.
	// size will change ~ 1/r.
	size = backEnd.viewParms.viewportWidth * (r_flareSize->value / (distance * -2.0f));
*/

/*
 * As flare sizes stay nearly constant with increasing distance we must decrease the intensity
 * to achieve a reasonable visual result. The intensity is ~ (size^2 / distance^2) which can be
 * got by considering the ratio of
 * (flaresurface on screen) : (Surface of sphere defined by flare origin and distance from flare)
 * An important requirement is:
 * intensity <= 1 for all distances.
 *
 * The formula used here to compute the intensity is as follows:
 * intensity = flareCoeff * size^2 / (distance + size*sqrt(flareCoeff))^2
 * As you can see, the intensity will have a max. of 1 when the distance is 0.
 * The coefficient flareCoeff will determine the falloff speed with increasing distance.
*/

	factor = distance + size * sqrt(flareCoeff);
	
	intensity = flareCoeff * size * size / (factor * factor);

	VectorScale(f->color, f->drawIntensity * intensity, color);

	// Calculations for fogging
	if(tr.world && f->fogNum > 0 && f->fogNum < tr.world->numfogs)
	{
		tess.numVertexes = 1;
		VectorCopy(f->origin, tess.xyz[0]);
		tess.fogNum = f->fogNum;
	
		RB_CalcModulateColorsByFog(fogFactors);
		
		// We don't need to render the flare if colors are 0 anyways.
		if(!(fogFactors[0] || fogFactors[1] || fogFactors[2]))
			return;
	}

	iColor[0] = color[0] * fogFactors[0] * 257;
	iColor[1] = color[1] * fogFactors[1] * 257;
	iColor[2] = color[2] * fogFactors[2] * 257;

	if ( f->flags & 2 ) {  // spotlight flare
		RB_BeginSurface( tr.spotFlareShader, f->fogNum, 0 );
	} else {
		RB_BeginSurface( tr.flareShader, f->fogNum, 0 );
	}

	// FIXME: use quadstamp?
	tess.xyz[tess.numVertexes][0] = f->windowX - size;
	tess.xyz[tess.numVertexes][1] = f->windowY - size;
	tess.texCoords[tess.numVertexes][0] = 0;
	tess.texCoords[tess.numVertexes][1] = 0;
	tess.color[tess.numVertexes][0] = iColor[0];
	tess.color[tess.numVertexes][1] = iColor[1];
	tess.color[tess.numVertexes][2] = iColor[2];
//	tess.color[tess.numVertexes][3] = 65535;			//rend2
	tess.color[tess.numVertexes][3] = f->drawIntensity * 65535;	//----(SA)	mod for alpha blend rather than additive
	tess.numVertexes++;

	tess.xyz[tess.numVertexes][0] = f->windowX - size;
	tess.xyz[tess.numVertexes][1] = f->windowY + size;
	tess.texCoords[tess.numVertexes][0] = 0;
	tess.texCoords[tess.numVertexes][1] = 1;
	tess.color[tess.numVertexes][0] = iColor[0];
	tess.color[tess.numVertexes][1] = iColor[1];
	tess.color[tess.numVertexes][2] = iColor[2];
//	tess.color[tess.numVertexes][3] = 65535;			// rend2
	tess.color[tess.numVertexes][3] = f->drawIntensity * 65535;	//----(SA)	mod for alpha blend rather than additive
	tess.numVertexes++;

	tess.xyz[tess.numVertexes][0] = f->windowX + size;
	tess.xyz[tess.numVertexes][1] = f->windowY + size;
	tess.texCoords[tess.numVertexes][0] = 1;
	tess.texCoords[tess.numVertexes][1] = 1;
	tess.color[tess.numVertexes][0] = iColor[0];
	tess.color[tess.numVertexes][1] = iColor[1];
	tess.color[tess.numVertexes][2] = iColor[2];
//	tess.color[tess.numVertexes][3] = 65535;			// rend2
	tess.color[tess.numVertexes][3] = f->drawIntensity * 65535;	//----(SA)	mod for alpha blend rather than additive
	tess.numVertexes++;

	tess.xyz[tess.numVertexes][0] = f->windowX + size;
	tess.xyz[tess.numVertexes][1] = f->windowY - size;
	tess.texCoords[tess.numVertexes][0] = 1;
	tess.texCoords[tess.numVertexes][1] = 0;
	tess.color[tess.numVertexes][0] = iColor[0];
	tess.color[tess.numVertexes][1] = iColor[1];
	tess.color[tess.numVertexes][2] = iColor[2];
//	tess.color[tess.numVertexes][3] = 65535;			// rend2
	tess.color[tess.numVertexes][3] = f->drawIntensity * 65535;	//----(SA)	mod for alpha blend rather than additive
	tess.numVertexes++;

	tess.indexes[tess.numIndexes++] = 0;
	tess.indexes[tess.numIndexes++] = 1;
	tess.indexes[tess.numIndexes++] = 2;
	tess.indexes[tess.numIndexes++] = 0;
	tess.indexes[tess.numIndexes++] = 2;
	tess.indexes[tess.numIndexes++] = 3;

	RB_EndSurface();
}

/*
==================
RB_RenderFlares

Because flares are simulating an occular effect, they should be drawn after
everything (all views) in the entire frame has been drawn.

Because of the way portals use the depth buffer to mark off areas, the
needed information would be lost after each view, so we are forced to draw
flares after each view.

The resulting artifact is that flares in mirrors or portals don't dim properly
when occluded by something in the main view, and portal flares that should
extend past the portal edge will be overwritten.
==================
*/
void RB_RenderFlares( void ) {
	flare_t     *f;
	flare_t     **prev;
	qboolean draw;
	mat4_t    oldmodelview, oldprojection, matrix;

	if ( !r_flares->integer ) {
		return;
	}

	if(r_flareCoeff->modified)
	{
		R_SetFlareCoeff();
		r_flareCoeff->modified = qfalse;
	}

	// Reset currentEntity to world so that any previously referenced entities
	// don't have influence on the rendering of these flares (i.e. RF_ renderer flags).
	backEnd.currentEntity = &tr.worldEntity;
	backEnd.or = backEnd.viewParms.world;

	// (SA) turned light flares back on.  must evaluate problem id had with this
	RB_AddDlightFlares();
	RB_AddCoronaFlares();

	if ( r_flaresAsync->integer && glRefConfig.occlusionQuery ) {
		RB_BeginFlareQueries( oldprojection, oldmodelview );
	}

	// perform z buffer readback on each flare in this view
	draw = qfalse;
	prev = &r_activeFlares;
	while ( ( f = *prev ) != NULL ) {
		// throw out any flares that weren't added last frame
		if ( f->addedFrame < backEnd.viewParms.frameCount - 1 ) {
			*prev = f->next;
			f->next = r_inactiveFlares;
			r_inactiveFlares = f;
			continue;
		}

		// don't draw any here that aren't from this scene / portal
		f->drawIntensity = 0;
		if ( f->frameSceneNum == backEnd.viewParms.frameSceneNum
			 && f->inPortal == backEnd.viewParms.isPortal ) {
			RB_TestFlare( f );
			if ( f->drawIntensity ) {
				draw = qtrue;
			} else {
				// this flare has completely faded out, so remove it from the chain
				*prev = f->next;
				f->next = r_inactiveFlares;
				r_inactiveFlares = f;
				continue;
			}
		}

		prev = &f->next;
	}

	if ( flareOcc.active ) {
		RB_EndFlareQueries( oldprojection, oldmodelview );
	}

	if ( !draw ) {
		return;     // none visible
	}

	Mat4Copy(glState.projection, oldprojection);
	Mat4Copy(glState.modelview, oldmodelview);
	Mat4Identity(matrix);
	GL_SetModelviewMatrix(matrix);
	Mat4Ortho( backEnd.viewParms.viewportX, backEnd.viewParms.viewportX + backEnd.viewParms.viewportWidth,
	               backEnd.viewParms.viewportY, backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight,
	               -99999, 99999, matrix );
	GL_SetProjectionMatrix(matrix);

	for ( f = r_activeFlares ; f ; f = f->next ) {
		if ( f->frameSceneNum == backEnd.viewParms.frameSceneNum
			 && f->inPortal == backEnd.viewParms.isPortal
			 && f->drawIntensity ) {
			RB_RenderFlare( f );
		}
	}

	GL_SetProjectionMatrix(oldprojection);
	GL_SetModelviewMatrix(oldmodelview);
}

