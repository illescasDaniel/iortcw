/*
===========================================================================
Loading screen snapshot and replay (codmod).

The last full loading frame is copied into a raw GL renderbuffer + FBO that lives in
this file's statics, NOT in tr / glState / FBO_t / image_t, so it survives
RE_Shutdown(qfalse) and the memset of tr in R_Init (the GL context stays alive across map
loads). The replay re-presents that image with a spinner on top, using raw qgl calls
only (no shaders, VAOs, textures, GL_Bind*, FBO_Bind, tr.* or GL_State), so it can be
called from anywhere: while the renderer is shut down, half initialised, or in the
middle of a shader / image / model load.

Both paths copy bytes between RGBA8 buffers with glBlitFramebuffer; GL_FRAMEBUFFER_SRGB is
never enabled by this renderer and is not touched here, so no colour conversion happens
and the replay is identical to the frame that was captured.
===========================================================================
*/

#include "tr_local.h"

#define LS_SEGMENTS 12      // squares in the spinner ring
#define LS_STEP_MS  83      // ~12 steps per second

typedef struct {
	GLuint fbo;
	GLuint rb;
	int w, h;                   // snapshot size (= glConfig.vid size at capture)
	int drawW, drawH;           // window drawable size at capture
	qboolean valid;
	qboolean sessionOn;         // inside one load: LoadingSnapshot(qtrue) seen without a (qfalse) since
	qboolean captureNext;       // the next swap is a full loading frame: capture it
} loadScreen_t;

static loadScreen_t ls;

/*
===============
R_LoadScreenInvalidate

The GL context is gone (or about to be): forget the objects without deleting them.
===============
*/
void R_LoadScreenInvalidate( void ) {
	Com_Memset( &ls, 0, sizeof( ls ) );
}

/*
===============
R_LoadScreenShutdown

Real shutdown (window / context destroyed next): free the GL objects.
===============
*/
void R_LoadScreenShutdown( void ) {
	if ( ls.fbo && qglDeleteFramebuffers ) {
		qglDeleteFramebuffers( 1, &ls.fbo );
	}
	if ( ls.rb && qglDeleteRenderbuffers ) {
		qglDeleteRenderbuffers( 1, &ls.rb );
	}
	R_LoadScreenInvalidate();
}

/*
===============
RE_LoadingSnapshot

enable: the frames now being drawn belong to a map load; capture each one's final
image. Starting a new session drops the previous load's image. Called by the client
before every SCR_UpdateScreen, so gameplay frames pass enable = qfalse (cheap).
===============
*/
void RE_LoadingSnapshot( qboolean enable ) {
	if ( enable ) {
		if ( !ls.sessionOn ) {
			ls.sessionOn = qtrue;
			ls.valid = qfalse;
		}
		ls.captureNext = qtrue;
	} else {
		ls.sessionOn = qfalse;
		ls.captureNext = qfalse;
	}
}

/*
===============
LS_Alloc

Create (or recreate on size change) the snapshot buffer. Leaves the draw framebuffer
binding on ls.fbo and the renderbuffer binding as it found it.
===============
*/
static qboolean LS_Alloc( int w, int h ) {
	GLint prevRb = 0;

	if ( ls.fbo ) {
		qglDeleteFramebuffers( 1, &ls.fbo );
		ls.fbo = 0;
	}
	if ( ls.rb ) {
		qglDeleteRenderbuffers( 1, &ls.rb );
		ls.rb = 0;
	}
	ls.valid = qfalse;

	qglGetIntegerv( GL_RENDERBUFFER_BINDING, &prevRb );
	qglGenRenderbuffers( 1, &ls.rb );
	qglBindRenderbuffer( GL_RENDERBUFFER, ls.rb );
	qglRenderbufferStorage( GL_RENDERBUFFER, GL_RGBA8, w, h );
	qglBindRenderbuffer( GL_RENDERBUFFER, prevRb );

	qglGenFramebuffers( 1, &ls.fbo );
	qglBindFramebuffer( GL_DRAW_FRAMEBUFFER, ls.fbo );
	qglFramebufferRenderbuffer( GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ls.rb );
	if ( qglCheckFramebufferStatus( GL_DRAW_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE ) {
		qglBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
		qglDeleteFramebuffers( 1, &ls.fbo );
		qglDeleteRenderbuffers( 1, &ls.rb );
		ls.fbo = ls.rb = 0;
		return qfalse;
	}
	ls.w = w;
	ls.h = h;
	return qtrue;
}

/*
===============
LS_DrawSpinner

Ring of LS_SEGMENTS small squares in the bottom right corner (size from the window
height, so it looks the same at any aspect). The head segment advances with the clock,
trailing segments fade out, so it animates even over a static image. Scissored
glClear only. Caller has the target framebuffer bound; GL state touched is saved and
restored here.
===============
*/
static void LS_DrawSpinner( int w, int h ) {
	static const float dx[LS_SEGMENTS] = { 0.0f, 0.5f, 0.866f, 1.0f, 0.866f, 0.5f, 0.0f, -0.5f, -0.866f, -1.0f, -0.866f, -0.5f };
	static const float dy[LS_SEGMENTS] = { 1.0f, 0.866f, 0.5f, 0.0f, -0.5f, -0.866f, -1.0f, -0.866f, -0.5f, 0.0f, 0.5f, 0.866f };
	GLboolean scissorOn = GL_FALSE, mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
	GLint box[4] = { 0, 0, 0, 0 };
	GLfloat clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	int ms, head, i, radius, q, cx, cy, margin;

	qglGetBooleanv( GL_SCISSOR_TEST, &scissorOn );
	qglGetIntegerv( GL_SCISSOR_BOX, box );
	qglGetBooleanv( GL_COLOR_WRITEMASK, mask );
	qglGetFloatv( GL_COLOR_CLEAR_VALUE, clear );

	radius = h * 3 / 100;
	if ( radius < 8 ) {
		radius = 8;
	}
	q = radius * 40 / 100;          // square size
	margin = radius + q + h * 2 / 100;
	cx = w - margin;
	cy = margin;
	ms = ri.Milliseconds ? ri.Milliseconds() : 0;
	head = ( ms / LS_STEP_MS ) % LS_SEGMENTS;

	qglEnable( GL_SCISSOR_TEST );
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	for ( i = 0; i < LS_SEGMENTS; i++ ) {
		int age = ( head - i + LS_SEGMENTS ) % LS_SEGMENTS;    // 0 = head, trails behind it
		float v = age < 6 ? 1.0f - age * 0.13f : 0.30f;
		int x = cx + (int)( dx[i] * radius ) - q / 2;
		int y = cy + (int)( dy[i] * radius ) - q / 2;

		// dark backing so it reads on bright images
		qglScissor( x - 2, y - 2, q + 4, q + 4 );
		qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
		qglClear( GL_COLOR_BUFFER_BIT );
		qglScissor( x, y, q, q );
		qglClearColor( v, v, v, 1.0f );
		qglClear( GL_COLOR_BUFFER_BIT );
	}

	qglClearColor( clear[0], clear[1], clear[2], clear[3] );
	qglColorMask( mask[0], mask[1], mask[2], mask[3] );
	qglScissor( box[0], box[1], box[2], box[3] );
	if ( !scissorOn ) {
		qglDisable( GL_SCISSOR_TEST );
	}
}

/*
===============
R_LoadScreenCapture

Called from RB_SwapBuffers right after the final image was blitted to the default
framebuffer (bindings are 0/0 then), before the swap. No-op unless the client marked
this frame as a loading frame. Copies the back buffer into the snapshot, then draws
the spinner on the frame being presented (not into the snapshot).
===============
*/
void R_LoadScreenCapture( void ) {
	int w, h, dw, dh;
	GLboolean scissorOn = GL_FALSE;

	if ( !ls.captureNext ) {
		return;
	}
	ls.captureNext = qfalse;

	if ( !glRefConfig.framebufferObject || !glRefConfig.framebufferBlit || !qglBlitFramebuffer ) {
		return;
	}
	if ( Q_stricmp( r_drawBuffer->string, "GL_BACK" ) != 0 ) {
		return;
	}
	w = glConfig.vidWidth;
	h = glConfig.vidHeight;
	if ( w <= 0 || h <= 0 || !GLimp_GetDrawableSize( &dw, &dh ) ) {
		return;
	}

	qglGetBooleanv( GL_SCISSOR_TEST, &scissorOn );
	if ( scissorOn ) {
		qglDisable( GL_SCISSOR_TEST );      // the blit would be clipped by the scissor box
	}

	if ( !ls.fbo || ls.w != w || ls.h != h ) {
		if ( !LS_Alloc( w, h ) ) {
			qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
			if ( scissorOn ) {
				qglEnable( GL_SCISSOR_TEST );
			}
			return;
		}
	}

	qglBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
	qglBindFramebuffer( GL_DRAW_FRAMEBUFFER, ls.fbo );
	qglBlitFramebuffer( 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	ls.drawW = dw;
	ls.drawH = dh;
	ls.valid = qtrue;

	if ( scissorOn ) {
		qglEnable( GL_SCISSOR_TEST );
	}
	LS_DrawSpinner( w, h );
}

/*
===============
RE_PresentLoadingSnapshot

Re-present the last captured loading frame with the spinner on top. Raw GL only; the
GL bindings and state touched are read back from the driver and restored, so the
renderer's cached state stays valid whatever it currently is.
===============
*/
void RE_PresentLoadingSnapshot( void ) {
	GLint prevDraw = 0, prevRead = 0, scissorBox[4] = { 0, 0, 0, 0 };
	GLboolean scissorOn = GL_FALSE;
	int dw, dh;

	if ( !ls.valid || !ls.fbo || !qglBlitFramebuffer || !qglBindFramebuffer || !qglGetIntegerv ) {
		return;
	}
	// window resized (or fullscreen toggled) since the capture: do not stretch / crop
	if ( !GLimp_GetDrawableSize( &dw, &dh ) || dw != ls.drawW || dh != ls.drawH ) {
		return;
	}

	qglGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw );
	qglGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &prevRead );
	qglGetBooleanv( GL_SCISSOR_TEST, &scissorOn );
	qglGetIntegerv( GL_SCISSOR_BOX, scissorBox );

	qglDisable( GL_SCISSOR_TEST );
	qglBindFramebuffer( GL_READ_FRAMEBUFFER, ls.fbo );
	qglBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
	qglBlitFramebuffer( 0, 0, ls.w, ls.h, 0, 0, ls.w, ls.h, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	LS_DrawSpinner( ls.w, ls.h );
	GLimp_SwapOnly();

	qglBindFramebuffer( GL_READ_FRAMEBUFFER, prevRead );
	qglBindFramebuffer( GL_DRAW_FRAMEBUFFER, prevDraw );
	qglScissor( scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3] );
	if ( scissorOn ) {
		qglEnable( GL_SCISSOR_TEST );
	}
}
