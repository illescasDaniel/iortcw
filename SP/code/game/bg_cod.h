// codmod: new file (phase 2.0) -- bg_cod.h
//
// Shared CoD4-style tuning table for the player. Compiled into *both* qagame and cgame
// (see bg_pmove.o entries in SP/Makefile) -- each module gets its own copy of bg_cod,
// kept in sync every frame because g_main.c and cg_main.c both read the same cvars.
//
// See docs/architecture.md section 5 (outer repo) for the design rationale.

#ifndef __BG_COD_H__
#define __BG_COD_H__

#include "../qcommon/q_shared.h"

// codmod: all CoD-style tuning in one struct, filled in by BG_CodUpdateCvars() every frame.
// Never add playerState_t fields for this -- see the golden rules in architecture.md.
typedef struct {
	int enable;             // master switch: 0 = vanilla behaviour everywhere
	int moveEnable;         // speed/sprint/walk changes (2.1)
	int leanEnable;         // lean changes (2.3)

	float runSpeed;         // u/s, player jog
	float sprintScale;      // x runSpeed
	float walkScale;        // x runSpeed when BUTTON_WALKING
	float crouchScale;      // x runSpeed
	float backScale;        // x runSpeed when moving backward
	float strafeScale;      // x runSpeed when strafing

	float sprintTime;           // seconds, full stamina bar drain time while sprinting+moving
	float sprintRechargeTime;   // seconds, empty -> full recharge time
	float sprintMinTime;        // seconds of stamina required to *start* a new sprint
	int sprintForwardOnly;      // qboolean-ish: sprint requires cmd.forwardmove > 0
	float sprintJumpCost;       // stamina units taken on jump (vanilla default 2000)

	float duckTime;    // ms, cgame duck viewheight smoothing
	int duckEase;       // ease-out curve instead of linear
	int duckGunDip;     // small viewmodel dip following the same curve

	float leanMax;         // u, lean offset
	float leanRollScale;   // camera roll = leanf * leanRollScale
	float leanTimeIn;      // ms, time to reach full lean
	float leanTimeOut;     // ms, time to return to center
	int leanAllowFire;     // allow firing (and crosshair) while leaned
	int leanEase;          // smoothstep the displayed lean fraction (cgame-only)

	int aiSpeedMode;     // 0 = vanilla, 1 = match player speeds, 2 = scale anim-derived speeds
	float aiSpeedScale;  // multiplier used by aiSpeedMode 2

	int debugHud;    // 0 off, 1 HUD, 2 HUD + nearest AI speed
} codTuning_t;

// codmod: one instance per module (qagame and cgame each link their own bg_cod.o).
extern codTuning_t bg_cod;

// codmod: single source of truth for cvar name/default/flags and where it lands in bg_cod.
typedef struct {
	const char *name;
	const char *def;
	int flags;
	size_t ofs;      // offset into codTuning_t, see CODOFS() in bg_cod.c
	int isInt;       // qtrue: read cvar.integer, qfalse: read cvar.value
} codCvarDef_t;

extern const codCvarDef_t bg_codCvars[];
extern const int bg_numCodCvars;

// codmod: g_main.c / cg_main.c each keep a static vmCvar_t[BG_COD_MAX_CVARS] and pass it in.
// Bump this if bg_codCvars ever grows past it -- BG_CodRegisterCvars() clamps defensively.
#define BG_COD_MAX_CVARS 32

void BG_CodRegisterCvars( vmCvar_t *cvars, int maxCvars );
void BG_CodUpdateCvars( vmCvar_t *cvars, int maxCvars );

#endif // __BG_COD_H__
