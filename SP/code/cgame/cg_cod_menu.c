// codmod: new file -- cg_cod_menu.c
//
// In-game sectioned tuning menu for all cod_* cvars. Open with /cod_menu (needs leading
// slash in the tilde console). Changes and dumps are logged to the console and to
// codmod_tune.log under fs_homepath so an agent can read them after a play session.

#include "cg_local.h"
#include "../client/keycodes.h"

#define COD_MENU_LOG_FILE   "codmod_tune.log"
#define COD_MENU_Y          8
#define COD_MENU_PAD        4
#define COD_MENU_MARGIN     8
#define COD_MENU_MAX_H      400   // virtual-pixel cap so the overlay never eats the whole screen
#define COD_MENU_BASE_W     292   // width at scale 1.0 (SMALLCHAR)
#define COD_MENU_SCALE_MIN  0.5f
#define COD_MENU_SCALE_MAX  1.0f

typedef enum {
	COD_MI_SECTION,
	COD_MI_FLOAT,
	COD_MI_INT,
	COD_MI_BOOL,
	COD_MI_ACTION
} codMenuItemType_t;

typedef struct {
	codMenuItemType_t type;
	const char *label;
	const char *cvar;       // NULL for section / some actions
	float step;
	float stepFast;
	void ( *action )( void );
} codMenuItem_t;

static qboolean cg_codMenuOpen;
static int cg_codMenuCursor;
static int cg_codMenuScroll;

/*
================
CG_CodMenu_Log

codmod: write one line to rtcwconsole.log (via CG_Printf) and append the same line to
codmod_tune.log under the mod homepath for agent-side tuning review.
================
*/
static void CG_CodMenu_Log( const char *line ) {
	fileHandle_t f;
	char buf[512];
	int len;

	CG_Printf( "%s\n", line );

	Com_sprintf( buf, sizeof( buf ), "%s\n", line );
	len = (int)strlen( buf );
	if ( trap_FS_FOpenFile( COD_MENU_LOG_FILE, &f, FS_APPEND ) >= 0 && f ) {
		trap_FS_Write( buf, len, f );
		trap_FS_FCloseFile( f );
	}
}

static const char *CG_CodMenu_CvarDefault( const char *name ) {
	int i;

	for ( i = 0; i < bg_numCodCvars; i++ ) {
		if ( !Q_stricmp( bg_codCvars[i].name, name ) ) {
			return bg_codCvars[i].def;
		}
	}
	return "0";
}

static void CG_CodMenu_GetValue( const char *name, char *out, int outSize ) {
	trap_Cvar_VariableStringBuffer( name, out, outSize );
}

static void CG_CodMenu_SetValue( const char *name, const char *value ) {
	char line[256];

	trap_Cvar_Set( name, value );
	Com_sprintf( line, sizeof( line ), "codmenu: change /set %s \"%s\"", name, value );
	CG_CodMenu_Log( line );
}

static void CG_CodMenu_Adjust( const codMenuItem_t *item, float dir, qboolean fast ) {
	char cur[MAX_CVAR_VALUE_STRING];
	char out[64];
	float step;
	float v;
	int iv;

	if ( !item->cvar ) {
		return;
	}

	CG_CodMenu_GetValue( item->cvar, cur, sizeof( cur ) );
	step = fast ? item->stepFast : item->step;
	if ( step <= 0.0f ) {
		step = 1.0f;
	}

	if ( item->type == COD_MI_BOOL ) {
		iv = atoi( cur ) ? 0 : 1;
		Com_sprintf( out, sizeof( out ), "%d", iv );
		CG_CodMenu_SetValue( item->cvar, out );
		return;
	}

	if ( item->type == COD_MI_INT ) {
		iv = atoi( cur ) + (int)( dir * step );
		Com_sprintf( out, sizeof( out ), "%d", iv );
		CG_CodMenu_SetValue( item->cvar, out );
		return;
	}

	// float
	v = atof( cur ) + dir * step;
	// codmod: keep menu scale in a usable range (also clamped when drawing)
	if ( item->cvar && !Q_stricmp( item->cvar, "cod_menuScale" ) ) {
		if ( v < COD_MENU_SCALE_MIN ) {
			v = COD_MENU_SCALE_MIN;
		} else if ( v > COD_MENU_SCALE_MAX ) {
			v = COD_MENU_SCALE_MAX;
		}
	}
	Com_sprintf( out, sizeof( out ), "%g", v );
	CG_CodMenu_SetValue( item->cvar, out );
}

static void CG_CodMenu_ResetItem( const codMenuItem_t *item ) {
	if ( !item->cvar ) {
		return;
	}
	CG_CodMenu_SetValue( item->cvar, CG_CodMenu_CvarDefault( item->cvar ) );
}

static void CG_CodMenu_DumpAll( void );
static void CG_CodMenu_DumpRuntime( void );

static void CG_CodMenu_ActionDump( void ) {
	CG_CodMenu_DumpAll();
}

static void CG_CodMenu_ActionLockAds( void ) {
	char cur[MAX_CVAR_VALUE_STRING];
	CG_CodMenu_GetValue( "cod_adsLock", cur, sizeof( cur ) );
	CG_CodMenu_SetValue( "cod_adsLock", atoi( cur ) ? "0" : "1" );
}

static void CG_CodMenu_ActionToggleHud( void ) {
	char cur[MAX_CVAR_VALUE_STRING];
	int v;

	CG_CodMenu_GetValue( "cod_debugHud", cur, sizeof( cur ) );
	v = atoi( cur ) + 1;
	if ( v > 2 ) {
		v = 0;
	}
	CG_CodMenu_SetValue( "cod_debugHud", va( "%d", v ) );
}

// codmod: every tunable, grouped for the overlay. Keep in sync with bg_codCvars.
static const codMenuItem_t cg_codMenuItems[] = {
	{ COD_MI_SECTION, "MENU UI", NULL, 0, 0, NULL },
	{ COD_MI_FLOAT, "cod_menuScale",          "cod_menuScale",          0.05f, 0.1f, NULL },

	{ COD_MI_SECTION, "MASTER", NULL, 0, 0, NULL },
	{ COD_MI_BOOL,  "cod_enable",             "cod_enable",             1, 1, NULL },
	{ COD_MI_BOOL,  "cod_moveEnable",         "cod_moveEnable",         1, 1, NULL },
	{ COD_MI_BOOL,  "cod_leanEnable",         "cod_leanEnable",         1, 1, NULL },
	{ COD_MI_BOOL,  "cod_adsEnable",          "cod_adsEnable",          1, 1, NULL },
	{ COD_MI_BOOL,  "cod_gaitEnable",         "cod_gaitEnable",         1, 1, NULL },
	{ COD_MI_BOOL,  "cod_weaponEnable",       "cod_weaponEnable",       1, 1, NULL },
	{ COD_MI_BOOL,  "cod_vitalsEnable",       "cod_vitalsEnable",       1, 1, NULL },

	{ COD_MI_SECTION, "MOVEMENT SPEEDS", NULL, 0, 0, NULL },
	{ COD_MI_FLOAT, "cod_runSpeed",           "cod_runSpeed",           5,    25,   NULL },
	{ COD_MI_FLOAT, "cod_sprintScale",        "cod_sprintScale",        0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_walkScale",          "cod_walkScale",          0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_crouchScale",        "cod_crouchScale",        0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_backScale",          "cod_backScale",          0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_strafeScale",        "cod_strafeScale",        0.05f, 0.25f, NULL },

	{ COD_MI_SECTION, "SPRINT", NULL, 0, 0, NULL },
	{ COD_MI_FLOAT, "cod_sprintTime",         "cod_sprintTime",         0.25f, 1,    NULL },
	{ COD_MI_FLOAT, "cod_sprintRechargeTime", "cod_sprintRechargeTime", 0.25f, 1,    NULL },
	{ COD_MI_FLOAT, "cod_sprintMinTime",      "cod_sprintMinTime",      0.25f, 1,    NULL },
	{ COD_MI_BOOL,  "cod_sprintForwardOnly",  "cod_sprintForwardOnly",  1,     1,    NULL },
	{ COD_MI_FLOAT, "cod_sprintJumpCost",     "cod_sprintJumpCost",     100,   500,  NULL },
	{ COD_MI_BOOL,  "cod_sprintLinear",       "cod_sprintLinear",       1,     1,    NULL },
	{ COD_MI_FLOAT, "cod_sprintRechargeDelay","cod_sprintRechargeDelay",50,    200,  NULL },

	{ COD_MI_SECTION, "VITALS (health / HUD)", NULL, 0, 0, NULL },
	{ COD_MI_FLOAT, "cod_maxHealth",          "cod_maxHealth",          5,     25,   NULL },
	{ COD_MI_BOOL,  "cod_healthRegenEnable",  "cod_healthRegenEnable",  1,     1,    NULL },
	{ COD_MI_FLOAT, "cod_healthRegenDelay",   "cod_healthRegenDelay",   250,   1000, NULL },
	{ COD_MI_FLOAT, "cod_healthRegenRate",    "cod_healthRegenRate",    1,     5,    NULL },
	{ COD_MI_BOOL,  "cod_healthRegenPauseHurt","cod_healthRegenPauseHurt", 1,  1,    NULL },
	{ COD_MI_BOOL,  "cod_healthBarNumber",    "cod_healthBarNumber",    1,     1,    NULL },

	{ COD_MI_SECTION, "GAIT / SPRINT GUN", NULL, 0, 0, NULL },
	{ COD_MI_FLOAT, "cod_sprintRaiseTime",    "cod_sprintRaiseTime",    10,   50,   NULL },
	{ COD_MI_FLOAT, "cod_sprintGunPitch",     "cod_sprintGunPitch",     0.5f, 2,    NULL },
	{ COD_MI_FLOAT, "cod_sprintGunRoll",      "cod_sprintGunRoll",      0.5f, 2,    NULL },
	{ COD_MI_FLOAT, "cod_sprintGunFwd",       "cod_sprintGunFwd",       0.25f, 1,   NULL },
	{ COD_MI_FLOAT, "cod_sprintGunUp",        "cod_sprintGunUp",        0.25f, 1,   NULL },
	{ COD_MI_FLOAT, "cod_bobWalkRate",        "cod_bobWalkRate",        0.02f, 0.05f, NULL },
	{ COD_MI_FLOAT, "cod_bobRunRate",         "cod_bobRunRate",         0.02f, 0.05f, NULL },
	{ COD_MI_FLOAT, "cod_bobSprintRate",      "cod_bobSprintRate",      0.02f, 0.05f, NULL },
	{ COD_MI_FLOAT, "cod_bobWalkAmp",         "cod_bobWalkAmp",         0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_bobRunAmp",          "cod_bobRunAmp",          0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_bobSprintAmp",       "cod_bobSprintAmp",       0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_sprintRunPitch",     "cod_sprintRunPitch",     0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_sprintRunRoll",      "cod_sprintRunRoll",      0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_sprintFovScale",     "cod_sprintFovScale",     0.01f, 0.05f, NULL },

	{ COD_MI_SECTION, "FEEL (jump / inertia / land)", NULL, 0, 0, NULL },
	{ COD_MI_FLOAT, "cod_jumpVelocity",       "cod_jumpVelocity",       5,    25,   NULL },
	{ COD_MI_FLOAT, "cod_gravity",            "cod_gravity",            10,   50,   NULL },
	{ COD_MI_FLOAT, "cod_accelerate",         "cod_accelerate",         0.25f, 1,   NULL },
	{ COD_MI_FLOAT, "cod_friction",           "cod_friction",           0.25f, 1,   NULL },
	{ COD_MI_FLOAT, "cod_stopspeed",          "cod_stopspeed",          5,    25,   NULL },
	{ COD_MI_BOOL,  "cod_landSlowEnable",     "cod_landSlowEnable",     1,    1,    NULL },
	{ COD_MI_FLOAT, "cod_landSlowTime",       "cod_landSlowTime",       25,   100,  NULL },
	{ COD_MI_FLOAT, "cod_landSlowScale",      "cod_landSlowScale",      0.05f, 0.25f, NULL },

	{ COD_MI_SECTION, "WEAPON FIRE", NULL, 0, 0, NULL },
	{ COD_MI_BOOL,  "cod_pistolTapFire",      "cod_pistolTapFire",      1,    1,    NULL },
	{ COD_MI_FLOAT, "cod_pistolTapThreshold", "cod_pistolTapThreshold", 10,   50,   NULL },
	{ COD_MI_FLOAT, "cod_pistolNextShot",     "cod_pistolNextShot",     10,   50,   NULL },
	{ COD_MI_FLOAT, "cod_pistolFireDelay",    "cod_pistolFireDelay",    10,   50,   NULL },
	{ COD_MI_FLOAT, "cod_autoFireScale",      "cod_autoFireScale",      0.05f, 0.25f, NULL },
	{ COD_MI_BOOL,  "cod_quickKnifeEnable",   "cod_quickKnifeEnable",   1,    1,    NULL },
	{ COD_MI_FLOAT, "cod_quickKnifeTimeout",  "cod_quickKnifeTimeout",  50,   200,  NULL },
	{ COD_MI_FLOAT, "cod_knifeSwitchTime",    "cod_knifeSwitchTime",    5,    25,   NULL },

	{ COD_MI_SECTION, "CROUCH / DUCK", NULL, 0, 0, NULL },
	{ COD_MI_FLOAT, "cod_duckTime",           "cod_duckTime",           5,    25,   NULL },
	{ COD_MI_BOOL,  "cod_duckEase",           "cod_duckEase",           1,    1,    NULL },
	{ COD_MI_BOOL,  "cod_duckGunDip",         "cod_duckGunDip",         1,    1,    NULL },

	{ COD_MI_SECTION, "LEAN", NULL, 0, 0, NULL },
	{ COD_MI_FLOAT, "cod_leanMax",            "cod_leanMax",            1,    5,    NULL },
	{ COD_MI_FLOAT, "cod_leanRollScale",      "cod_leanRollScale",      0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_leanTimeIn",         "cod_leanTimeIn",         10,   50,   NULL },
	{ COD_MI_FLOAT, "cod_leanTimeOut",        "cod_leanTimeOut",        10,   50,   NULL },
	{ COD_MI_BOOL,  "cod_leanAllowFire",      "cod_leanAllowFire",      1,    1,    NULL },
	{ COD_MI_BOOL,  "cod_leanEase",           "cod_leanEase",           1,    1,    NULL },

	{ COD_MI_SECTION, "AI SPEEDS", NULL, 0, 0, NULL },
	{ COD_MI_INT,   "cod_aiSpeedMode",        "cod_aiSpeedMode",        1,    1,    NULL },
	{ COD_MI_FLOAT, "cod_aiSpeedScale",       "cod_aiSpeedScale",       0.05f, 0.25f, NULL },

	{ COD_MI_SECTION, "ADS GAMEPLAY", NULL, 0, 0, NULL },
	{ COD_MI_FLOAT, "cod_adsMoveScale",       "cod_adsMoveScale",       0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_adsFovScale",        "cod_adsFovScale",        0.05f, 0.1f,  NULL },
	{ COD_MI_FLOAT, "cod_adsTimeIn",          "cod_adsTimeIn",          10,   50,   NULL },
	{ COD_MI_FLOAT, "cod_adsTimeOut",         "cod_adsTimeOut",         10,   50,   NULL },
	{ COD_MI_FLOAT, "cod_adsSpreadScale",     "cod_adsSpreadScale",     0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_adsSensScale",       "cod_adsSensScale",       0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_adsBobScale",        "cod_adsBobScale",        0.05f, 0.25f, NULL },
	{ COD_MI_FLOAT, "cod_adsGunFovComp",      "cod_adsGunFovComp",      0.5f, 2,    NULL },
	{ COD_MI_FLOAT, "cod_scopeSwitchTime",    "cod_scopeSwitchTime",    10,   50,   NULL },

	{ COD_MI_SECTION, "ADS IRON SIGHTS", NULL, 0, 0, NULL },
	{ COD_MI_FLOAT, "cod_adsSightHeight",     "cod_adsSightHeight",     0.5f, 2,    NULL },
	{ COD_MI_FLOAT, "cod_adsBarrelBack",      "cod_adsBarrelBack",      0.5f, 2,    NULL },
	{ COD_MI_FLOAT, "cod_adsSightDist",       "cod_adsSightDist",       0.5f, 2,    NULL },
	{ COD_MI_FLOAT, "cod_adsPitch",           "cod_adsPitch",           0.5f, 2,    NULL },
	{ COD_MI_FLOAT, "cod_adsYaw",             "cod_adsYaw",             0.5f, 2,    NULL },
	{ COD_MI_FLOAT, "cod_adsRoll",            "cod_adsRoll",            0.5f, 2,    NULL },
	{ COD_MI_FLOAT, "cod_adsRight",           "cod_adsRight",           0.25f, 1,   NULL },
	{ COD_MI_INT,   "cod_adsCrosshair",       "cod_adsCrosshair",       1,    1,    NULL },

	{ COD_MI_SECTION, "ADS TUNE (per-weapon live)", NULL, 0, 0, NULL },
	{ COD_MI_BOOL,  "cod_adsLock",            "cod_adsLock",            1,    1,    NULL },
	{ COD_MI_BOOL,  "cod_adsTune",            "cod_adsTune",            1,    1,    NULL },
	{ COD_MI_FLOAT, "cod_adsTuneFwd",         "cod_adsTuneFwd",         0.25f, 1,   NULL },
	{ COD_MI_FLOAT, "cod_adsTuneRight",       "cod_adsTuneRight",       0.25f, 1,   NULL },
	{ COD_MI_FLOAT, "cod_adsTuneUp",          "cod_adsTuneUp",          0.25f, 1,   NULL },
	{ COD_MI_FLOAT, "cod_adsTunePitch",       "cod_adsTunePitch",       0.5f, 2,    NULL },
	{ COD_MI_FLOAT, "cod_adsTuneYaw",         "cod_adsTuneYaw",         0.5f, 2,    NULL },
	{ COD_MI_FLOAT, "cod_adsTuneRoll",        "cod_adsTuneRoll",        0.5f, 2,    NULL },
	{ COD_MI_FLOAT, "cod_adsTuneFov",         "cod_adsTuneFov",         0.05f, 0.1f, NULL },

	{ COD_MI_SECTION, "DEBUG / ACTIONS", NULL, 0, 0, NULL },
	{ COD_MI_INT,   "cod_debugHud",           "cod_debugHud",           1,    1,    NULL },
	{ COD_MI_ACTION, "toggle ads lock",       NULL, 0, 0, CG_CodMenu_ActionLockAds },
	{ COD_MI_ACTION, "cycle debug HUD",       NULL, 0, 0, CG_CodMenu_ActionToggleHud },
	{ COD_MI_ACTION, "DUMP all to log",       NULL, 0, 0, CG_CodMenu_ActionDump },
};

static const int cg_codMenuItemCount = sizeof( cg_codMenuItems ) / sizeof( cg_codMenuItems[0] );

/*
================
CG_CodMenu_Scale / CG_CodMenu_Metrics

codmod: resolve cod_menuScale (live cvar read so LEFT/RIGHT updates the same frame) and
derive char size, panel width/x, and how many rows fit under COD_MENU_MAX_H.
================
*/
static float CG_CodMenu_Scale( void ) {
	char cur[MAX_CVAR_VALUE_STRING];
	float s;

	CG_CodMenu_GetValue( "cod_menuScale", cur, sizeof( cur ) );
	s = (float)atof( cur );
	if ( s < COD_MENU_SCALE_MIN ) {
		s = COD_MENU_SCALE_MIN;
	} else if ( s > COD_MENU_SCALE_MAX ) {
		s = COD_MENU_SCALE_MAX;
	}
	return s;
}

static void CG_CodMenu_Metrics( int *charW, int *charH, int *rowH, int *menuW, int *menuX, int *visible ) {
	float s;
	int headerRows;
	int avail;

	s = CG_CodMenu_Scale();
	*charW = (int)( SMALLCHAR_WIDTH * s + 0.5f );
	*charH = (int)( SMALLCHAR_HEIGHT * s + 0.5f );
	if ( *charW < 4 ) {
		*charW = 4;
	}
	if ( *charH < 8 ) {
		*charH = 8;
	}
	*rowH = *charH + 1;

	*menuW = (int)( COD_MENU_BASE_W * s + 0.5f );
	if ( *menuW < 180 ) {
		*menuW = 180;
	}
	*menuX = SCREEN_WIDTH - *menuW - COD_MENU_MARGIN;

	// title + runtime + 2 hint lines
	headerRows = 4;
	avail = COD_MENU_MAX_H - COD_MENU_PAD * 2 - headerRows * ( *rowH );
	*visible = avail / ( *rowH );
	if ( *visible < 8 ) {
		*visible = 8;
	}
	if ( *visible > cg_codMenuItemCount ) {
		*visible = cg_codMenuItemCount;
	}
}

static void CG_CodMenu_DrawText( int x, int y, int charW, int charH, const char *s, const float *color ) {
	CG_DrawStringExt( x, y, s, color, qtrue, qfalse, charW, charH, 0 );
}

static void CG_CodMenu_DumpRuntime( void ) {
	playerState_t *ps;
	float xySpeed;
	char line[256];

	ps = &cg.predictedPlayerState;
	xySpeed = sqrt( ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1] );
	Com_sprintf( line, sizeof( line ),
				 "codmenu: runtime ads=%.3f fov=%.1f spd=%.1f weapon=%d viewh=%d lean=%.1f",
				 CG_CodAdsFrac(), cg.refdef.fov_x, xySpeed, ps->weapon, ps->viewheight, ps->leanf );
	CG_CodMenu_Log( line );
}

static void CG_CodMenu_DumpAll( void ) {
	int i;
	const char *section;
	char value[MAX_CVAR_VALUE_STRING];
	char line[256];

	CG_CodMenu_Log( "codmenu: ===== DUMP begin =====" );
	CG_CodMenu_DumpRuntime();

	section = "GENERAL";
	for ( i = 0; i < cg_codMenuItemCount; i++ ) {
		const codMenuItem_t *item = &cg_codMenuItems[i];

		if ( item->type == COD_MI_SECTION ) {
			section = item->label;
			Com_sprintf( line, sizeof( line ), "codmenu: --- %s ---", section );
			CG_CodMenu_Log( line );
			continue;
		}
		if ( !item->cvar ) {
			continue;
		}
		CG_CodMenu_GetValue( item->cvar, value, sizeof( value ) );
		Com_sprintf( line, sizeof( line ), "codmenu: /set %s \"%s\"", item->cvar, value );
		CG_CodMenu_Log( line );
	}

	CG_CodMenu_Log( "codmenu: ===== DUMP end =====" );
	CG_Printf( "^2codmenu: wrote %s (and console). Agent can read run/home/codmod/%s\n",
			   COD_MENU_LOG_FILE, COD_MENU_LOG_FILE );
}

static qboolean CG_CodMenu_ItemSelectable( const codMenuItem_t *item ) {
	return item->type != COD_MI_SECTION;
}

static int CG_CodMenu_NextSelectable( int from, int dir ) {
	int i;
	int idx;

	idx = from;
	for ( i = 0; i < cg_codMenuItemCount; i++ ) {
		idx += dir;
		if ( idx < 0 ) {
			idx = cg_codMenuItemCount - 1;
		} else if ( idx >= cg_codMenuItemCount ) {
			idx = 0;
		}
		if ( CG_CodMenu_ItemSelectable( &cg_codMenuItems[idx] ) ) {
			return idx;
		}
	}
	return from;
}

static int CG_CodMenu_NextSection( int from, int dir ) {
	int i;
	int idx;

	idx = from;
	for ( i = 0; i < cg_codMenuItemCount; i++ ) {
		idx += dir;
		if ( idx < 0 ) {
			idx = cg_codMenuItemCount - 1;
		} else if ( idx >= cg_codMenuItemCount ) {
			idx = 0;
		}
		if ( cg_codMenuItems[idx].type == COD_MI_SECTION ) {
			return CG_CodMenu_NextSelectable( idx, 1 );
		}
	}
	return from;
}

static void CG_CodMenu_EnsureCursorVisible( void ) {
	int charW, charH, rowH, menuW, menuX, visible;

	CG_CodMenu_Metrics( &charW, &charH, &rowH, &menuW, &menuX, &visible );

	if ( cg_codMenuCursor < cg_codMenuScroll ) {
		cg_codMenuScroll = cg_codMenuCursor;
	}
	if ( cg_codMenuCursor >= cg_codMenuScroll + visible ) {
		cg_codMenuScroll = cg_codMenuCursor - visible + 1;
	}
	if ( cg_codMenuScroll < 0 ) {
		cg_codMenuScroll = 0;
	}
}

static qboolean CG_CodMenu_Edge( int *state, qboolean down ) {
	qboolean pressed;

	pressed = down && !*state;
	*state = down ? 1 : 0;
	return pressed;
}

/*
================
CG_CodMenu_HandleInput

codmod: poll keys while the menu is open (no KEYCATCH -- gameplay/mouse-look keep working
so you can ADS-tune and adjust at the same time).
================
*/
static void CG_CodMenu_HandleInput( void ) {
	static int stUp, stDown, stLeft, stRight, stEnter, stEsc, stPgUp, stPgDn, stDump, stReset;
	static int stWheelUp, stWheelDn;
	const codMenuItem_t *item;
	qboolean fast;
	qboolean up, down, left, right, enter, esc, pgup, pgdn, dump, reset;
	qboolean wup, wdn;

	if ( !cg_codMenuOpen ) {
		stUp = stDown = stLeft = stRight = stEnter = stEsc = stPgUp = stPgDn = stDump = stReset = 0;
		stWheelUp = stWheelDn = 0;
		return;
	}

	// ignore input while the tilde console is up
	if ( trap_Key_GetCatcher() & KEYCATCH_CONSOLE ) {
		return;
	}

	fast = trap_Key_IsDown( K_SHIFT );
	up = trap_Key_IsDown( K_UPARROW ) || trap_Key_IsDown( K_KP_UPARROW );
	down = trap_Key_IsDown( K_DOWNARROW ) || trap_Key_IsDown( K_KP_DOWNARROW );
	left = trap_Key_IsDown( K_LEFTARROW ) || trap_Key_IsDown( K_KP_LEFTARROW ) || trap_Key_IsDown( K_MWHEELDOWN );
	right = trap_Key_IsDown( K_RIGHTARROW ) || trap_Key_IsDown( K_KP_RIGHTARROW ) || trap_Key_IsDown( K_MWHEELUP );
	// separate wheel edge (also bound above as left/right held -- use dedicated edge below)
	wup = trap_Key_IsDown( K_MWHEELUP );
	wdn = trap_Key_IsDown( K_MWHEELDOWN );
	enter = trap_Key_IsDown( K_ENTER ) || trap_Key_IsDown( K_KP_ENTER ) || trap_Key_IsDown( K_SPACE );
	esc = trap_Key_IsDown( K_ESCAPE );
	pgup = trap_Key_IsDown( K_PGUP ) || trap_Key_IsDown( K_KP_PGUP );
	pgdn = trap_Key_IsDown( K_PGDN ) || trap_Key_IsDown( K_KP_PGDN );
	// F5 dump / Backspace reset -- avoid letter keys (they fight movement/reload binds).
	dump = trap_Key_IsDown( K_F5 );
	reset = trap_Key_IsDown( K_BACKSPACE );

	// don't treat mouse wheel as arrow hold -- clear left/right from wheel
	left = trap_Key_IsDown( K_LEFTARROW ) || trap_Key_IsDown( K_KP_LEFTARROW );
	right = trap_Key_IsDown( K_RIGHTARROW ) || trap_Key_IsDown( K_KP_RIGHTARROW );

	if ( CG_CodMenu_Edge( &stUp, up ) ) {
		cg_codMenuCursor = CG_CodMenu_NextSelectable( cg_codMenuCursor, -1 );
		CG_CodMenu_EnsureCursorVisible();
	}
	if ( CG_CodMenu_Edge( &stDown, down ) ) {
		cg_codMenuCursor = CG_CodMenu_NextSelectable( cg_codMenuCursor, 1 );
		CG_CodMenu_EnsureCursorVisible();
	}
	if ( CG_CodMenu_Edge( &stPgUp, pgup ) ) {
		cg_codMenuCursor = CG_CodMenu_NextSection( cg_codMenuCursor, -1 );
		CG_CodMenu_EnsureCursorVisible();
	}
	if ( CG_CodMenu_Edge( &stPgDn, pgdn ) ) {
		cg_codMenuCursor = CG_CodMenu_NextSection( cg_codMenuCursor, 1 );
		CG_CodMenu_EnsureCursorVisible();
	}

	item = &cg_codMenuItems[cg_codMenuCursor];

	if ( CG_CodMenu_Edge( &stLeft, left ) || CG_CodMenu_Edge( &stWheelDn, wdn ) ) {
		if ( item->type == COD_MI_BOOL || item->type == COD_MI_ACTION ) {
			if ( item->type == COD_MI_BOOL ) {
				CG_CodMenu_Adjust( item, 1, qfalse );
			} else if ( item->action ) {
				item->action();
			}
		} else {
			CG_CodMenu_Adjust( item, -1.0f, fast );
		}
	}
	if ( CG_CodMenu_Edge( &stRight, right ) || CG_CodMenu_Edge( &stWheelUp, wup ) ) {
		if ( item->type == COD_MI_BOOL || item->type == COD_MI_ACTION ) {
			if ( item->type == COD_MI_BOOL ) {
				CG_CodMenu_Adjust( item, 1, qfalse );
			} else if ( item->action ) {
				item->action();
			}
		} else {
			CG_CodMenu_Adjust( item, 1.0f, fast );
		}
	}
	if ( CG_CodMenu_Edge( &stEnter, enter ) ) {
		if ( item->type == COD_MI_BOOL ) {
			CG_CodMenu_Adjust( item, 1, qfalse );
		} else if ( item->type == COD_MI_ACTION && item->action ) {
			item->action();
		}
	}
	if ( CG_CodMenu_Edge( &stReset, reset ) ) {
		if ( item->cvar ) {
			CG_CodMenu_ResetItem( item );
		}
	}
	if ( CG_CodMenu_Edge( &stDump, dump ) ) {
		CG_CodMenu_DumpAll();
	}
	if ( CG_CodMenu_Edge( &stEsc, esc ) ) {
		cg_codMenuOpen = qfalse;
		CG_CodMenu_Log( "codmenu: closed" );
	}
}

/*
================
CG_CodDrawDebugMenu

codmod: right-side overlay listing every cod_* tunable by section. Call from CG_Draw2D.
================
*/
void CG_CodDrawDebugMenu( void ) {
	vec4_t bg = { 0.0f, 0.0f, 0.0f, 0.72f };
	vec4_t selBg = { 0.15f, 0.35f, 0.55f, 0.85f };
	vec4_t sectionCol = { 1.0f, 0.85f, 0.3f, 1.0f };
	vec4_t textCol = { 0.9f, 0.9f, 0.9f, 1.0f };
	vec4_t selCol = { 1.0f, 1.0f, 1.0f, 1.0f };
	vec4_t dimCol = { 0.65f, 0.65f, 0.65f, 1.0f };
	vec4_t hintCol = { 0.7f, 0.8f, 1.0f, 1.0f };
	char line[128];
	char value[MAX_CVAR_VALUE_STRING];
	int i, y, drawn;
	int charW, charH, rowH, menuW, menuX, visible;
	float menuH;

	CG_CodMenu_HandleInput();

	if ( !cg_codMenuOpen ) {
		return;
	}

	CG_CodMenu_Metrics( &charW, &charH, &rowH, &menuW, &menuX, &visible );
	menuH = (float)( COD_MENU_PAD * 2 + ( visible + 4 ) * rowH );
	CG_FillRect( (float)menuX, COD_MENU_Y, (float)menuW, menuH, bg );

	y = COD_MENU_Y + COD_MENU_PAD;
	CG_CodMenu_DrawText( menuX + COD_MENU_PAD, y, charW, charH, "CODMOD TUNE MENU", sectionCol );
	y += rowH;
	Com_sprintf( line, sizeof( line ), "ads %.2f  fov %.0f  weap %d",
				 CG_CodAdsFrac(), cg.refdef.fov_x, cg.predictedPlayerState.weapon );
	CG_CodMenu_DrawText( menuX + COD_MENU_PAD, y, charW, charH, line, hintCol );
	y += rowH + 2;

	drawn = 0;
	for ( i = cg_codMenuScroll; i < cg_codMenuItemCount && drawn < visible; i++ ) {
		const codMenuItem_t *item = &cg_codMenuItems[i];
		int x = menuX + COD_MENU_PAD;

		if ( i == cg_codMenuCursor && CG_CodMenu_ItemSelectable( item ) ) {
			CG_FillRect( (float)( menuX + 2 ), y - 1, (float)( menuW - 4 ), (float)rowH, selBg );
		}

		if ( item->type == COD_MI_SECTION ) {
			Com_sprintf( line, sizeof( line ), "-- %s --", item->label );
			CG_CodMenu_DrawText( x, y, charW, charH, line, sectionCol );
		} else if ( item->type == COD_MI_ACTION ) {
			Com_sprintf( line, sizeof( line ), "[ %s ]", item->label );
			CG_CodMenu_DrawText( x, y, charW, charH, line, ( i == cg_codMenuCursor ) ? selCol : dimCol );
		} else {
			CG_CodMenu_GetValue( item->cvar, value, sizeof( value ) );
			Com_sprintf( line, sizeof( line ), "%-22s %s", item->label, value );
			CG_CodMenu_DrawText( x, y, charW, charH, line, ( i == cg_codMenuCursor ) ? selCol : textCol );
		}

		y += rowH;
		drawn++;
	}

	y += 2;
	CG_CodMenu_DrawText( menuX + COD_MENU_PAD, y, charW, charH,
						 "UP/DN move  L/R adjust  Shift=fast", hintCol );
	y += rowH;
	CG_CodMenu_DrawText( menuX + COD_MENU_PAD, y, charW, charH,
						 "Enter toggle  Bksp reset  F5 dump  Esc close", hintCol );
}

/*
================
CG_CodMenu_f / CG_CodMenuDump_f

codmod: console entry points. In-game tilde console needs a leading / :
  /cod_menu
  /cod_menu_dump
================
*/
void CG_CodMenu_f( void ) {
	cg_codMenuOpen = !cg_codMenuOpen;
	if ( cg_codMenuOpen ) {
		if ( !CG_CodMenu_ItemSelectable( &cg_codMenuItems[cg_codMenuCursor] ) ) {
			cg_codMenuCursor = CG_CodMenu_NextSelectable( cg_codMenuCursor, 1 );
		}
		CG_CodMenu_EnsureCursorVisible();
		CG_CodMenu_Log( "codmenu: opened" );
		CG_Printf( "^2codmenu: UP/DN select, LEFT/RIGHT adjust, F5 dump, Esc close\n" );
	} else {
		CG_CodMenu_Log( "codmenu: closed" );
	}
}

void CG_CodMenuDump_f( void ) {
	CG_CodMenu_DumpAll();
}
