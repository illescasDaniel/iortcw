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

void RB_SunRays(FBO_t *srcFbo, ivec4_t srcBox, FBO_t *dstFbo, ivec4_t dstBox)
{
	vec4_t color;
	float dot;
	const float cutoff = 0.25f;
	qboolean colorize = qtrue;

//	float w, h, w2, h2;
	mat4_t mvp;
	vec4_t pos, hpos;

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

	RB_GpuMark( "rays_down" );

	// radial blur passes, ping-ponging between the two quarter-size buffers
	{
		const float stretch_add = 2.f/3.f;
		float stretch = 1.f + stretch_add;
		int i;
		for (i=0; i<2; ++i)
		{
			RB_RadialBlur(tr.quarterFbo[i&1], tr.quarterFbo[(~i) & 1], 5, stretch, 0.f, 0.f, tr.quarterFbo[0]->width, tr.quarterFbo[0]->height, pos[0], pos[1], 1.125f);
			stretch += stretch_add;
		}
	}
	
	RB_GpuMark( "rays_blur" );

	// add result back on top of the main buffer
	{
		float mul = 1.f;

		VectorSet4(color, mul, mul, mul, 1);

		FBO_Blit(tr.quarterFbo[0], NULL, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
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

qboolean RB_PostFx(FBO_t *srcFbo, ivec4_t srcBox)
{
	static const float bloomLevel[4] = { 0.0f, 1.0f, 1.6f, 2.4f };
	float bloomMul = bloomLevel[r_bloom->integer < 0 ? 0 : (r_bloom->integer > 3 ? 3 : r_bloom->integer)];
	qboolean bloom = bloomMul > 0.0f && r_bloomIntensity->value > 0.0f && tr.bloomFbo[BLOOM_LEVELS - 1];
	qboolean grade = r_grade->integer != 0;
	vec4_t color, viewInfo;
	float norm = 0.0f;

	if (!srcFbo || !tr.screenScratchFbo || (!bloom && !grade))
		return qfalse;

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
		FBO_Blit(srcFbo, NULL, NULL, tr.bloomFbo[0], NULL, &tr.bloomBrightShader, color, 0);
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

	VectorSet4(color, bloom ? r_bloomIntensity->value * bloomMul / norm : 0.0f, grade ? Com_Clamp(0.5f, 2.5f, r_gradeBrightness->value) : 1.0f, 0.0f, 1.0f);
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
	FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.gradeShader, color, 0);
	FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	RB_GpuMark( "grade" );

	return qtrue;
}
