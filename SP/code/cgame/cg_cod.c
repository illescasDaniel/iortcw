// codmod: new file (phase 2.0) -- cg_cod.c
//
// Debug HUD and dump command for the cod_* tuning values. See docs/architecture.md
// section 6. Extended in later phases (ADS, slide, prone).

#include "cg_local.h"

// codmod: Phase 3.5 per-weapon ADS viewmodel offsets. Rough guesses -- RTCW viewmodels
// were never authored for ADS, so these raise/center the gun rather than align true iron
// sights. Tune in-game with cod_adsLock 1; cod_adsTune 1 (see docs/phase-3-ads.md section
// 3.5), then cod_adsTuneDump to get an updated initializer line to paste back here. Not
// const: the tuning tool overwrites entries live while cod_adsTune is set.
codAdsView_t cg_codAdsView[WP_NUM_WEAPONS] = {
	// pistols: small gun, brought up close to the eye
	[WP_LUGER]          = { 1.5f, -3.5f, 2.0f, 0.0f, 0.0f, 0.0f },
	[WP_SILENCER]       = { 1.8f, -3.5f, 2.0f, 0.0f, 0.0f, 0.0f },
	[WP_COLT]           = { 1.5f, -3.5f, 2.0f, 0.0f, 0.0f, 0.0f },

	// SMGs
	[WP_MP40]           = { 2.0f, -4.6f, 1.2f, 0.0f, 0.0f, 0.0f },
	[WP_THOMPSON]       = { 2.0f, -4.4f, 1.3f, 0.0f, 0.0f, 0.0f },
	[WP_STEN]           = { 1.8f, -4.0f, 1.4f, 0.0f, 0.0f, 0.0f },

	// rifles (iron sights)
	[WP_MAUSER]         = { 3.0f, -2.0f, 1.0f, 0.0f, 0.0f, 0.0f },
	[WP_GARAND]         = { 3.0f, -2.2f, 1.0f, 0.0f, 0.0f, 0.0f },
	[WP_FG42]           = { 2.6f, -2.5f, 1.1f, 0.0f, 0.0f, 0.0f },

	// scoped variants draw no viewmodel offset of their own -- the real scope (phase 3.8)
	// takes over the whole screen, so these stay zeroed.
};

/*
================
CG_CodAdsFrac

codmod: raw ADS fraction, 0..1 (phase 3.5). cod_adsLock pins it to 1 so the alignment
tooling doesn't need a held button; every other visual ADS reader (viewmodel, FOV,
sensitivity, crosshair, debug HUD) goes through this so the lock behaves identically
everywhere.
================
*/
float CG_CodAdsFrac( void ) {
	if ( bg_cod.adsLock ) {
		return 1.0f;
	}
	return cg.predictedPlayerState.stats[STAT_COD_ADS] / 1000.0f;
}

/*
================
CG_CodUpdateAdsTune

codmod: cod_adsTune 1 live-overwrites the current weapon's cg_codAdsView entry from the
cod_adsTuneFwd/Right/Up/Pitch/Yaw/Roll cvars every frame (phase 3.5 alignment tooling).
Called once per real frame from CG_DrawActiveFrame (cg_view.c).
================
*/
void CG_CodUpdateAdsTune( void ) {
	int weapon;
	codAdsView_t *ads;

	if ( !bg_cod.adsTune ) {
		return;
	}

	weapon = cg.predictedPlayerState.weapon;
	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS ) {
		return;
	}

	ads = &cg_codAdsView[weapon];
	ads->fwd   = bg_cod.adsTuneFwd;
	ads->right = bg_cod.adsTuneRight;
	ads->up    = bg_cod.adsTuneUp;
	ads->pitch = bg_cod.adsTunePitch;
	ads->yaw   = bg_cod.adsTuneYaw;
	ads->roll  = bg_cod.adsTuneRoll;
}

/*
================
CG_CodAdsTuneLoad_f

codmod: "cod_adsTuneLoad" console command (phase 3.5 alignment tooling). Copies the
current weapon's cg_codAdsView entry (and its resolved fovScale) into the cod_adsTune*
cvars, so tuning a weapon that already has values starts from them instead of from zero.
================
*/
void CG_CodAdsTuneLoad_f( void ) {
	int weapon;
	codAdsView_t *ads;

	weapon = cg.predictedPlayerState.weapon;
	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS ) {
		CG_Printf( "cod_adsTuneLoad: no weapon selected\n" );
		return;
	}

	ads = &cg_codAdsView[weapon];
	trap_Cvar_Set( "cod_adsTuneFwd", va( "%g", ads->fwd ) );
	trap_Cvar_Set( "cod_adsTuneRight", va( "%g", ads->right ) );
	trap_Cvar_Set( "cod_adsTuneUp", va( "%g", ads->up ) );
	trap_Cvar_Set( "cod_adsTunePitch", va( "%g", ads->pitch ) );
	trap_Cvar_Set( "cod_adsTuneYaw", va( "%g", ads->yaw ) );
	trap_Cvar_Set( "cod_adsTuneRoll", va( "%g", ads->roll ) );
	trap_Cvar_Set( "cod_adsTuneFov", va( "%g", BG_CodAdsFovScale( weapon ) ) );

	CG_Printf( "cod_adsTuneLoad: loaded weapon %d\n", weapon );
}

// codmod: weapon enum -> name, for CG_CodAdsTuneDump_f's pasteable output (phase 3.5).
// Only the ADS-relevant weapons are named; anything else just prints its numeric index.
static const char *CG_CodWeaponName( int weapon ) {
	switch ( weapon ) {
	case WP_LUGER:        return "WP_LUGER";
	case WP_SILENCER:     return "WP_SILENCER";
	case WP_COLT:         return "WP_COLT";
	case WP_MP40:         return "WP_MP40";
	case WP_THOMPSON:     return "WP_THOMPSON";
	case WP_STEN:         return "WP_STEN";
	case WP_MAUSER:       return "WP_MAUSER";
	case WP_GARAND:       return "WP_GARAND";
	case WP_FG42:         return "WP_FG42";
	case WP_SNIPERRIFLE:  return "WP_SNIPERRIFLE";
	case WP_SNOOPERSCOPE: return "WP_SNOOPERSCOPE";
	case WP_FG42SCOPE:    return "WP_FG42SCOPE";
	default:              return NULL;
	}
}

/*
================
CG_CodAdsTuneDump_f

codmod: "cod_adsTuneDump" console command (phase 3.5 alignment tooling). Prints a
ready-to-paste C initializer line for the current weapon's cg_codAdsView entry.
================
*/
void CG_CodAdsTuneDump_f( void ) {
	int weapon;
	codAdsView_t *ads;
	const char *name;

	weapon = cg.predictedPlayerState.weapon;
	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS ) {
		CG_Printf( "cod_adsTuneDump: no weapon selected\n" );
		return;
	}

	ads = &cg_codAdsView[weapon];
	name = CG_CodWeaponName( weapon );

	if ( name ) {
		CG_Printf( "[%s] = { %gf, %gf, %gf, %gf, %gf, %gf },\n",
				   name, ads->fwd, ads->right, ads->up, ads->pitch, ads->yaw, ads->roll );
	} else {
		CG_Printf( "[%d] = { %gf, %gf, %gf, %gf, %gf, %gf },\n",
				   weapon, ads->fwd, ads->right, ads->up, ads->pitch, ads->yaw, ads->roll );
	}
}

/*
================
CG_CodStateName

codmod: picks the debug HUD state label. Priority matches the order in architecture.md
section 6 (RUN|WALK|SPRINT|CROUCH|AIR); PRONE/CRAWL/SLIDE are added in phases 4/4b.
================
*/
static const char *CG_CodStateName( void ) {
	playerState_t *ps;
	usercmd_t cmd;

	ps = &cg.predictedPlayerState;

	if ( ps->groundEntityNum == ENTITYNUM_NONE ) {
		return "AIR";
	}
	if ( ps->pm_flags & PMF_DUCKED ) {
		return "CROUCH";
	}
	if ( ps->stats[STAT_COD_FLAGS] & COD_SF_SPRINTING ) {
		return "SPRINT";
	}

	trap_GetUserCmd( trap_GetCurrentCmdNumber(), &cmd );
	if ( cmd.buttons & BUTTON_WALKING ) {
		return "WALK";
	}

	return "RUN";
}

/*
================
CG_CodNearestAISpeed

codmod: cod_debugHud 2 extra line (phase 2.4 check). Cgame doesn't get other entities'
playerState_t, only entityState_t, so "nearest AI" here means the nearest other ET_PLAYER
entity -- close enough for a tuning HUD. Reads cg_entities the same way
CG_PlayerAnimation does for its own speed estimate (cg_players.c).
================
*/
static float CG_CodNearestAISpeed( void ) {
	int i;
	float bestDistSq;
	float speed;

	bestDistSq = -1.0f;
	speed = 0.0f;

	for ( i = 0; i < MAX_GENTITIES; i++ ) {
		centity_t *cent;
		float distSq;

		cent = &cg_entities[i];

		if ( !cent->currentValid ) {
			continue;
		}
		if ( cent->currentState.eType != ET_PLAYER ) {
			continue;
		}
		if ( cent->currentState.number == cg.snap->ps.clientNum ) {
			continue;   // that's the local player
		}

		distSq = DistanceSquared( cg.predictedPlayerState.origin, cent->lerpOrigin );
		if ( bestDistSq < 0.0f || distSq < bestDistSq ) {
			bestDistSq = distSq;
			speed = sqrt( cent->currentState.pos.trDelta[0] * cent->currentState.pos.trDelta[0]
						  + cent->currentState.pos.trDelta[1] * cent->currentState.pos.trDelta[1] );
		}
	}

	return speed;
}

/*
================
CG_CodDrawDebugHud

codmod: top-left overlay, "cod_debugHud 1" (phase 2.0). Hooked from CG_Draw2D next to
cg_drawFPS. Horizontal speed is sqrt(vx^2+vy^2) of the predicted player's velocity, as
specified in architecture.md section 6.
================
*/
void CG_CodDrawDebugHud( void ) {
	playerState_t *ps;
	vec4_t color = { 1.0f, 1.0f, 1.0f, 1.0f };
	float xySpeed;
	char line[128];
	int y;

	if ( !bg_cod.debugHud ) {
		return;
	}

	ps = &cg.predictedPlayerState;
	xySpeed = sqrt( ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1] );
	y = 4;

	Com_sprintf( line, sizeof( line ), "spd %.1f u/s  state %s", xySpeed, CG_CodStateName() );
	CG_DrawSmallStringColor( 4, y, line, color );
	y += SMALLCHAR_HEIGHT;

	Com_sprintf( line, sizeof( line ), "stam %d/20000  viewh %d", ps->sprintTime, ps->viewheight );
	CG_DrawSmallStringColor( 4, y, line, color );
	y += SMALLCHAR_HEIGHT;

	Com_sprintf( line, sizeof( line ), "lean %.1f/%.0f", ps->leanf, bg_cod.leanMax );
	CG_DrawSmallStringColor( 4, y, line, color );
	y += SMALLCHAR_HEIGHT;

	if ( bg_cod.debugHud >= 2 ) {
		Com_sprintf( line, sizeof( line ), "ai spd %.1f u/s", CG_CodNearestAISpeed() );
		CG_DrawSmallStringColor( 4, y, line, color );
		y += SMALLCHAR_HEIGHT;
	}
}

/*
================
CG_CodDump_f

codmod: "cod_dump" console command (phase 2.0). Prints "seta cod_xxx value" for every
tuning cvar so a tuned feel can be pasted into mod/codmod/codmod.cfg or back into
bg_codCvars (bg_cod.c).
================
*/
void CG_CodDump_f( void ) {
	int i;

	for ( i = 0; i < bg_numCodCvars; i++ ) {
		char value[MAX_CVAR_VALUE_STRING];

		trap_Cvar_VariableStringBuffer( bg_codCvars[i].name, value, sizeof( value ) );
		CG_Printf( "seta %s \"%s\"\n", bg_codCvars[i].name, value );
	}
}
