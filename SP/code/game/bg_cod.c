// codmod: new file (phase 2.0) -- bg_cod.c
//
// Defines the cod_* tuning cvar table and the codTuning_t instance that bg_pmove.c (and
// qagame/cgame modules) read every frame. See bg_cod.h and docs/architecture.md section 5.

#include "../qcommon/q_shared.h"
#include "bg_public.h"
#include "bg_cod.h"

// codmod: forward-declared the same way bg_pmove.c declares cg_gameType/g_gametype --
// this file is compiled once into qagame (-DGAMEDLL) and once into cgame (-DCGAMEDLL),
// and both modules already have these traps with an identical signature.
void trap_Cvar_Register( vmCvar_t *cvar, const char *var_name, const char *value, int flags );
void trap_Cvar_Update( vmCvar_t *cvar );

// codmod: zero-initialized; real values come from BG_CodUpdateCvars() before first use.
codTuning_t bg_cod;

#define CODOFS( x ) ( (size_t)&( ( (codTuning_t *)0 )->x ) )

// codmod: Phase 2 cvars (docs/reference-values.md + docs/phase-2-movement.md). Flags are 0
// for tunables (not archived, so code defaults always apply -- user overrides live in
// mod/codmod/codmod.cfg) except cod_debugHud, which is a user preference.
const codCvarDef_t bg_codCvars[] = {
	{ "cod_enable",             "1",    0,            CODOFS( enable ),             1 },
	{ "cod_moveEnable",         "1",    0,            CODOFS( moveEnable ),         1 },
	{ "cod_leanEnable",         "1",    0,            CODOFS( leanEnable ),         1 },

	{ "cod_runSpeed",           "200",  0,            CODOFS( runSpeed ),           0 },
	{ "cod_sprintScale",        "1.5",  0,            CODOFS( sprintScale ),        0 },
	{ "cod_walkScale",          "0.45", 0,            CODOFS( walkScale ),          0 },
	{ "cod_crouchScale",        "0.65", 0,            CODOFS( crouchScale ),        0 },
	{ "cod_backScale",          "0.7",  0,            CODOFS( backScale ),          0 },
	{ "cod_strafeScale",        "0.85", 0,            CODOFS( strafeScale ),        0 },

	{ "cod_sprintTime",         "4",    0,            CODOFS( sprintTime ),         0 },
	{ "cod_sprintRechargeTime", "5",    0,            CODOFS( sprintRechargeTime ), 0 },
	{ "cod_sprintMinTime",      "1",    0,            CODOFS( sprintMinTime ),      0 },
	{ "cod_sprintForwardOnly",  "1",    0,            CODOFS( sprintForwardOnly ),  1 },
	{ "cod_sprintJumpCost",     "2000", 0,            CODOFS( sprintJumpCost ),     0 },

	{ "cod_duckTime",           "70",   0,            CODOFS( duckTime ),           0 },
	{ "cod_duckEase",           "1",    0,            CODOFS( duckEase ),           1 },
	{ "cod_duckGunDip",         "1",    0,            CODOFS( duckGunDip ),         1 },

	{ "cod_leanMax",            "32",   0,            CODOFS( leanMax ),            0 },
	{ "cod_leanRollScale",      "0.6",  0,            CODOFS( leanRollScale ),      0 },
	{ "cod_leanTimeIn",         "200",  0,            CODOFS( leanTimeIn ),         0 },
	{ "cod_leanTimeOut",        "180",  0,            CODOFS( leanTimeOut ),        0 },
	{ "cod_leanAllowFire",      "1",    0,            CODOFS( leanAllowFire ),      1 },
	{ "cod_leanEase",           "1",    0,            CODOFS( leanEase ),           1 },

	{ "cod_aiSpeedMode",        "1",    0,            CODOFS( aiSpeedMode ),        1 },
	{ "cod_aiSpeedScale",       "1",    0,            CODOFS( aiSpeedScale ),       0 },

	{ "cod_debugHud",           "0",    CVAR_ARCHIVE, CODOFS( debugHud ),           1 },
};

const int bg_numCodCvars = sizeof( bg_codCvars ) / sizeof( bg_codCvars[0] );

/*
================
BG_CodRegisterCvars

codmod: registers every entry of bg_codCvars into the caller's vmCvar_t array (one slot
per entry, in order), then applies the just-registered values into bg_cod.
================
*/
void BG_CodRegisterCvars( vmCvar_t *cvars, int maxCvars ) {
	int i;
	int n;

	n = bg_numCodCvars;
	if ( n > maxCvars ) {
		n = maxCvars;    // codmod: should never happen -- bump BG_COD_MAX_CVARS if it does
	}

	for ( i = 0; i < n; i++ ) {
		trap_Cvar_Register( &cvars[i], bg_codCvars[i].name, bg_codCvars[i].def, bg_codCvars[i].flags );
	}

	BG_CodUpdateCvars( cvars, n );
}

/*
================
BG_CodUpdateCvars

codmod: pulls current cvar values into bg_cod. Called every frame from G_UpdateCvars /
CG_UpdateCvars so cvar changes (and savegame loads, see architecture.md section 7) take
effect immediately.
================
*/
void BG_CodUpdateCvars( vmCvar_t *cvars, int maxCvars ) {
	int i;
	int n;
	byte *dst;

	n = bg_numCodCvars;
	if ( n > maxCvars ) {
		n = maxCvars;
	}

	for ( i = 0; i < n; i++ ) {
		trap_Cvar_Update( &cvars[i] );

		dst = (byte *)&bg_cod + bg_codCvars[i].ofs;
		if ( bg_codCvars[i].isInt ) {
			*(int *)dst = cvars[i].integer;
		} else {
			*(float *)dst = cvars[i].value;
		}
	}
}
