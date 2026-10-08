// codmod: new file (phase 2.0) -- cg_cod.c
//
// Debug HUD and dump command for the cod_* tuning values. See docs/architecture.md
// section 6. Extended in later phases (ADS, slide, prone).

#include "cg_local.h"

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
