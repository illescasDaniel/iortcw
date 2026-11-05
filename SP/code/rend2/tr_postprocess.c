/*
===========================================================================
Copyright (C) 2011 Andrei Drexler, Richard Allen, James Canete

This file is part of Reaction source code.

Reaction source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Reaction source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Reaction source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "tr_local.h"

/*
=============
RB_SMAA

Subpixel Morphological Anti-Aliasing, 1x (Jimenez et al., http://www.iryoku.com/smaa/).
Runs on the tone-mapped image after tone mapping and before any 2D, so the HUD stays crisp.
  1. luma edge detection        srcFbo   -> smaaEdgesFbo
  2. blending weight calculation edges   -> smaaBlendFbo
  3. neighborhood blending       srcFbo+weights -> dstFbo
When srcFbo == dstFbo the blend goes through screenScratchFbo and is copied back. The tone
mapper already leaves its result in screenScratchFbo, so normally src is that and dst the
scene FBO, which saves two full-screen copies (~0.37 ms at 2560x1600).
r_smaa selects the preset (1 low .. 4 ultra); r_smaaDebug 1/2 shows the edges/weights buffer instead.
Returns qfalse if nothing was written to dstFbo.
=============
*/
qboolean RB_SMAA(FBO_t *srcFbo, FBO_t *dstFbo, ivec4_t srcBox)
{
	int preset = r_smaa->integer - 1;
	vec4_t metrics, predInfo;
	qboolean useStencil, predicate;

	if (preset < 0 || preset > 3)
		return qfalse;

	if (!tr.smaaAvailable || !srcFbo || !dstFbo || !tr.smaaEdgesFbo || !tr.smaaBlendFbo || !tr.screenScratchFbo ||
		srcFbo->width != tr.smaaEdgesFbo->width || srcFbo->height != tr.smaaEdgesFbo->height)
		return qfalse;

	useStencil = tr.smaaEdgesFbo->packedDepthStencilBuffer && r_smaaStencil->integer;

	metrics[0] = 1.0f / srcFbo->width;
	metrics[1] = 1.0f / srcFbo->height;
	metrics[2] = (float)srcFbo->width;
	metrics[3] = (float)srcFbo->height;

	// the edge shader discards pixels without an edge, so the target must start at zero
	FBO_Bind(tr.smaaEdgesFbo);
	qglScissor(0, 0, tr.smaaEdgesFbo->width, tr.smaaEdgesFbo->height);
	qglClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	if (useStencil)
	{
		// edge pixels get stencil 1 (discarded ones keep 0)
		qglClearStencil(0);
		qglStencilMask(0xff);
		qglClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		qglEnable(GL_STENCIL_TEST);
		qglStencilFunc(GL_ALWAYS, 1, 0xff);
		qglStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	}
	else
		qglClear(GL_COLOR_BUFFER_BIT);
	qglClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	// codmod: depth predication. The depth copy is valid when the main view ran a depth prepass; "off"
	// (scale 1, strength 0) gives exactly the plain threshold, so the same program serves both.
	predicate = r_smaaPredication->integer && tr.hdrDepthImage && r_depthPrepass->integer &&
		!(backEnd.refdef.rdflags & RDF_NOWORLDMODEL);
	VectorSet4(predInfo, backEnd.viewParms.zFar / r_znear->value, r_smaaPredThreshold->value,
		predicate ? r_smaaPredScale->value : 1.0f, predicate ? r_smaaPredStrength->value : 0.0f);
	GL_BindToTMU(predicate ? tr.hdrDepthImage : tr.whiteImage, TB_LEVELSMAP);

	GLSL_BindProgram(&tr.smaaEdgeShader[preset]);
	GLSL_SetUniformVec4(&tr.smaaEdgeShader[preset], UNIFORM_SMAAMETRICS, metrics);
	GLSL_SetUniformVec4(&tr.smaaEdgeShader[preset], UNIFORM_VIEWINFO, predInfo);
	FBO_Blit(srcFbo, srcBox, NULL, tr.smaaEdgesFbo, srcBox, &tr.smaaEdgeShader[preset], NULL, 0);
	RB_GpuMark( "smaa_edges" );

	if (r_smaaDebug->integer == 1)
	{
		if (useStencil)
			qglDisable(GL_STENCIL_TEST);
		FBO_FastBlit(tr.smaaEdgesFbo, srcBox, dstFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		return qtrue;
	}

	GL_BindToTMU(tr.smaaAreaImage,   TB_LEVELSMAP);
	GL_BindToTMU(tr.smaaSearchImage, TB_NORMALMAP);

	if (useStencil)
	{
		// the weights are only nonzero on edge pixels: clear to zero and shade just those
		// (the stencil optimisation from the SMAA reference implementation, same output)
		FBO_Bind(tr.smaaBlendFbo);
		qglClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		qglClear(GL_COLOR_BUFFER_BIT);
		qglClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		qglStencilFunc(GL_EQUAL, 1, 0xff);
		qglStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	}

	GLSL_BindProgram(&tr.smaaWeightsShader[preset]);
	GLSL_SetUniformVec4(&tr.smaaWeightsShader[preset], UNIFORM_SMAAMETRICS, metrics);
	FBO_Blit(tr.smaaEdgesFbo, srcBox, NULL, tr.smaaBlendFbo, srcBox, &tr.smaaWeightsShader[preset], NULL, 0);

	if (useStencil)
		qglDisable(GL_STENCIL_TEST);
	RB_GpuMark( "smaa_weights" );

	if (r_smaaDebug->integer == 2)
	{
		FBO_FastBlit(tr.smaaBlendFbo, srcBox, dstFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		return qtrue;
	}

	GL_BindToTMU(tr.smaaBlendImage, TB_LEVELSMAP);

	GLSL_BindProgram(&tr.smaaBlendShader);
	GLSL_SetUniformVec4(&tr.smaaBlendShader, UNIFORM_SMAAMETRICS, metrics);
	if (srcFbo != dstFbo)
	{
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, srcBox, &tr.smaaBlendShader, NULL, 0);
		RB_GpuMark( "smaa_blend" );
		return qtrue;
	}

	FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.smaaBlendShader, NULL, 0);
	RB_GpuMark( "smaa_blend" );
	FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	return qtrue;
}

/*
=============
SMAA T2x helpers: 4x4 column-major matrices in double precision
=============
*/
static void SmaaMatMul(const double *a, const double *b, double *out)
{
	int i, j, k;

	for (j = 0; j < 4; j++)
		for (i = 0; i < 4; i++)
		{
			double s = 0.0;
			for (k = 0; k < 4; k++)
				s += a[k * 4 + i] * b[j * 4 + k];
			out[j * 4 + i] = s;
		}
}

static qboolean SmaaMatInverse(const double *m, double *inv)
{
	double t[4][8];
	int i, j, k;

	// Gauss-Jordan on [m | I]; row i of t is row i of the matrix (m is column-major: m[col * 4 + row])
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
		{
			t[i][j] = m[j * 4 + i];
			t[i][j + 4] = (i == j) ? 1.0 : 0.0;
		}

	for (i = 0; i < 4; i++)
	{
		int pivot = i;
		double div;

		for (k = i + 1; k < 4; k++)
			if (fabs(t[k][i]) > fabs(t[pivot][i]))
				pivot = k;
		if (fabs(t[pivot][i]) < 1e-30)
			return qfalse;
		if (pivot != i)
			for (j = 0; j < 8; j++)
			{
				double tmp = t[i][j];
				t[i][j] = t[pivot][j];
				t[pivot][j] = tmp;
			}

		div = 1.0 / t[i][i];
		for (j = 0; j < 8; j++)
			t[i][j] *= div;

		for (k = 0; k < 4; k++)
		{
			double f;
			if (k == i)
				continue;
			f = t[k][i];
			for (j = 0; j < 8; j++)
				t[k][j] -= f * t[i][j];
		}
	}

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			inv[j * 4 + i] = t[i][j + 4];
	return qtrue;
}

// prevVP * inverse(curVP): current NDC (with depth) -> previous frame's NDC
static qboolean SmaaReprojection(const float *cur, const float *prev, float *out)
{
	double c[16], p[16], ci[16], r[16];
	int i;

	for (i = 0; i < 16; i++)
	{
		c[i] = cur[i];
		p[i] = prev[i];
	}
	if (!SmaaMatInverse(c, ci))
		return qfalse;
	SmaaMatMul(p, ci, r);
	for (i = 0; i < 16; i++)
		out[i] = (float)r[i];
	return qtrue;
}

/*
=============
SMAA T2x per-entity motion ("3-obj")

The depth prepass writes a motion vector for every pixel of an entity (RB_IterateStagesGeneric picks the
USE_VELOCITY shader variants). The world has none: those pixels keep the cleared sentinel and the resolve rebuilds
camera motion from depth. An entity's previous transform is found by matching last frame's records on model handle,
entity number and nearest position (refdef entity indexes are not stable between frames). No match (new entity,
teleport, culled last frame) means zero object motion. MD3/MDC vertex animation: the shader moves the vertex along
(oldframe position - frame position) by the lerp change; the previous lerp is expressed in the current frame pair.
=============
*/
#define SMAAVEL_MAX_RECORDS 512

typedef struct {
	int			hModel, entityNum, firstPerson;
	float		xf[16];		// entity -> world (first person: entity -> view)
	int			frame, oldframe;
	float		lerp;		// effective backlerp (0 when frame == oldframe)
	qboolean	claimed;
} smaaVelRecord_t;

static smaaVelRecord_t	smaaVelPrev[SMAAVEL_MAX_RECORDS], smaaVelCur[SMAAVEL_MAX_RECORDS];
static int				smaaVelNumPrev, smaaVelNumCur, smaaVelListFrame = -1000;
static struct {
	int				stamp, hModel;
	smaaEntVel_t	v;
}						smaaVelCache[MAX_REFENTITIES];

extern void myGlMultMatrix( const float *a, const float *b, float *out );

/*
MDS skeletal characters are skinned on the CPU (RB_SurfaceAnim), so the shader cannot rebuild last frame's limb pose.
Instead the skinned positions (entity model space) of every MDS surface drawn in the velocity prepass are kept for one
frame, keyed by (entityNum, model, surface). Next frame the surface uploads them as ATTR_POSITION2 and the shader
transforms them with the previous entity matrix. Missing entry (new entity, entity record not matched, frame gap,
surface not drawn last frame) or vertices beyond the previous LOD count: the previous position is the current one, which
gives entity-transform motion only. Two generations, bump-allocated, no per-frame allocation.
*/
#define SMAAMDS_MAX_ENTRIES 1024
#define SMAAMDS_MAX_VERTS 65536

typedef struct {
	int			entityNum, hModel, numVerts, offset;
	const void	*surf;
} smaaMdsEntry_t;

typedef struct {
	int				numEntries, numVerts;
	smaaMdsEntry_t	entries[SMAAMDS_MAX_ENTRIES];
	float			xyz[SMAAMDS_MAX_VERTS * 3];
} smaaMdsGen_t;

static smaaMdsGen_t		smaaMds[2];
static int				smaaMdsCur, smaaMdsFrame = -1000, smaaMdsHint;

void RB_SmaaMdsPrevPositions( const trRefEntity_t *ent, const void *surf, const float *cur, int count, float *prevOut )
{
	const smaaEntVel_t *ev = RB_SmaaEntityVelocity( ent );
	smaaMdsGen_t *c, *p;
	const smaaMdsEntry_t *prevEntry = NULL;
	int i, j;

	if ( smaaMdsFrame != tr.frameCount ) {
		smaaMdsCur ^= 1;
		if ( smaaMdsFrame != tr.frameCount - 1 ) {
			smaaMds[smaaMdsCur ^ 1].numEntries = 0;		// frame gap: nothing to compare with
		}
		smaaMds[smaaMdsCur].numEntries = smaaMds[smaaMdsCur].numVerts = 0;
		smaaMdsFrame = tr.frameCount;
		smaaMdsHint = 0;
	}
	c = &smaaMds[smaaMdsCur];
	p = &smaaMds[smaaMdsCur ^ 1];

	if ( ev && ev->matched ) {
		for ( i = 0; i < p->numEntries; i++ ) {
			const smaaMdsEntry_t *e = &p->entries[( smaaMdsHint + i ) % p->numEntries];

			if ( e->surf == surf && e->entityNum == ent->e.entityNum && e->hModel == ent->e.hModel ) {
				prevEntry = e;
				smaaMdsHint = ( smaaMdsHint + i + 1 ) % p->numEntries;
				break;
			}
		}
	}

	for ( j = 0; j < count; j++, cur += 4, prevOut += 4 ) {
		if ( prevEntry && j < prevEntry->numVerts ) {
			const float *pp = &p->xyz[( prevEntry->offset + j ) * 3];

			VectorCopy( pp, prevOut );
		} else {
			VectorCopy( cur, prevOut );
		}
		prevOut[3] = 1.0f;
	}

	if ( c->numEntries < SMAAMDS_MAX_ENTRIES && c->numVerts + count <= SMAAMDS_MAX_VERTS ) {
		smaaMdsEntry_t *e = &c->entries[c->numEntries++];
		float *dst = &c->xyz[c->numVerts * 3];

		e->surf = surf;
		e->entityNum = ent->e.entityNum;
		e->hModel = ent->e.hModel;
		e->numVerts = count;
		e->offset = c->numVerts;
		c->numVerts += count;
		cur -= count * 4;
		for ( j = 0; j < count; j++, cur += 4, dst += 3 ) {
			VectorCopy( cur, dst );
		}
	}
}

void RB_SMAAResetMotion( void )
{
	smaaVelNumPrev = smaaVelNumCur = 0;
	smaaVelListFrame = -1000;
	Com_Memset( smaaVelCache, 0, sizeof( smaaVelCache ) );
	smaaMds[0].numEntries = smaaMds[1].numEntries = 0;
	smaaMdsFrame = -1000;
}

const smaaEntVel_t *RB_SmaaEntityVelocity( const trRefEntity_t *ent )
{
	const viewParms_t *vp = &backEnd.viewParms;
	int idx = (int)( ent - backEnd.refdef.entities );
	int stamp = tr.frameCount + 1;
	const float *xf;
	smaaVelRecord_t cur, *best = NULL;
	float bestDist = 0.0f, maxDist;
	smaaVelRecord_t *rec;
	float prevXf[16];
	int i;

	if ( idx < 0 || idx >= MAX_REFENTITIES || ent->e.reType != RT_MODEL ) {
		return NULL;
	}

	if ( smaaVelListFrame != tr.frameCount ) {
		if ( smaaVelListFrame == tr.frameCount - 1 ) {
			Com_Memcpy( smaaVelPrev, smaaVelCur, smaaVelNumCur * sizeof( smaaVelCur[0] ) );
			smaaVelNumPrev = smaaVelNumCur;
		} else {
			smaaVelNumPrev = 0;
		}
		smaaVelNumCur = 0;
		smaaVelListFrame = tr.frameCount;
	}

	if ( smaaVelCache[idx].stamp == stamp && smaaVelCache[idx].hModel == ent->e.hModel ) {
		return &smaaVelCache[idx].v;
	}

	cur.firstPerson = ( ent->e.renderfx & ( RF_DEPTHHACK | RF_FIRST_PERSON ) ) != 0;
	xf = cur.firstPerson ? backEnd.or.modelMatrix : backEnd.or.transformMatrix;
	Com_Memcpy( cur.xf, xf, sizeof( cur.xf ) );
	cur.hModel = ent->e.hModel;
	cur.entityNum = ent->e.entityNum;
	cur.frame = ent->e.frame;
	cur.oldframe = ent->e.oldframe;
	cur.lerp = ( ent->e.frame == ent->e.oldframe ) ? 0.0f : ent->e.backlerp;
	cur.claimed = qfalse;

	maxDist = cur.firstPerson ? 48.0f : 100.0f;
	maxDist *= maxDist;
	for ( i = 0; i < smaaVelNumPrev; i++ ) {
		float d[3], dist;

		rec = &smaaVelPrev[i];
		if ( rec->claimed || rec->hModel != cur.hModel || rec->entityNum != cur.entityNum || rec->firstPerson != cur.firstPerson ) {
			continue;
		}
		VectorSubtract( &rec->xf[12], &cur.xf[12], d );
		dist = DotProduct( d, d );
		if ( dist <= maxDist && ( !best || dist < bestDist ) ) {
			best = rec;
			bestDist = dist;
		}
	}

	{
		smaaEntVel_t *out = &smaaVelCache[idx].v;
		float lerpPrev = cur.lerp;

		if ( best ) {
			best->claimed = qtrue;
			Com_Memcpy( prevXf, best->xf, sizeof( prevXf ) );
			// previous vertex-animation lerp in terms of the current frame pair (shader lerp 0 = frame, 1 = oldframe)
			if ( best->frame == cur.frame ) {
				lerpPrev = best->lerp;
			} else if ( best->frame == cur.oldframe ) {
				lerpPrev = 1.0f + best->lerp;
			}
		} else {
			Com_Memcpy( prevXf, cur.xf, sizeof( prevXf ) );
		}

		if ( cur.firstPerson ) {
			myGlMultMatrix( prevXf, vp->smaaProj, out->prevClip );	// camera-attached: view space motion only
		} else {
			myGlMultMatrix( prevXf, tr.smaaPrevViewProj, out->prevClip );
		}
		out->prevLerp = lerpPrev;
		out->matched = best != NULL;
		smaaVelCache[idx].stamp = stamp;
		smaaVelCache[idx].hModel = ent->e.hModel;
	}

	if ( smaaVelNumCur < SMAAVEL_MAX_RECORDS ) {
		smaaVelCur[smaaVelNumCur++] = cur;
	}

	return &smaaVelCache[idx].v;
}

/*
=============
RB_SMAATemporal

SMAA T2x, camera-only ("3-cam"). The main view was rendered with a +-0.25 px jitter (R_RenderView); the result of
RB_SMAA is blended with the previous resolved frame, which is looked up by reprojecting each pixel's depth through
the previous frame's unjittered viewProj, and clamped to the current 3x3 neighborhood (smaa_resolve_fp.glsl).
Things that move on their own get a wrong history position, so the clamp rejects it and they look like SMAA 1x.
Runs on the tone-mapped LDR image in srcFbo (= tr.renderImage), result goes back to srcFbo and into the history.
History is dropped on: a missed frame (map load, pause, no-world frames), r_aa change, camera cut, big FOV change.
=============
*/
FBO_t *RB_SMAATemporal(FBO_t *srcFbo, ivec4_t srcBox, qboolean skipSceneCopy)
{
	FBO_t *result = NULL;	// the resolved frame when it is not in srcFbo (see skipSceneCopy)
	const viewParms_t *vp = &backEnd.viewParms;
	vec4_t metrics, params;
	float reproj[16], reprojRot[16];
	qboolean valid;
	vec3_t delta;
	float ratio0, ratio5;
	qboolean debug = (r_smaaDebug->integer == 3 || r_smaaDebug->integer == 4);
	qboolean velocity = backEnd.smaaVelocityDrawn && tr.velocityImage;
	// r_smaaT2xPingPong: histCur holds the previous frame, histNext receives the resolve (flipped after it)
	qboolean pingPong = r_smaaT2xPingPong->integer && tr.smaaHistoryFboB && tr.smaaHistoryImageB && !debug;
	image_t *histImage = tr.smaaHistoryFlip ? tr.smaaHistoryImageB : tr.smaaHistoryImage;
	FBO_t *histFbo = tr.smaaHistoryFlip ? tr.smaaHistoryFboB : tr.smaaHistoryFbo;
	FBO_t *histNextFbo = tr.smaaHistoryFlip ? tr.smaaHistoryFbo : tr.smaaHistoryFboB;

	backEnd.smaaVelocityDrawn = qfalse;

	// non-jittered views (sky portal, etc.) neither use nor invalidate the main view's history
	if (!vp->smaaJitter)
		return NULL;

	tr.warp.velValid = qfalse;	// frame warp object motion: re-armed below when this frame's velocity buffer is valid

	if (!tr.smaaResolveAvailable || !tr.smaaHistoryFbo || !tr.screenScratchFbo || !tr.hdrDepthImage ||
		srcFbo != R_SceneFbo(vp) || !r_smaaTemporal->integer || (r_smaaDebug->integer && !debug) ||
		srcFbo->width != tr.smaaHistoryFbo->width || srcFbo->height != tr.smaaHistoryFbo->height ||
		srcBox[0] > 2 || srcBox[1] > 2 || abs(srcBox[2] - srcFbo->width) > 2 || abs(srcBox[3] - srcFbo->height) > 2)
	{
		tr.smaaHistoryValid = qfalse;
		if (debug && !(tr.frameCount & 127))
			ri.Printf(PRINT_ALL, "T2x resolve skipped: avail %d hist %d scratch %d depth %d srcIsRender %d temporal %d box %d,%d %dx%d fbo %dx%d\n",
				tr.smaaResolveAvailable, tr.smaaHistoryFbo != NULL, tr.screenScratchFbo != NULL, tr.hdrDepthImage != NULL,
				srcFbo == R_SceneFbo(vp), r_smaaTemporal->integer, srcBox[0], srcBox[1], srcBox[2], srcBox[3], srcFbo->width, srcFbo->height);
		return NULL;
	}

	valid = tr.smaaHistoryValid && (tr.frameCount - tr.smaaHistoryFrame) <= 1 && tr.frameCount >= tr.smaaHistoryFrame;

	if (valid)
	{
		VectorSubtract(vp->or.origin, tr.smaaPrevOrigin, delta);
		ratio0 = vp->projectionMatrix[0] / tr.smaaPrevProj0;
		ratio5 = vp->projectionMatrix[5] / tr.smaaPrevProj5;
		if (VectorLength(delta) > 128.0f ||					// teleport, camera cut, respawn
			DotProduct(vp->or.axis[0], tr.smaaPrevAxis0) < 0.5f ||	// 60+ degrees in one frame
			ratio0 < 0.5f || ratio0 > 2.0f || ratio5 < 0.5f || ratio5 > 2.0f)	// scope / binocular zoom jump
			valid = qfalse;
	}

	if (valid && (!SmaaReprojection(vp->smaaViewProj, tr.smaaPrevViewProj, reproj) ||
			!SmaaReprojection(vp->smaaViewProjRot, tr.smaaPrevViewProjRot, reprojRot)))
		valid = qfalse;

	if (debug && !valid && !(tr.frameCount & 63))
		ri.Printf(PRINT_ALL, "T2x history invalid: histValid %d frame %d/%d\n", tr.smaaHistoryValid, tr.frameCount, tr.smaaHistoryFrame);

	// frame warp (RB_WarpMeshCapture runs after this and the prepass of the next frame overwrites the buffer): keep
	// the matrix the velocity was built against
	if (valid && velocity)
	{
		Com_Memcpy(tr.warp.velReproj, reproj, sizeof(tr.warp.velReproj));
		tr.warp.velJitter[0] = vp->smaaJitterNdc[0];
		tr.warp.velJitter[1] = vp->smaaJitterNdc[1];
		tr.warp.velFrame = tr.frameCount;
		tr.warp.velValid = qtrue;
	}

	// remember this frame for the next one (before any early-out below)
	Com_Memcpy(tr.smaaPrevViewProj, vp->smaaViewProj, sizeof(tr.smaaPrevViewProj));
	Com_Memcpy(tr.smaaPrevViewProjRot, vp->smaaViewProjRot, sizeof(tr.smaaPrevViewProjRot));
	tr.smaaPrevProj0 = vp->projectionMatrix[0];
	tr.smaaPrevProj5 = vp->projectionMatrix[5];
	VectorCopy(vp->or.origin, tr.smaaPrevOrigin);
	VectorCopy(vp->or.axis[0], tr.smaaPrevAxis0);

	metrics[0] = 1.0f / srcFbo->width;
	metrics[1] = 1.0f / srcFbo->height;
	metrics[2] = (float)srcFbo->width;
	metrics[3] = (float)srcFbo->height;

	params[0] = r_smaaTemporalWeight->value;
	params[1] = valid ? 1.0f : 0.0f;
	params[2] = debug ? (float)r_smaaDebug->integer : 0.0f;
	params[3] = velocity ? 1.0f : 0.0f;

	if (!valid)
	{
		// first frame of a history: nothing to blend, just seed it with the current image
		// (into the current history image, no flip: the next resolve reads it and writes the other one)
		FBO_FastBlit(srcFbo, srcBox, histFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		tr.smaaHistoryValid = qtrue;
		tr.smaaHistoryFrame = tr.frameCount;
		RB_GpuMark("smaa_t2x_reset");
		return NULL;
	}

	GL_BindToTMU(histImage, TB_LEVELSMAP);
	GL_BindToTMU(tr.hdrDepthImage, TB_NORMALMAP);
	if (velocity)
		GL_BindToTMU(tr.velocityImage, TB_SPECULARMAP);

	GLSL_BindProgram(&tr.smaaResolveShader);
	GLSL_SetUniformVec4(&tr.smaaResolveShader, UNIFORM_SMAAMETRICS, metrics);
	GLSL_SetUniformVec4(&tr.smaaResolveShader, UNIFORM_TEMPORALPARAMS, params);
	GLSL_SetUniformMat4(&tr.smaaResolveShader, UNIFORM_REPROJMATRIX, reproj);
	GLSL_SetUniformMat4(&tr.smaaResolveShader, UNIFORM_REPROJROTMATRIX, reprojRot);
	FBO_Blit(srcFbo, srcBox, NULL, pingPong ? histNextFbo : tr.screenScratchFbo, srcBox, &tr.smaaResolveShader, NULL, 0);
	RB_GpuMark("smaa_t2x_resolve");

	if (pingPong)
	{
		// the resolve already is the next history: only the scene copy remains, then swap roles
		if (!skipSceneCopy)
			FBO_FastBlit(histNextFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		RB_GpuMark("smaa_t2x_copy_scene");
		result = histNextFbo;	// = the history of the next frame, read-only for the caller
		tr.smaaHistoryFlip = !tr.smaaHistoryFlip;
	}
	else if (debug)
	{
		// the debug image must not feed back: the history keeps the plain (unresolved) frame
		FBO_FastBlit(srcFbo, srcBox, histFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	else
	{
		if (!skipSceneCopy)
			FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		RB_GpuMark("smaa_t2x_copy_scene");
		FBO_FastBlit(tr.screenScratchFbo, srcBox, histFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		result = tr.screenScratchFbo;
	}
	tr.smaaHistoryValid = qtrue;
	tr.smaaHistoryFrame = tr.frameCount;
	RB_GpuMark("smaa_t2x_copy_history");	// ~0 with r_smaaT2xPingPong (kept so the profile columns stay)
	return result;
}

void RB_ToneMap(FBO_t *hdrFbo, ivec4_t hdrBox, FBO_t *ldrFbo, ivec4_t ldrBox, int autoExposure)
{
	ivec4_t srcBox, dstBox;
	vec4_t color;
	static int lastFrameCount = 0;

	if (autoExposure)
	{
		if (lastFrameCount == 0 || tr.frameCount < lastFrameCount || tr.frameCount - lastFrameCount > 5)
		{
			// determine average log luminance
			FBO_t *srcFbo, *dstFbo, *tmp;
			int size = 256;

			lastFrameCount = tr.frameCount;

			VectorSet4(dstBox, 0, 0, size, size);

			FBO_Blit(hdrFbo, hdrBox, NULL, tr.textureScratchFbo[0], dstBox, &tr.calclevels4xShader[0], NULL, 0);

			srcFbo = tr.textureScratchFbo[0];
			dstFbo = tr.textureScratchFbo[1];

			// downscale to 1x1 texture
			while (size > 1)
			{
				VectorSet4(srcBox, 0, 0, size, size);
				//size >>= 2;
				size >>= 1;
				VectorSet4(dstBox, 0, 0, size, size);

				if (size == 1)
					dstFbo = tr.targetLevelsFbo;

				//FBO_Blit(targetFbo, srcBox, NULL, tr.textureScratchFbo[nextScratch], dstBox, &tr.calclevels4xShader[1], NULL, 0);
				FBO_FastBlit(srcFbo, srcBox, dstFbo, dstBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);

				tmp = srcFbo;
				srcFbo = dstFbo;
				dstFbo = tmp;
			}
		}

		// blend with old log luminance for gradual change
		VectorSet4(srcBox, 0, 0, 0, 0);

		color[0] = 
		color[1] =
		color[2] = 1.0f;
		if (glRefConfig.textureFloat)
			color[3] = 0.03f;
		else
			color[3] = 0.1f;

		FBO_Blit(tr.targetLevelsFbo, srcBox, NULL, tr.calcLevelsFbo, NULL,  NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	}

	RB_GpuMark( "tm_exposure" );

	// tonemap (same exposure as gameplay — do not zero this in cutscenes)
	{
		float exposure = r_cameraExposure->value;

		if ( exposure < -2.0f ) {
			exposure = -2.0f;
		} else if ( exposure > 4.0f ) {
			exposure = 4.0f;
		}

		color[0] =
		color[1] =
		color[2] = pow( 2, exposure - autoExposure ); //exp2(exposure)
		color[3] = 1.0f;
	}

	if (autoExposure)
		GL_BindToTMU(tr.calcLevelsImage,  TB_LEVELSMAP);
	else
		GL_BindToTMU(tr.fixedLevelsImage, TB_LEVELSMAP);

	FBO_Blit(hdrFbo, hdrBox, NULL, ldrFbo, ldrBox, &tr.tonemapShader, color, 0);
	RB_GpuMark( "tm_apply" );
}

/*
=============
RB_BokehBlur


Blurs a part of one framebuffer to another.

Framebuffers can be identical. 
=============
*/
void RB_BokehBlur(FBO_t *src, ivec4_t srcBox, FBO_t *dst, ivec4_t dstBox, float blur)
{
//	ivec4_t srcBox, dstBox;
	vec4_t color;
	
	blur *= 10.0f;

	if (blur < 0.004f)
		return;

	if (glRefConfig.framebufferObject)
	{
		// bokeh blur
		if (blur > 0.0f)
		{
			ivec4_t quarterBox;

			quarterBox[0] = 0;
			quarterBox[1] = tr.quarterFbo[0]->height;
			quarterBox[2] = tr.quarterFbo[0]->width;
			quarterBox[3] = -tr.quarterFbo[0]->height;

			// create a quarter texture
			//FBO_Blit(NULL, NULL, NULL, tr.quarterFbo[0], NULL, NULL, NULL, 0);
			FBO_FastBlit(src, srcBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}

#ifndef HQ_BLUR
		if (blur > 1.0f)
		{
			// create a 1/16th texture
			//FBO_Blit(tr.quarterFbo[0], NULL, NULL, tr.textureScratchFbo[0], NULL, NULL, NULL, 0);
			FBO_FastBlit(tr.quarterFbo[0], NULL, tr.textureScratchFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
#endif

		if (blur > 0.0f && blur <= 1.0f)
		{
			// Crossfade original with quarter texture
			VectorSet4(color, 1, 1, 1, blur);

			FBO_Blit(tr.quarterFbo[0], NULL, NULL, dst, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		}
#ifndef HQ_BLUR
		// ok blur, but can see some pixelization
		else if (blur > 1.0f && blur <= 2.0f)
		{
			// crossfade quarter texture with 1/16th texture
			FBO_Blit(tr.quarterFbo[0], NULL, NULL, dst, dstBox, NULL, NULL, 0);

			VectorSet4(color, 1, 1, 1, blur - 1.0f);

			FBO_Blit(tr.textureScratchFbo[0], NULL, NULL, dst, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		}
		else if (blur > 2.0f)
		{
			// blur 1/16th texture then replace
			int i;

			for (i = 0; i < 2; i++)
			{
				vec2_t blurTexScale;
				float subblur;

				subblur = ((blur - 2.0f) / 2.0f) / 3.0f * (float)(i + 1);

				blurTexScale[0] =
				blurTexScale[1] = subblur;

				color[0] =
				color[1] =
				color[2] = 0.5f;
				color[3] = 1.0f;

				if (i != 0)
					FBO_Blit(tr.textureScratchFbo[0], NULL, blurTexScale, tr.textureScratchFbo[1], NULL, &tr.bokehShader, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
				else
					FBO_Blit(tr.textureScratchFbo[0], NULL, blurTexScale, tr.textureScratchFbo[1], NULL, &tr.bokehShader, color, 0);
			}

			FBO_Blit(tr.textureScratchFbo[1], NULL, NULL, dst, dstBox, NULL, NULL, 0);
		}
#else // higher quality blur, but slower
		else if (blur > 1.0f)
		{
			// blur quarter texture then replace
			int i;

			src = tr.quarterFbo[0];
			dst = tr.quarterFbo[1];

			VectorSet4(color, 0.5f, 0.5f, 0.5f, 1);

			for (i = 0; i < 2; i++)
			{
				vec2_t blurTexScale;
				float subblur;

				subblur = (blur - 1.0f) / 2.0f * (float)(i + 1);

				blurTexScale[0] =
				blurTexScale[1] = subblur;

				color[0] =
				color[1] =
				color[2] = 1.0f;
				if (i != 0)
					color[3] = 1.0f;
				else
					color[3] = 0.5f;

				FBO_Blit(tr.quarterFbo[0], NULL, blurTexScale, tr.quarterFbo[1], NULL, &tr.bokehShader, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
			}

			FBO_Blit(tr.quarterFbo[1], NULL, NULL, dst, dstBox, NULL, NULL, 0);
		}
#endif
	}
}


static void RB_RadialBlur(FBO_t *srcFbo, FBO_t *dstFbo, int passes, float stretch, float x, float y, float w, float h, float xcenter, float ycenter, float alpha)
{
	ivec4_t srcBox, dstBox;
	int srcWidth, srcHeight;
	vec4_t color;
	const float inc = 1.f / passes;
	const float mul = powf(stretch, inc);
	float scale;

	alpha *= inc;
	VectorSet4(color, alpha, alpha, alpha, 1.0f);

	srcWidth  = srcFbo ? srcFbo->width  : glConfig.vidWidth;
	srcHeight = srcFbo ? srcFbo->height : glConfig.vidHeight;

	VectorSet4(srcBox, 0, 0, srcWidth, srcHeight);

	VectorSet4(dstBox, x, y, w, h);
	FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, 0);

	--passes;
	scale = mul;
	while (passes > 0)
	{
		float iscale = 1.f / scale;
		float s0 = xcenter * (1.f - iscale);
		float t0 = (1.0f - ycenter) * (1.f - iscale);

		srcBox[0] = s0 * srcWidth;
		srcBox[1] = t0 * srcHeight;
		srcBox[2] = iscale * srcWidth;
		srcBox[3] = iscale * srcHeight;
			
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );

		scale *= mul;
		--passes;
	}
}


/*
codmod: stock code read the previous frame's query with a blocking GL_QUERY_RESULT, so the
CPU could never get more than about one frame ahead of the GPU. Now the newest finished
query of a small ring decides (a few frames of latency at most), and the CPU never waits.
*/
static qboolean RB_UpdateSunFlareVis(void)
{
	static qboolean visible = qtrue;
	int n = ARRAY_LEN(tr.sunFlareQuery);
	int i;

	if (!glRefConfig.occlusionQuery)
		return qtrue;

	// oldest first, so the newest available result wins
	for (i = 0; i < n; i++)
	{
		int slot = (tr.sunFlareQueryIndex + i) % n;
		GLint available = 0;
		GLuint sampleCount = 0;

		if (!tr.sunFlareQueryActive[slot])
			continue;

		qglGetQueryObjectiv(tr.sunFlareQuery[slot], GL_QUERY_RESULT_AVAILABLE, &available);
		if (!available)
			continue;

		qglGetQueryObjectuiv(tr.sunFlareQuery[slot], GL_QUERY_RESULT, &sampleCount);
		tr.sunFlareQueryActive[slot] = qfalse;
		visible = sampleCount > 0;
	}

	return visible;
}

/*
codmod r_sunRaysOpt 1: bound where the sun-ray result can be non-zero so the passes can be scissored.

The mask (sun flare quad drawn into the black sun-ray buffer) is zero outside the projected quad.
Every radial-blur tap samples c + (p - c) / scale, scale >= 1, so a pixel only receives energy
from texels on the segment between it and the sun centre c. The result is therefore zero outside
the quad's bounding box (plus c) scaled about c by the product of the largest scale of each blur
pass. The box contains c, so it is convex around c: all taps of an in-box pixel stay in the box.
Fails (qfalse, caller runs the full-screen passes) if the sun centre is off-screen (edge clamping
would smear edge texels), a corner is behind the eye, or the old flare quads (r_drawSun > 1) draw
into the mask. y is the union of the box and its mirror because the quarter buffers are stored
flipped; the result is only ever a superset.
Returns box in view-relative uv: x0, x1, y0, y1.
*/
static qboolean RB_SunRaysRegion(const mat4_t mvp, float cx, float cy, float maxScale, int qW, int qH, float box[4])
{
	float dist, size;
	vec3_t dir, vec1, vec2, org;
	float x0, x1, y0, y1, mx, my;
	int i;

	if (r_drawSun->integer > 1)
		return qfalse;
	if (cx < 0.f || cx > 1.f || cy < 0.f || cy > 1.f)
		return qfalse;

	dist = backEnd.viewParms.zFar / 1.75;
	size = dist * 0.3f;		// RB_DrawSun(0.3, tr.sunFlareShader)
	VectorCopy(tr.sunDirection, dir);
	VectorScale(dir, dist, org);
	PerpendicularVector(vec1, dir);
	CrossProduct(dir, vec1, vec2);
	VectorScale(vec1, size, vec1);
	VectorScale(vec2, size, vec2);

	x0 = x1 = cx;
	y0 = y1 = cy;

	for (i = 0; i < 4; i++)
	{
		vec4_t p, h;
		float sx = (i == 0 || i == 3) ? 1.f : -1.f;
		float sy = (i < 2) ? 1.f : -1.f;
		float u, v;

		p[0] = org[0] + sx * vec1[0] + sy * vec2[0];
		p[1] = org[1] + sx * vec1[1] + sy * vec2[1];
		p[2] = org[2] + sx * vec1[2] + sy * vec2[2];
		p[3] = 1.f;
		Mat4Transform(mvp, p, h);

		if (h[3] < 1.f)
			return qfalse;

		u = 0.5f + 0.5f * h[0] / h[3];
		v = 0.5f + 0.5f * h[1] / h[3];

		if (u < x0) x0 = u;
		if (u > x1) x1 = u;
		if (v < y0) y0 = v;
		if (v > y1) y1 = v;
	}

	// clip to the view first (nothing is drawn outside it), then scale about c
	if (x0 < 0.f) x0 = 0.f;
	if (y0 < 0.f) y0 = 0.f;
	if (x1 > 1.f) x1 = 1.f;
	if (y1 > 1.f) y1 = 1.f;

	x0 = cx + (x0 - cx) * maxScale;
	x1 = cx + (x1 - cx) * maxScale;
	y0 = cy + (y0 - cy) * maxScale;
	y1 = cy + (y1 - cy) * maxScale;

	// mirror union for y
	{
		float fy0 = 1.f - y1, fy1 = 1.f - y0;
		if (fy0 < y0) y0 = fy0;
		if (fy1 > y1) y1 = fy1;
	}

	// 8 quarter-res texels: bilinear spread, integer srcBox truncation in RB_RadialBlur
	mx = 8.f / qW;
	my = 8.f / qH;
	x0 -= mx; x1 += mx; y0 -= my; y1 += my;

	box[0] = x0 < 0.f ? 0.f : x0;
	box[1] = x1 > 1.f ? 1.f : x1;
	box[2] = y0 < 0.f ? 0.f : y0;
	box[3] = y1 > 1.f ? 1.f : y1;
	return qtrue;
}

// view-relative uv box -> pixel rect (x, y, w, h) of a w x h buffer, grown by grow texels
static void RB_SunRaysRect(const float box[4], int ox, int oy, int w, int h, int grow, int out[4])
{
	int x0 = ox + (int)floorf(box[0] * w) - grow;
	int x1 = ox + (int)ceilf(box[1] * w) + grow;
	int y0 = oy + (int)floorf(box[2] * h) - grow;
	int y1 = oy + (int)ceilf(box[3] * h) + grow;

	if (x0 < ox) x0 = ox;
	if (y0 < oy) y0 = oy;
	if (x1 > ox + w) x1 = ox + w;
	if (y1 > oy + h) y1 = oy + h;

	out[0] = x0;
	out[1] = y0;
	out[2] = x1 - x0;
	out[3] = y1 - y0;
}

void RB_SunRays(FBO_t *srcFbo, ivec4_t srcBox, FBO_t *dstFbo, ivec4_t dstBox)
{
	vec4_t color;
	float dot;
	const float cutoff = 0.25f;
	qboolean colorize = qtrue;

//	float w, h, w2, h2;
	mat4_t mvp;
	vec4_t pos, hpos;
	qboolean scissored = qfalse;
	int blurPasses = 5;
	float maxScale = 1.f;
	float region[4];
	int rectDown[4], rectBlur0[4], rectBlur1[4], rectAdd[4];

	dot = DotProduct(tr.sunDirection, backEnd.viewParms.or.axis[0]);
	if (dot < cutoff)
		return;

	if (!RB_UpdateSunFlareVis())
		return;

	// From RB_DrawSun()
	{
		float dist;
		mat4_t trans, model;

		Mat4Translation( backEnd.viewParms.or.origin, trans );
		Mat4Multiply( backEnd.viewParms.world.modelMatrix, trans, model );
		Mat4Multiply(backEnd.viewParms.projectionMatrix, model, mvp);

		dist = backEnd.viewParms.zFar / 1.75;		// div sqrt(3)

		VectorScale( tr.sunDirection, dist, pos );
	}

	// project sun point
	//Mat4Multiply(backEnd.viewParms.projectionMatrix, backEnd.viewParms.world.modelMatrix, mvp);
	Mat4Transform(mvp, pos, hpos);

	// transform to UV coords
	hpos[3] = 0.5f / hpos[3];

	pos[0] = 0.5f + hpos[0] * hpos[3];
	pos[1] = 0.5f + hpos[1] * hpos[3];

	if (r_sunRaysTaps->integer > 0)
		blurPasses = r_sunRaysTaps->integer > 16 ? 16 : r_sunRaysTaps->integer;	// quality change, default off

	if (r_sunRaysOpt->integer > 0)
	{
		const float stretch_add = 2.f/3.f;
		float stretch = 1.f + stretch_add;
		int i;

		// largest sample scale of each blur pass is stretch^((passes-1)/passes)
		for (i = 0; i < 2; ++i)
		{
			maxScale *= powf(stretch, (float)(blurPasses - 1) / blurPasses);
			stretch += stretch_add;
		}

		if (RB_SunRaysRegion(mvp, pos[0], pos[1], maxScale, tr.quarterFbo[0]->width, tr.quarterFbo[0]->height, region))
		{
			int qW = tr.quarterFbo[0]->width, qH = tr.quarterFbo[0]->height;

			// each pass must only read texels the previous pass wrote: grow the earlier ones
			RB_SunRaysRect(region, 0, 0, qW, qH, 2, rectBlur1);
			RB_SunRaysRect(region, 0, 0, qW, qH, 4, rectBlur0);
			RB_SunRaysRect(region, 0, 0, qW, qH, 6, rectDown);
			RB_SunRaysRect(region, srcBox[0], srcBox[1], srcBox[2], srcBox[3], 0, rectAdd);
			scissored = qtrue;
		}
	}

	// initialize quarter buffers
	{
		float mul = 1.f;
		ivec4_t rayBox, quarterBox;
		int srcWidth  = srcFbo ? srcFbo->width  : glConfig.vidWidth;
		int srcHeight = srcFbo ? srcFbo->height : glConfig.vidHeight;

		VectorSet4(color, mul, mul, mul, 1);

		rayBox[0] = srcBox[0] * tr.sunRaysFbo->width  / srcWidth;
		rayBox[1] = srcBox[1] * tr.sunRaysFbo->height / srcHeight;
		rayBox[2] = srcBox[2] * tr.sunRaysFbo->width  / srcWidth;
		rayBox[3] = srcBox[3] * tr.sunRaysFbo->height / srcHeight;

		quarterBox[0] = 0;
		quarterBox[1] = tr.quarterFbo[0]->height;
		quarterBox[2] = tr.quarterFbo[0]->width;
		quarterBox[3] = -tr.quarterFbo[0]->height;

		if (scissored)
			FBO_SetScissorOverride(rectDown);

		// first, downsample the framebuffer
		if (colorize)
		{
			FBO_FastBlit(srcFbo, srcBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			FBO_Blit(tr.sunRaysFbo, rayBox, NULL, tr.quarterFbo[0], quarterBox, NULL, color, GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO);
		}
		else
		{
			FBO_FastBlit(tr.sunRaysFbo, rayBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
	}

	FBO_SetScissorOverride(NULL);

	RB_GpuMark( "rays_down" );

	// radial blur passes, ping-ponging between the two quarter-size buffers
	{
		const float stretch_add = 2.f/3.f;
		float stretch = 1.f + stretch_add;
		int i;
		for (i=0; i<2; ++i)
		{
			if (scissored)
				FBO_SetScissorOverride(i == 0 ? rectBlur0 : rectBlur1);
			RB_RadialBlur(tr.quarterFbo[i&1], tr.quarterFbo[(~i) & 1], blurPasses, stretch, 0.f, 0.f, tr.quarterFbo[0]->width, tr.quarterFbo[0]->height, pos[0], pos[1], 1.125f);
			stretch += stretch_add;
		}
	}
	
	FBO_SetScissorOverride(NULL);

	RB_GpuMark( "rays_blur" );

	// add result back on top of the main buffer
	{
		float mul = 1.f;

		VectorSet4(color, mul, mul, mul, 1);

		if (scissored)
			FBO_SetScissorOverride(rectAdd);
		FBO_Blit(tr.quarterFbo[0], NULL, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
		FBO_SetScissorOverride(NULL);
	}
}

static void RB_BlurAxis(FBO_t *srcFbo, FBO_t *dstFbo, float strength, qboolean horizontal)
{
	float dx, dy;
	float xmul, ymul;
	float weights[3] = {
		0.227027027f,
		0.316216216f,
		0.070270270f,
	};
	float offsets[3] = {
		0.f,
		1.3846153846f,
		3.2307692308f,
	};

	xmul = horizontal;
	ymul = 1.f - xmul;

	xmul *= strength;
	ymul *= strength;

	{
		ivec4_t srcBox, dstBox;
		vec4_t color;

		VectorSet4(color, weights[0], weights[0], weights[0], 1.0f);
		VectorSet4(srcBox, 0, 0, srcFbo->width, srcFbo->height);
		VectorSet4(dstBox, 0, 0, dstFbo->width, dstFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, 0);

		VectorSet4(color, weights[1], weights[1], weights[1], 1.0f);
		dx = offsets[1] * xmul;
		dy = offsets[1] * ymul;
		VectorSet4(srcBox, dx, dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
		VectorSet4(srcBox, -dx, -dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

		VectorSet4(color, weights[2], weights[2], weights[2], 1.0f);
		dx = offsets[2] * xmul;
		dy = offsets[2] * ymul;
		VectorSet4(srcBox, dx, dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
		VectorSet4(srcBox, -dx, -dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	}
}

static void RB_HBlur(FBO_t *srcFbo, FBO_t *dstFbo, float strength)
{
	RB_BlurAxis(srcFbo, dstFbo, strength, qtrue);
}

static void RB_VBlur(FBO_t *srcFbo, FBO_t *dstFbo, float strength)
{
	RB_BlurAxis(srcFbo, dstFbo, strength, qfalse);
}

void RB_GaussianBlur(FBO_t *srcFbo, FBO_t *dstFbo, float blur)
{
	//float mul = 1.f;
	float factor = Com_Clamp(0.f, 1.f, blur);

	if (factor <= 0.f)
		return;

	{
		ivec4_t srcBox, dstBox;
		vec4_t color;

		VectorSet4(color, 1, 1, 1, 1);

		// first, downsample the framebuffer
		FBO_FastBlit(srcFbo, NULL, tr.quarterFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		FBO_FastBlit(tr.quarterFbo[0], NULL, tr.textureScratchFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);

		// set the alpha channel
		qglColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
		FBO_BlitFromTexture(tr.whiteImage, NULL, NULL, tr.textureScratchFbo[0], NULL, NULL, color, GLS_DEPTHTEST_DISABLE);
		qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		// blur the tiny buffer horizontally and vertically
		RB_HBlur(tr.textureScratchFbo[0], tr.textureScratchFbo[1], factor);
		RB_VBlur(tr.textureScratchFbo[1], tr.textureScratchFbo[0], factor);

		// finally, merge back to framebuffer
		VectorSet4(srcBox, 0, 0, tr.textureScratchFbo[0]->width, tr.textureScratchFbo[0]->height);
		VectorSet4(dstBox, 0, 0, glConfig.vidWidth,              glConfig.vidHeight);
		color[3] = factor;
		FBO_Blit(tr.textureScratchFbo[0], srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	}
}

/*
=============
RB_PostFx

codmod: optional bloom (r_bloom) and colour grade (r_grade), run on the tone-mapped, anti-aliased 3D scene.
Bloom: soft-knee bright pass into a half resolution image, a chain of 2x downsamples (5 levels), then the
levels are added back up with a 3x3 tent filter, each level weighted down, so the result is a tight glow
around lights with a wide faint halo. The grade shader then adds it (screen blend) and applies contrast /
saturation / split tint / vignette in one fullscreen pass (scene -> screenScratch -> scene).
Returns qfalse when nothing was done.
=============
*/
#define BLOOM_UP_WEIGHT 0.8f

static qboolean RB_PostFxState(FBO_t *srcFbo, float *bloomMulOut, qboolean *bloomOut, qboolean *gradeOut)
{
	static const float bloomLevel[4] = { 0.0f, 1.0f, 1.6f, 2.4f };
	float bloomMul = bloomLevel[r_bloom->integer < 0 ? 0 : (r_bloom->integer > 3 ? 3 : r_bloom->integer)];
	qboolean bloom = bloomMul > 0.0f && r_bloomIntensity->value > 0.0f && tr.bloomFbo[BLOOM_LEVELS - 1];
	qboolean grade = r_grade->integer != 0;

	*bloomMulOut = bloomMul;
	*bloomOut = bloom;
	*gradeOut = grade;
	return srcFbo && tr.screenScratchFbo && (bloom || grade);
}

// the early-out test of RB_PostFx, so RB_PostProcess can route the scene around the grade copy
qboolean RB_PostFxWillRun(FBO_t *srcFbo)
{
	float bloomMul;
	qboolean bloom, grade;

	return RB_PostFxState(srcFbo, &bloomMul, &bloom, &grade);
}

// conservative: the cheap early-out of RB_SunRays (sun not in front of the camera), without consuming the visibility query
qboolean RB_SunRaysMayDraw(void)
{
	return DotProduct(tr.sunDirection, backEnd.viewParms.or.axis[0]) >= 0.25f;
}

/*
sceneFbo: where the anti-aliased scene is (RGBA8, same size as srcFbo), when it is not in srcFbo (r_gradeNoCopy).
The grade pass then reads it and writes srcFbo directly. NULL (or srcFbo) = the scene is in srcFbo: grade
goes through screenScratch and is copied back.
*/
qboolean RB_PostFx(FBO_t *srcFbo, FBO_t *sceneFbo, ivec4_t srcBox)
{
	float bloomMul;
	qboolean bloom, grade;
	vec4_t color, viewInfo;
	float norm = 0.0f;
	qboolean direct = sceneFbo && sceneFbo != srcFbo;

	if (!RB_PostFxState(srcFbo, &bloomMul, &bloom, &grade))
		return qfalse;
	if (!direct)
		sceneFbo = srcFbo;

	if (bloom)
	{
		float radius = Com_Clamp(0.25f, 4.0f, r_bloomRadius->value);
		float threshold = Com_Clamp(0.0f, 1.0f, r_bloomThreshold->value);
		float w;
		int i;

		// bright pass into mip 0. Whole texture to whole texture: FBO_Blit flips Y when a source box is
		// paired with a NULL destination box, which put every glow upside down (torches lit the floor).
		// The grade pass samples the bloom with the scene's texcoords, so the full-texture mapping lines up.
		VectorSet4(color, threshold, 0.15f, 0.0f, 1.0f);
		FBO_Blit(sceneFbo, NULL, NULL, tr.bloomFbo[0], NULL, &tr.bloomBrightShader, color, 0);
		RB_GpuMark( "bloom_bright" );

		for (i = 1; i < BLOOM_LEVELS; i++)
			FBO_FastBlit(tr.bloomFbo[i - 1], NULL, tr.bloomFbo[i], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		RB_GpuMark( "bloom_down" );

		// each smaller level is tent-filtered onto the next larger one, smallest first. A weight below 1
		// keeps the glow tight around the light with a faint wide halo; the sum is normalised below.
		VectorSet4(color, BLOOM_UP_WEIGHT, 0.0f, 0.0f, 1.0f);
		for (i = BLOOM_LEVELS - 2; i >= 0; i--)
		{
			// FBO_Blit sets u_InvTexRes = texScale / source size, so the radius goes in as the scale
			FBO_Blit(tr.bloomFbo[i + 1], NULL, (vec2_t){ radius, radius }, tr.bloomFbo[i], NULL, &tr.bloomBlurShader, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
		}
		RB_GpuMark( "bloom_up" );

		for (i = 0, w = 1.0f; i < BLOOM_LEVELS; i++, w *= BLOOM_UP_WEIGHT)
			norm += w;

		GL_BindToTMU(tr.bloomImage[0], TB_LIGHTMAP);
	}

	VectorSet4(color, bloom ? r_bloomIntensity->value * bloomMul / norm : 0.0f, grade ? Com_Clamp(0.5f, 2.5f, r_gradeBrightness->value) : 1.0f, direct ? 1.0f : 0.0f, 1.0f);
	if (grade)
	{
		VectorSet4(viewInfo, r_gradeContrast->value, r_gradeSaturation->value, Com_Clamp(-1.0f, 1.0f, r_gradeTint->value), r_gradeVignette->value);
	}
	else
	{
		VectorSet4(viewInfo, 1.0f, 1.0f, 0.0f, 0.0f);
	}

	GLSL_BindProgram(&tr.gradeShader);
	GLSL_SetUniformVec4(&tr.gradeShader, UNIFORM_VIEWINFO, viewInfo);
	if (direct)
	{
		FBO_Blit(sceneFbo, srcBox, NULL, srcFbo, srcBox, &tr.gradeShader, color, 0);
		RB_GpuMark( "grade" );
	}
	else
	{
		FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.gradeShader, color, 0);
		RB_GpuMark( "grade" );
		FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	RB_GpuMark( "grade_copy" );	// ~0 with r_gradeNoCopy (kept so the profile columns stay)

	return qtrue;
}

/*
=============
RB_SharpenViewOK

Views that get the 3D sharpeners (r_renderScaleSharp RCAS, r_dlsStrength DLS): not the skybox portal (drawn over by the
main view) nor model-only views.
=============
*/
static qboolean RB_SharpenViewOK(void)
{
	return !(backEnd.refdef.rdflags & (RDF_SKYBOXPORTAL | RDF_NOWORLDMODEL));
}

qboolean RB_DLSActive(void)
{
	return r_dlsStrength->value > 0.0f && tr.dlsShader.program && tr.dlsFbo && RB_SharpenViewOK();
}

/*
=============
RB_RCASSharpness

Effective r_renderScaleSharp, 0..1 (0 = RCAS off). Negative = auto: 0.5 when the scene is upscaled (tr.renderScale
active, value < 1), off at native scale, so the native extra pass and copy are skipped by default.
=============
*/
static float RB_RCASSharpness(void)
{
	float v = r_renderScaleSharp->value;

	if (v < 0.0f)
		return tr.renderScale.value < 1.0f ? 0.5f : 0.0f;
	return Com_Clamp(0.0f, 1.0f, v);
}

// RCAS as its own pass (native resolution or bilinear upscale); inside the FSR1 chain it is part of RB_RenderScaleUpscale
qboolean RB_RCASActive(void)
{
	return RB_RCASSharpness() > 0.0f && tr.upscaleRcasShader.program && tr.dlsFbo && RB_SharpenViewOK();
}

/*
=============
RB_RCAS

FSR1 RCAS, box of src into the same box of dst (different FBOs, both window-sized).
stops = 2 * (1 - sharpness): 1 = 0 stops (sharpest), 0.5 = 1 stop, 0.875 = 0.25 stops (sharpness: RB_RCASSharpness).
=============
*/
static void RB_RCAS(FBO_t *src, FBO_t *dst, ivec4_t box)
{
	vec4_t con;

	GLSL_BindProgram(&tr.upscaleRcasShader);
	VectorSet4(con, powf(2.0f, -2.0f * (1.0f - RB_RCASSharpness())), 0.0f, 0.0f, 0.0f);
	GLSL_SetUniformVec4(&tr.upscaleRcasShader, UNIFORM_RCASCON, con);
	VectorSet4(con, box[0], box[1], box[0] + box[2] - 1, box[1] + box[3] - 1);
	GLSL_SetUniformVec4(&tr.upscaleRcasShader, UNIFORM_RCASBOX, con);
	FBO_Blit(src, box, NULL, dst, box, &tr.upscaleRcasShader, NULL, 0);
	RB_GpuMark( "upscale_rcas" );
}

/*
=============
RB_DLS

NVIDIA image sharpening (Denoised Luma Sharpening, vkBasalt dls.frag.glsl, BSD-3), box of src into the same box of
dst (different FBOs, both window-sized). r_dlsStrength 0..1: sharpen = strength, denoise follows the old
low/medium/high presets (0.35/0.20, 0.55/0.17, 0.80/0.12) piecewise-linearly.
=============
*/
static void RB_DLS(FBO_t *src, FBO_t *dst, ivec4_t box)
{
	// denoise by strength: piecewise-linear through the low/medium/high presets
	static const float knot[5][2] = { { 0.0f, 0.22f }, { 0.35f, 0.20f }, { 0.55f, 0.17f }, { 0.80f, 0.12f }, { 1.0f, 0.12f } };
	const float kSharpnessMin = -1.0f / 14.0f, kSharpnessMax = -1.0f / 6.5f;
	const float kDenoiseMin = 0.001f, kDenoiseMax = -0.1f;	// as in the vkBasalt shader
	float sharpen = Com_Clamp(0.0f, 1.0f, r_dlsStrength->value), denoise = knot[4][1];
	vec4_t con;
	int i;

	for (i = 1; i < 5; i++)
	{
		if (sharpen <= knot[i][0])
		{
			denoise = knot[i - 1][1] + (knot[i][1] - knot[i - 1][1]) * (sharpen - knot[i - 1][0]) / (knot[i][0] - knot[i - 1][0]);
			break;
		}
	}

	GLSL_BindProgram(&tr.dlsShader);
	VectorSet4(con, kSharpnessMin + (kSharpnessMax - kSharpnessMin) * sharpen,
		1.0f / (kDenoiseMin + (kDenoiseMax - kDenoiseMin) * denoise), 0.0f, 0.0f);
	GLSL_SetUniformVec4(&tr.dlsShader, UNIFORM_DLSCON, con);
	VectorSet4(con, box[0], box[1], box[0] + box[2] - 1, box[1] + box[3] - 1);
	GLSL_SetUniformVec4(&tr.dlsShader, UNIFORM_DLSBOX, con);
	FBO_Blit(src, box, NULL, dst, box, &tr.dlsShader, NULL, 0);
	RB_GpuMark( "dls" );
}

/*
=============
RB_SharpenBlit

Last step of the 3D post chain at display resolution: copies src to dst (the same FBO is allowed) with RCAS and/or DLS
applied when on, ping-ponging through tr.dlsFbo. Plain blit when both are off.
=============
*/
void RB_SharpenBlit(FBO_t *src, ivec4_t srcBox, FBO_t *dst, ivec4_t dstBox)
{
	qboolean rcas = RB_RCASActive(), dls = RB_DLSActive();

	if (!rcas && !dls)
	{
		if (src != dst)
			FBO_FastBlit(src, srcBox, dst, dstBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		return;
	}

	if (rcas && dls)
	{
		RB_RCAS(src, tr.dlsFbo, srcBox);
		RB_DLS(tr.dlsFbo, dst, srcBox);
	}
	else if (src != dst)
	{
		if (rcas)
			RB_RCAS(src, dst, srcBox);
		else
			RB_DLS(src, dst, srcBox);
	}
	else
	{
		if (rcas)
			RB_RCAS(src, tr.dlsFbo, srcBox);
		else
			RB_DLS(src, tr.dlsFbo, srcBox);
		FBO_FastBlit(tr.dlsFbo, srcBox, dst, dstBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		RB_GpuMark( "sharpen_copy" );
	}
}

/*
=============
RB_RenderScaleUpscale

r_renderScale: scales the scene viewport srcBox of src into dstBox of the window-sized dst (both boxes GL
bottom-up). r_renderScaleFilter 0 or forceBilinear: one linear blit. 1: AMD FidelityFX FSR1 (MIT), EASU into
tr.upscaleFbo, then RCAS into dst; falls back to the blit when its shaders are missing.
=============
*/
void RB_RenderScaleUpscale(FBO_t *src, ivec4_t srcBox, FBO_t *dst, ivec4_t dstBox, qboolean forceBilinear)
{
	vec4_t con;
	FBO_t *easuOut;
	qboolean rcas, dls;

	if (forceBilinear || r_renderScaleFilter->integer != 1 || !tr.upscaleFbo ||
		!tr.upscaleEasuShader.program || !tr.upscaleRcasShader.program)
	{
		FBO_FastBlit(src, srcBox, dst, dstBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		RB_GpuMark( "upscale" );
		if (!forceBilinear)
			RB_SharpenBlit(dst, dstBox, dst, dstBox);
		return;
	}

	// EASU: output pixel p (relative to dstBox) samples input position p * scale + offset (relative to srcBox)
	GLSL_BindProgram(&tr.upscaleEasuShader);
	VectorSet4(con, srcBox[2] / (float)dstBox[2], srcBox[3] / (float)dstBox[3],
		0.5f * srcBox[2] / (float)dstBox[2] - 0.5f, 0.5f * srcBox[3] / (float)dstBox[3] - 0.5f);
	GLSL_SetUniformVec4(&tr.upscaleEasuShader, UNIFORM_EASUCON0, con);
	VectorSet4(con, srcBox[0], srcBox[1], srcBox[0] + srcBox[2] - 1, srcBox[1] + srcBox[3] - 1);
	GLSL_SetUniformVec4(&tr.upscaleEasuShader, UNIFORM_EASUCON1, con);
	VectorSet4(con, dstBox[0], dstBox[1], 0.0f, 0.0f);
	GLSL_SetUniformVec4(&tr.upscaleEasuShader, UNIFORM_EASUCON2, con);

	// chain: EASU [-> RCAS when r_renderScaleSharp > 0] [-> DLS when r_dlsStrength > 0] -> dst, ping-ponging
	// between upscaleFbo and dlsFbo so no pass needs a copy
	rcas = RB_RCASActive();
	dls = !forceBilinear && RB_DLSActive();
	easuOut = rcas ? tr.upscaleFbo : (dls ? tr.dlsFbo : dst);
	FBO_Blit(src, srcBox, NULL, easuOut, dstBox, &tr.upscaleEasuShader, NULL, 0);
	RB_GpuMark( "upscale_easu" );

	if (rcas)
		RB_RCAS(tr.upscaleFbo, dls ? tr.dlsFbo : dst, dstBox);
	if (dls)
		RB_DLS(tr.dlsFbo, dst, dstBox);
}
