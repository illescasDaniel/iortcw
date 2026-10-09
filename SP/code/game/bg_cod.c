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

	// codmod: Phase 5b tune — double stamina pool vs initial 4s CoD default (8s full bar).
	{ "cod_sprintTime",         "8",    0,            CODOFS( sprintTime ),         0 },
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

	// codmod: Phase 3 ADS cvars (docs/reference-values.md + docs/phase-3-ads.md). Per-weapon
	// table entries (bg_codWeapons below) override these when non-zero.
	{ "cod_adsEnable",          "1",    0,            CODOFS( adsEnable ),          1 },
	{ "cod_adsMoveScale",       "0.55", 0,            CODOFS( adsMoveScale ),       0 },
	{ "cod_adsFovScale",        "0.8",  0,            CODOFS( adsFovScale ),        0 },
	{ "cod_adsTimeIn",          "220",  0,            CODOFS( adsTimeIn ),          0 },
	{ "cod_adsTimeOut",         "180",  0,            CODOFS( adsTimeOut ),         0 },
	{ "cod_adsSpreadScale",     "0.35", 0,            CODOFS( adsSpreadScale ),     0 },
	{ "cod_adsSensScale",       "1",    0,            CODOFS( adsSensScale ),       0 },
	{ "cod_adsBobScale",        "0.85", 0,            CODOFS( adsBobScale ),        0 },
	// codmod: FOV zoom magnifies the RF_DEPTHHACK viewmodel; push it forward while ADS so
	// it doesn't fill the screen from the hip. Used as a fine bias on top of tag_flash align.
	{ "cod_adsGunFovComp",      "2",    0,            CODOFS( adsGunFovComp ),      0 },
	// codmod: CoD-style iron-sight auto-align defaults (cgame). Per-weapon table can override.
	// Luger tune from codmod_tune.log (2026-10-08, final session).
	{ "cod_adsSightHeight",     "2.5",  0,            CODOFS( adsSightHeight ),     0 },
	{ "cod_adsBarrelBack",      "13",   0,            CODOFS( adsBarrelBack ),      0 },
	{ "cod_adsSightDist",       "20",   0,            CODOFS( adsSightDist ),       0 },
	// codmod: RTCW viewmodels are built for hip-fire; tag_flash "straight" can still look
	// diagonally held. Applied before sight centering (Luger in-game tune).
	{ "cod_adsPitch",           "-1",   0,            CODOFS( adsPitch ),           0 },
	{ "cod_adsYaw",             "-3.5", 0,            CODOFS( adsYaw ),             0 },
	{ "cod_adsRoll",            "6.5",  0,            CODOFS( adsRoll ),            0 },
	// codmod: whole-weapon horizontal shift (viewaxis[1]); tune in /cod_menu.
	{ "cod_adsRight",           "1",    0,            CODOFS( adsRight ),           0 },
	// codmod: 1 = center dot while ADS (interim); 2 = hide (once all weapons are tuned).
	{ "cod_adsCrosshair",       "1",    0,            CODOFS( adsCrosshair ),       1 },
	{ "cod_scopeSwitchTime",    "0",    0,            CODOFS( scopeSwitchTime ),    0 },

	// codmod: Phase 3.5 alignment tooling -- see docs/phase-3-ads.md "Alignment tooling".
	// Dev-only, so these stay un-archived like the rest of the tunables.
	{ "cod_adsLock",            "0",    0,            CODOFS( adsLock ),            1 },
	{ "cod_adsTune",            "0",    0,            CODOFS( adsTune ),            1 },
	{ "cod_adsTuneFwd",         "0",    0,            CODOFS( adsTuneFwd ),         0 },
	{ "cod_adsTuneRight",       "0",    0,            CODOFS( adsTuneRight ),       0 },
	{ "cod_adsTuneUp",          "0",    0,            CODOFS( adsTuneUp ),          0 },
	{ "cod_adsTunePitch",       "0",    0,            CODOFS( adsTunePitch ),       0 },
	{ "cod_adsTuneYaw",         "0",    0,            CODOFS( adsTuneYaw ),         0 },
	{ "cod_adsTuneRoll",        "0",    0,            CODOFS( adsTuneRoll ),        0 },
	{ "cod_adsTuneFov",         "0.8",  0,            CODOFS( adsTuneFov ),         0 },

	// codmod: Phase 4 gait presentation (docs/phase-4-gait.md). Fire cancels sprint same
	// frame (pmove); gun-lower raise time is visual-only and must not gate firing.
	{ "cod_gaitEnable",         "1",    0,            CODOFS( gaitEnable ),         1 },
	{ "cod_sprintRaiseTime",    "180",  0,            CODOFS( sprintRaiseTime ),    0 },
	// codmod: raised after Phase 4 confirm (2026-10-08) so more of the gun stays on-screen
	// while still reading as "sprint lowered" (was Pitch 14 / Up -3).
	{ "cod_sprintGunPitch",     "9",    0,            CODOFS( sprintGunPitch ),     0 },
	{ "cod_sprintGunRoll",      "-8",   0,            CODOFS( sprintGunRoll ),      0 },
	{ "cod_sprintGunFwd",       "-1.5", 0,            CODOFS( sprintGunFwd ),       0 },
	{ "cod_sprintGunUp",        "-0.5", 0,            CODOFS( sprintGunUp ),        0 },
	{ "cod_bobWalkRate",        "0.28", 0,            CODOFS( bobWalkRate ),        0 },
	{ "cod_bobRunRate",         "0.4",  0,            CODOFS( bobRunRate ),         0 },
	{ "cod_bobSprintRate",      "0.5",  0,            CODOFS( bobSprintRate ),      0 },
	{ "cod_bobWalkAmp",         "0.65", 0,            CODOFS( bobWalkAmp ),         0 },
	{ "cod_bobRunAmp",          "1",    0,            CODOFS( bobRunAmp ),          0 },
	{ "cod_bobSprintAmp",       "1.35", 0,            CODOFS( bobSprintAmp ),       0 },
	{ "cod_sprintRunPitch",     "1.75", 0,            CODOFS( sprintRunPitch ),     0 },
	{ "cod_sprintRunRoll",      "1.5",  0,            CODOFS( sprintRunRoll ),      0 },
	{ "cod_sprintFovScale",     "1.03", 0,            CODOFS( sprintFovScale ),     0 },

	// codmod: Phase 5 feel — pistol click-fire + jump/gravity/inertia/land (docs/phase-5-feel.md)
	{ "cod_weaponEnable",       "1",    0,            CODOFS( weaponEnable ),       1 },
	{ "cod_pistolTapFire",      "1",    0,            CODOFS( pistolTapFire ),      1 },
	{ "cod_pistolTapThreshold", "150",  0,            CODOFS( pistolTapThreshold ), 0 },
	{ "cod_pistolNextShot",     "120",  0,            CODOFS( pistolNextShot ),     0 },
	{ "cod_pistolFireDelay",    "0",    0,            CODOFS( pistolFireDelay ),    0 },
	{ "cod_autoFireScale",      "1",    0,            CODOFS( autoFireScale ),      0 },
	{ "cod_jumpVelocity",       "250",  0,            CODOFS( jumpVelocity ),       0 },
	{ "cod_gravity",            "800",  0,            CODOFS( gravity ),            0 },
	{ "cod_accelerate",         "8",    0,            CODOFS( accelerate ),         0 },
	{ "cod_friction",           "8",    0,            CODOFS( friction ),           0 },
	{ "cod_stopspeed",          "100",  0,            CODOFS( stopspeed ),          0 },
	{ "cod_landSlowEnable",     "1",    0,            CODOFS( landSlowEnable ),     1 },
	{ "cod_landSlowTime",       "350",  0,            CODOFS( landSlowTime ),       0 },
	{ "cod_landSlowScale",      "0.55", 0,            CODOFS( landSlowScale ),      0 },

	{ "cod_quickKnifeEnable",   "1",    0,            CODOFS( quickKnifeEnable ),   1 },
	{ "cod_quickKnifeTimeout",  "900",  0,            CODOFS( quickKnifeTimeout ),  0 },
	// codmod: snappy knife pull (was 250+250). 75 keeps anim readable but CoD-connected.
	{ "cod_knifeSwitchTime",    "75",   0,            CODOFS( knifeSwitchTime ),    0 },

	// codmod: Phase 5b vitals (docs/phase-5b-vitals.md)
	{ "cod_vitalsEnable",       "1",    0,            CODOFS( vitalsEnable ),       1 },
	{ "cod_healthRegenEnable",  "1",    0,            CODOFS( healthRegenEnable ),  1 },
	{ "cod_healthRegenDelay",   "5000", 0,            CODOFS( healthRegenDelay ),   0 },
	{ "cod_healthRegenRate",    "20",   0,            CODOFS( healthRegenRate ),    0 },
	{ "cod_healthRegenPauseHurt","1",   0,            CODOFS( healthRegenPauseHurt ), 1 },
	{ "cod_healthBarNumber",    "0",    0,            CODOFS( healthBarNumber ),    1 },
	// codmod: half of vanilla/CoD 100 — glass-cannon SP with regen. (phase 5b)
	{ "cod_maxHealth",          "50",   0,            CODOFS( maxHealth ),          0 },
	{ "cod_sprintLinear",       "1",    0,            CODOFS( sprintLinear ),       1 },
	{ "cod_sprintRechargeDelay","600",  0,            CODOFS( sprintRechargeDelay ), 0 },
	{ "cod_ammoScale",          "2",    0,            CODOFS( ammoScale ),          0 },
	{ "cod_interactHint",       "1",    0,            CODOFS( interactHint ),       1 },

	// codmod: Phase 5c CoD HUD (docs/phase-5c-hud.md)
	{ "cod_hudEnable",          "1",    0,            CODOFS( hudEnable ),          1 },
	{ "cod_hudAmmoStyle",       "1",    0,            CODOFS( hudAmmoStyle ),       1 },
	{ "cod_hudHeatColor",       "1",    0,            CODOFS( hudHeatColor ),       1 },
	{ "cod_hudHealthCrosshair", "1",    0,            CODOFS( hudHealthCrosshair ), 1 },
	{ "cod_hudLowHealthBlood",  "1",    0,            CODOFS( hudLowHealthBlood ),  1 },
	{ "cod_hudLowHealthStart",  "0.4",  0,            CODOFS( hudLowHealthStart ),  0 },
	{ "cod_hudLowHealthMaxAlpha","0.55",0,            CODOFS( hudLowHealthMaxAlpha ), 0 },

	// codmod: CoD 4-post reticle — gap blooms with fire spread + move speed; ADS fades posts
	// (center dot kept while cod_adsCrosshair 1 until per-weapon ADS tune).
	{ "cod_crosshairStyle",     "1",    0,            CODOFS( crosshairStyle ),     1 },
	{ "cod_crosshairGap",       "4",    0,            CODOFS( crosshairGap ),       0 },
	{ "cod_crosshairLength",    "8",    0,            CODOFS( crosshairLength ),    0 },
	{ "cod_crosshairWidth",     "2",    0,            CODOFS( crosshairWidth ),     0 },
	{ "cod_crosshairDot",       "1",    0,            CODOFS( crosshairDot ),       1 },
	{ "cod_crosshairBloom",     "16",   0,            CODOFS( crosshairBloom ),     0 },
	{ "cod_crosshairMoveBloom", "14",   0,            CODOFS( crosshairMoveBloom ), 0 },
	{ "cod_crosshairCrouchScale","0.55",0,            CODOFS( crosshairCrouchScale ), 0 },
	{ "cod_crosshairSprintAlpha","0.35",0,            CODOFS( crosshairSprintAlpha ), 0 },
	{ "cod_crosshairOutline",   "1",    0,            CODOFS( crosshairOutline ),   1 },

	// codmod: E9 hit markers (CoD X flash + body/headshot ticks)
	{ "cod_hitMarkerEnable",    "1",    0,            CODOFS( hitMarkerEnable ),    1 },
	{ "cod_hitMarkerTime",      "250",  0,            CODOFS( hitMarkerTime ),      0 },
	{ "cod_hitMarkerGap",       "6",    0,            CODOFS( hitMarkerGap ),       0 },
	{ "cod_hitMarkerLength",    "7",    0,            CODOFS( hitMarkerLength ),    0 },
	{ "cod_hitMarkerWidth",     "2",    0,            CODOFS( hitMarkerWidth ),     0 },
	{ "cod_hitMarkerSound",     "1",    0,            CODOFS( hitMarkerSound ),     1 },

	{ "cod_debugHud",           "0",    CVAR_ARCHIVE, CODOFS( debugHud ),           1 },
	// codmod: /cod_menu overlay size (cgame). Archived like cod_debugHud — UI preference.
	{ "cod_menuScale",          "0.7",  CVAR_ARCHIVE, CODOFS( menuScale ),          0 },
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

// codmod: Phase 3.2 per-weapon ADS table. A field of 0 means "use the cod_ads* cvar
// default" (BG_CodAdsMoveScale() etc. below resolve that). Eligibility list from
// docs/phase-3-ads.md section 3.2:
//   Yes (iron-sight ADS): pistols, SMGs, Mauser/Garand/FG42 unscoped.
//   Scope instead of ADS: the three secondary-fire scope weapons (phase 3.8) -- not
//     canADS, but still get the rifle moveScale while in use (3.8.8).
//   No: knife, grenades, dynamite, akimbo, flamethrower, tesla, venom, panzerfaust,
//     monster/MP-only weapons -- left zero-initialized (canADS qfalse, all scales 0).
const codWeaponTuning_t bg_codWeapons[WP_NUM_WEAPONS] = {
	// pistols: fast up, light zoom, modest move penalty
	[WP_LUGER]          = { qtrue,  0.7f, 0.85f, 150, 0, 0 },
	[WP_SILENCER]       = { qtrue,  0.7f, 0.85f, 150, 0, 0 },
	[WP_COLT]           = { qtrue,  0.7f, 0.85f, 150, 0, 0 },

	// SMGs
	[WP_MP40]           = { qtrue,  0.6f, 0.82f, 200, 0, 0 },
	[WP_THOMPSON]       = { qtrue,  0.6f, 0.82f, 200, 0, 0 },
	[WP_STEN]           = { qtrue,  0.6f, 0.82f, 200, 0, 0 },

	// rifles (iron sights -- scoped alt weapons use the table entry below instead)
	[WP_MAUSER]         = { qtrue,  0.5f, 0.75f, 260, 0, 0 },
	[WP_GARAND]         = { qtrue,  0.5f, 0.75f, 260, 0, 0 },
	[WP_FG42]           = { qtrue,  0.5f, 0.75f, 260, 0, 0 },

	// scoped variants: no iron-sight ADS (the real scope takes over, phase 3.8), but the
	// same rifle move penalty applies while they're the active weapon (3.8.8)
	[WP_SNIPERRIFLE]    = { qfalse, 0.5f, 0,     0,   0, 0 },
	[WP_SNOOPERSCOPE]   = { qfalse, 0.5f, 0,     0,   0, 0 },
	[WP_FG42SCOPE]      = { qfalse, 0.5f, 0,     0,   0, 0 },
};

/*
================
BG_CodIsPistol

codmod: weapons that get SP tap-fire / pistol interval overrides (phase 5).
================
*/
qboolean BG_CodIsPistol( int weapon ) {
	return ( weapon == WP_LUGER || weapon == WP_COLT || weapon == WP_SILENCER ) ? qtrue : qfalse;
}

/*
================
BG_CodMaxAmmo

codmod: scaled max reserve for the player when cod_ammoScale > 1 (phase 5b).
Does not mutate ammoTable (shared with AI).
================
*/
int BG_CodMaxAmmo( int weapon ) {
	int maxa;

	if ( weapon < 0 || weapon >= WP_NUM_WEAPONS ) {
		return 0;
	}
	maxa = ammoTable[weapon].maxammo;
	if ( bg_cod.enable && bg_cod.vitalsEnable && bg_cod.ammoScale > 1.0f ) {
		maxa = (int)( maxa * bg_cod.ammoScale + 0.5f );
	}
	return maxa;
}

/*
================
BG_CodCanADS

codmod: ADS eligibility from playerState_t alone (phase 3.2). PM_CodUpdateADS adds the
waterlevel/ladder checks on top of this (those live in pmove_t/pml_t, not playerState_t).
================
*/
qboolean BG_CodCanADS( const playerState_t *ps ) {
	if ( ps->aiChar ) {
		return qfalse;
	}
	if ( !( bg_cod.enable && bg_cod.adsEnable ) ) {
		return qfalse;
	}
	if ( ps->weapon <= WP_NONE || ps->weapon >= WP_NUM_WEAPONS || !bg_codWeapons[ps->weapon].canADS ) {
		return qfalse;
	}

	switch ( ps->weaponstate ) {
	case WEAPON_RAISING:
	case WEAPON_RAISING_TORELOAD:
	case WEAPON_DROPPING:
	case WEAPON_DROPPING_TORELOAD:
	case WEAPON_RELOADING:
		return qfalse;
	default:
		break;
	}

	if ( ps->eFlags & ( EF_ZOOMING | EF_MG42_ACTIVE | EF_MELEE_ACTIVE ) ) {
		return qfalse;
	}

	return qtrue;
}

/*
================
BG_CodAdsMoveScale / BG_CodAdsFovScale / BG_CodAdsSpreadScale / BG_CodAdsTimeIn / BG_CodAdsTimeOut

codmod: resolve a weapon's ADS value, falling back to the matching cod_ads* cvar when the
table entry is 0 (phase 3.2). Weapon indices outside [WP_NONE, WP_NUM_WEAPONS) just get the
cvar default -- callers only reach these with a currently-equipped weapon, but stay safe.
================
*/
float BG_CodAdsMoveScale( int weapon ) {
	if ( weapon > WP_NONE && weapon < WP_NUM_WEAPONS && bg_codWeapons[weapon].moveScale != 0 ) {
		return bg_codWeapons[weapon].moveScale;
	}
	return bg_cod.adsMoveScale;
}

float BG_CodAdsFovScale( int weapon ) {
	if ( weapon > WP_NONE && weapon < WP_NUM_WEAPONS && bg_codWeapons[weapon].fovScale != 0 ) {
		return bg_codWeapons[weapon].fovScale;
	}
	return bg_cod.adsFovScale;
}

float BG_CodAdsSpreadScale( int weapon ) {
	if ( weapon > WP_NONE && weapon < WP_NUM_WEAPONS && bg_codWeapons[weapon].spreadScale != 0 ) {
		return bg_codWeapons[weapon].spreadScale;
	}
	return bg_cod.adsSpreadScale;
}

int BG_CodAdsTimeIn( int weapon ) {
	if ( weapon > WP_NONE && weapon < WP_NUM_WEAPONS && bg_codWeapons[weapon].timeIn != 0 ) {
		return bg_codWeapons[weapon].timeIn;
	}
	return (int)bg_cod.adsTimeIn;
}

int BG_CodAdsTimeOut( int weapon ) {
	if ( weapon > WP_NONE && weapon < WP_NUM_WEAPONS && bg_codWeapons[weapon].timeOut != 0 ) {
		return bg_codWeapons[weapon].timeOut;
	}
	return (int)bg_cod.adsTimeOut;
}

/*
================
BG_CodIsScopeWeapon

codmod: true for the three secondary-fire scope weapons (phase 3.8). They occupy a
contiguous enum range (bg_public.h WP_BEGINSECONDARY..WP_LASTSECONDARY).
================
*/
qboolean BG_CodIsScopeWeapon( int weapon ) {
	return ( weapon >= WP_BEGINSECONDARY && weapon <= WP_LASTSECONDARY ) ? qtrue : qfalse;
}
