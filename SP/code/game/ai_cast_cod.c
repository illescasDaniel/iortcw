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

//===========================================================================
//
// Name:			ai_cast_cod.c
// Function:		codmod tactical AI -- eligibility, per-soldier state and the combat state machine
// Tab Size:		4 (real tabs)
//
// docs/ai-overhaul-plan.md, Phase 1b/1c. Vanilla AIFunc_Battle / BattleTakeCover / BattleChase call
// AICast_CodTacticsActive() right after their danger and door checks; when it says yes they hand over to
// AIFunc_CodCombat, which fights from cover (OPEN -> MOVE -> HIDE <-> PEEK). Everything it cannot handle is
// handed back to the vanilla functions with a short back-off (noTacticsUntil) so the two can never ping-pong.
//
// Phase 2b (push for an angle): when nobody has had a real shot (AICast_CheckAttack) for cod_aiNoShotTime while
// token holders kept trying, the player sits behind cover that cannot be hit. One soldier (the squad's pusher, or a
// solo soldier itself) searches COVER_ANGLE (a spot with a line to the enemy's body points), runs there, and fights
// from it; fallbacks are a grenade flush or an advance hop. See CodTryPush / CodPushArrive / CodPushWatch.
//
// Phase 2 (ai_cast_cod_squad.c) adds squads on top of this: attack tokens decide who may shoot (OPEN, PEEK), a
// token-less soldier stays hidden longer or repositions, voluntary relocations (COVER_ADVANCE hops, the COVER_FLANK
// order) take a mover slot while the others fire from cover, and token holders suppress the threat's last known
// position while a teammate moves. A soldier outside any squad behaves exactly as in Phase 1.
//
// All state lives in the static codAI[] array (never in cast_state_t, which is saved as a raw struct). A savegame
// load just leaves it stale; CodGet() detects that (level.startTime / lastThink) and starts from scratch.
//===========================================================================

#include "g_local.h"
#include "../qcommon/q_shared.h"
#include "../botlib/botlib.h"      //bot lib interface
#include "../botlib/be_aas.h"
#include "../botlib/be_ea.h"
#include "../botlib/be_ai_gen.h"
#include "../botlib/be_ai_goal.h"
#include "../botlib/be_ai_move.h"
#include "../botlib/botai.h"          //bot ai interface

#include "ai_cast.h"
#include "bg_cod.h"
#include "ai_cast_cod.h"

// sub-states of AIFunc_CodCombat
typedef enum {
	COD_OPEN,       // no cover yet: fire while strafing, look for cover
	COD_MOVE,       // running to the chosen cover
	COD_HIDE,       // at the cover, out of the line of fire
	COD_PEEK        // stepped out / stood up to shoot
} codState_t;

static const char *codStateNames[] = { "OPEN", "MOVE", "HIDE", "PEEK" };
static const char *codModeNames[] = { "attack", "hide", "advance", "flank", "angle" };

// why CodEligibility() said no
#define COD_ELIG_OK     0
#define COD_ELIG_NO     1
#define COD_ELIG_LOST   2       // everything else is fine but the enemy has not been seen for cod_aiMemoryTime

#define COD_STALE_MS    5000    // a state this old (or from the future) is not ours any more
#define COD_PEEK_LOST   2500    // enemy unseen from the peek for this long: hand over to the vanilla chase
#define COD_DRAW_MS     300     // cod_aiDebug 2: redraw interval

typedef struct {
	int levelStart;                 // level.startTime this state belongs to
	int lastThink;                  // last AIFunc_CodCombat think
	int noTacticsUntil;             // vanilla runs until then (hand-over back-off)

	int state;
	int stateTime;                  // when the sub-state was entered
	qboolean hasCover;
	codCover_t cover;

	int nextSearchTime;
	int searchFails;
	qboolean urgent;                // next search bypasses the ration
	int lastPainSeen;
	vec3_t avoidPos;                // a spot that just failed
	int avoidUntil;

	// OPEN
	int strafeDir;
	int strafeUntil;
	int nextCrouchRoll;
	int crouchUntil;                // the crouch override (AICast_CodWantsCrouch) wants a crouch until then

	// MOVE
	int moveTimeout;
	int nextVisCheck;

	// HIDE / PEEK
	int hideUntil;
	int hideStart;
	int hideCheck;
	int settledAt;                  // back on the hidden spot since then (0 = still stepping back)
	qboolean reloadIssued;
	int peekUntil;
	int lastSawEnemy;
	int peekCycles;
	int maxPeekCycles;

	// Phase 2
	int relocKind;                  // 0 none, 1 advance hop, 2 flank (this MOVE was a voluntary relocation)
	int nextAdvance;                // next time an advance roll is allowed
	int tokenWaitStart;             // HIDE: first time a peek was refused for lack of a token (0 = not waiting)
	int nextSuppress;               // next time a suppression peek is allowed
	qboolean suppressPeek;          // the current PEEK is a suppression burst

	// Phase 2b: push for an angle
	int noShotMs;                   // ms this soldier tried to shoot (token held, PEEK / OPEN) without a real shot
	int lastTry;
	vec3_t noShotPos;               // threat position the clock belongs to
	int nextPush;                   // solo: next time a push is allowed
	int nextPushTry;                // rate limit of failed attempts
	int pushMode;                   // 0 none, 1 on its way, 2 at the spot
	int pushArrive;
	int pushTryMs;                  // at the spot: ms tried without a shot yet
	qboolean pushShotOk;
	int pushLastShot;
	int nextPushDbg;

	int nextDraw;
} codAI_t;

#define COD_RELOC_NONE      0
#define COD_RELOC_ADVANCE   1
#define COD_RELOC_FLANK     2
#define COD_RELOC_PUSH      3

#define COD_PUSH_NONE       0       // CodTryPush results
#define COD_PUSH_MOVING     1
#define COD_PUSH_GRENADE    2
#define COD_PUSH_GIVEUP_MS  2500    // at the angle spot: tried this long without a shot -> give the spot up
#define COD_PUSH_MAX_MS     9000

static codAI_t codAI[MAX_CLIENTS];

//===========================================================================
// small helpers
//===========================================================================

static int CodRandMs( float lo, float hi ) {
	if ( hi < lo ) {
		hi = lo;
	}
	return (int)( lo + random() * ( hi - lo ) );
}

static const char *CodName( cast_state_t *cs ) {
	char *name = g_entities[cs->entityNum].aiName;

	return name ? name : "?";
}

static void CodClear( codAI_t *ca, int entNum ) {
	memset( ca, 0, sizeof( *ca ) );
	ca->levelStart = level.startTime;
	AICast_CodReleaseCover( entNum );
}

/*
============
CodGet

the soldier's state, cleared when it is stale (savegame load, map change, long gap since our last think)
============
*/
static codAI_t *CodGet( cast_state_t *cs ) {
	codAI_t *ca = &codAI[cs->entityNum];

	if ( ca->levelStart != level.startTime || ca->lastThink > level.time || ca->lastThink < level.time - COD_STALE_MS ) {
		// keep a back-off that is still running (it was set by our own hand-over)
		int keep = ( ca->levelStart == level.startTime && ca->noTacticsUntil > level.time && ca->noTacticsUntil < level.time + 10000 ) ? ca->noTacticsUntil : 0;

		CodClear( ca, cs->entityNum );
		ca->noTacticsUntil = keep;
	}
	return ca;
}

/*
============
AICast_CodReset

codmod: spawn / death of a soldier: forget everything about it
============
*/
void AICast_CodReset( int entNum ) {
	if ( entNum < 0 || entNum >= MAX_CLIENTS ) {
		return;
	}
	CodClear( &codAI[entNum], entNum );
	AICast_CodSquadForget( entNum );
}

/*
============
AICast_CodCanCrouch

codmod: qtrue when this character can physically crouch (the same gate AICast_InputToUserCommand applies)
============
*/
qboolean AICast_CodCanCrouch( cast_state_t *cs ) {
	return ( aiDefaults[cs->aiCharacter].attributes[ATTACK_CROUCH] > 0 ) ? qtrue : qfalse;
}

/*
============
AICast_CodWantsCrouch

codmod: called from the crouch block in AICast_Think. The vanilla rule makes a stationary AI that fired in the
last 2 s stand up, which would defeat crouching behind low cover; this lets the tactical AI keep its crouch.
============
*/
qboolean AICast_CodWantsCrouch( cast_state_t *cs ) {
	codAI_t *ca;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS ) {
		return qfalse;
	}
	ca = &codAI[cs->entityNum];
	if ( ca->levelStart != level.startTime || ca->lastThink < level.time - 300 || cs->aifunc != AIFunc_CodCombat ) {
		return qfalse;
	}
	return ( ca->crouchUntil >= level.time ) ? qtrue : qfalse;
}

//===========================================================================
// eligibility
//===========================================================================

static qboolean CodIsTacticalType( int aiCharacter ) {
	switch ( aiCharacter ) {
	case AICHAR_SOLDIER:
	case AICHAR_AMERICAN:
	case AICHAR_ELITEGUARD:
	case AICHAR_BLACKGUARD:
	case AICHAR_PARTISAN:
	case AICHAR_FROGMAN:
		return qtrue;
	default:
		return qfalse;
	}
}

// bullet weapons that suit fighting from cover (no panzerfaust, scoped rifles, flamers, monster attacks, melee)
static qboolean CodUsableWeapon( int weapon ) {
	switch ( weapon ) {
	case WP_LUGER:
	case WP_COLT:
	case WP_SILENCER:
	case WP_AKIMBO:
	case WP_MP40:
	case WP_THOMPSON:
	case WP_STEN:
	case WP_FG42:
	case WP_MAUSER:
	case WP_GARAND:
		return qtrue;
	default:
		return qfalse;
	}
}

static qboolean CodEnemyVisible( cast_state_t *cs ) {
	cast_visibility_t *vis = &cs->vislist[cs->enemyNum];

	return ( vis->real_visible_timestamp && vis->real_visible_timestamp == vis->real_update_timestamp
			 && vis->real_visible_timestamp > level.time - 500 ) ? qtrue : qfalse;
}

/*
============
CodEligibility

everything in the plan's eligibility list except the hand-over back-off
============
*/
static int CodEligibility( cast_state_t *cs ) {
	gentity_t *ent, *enemy;
	cast_visibility_t *vis;
	int n = cs->entityNum;
	int lastSeen;

	if ( !bg_cod.enable || !bg_cod.aiTactics ) {
		return COD_ELIG_NO;
	}
	if ( n < 0 || n >= MAX_CLIENTS || !cs->bs ) {
		return COD_ELIG_NO;
	}
	ent = &g_entities[n];
	if ( !ent->inuse || !ent->client || ent->health <= 0 ) {
		return COD_ELIG_NO;
	}
	if ( !CodIsTacticalType( cs->aiCharacter ) || cs->aasWorldIndex != 0 ) {
		return COD_ELIG_NO;
	}
	if ( cs->enemyNum < 0 || cs->enemyNum >= MAX_CLIENTS ) {
		return COD_ELIG_NO;
	}
	enemy = &g_entities[cs->enemyNum];
	if ( !enemy->inuse || !enemy->client || enemy->health <= 0 ) {
		return COD_ELIG_NO;
	}
	if ( cs->leaderNum >= 0 ) {
		return COD_ELIG_NO;
	}
	// a usable bullet weapon, not mounted, not zoomed
	if ( ent->active || !CodUsableWeapon( cs->weaponNum ) ) {
		return COD_ELIG_NO;
	}
	if ( cs->aiFlags & ( AIFL_ZOOMING | AIFL_DENYACTION | AIFL_SPECIAL_FUNC | AIFL_WALKFORWARD | AIFL_DISMOUNTING | AIFL_WAITINGTOSPAWN ) ) {
		return COD_ELIG_NO;
	}
	// no script in control
	if ( cs->castScriptStatus.castScriptEventIndex >= 0
		 || cs->castScriptStatus.scriptNoMoveTime >= level.time
		 || cs->castScriptStatus.scriptNoAttackTime >= level.time
		 || cs->castScriptStatus.scriptNoSightTime >= level.time
		 || cs->castScriptStatus.scriptGotoEnt >= 0
		 || cs->castScriptStatus.scriptAttackEnt >= 0
		 || cs->pauseTime >= level.time ) {
		return COD_ELIG_NO;
	}
	// campers keep their post
	if ( cs->attributes[CAMPER] >= 0.5f ) {
		return COD_ELIG_NO;
	}
	// the enemy was seen recently
	vis = &cs->vislist[cs->enemyNum];
	lastSeen = vis->real_visible_timestamp;
	{
		vec3_t sqPos;
		int sqTime;

		// codmod: Phase 2, a squad mate saw the enemy more recently
		if ( AICast_CodSquadThreat( cs, sqPos, &sqTime ) && sqTime > lastSeen ) {
			lastSeen = sqTime;
		}
	}
	if ( !lastSeen || lastSeen <= level.time - (int)bg_cod.aiMemoryTime ) {
		return COD_ELIG_LOST;
	}
	return COD_ELIG_OK;
}

/*
============
AICast_CodTacticsActive

codmod: the router test used by AIFunc_Battle / AIFunc_BattleTakeCover / AIFunc_BattleChase
============
*/
qboolean AICast_CodTacticsActive( cast_state_t *cs ) {
	codAI_t *ca;

	if ( !bg_cod.enable || !bg_cod.aiTactics ) {
		return qfalse;
	}
	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS ) {
		return qfalse;
	}
	ca = CodGet( cs );
	if ( ca->noTacticsUntil >= level.time ) {
		return qfalse;
	}
	return ( CodEligibility( cs ) == COD_ELIG_OK ) ? qtrue : qfalse;
}

//===========================================================================
// debug
//===========================================================================

static void CodThreatPos( cast_state_t *cs, vec3_t out ) {
	cast_visibility_t *vis = &cs->vislist[cs->enemyNum];

	vec3_t sqPos;
	int sqTime;

	if ( CodEnemyVisible( cs ) ) {
		VectorCopy( g_entities[cs->enemyNum].r.currentOrigin, out );
	} else if ( AICast_CodSquadThreat( cs, sqPos, &sqTime ) && sqTime > vis->real_visible_timestamp ) {
		VectorCopy( sqPos, out );       // codmod: Phase 2, what a squad mate saw is newer than what we saw
	} else if ( vis->real_visible_timestamp ) {
		VectorCopy( vis->real_visible_pos, out );
	} else {
		VectorCopy( vis->visible_pos, out );
	}
}

/*
============
CodSetState

enter a sub-state; cod_aiDebug >= 1 prints "[ai] <name> OPEN->MOVE cover=LOW d=312 <why>"
============
*/
static void CodSetState( cast_state_t *cs, codAI_t *ca, int newState, const char *why ) {
	if ( bg_cod.aiDebug >= 1 ) {
		vec3_t threat;

		CodThreatPos( cs, threat );
		G_Printf( "[ai] %s sq%d/%s %s->%s cover=%s d=%d %s\n", CodName( cs ), AICast_CodSquadId( cs ), AICast_CodSquadRoleName( cs ),
				  codStateNames[ca->state], codStateNames[newState],
				  ca->hasCover ? AICast_CodCoverName( ca->cover.type ) : "NONE",
				  (int)Distance( cs->bs->origin, threat ), why ? why : "" );
	}
	ca->state = newState;
	ca->stateTime = level.time;
}

static void CodDebugLine( cast_state_t *cs, vec3_t from, vec3_t to, int box ) {
	gentity_t *tent;

	tent = G_TempEntity( from, EV_RAILTRAIL );
	VectorCopy( to, tent->s.origin2 );
	tent->s.otherEntityNum2 = cs->entityNum;
	tent->s.dmgFlags = box;     // 0 = line, 1 = box (cgame CG_RailTrail)
}

/*
============
CodDebugDraw

cod_aiDebug 2: the chosen cover (box on the floor plus a pole to eye height) and the peek spot, redrawn every
COD_DRAW_MS with short-lived rail trails the player can see
============
*/
static void CodDebugDraw( cast_state_t *cs, codAI_t *ca ) {
	vec3_t a, b;

	if ( bg_cod.aiDebug < 2 || !ca->hasCover || level.time < ca->nextDraw ) {
		return;
	}
	ca->nextDraw = level.time + COD_DRAW_MS;

	VectorCopy( ca->cover.pos, a );
	a[0] -= 16;
	a[1] -= 16;
	a[2] -= 24;
	VectorCopy( ca->cover.pos, b );
	b[0] += 16;
	b[1] += 16;
	b[2] -= 20;
	CodDebugLine( cs, a, b, 1 );

	VectorCopy( ca->cover.pos, a );
	a[2] -= 24;
	VectorCopy( ca->cover.pos, b );
	b[2] += DEFAULT_VIEWHEIGHT;
	CodDebugLine( cs, a, b, 0 );

	if ( ca->cover.type != COVER_LOW ) {
		VectorCopy( ca->cover.pos, a );
		a[2] += DEFAULT_VIEWHEIGHT;
		VectorCopy( ca->cover.peekPos, b );
		b[2] += DEFAULT_VIEWHEIGHT;
		CodDebugLine( cs, a, b, 0 );
	}
}

//===========================================================================
// weapon, movement and view helpers
//===========================================================================

/*
============
CodWeaponTick

the vanilla AIFunc_Battle reload / attack tail: reload when the magazine is empty, switch away when there is no
ammo at all, otherwise fire (if attack). Returns qfalse when the soldier is out of ammo for good.
============
*/
static qboolean CodWeaponTick( cast_state_t *cs, qboolean attack ) {
	if ( cs->bs->cur_ps.weaponstate != WEAPON_RELOADING
		 && cs->bs->cur_ps.ammoclip[BG_FindClipForWeapon( cs->bs->cur_ps.weapon )] < (int)( ammoTable[cs->bs->cur_ps.weapon].uses ) ) {
		if ( AICast_GotEnoughAmmoForWeapon( cs, cs->weaponNum ) ) {
			trap_EA_Reload( cs->entityNum );
		} else {    // no ammo, switch?
			AICast_ChooseWeapon( cs, qfalse );
			if ( cs->weaponNum == WP_NONE || !AICast_GotEnoughAmmoForWeapon( cs, cs->weaponNum ) ) {
				return qfalse;
			}
		}
	} else if ( attack ) {
		AICast_ProcessAttack( cs );
	}
	return qtrue;
}

// qtrue when the current magazine is below fraction/denom of its size
static qboolean CodClipBelow( cast_state_t *cs, int num, int denom ) {
	int clip = cs->bs->cur_ps.ammoclip[BG_FindClipForWeapon( cs->bs->cur_ps.weapon )];
	int max = ammoTable[cs->bs->cur_ps.weapon].maxclip;

	return ( max > 0 && clip * denom < max * num ) ? qtrue : qfalse;
}

// magazine empty and a reload is possible
static qboolean CodNeedsReload( cast_state_t *cs ) {
	int w = cs->bs->cur_ps.weapon;

	return ( cs->bs->cur_ps.ammoclip[BG_FindClipForWeapon( w )] < (int)ammoTable[w].uses
			 && g_entities[cs->entityNum].client->ps.ammo[BG_FindAmmoForWeapon( w )] >= (int)ammoTable[w].uses ) ? qtrue : qfalse;
}

// walk straight at pos (short hops only, the spot was hull-traced by the cover finder)
static void CodMoveDirect( cast_state_t *cs, vec3_t pos, float speedScale ) {
	vec3_t dir;

	VectorSubtract( pos, cs->bs->origin, dir );
	dir[2] = 0;
	if ( VectorNormalize( dir ) < 1.0f ) {
		return;
	}
	trap_EA_Move( cs->entityNum, dir, 400 );
	cs->speedScale = speedScale;
}

static float CodFlatDist( cast_state_t *cs, vec3_t pos ) {
	vec3_t v;

	VectorSubtract( pos, cs->bs->origin, v );
	v[2] *= 0.2f;       // same weighting the vanilla take-cover code uses
	return VectorLength( v );
}

static void CodFaceThreat( cast_state_t *cs, vec3_t threatPos ) {
	vec3_t dir, eye;

	VectorCopy( cs->bs->origin, eye );
	eye[2] += cs->bs->cur_ps.viewheight;
	VectorSubtract( threatPos, eye, dir );
	vectoangles( dir, cs->ideal_viewangles );
}

//===========================================================================
// hand-overs
//===========================================================================

/*
============
CodExit

leaving AIFunc_CodCombat: drop the claim and our crouch, and keep tactics off for backoffMs so the vanilla
function we hand over to cannot route straight back to us
============
*/
static void CodRelocEnd( cast_state_t *cs, codAI_t *ca, qboolean arrived );

// Phase 2b: a push is over (ok: it found its shot, or was replaced by a normal cover fight). Starts the cooldown.
static void CodPushEnd( cast_state_t *cs, codAI_t *ca, qboolean ok ) {
	if ( !ca->pushMode ) {
		return;
	}
	ca->pushMode = 0;
	ca->noShotMs = 0;
	if ( AICast_CodSquadId( cs ) ) {
		AICast_CodSquadPushDone( cs, ok );
	} else {
		ca->nextPush = level.time + (int)( bg_cod.aiPushCooldown * ( ok ? 1.0f : 0.5f ) );
	}
}

static void CodExit( cast_state_t *cs, codAI_t *ca, int backoffMs ) {
	CodRelocEnd( cs, ca, qfalse );
	CodPushEnd( cs, ca, qfalse );
	AICast_CodTokenRelease( cs );
	AICast_CodSquadPublish( cs, qfalse );
	AICast_CodReleaseCover( cs->entityNum );
	ca->hasCover = qfalse;
	ca->crouchUntil = 0;
	cs->attackcrouch_time = 0;
	ca->state = COD_OPEN;
	if ( backoffMs > 0 ) {
		ca->noTacticsUntil = level.time + backoffMs;
	}
}

// the cover we were using is no good any more: back to OPEN and search again (excluding that spot for a while)
static void CodCoverFailed( cast_state_t *cs, codAI_t *ca, qboolean urgent, const char *why ) {
	CodRelocEnd( cs, ca, qfalse );
	CodPushEnd( cs, ca, qfalse );
	AICast_CodTokenRelease( cs );
	if ( ca->hasCover ) {
		VectorCopy( ca->cover.pos, ca->avoidPos );
		ca->avoidUntil = level.time + 3000;
	}
	AICast_CodReleaseCover( cs->entityNum );
	ca->hasCover = qfalse;
	ca->crouchUntil = 0;
	cs->attackcrouch_time = 0;
	ca->nextSearchTime = level.time;
	ca->urgent = urgent;
	CodSetState( cs, ca, COD_OPEN, why );
}

/*
============
CodExposed

is the cover (chosen against threat position cover.threatPos) visible from where the threat is now?
============
*/
static qboolean CodExposed( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	vec3_t threatEye, p;

	VectorCopy( threatPos, threatEye );
	threatEye[2] += DEFAULT_VIEWHEIGHT;

	if ( ca->cover.type == COVER_LOW ) {
		VectorCopy( ca->cover.pos, p );
		p[2] += CROUCH_VIEWHEIGHT;
		if ( AICast_CodLineClear( p, threatEye ) ) {
			return qtrue;
		}
		p[2] = ca->cover.pos[2] + 8;
		return AICast_CodLineClear( p, threatEye );
	}
	VectorCopy( ca->cover.pos, p );
	p[2] += DEFAULT_VIEWHEIGHT;
	return AICast_CodLineClear( p, threatEye );
}

/*
============
CodRelocEnd

a voluntary relocation (advance / flank) is over, arrived or not: give back the mover slot and tell the squad
============
*/
static void CodRelocEnd( cast_state_t *cs, codAI_t *ca, qboolean arrived ) {
	if ( ca->relocKind == COD_RELOC_NONE ) {
		return;
	}
	if ( ca->relocKind == COD_RELOC_FLANK ) {
		AICast_CodSquadFlankDone( cs );
	}
	ca->relocKind = COD_RELOC_NONE;
	AICast_CodMoverRelease( cs );
}

// a search succeeded: claim the spot and run to it
static void CodTakeCover( cast_state_t *cs, codAI_t *ca, int mode ) {
	CodPushEnd( cs, ca, qtrue );        // a new spot ends any push that was at the old one
	ca->hasCover = qtrue;
	AICast_CodClaimCover( cs->entityNum, ca->cover.pos );
	ca->searchFails = 0;
	ca->moveTimeout = level.time + ca->cover.travel * 10 * 2 + 1000;
	ca->nextVisCheck = level.time + 300;
	ca->peekCycles = 0;
	ca->tokenWaitStart = 0;
	ca->suppressPeek = qfalse;
	ca->maxPeekCycles = 3 + rand() % 3;
	ca->reloadIssued = qfalse;
	ca->crouchUntil = 0;
	cs->attackcrouch_time = 0;
	CodSetState( cs, ca, COD_MOVE, va( "%s pick %s travel=%d score=%d cands=%d/%d", codModeNames[mode],
									   AICast_CodCoverName( ca->cover.type ), ca->cover.travel, (int)ca->cover.score,
									   codCoverLastValid, codCoverLastCandidates ) );
}

// run a search of the given mode; qtrue (and the soldier is on its way) when cover was found
static qboolean CodSearch( cast_state_t *cs, codAI_t *ca, vec3_t threatPos, int mode, vec3_t extraAvoid, vec3_t refPos ) {
	vec3_t *avoid = NULL;

	if ( extraAvoid ) {
		avoid = (vec3_t *)extraAvoid;
	} else if ( ca->avoidUntil > level.time ) {
		avoid = &ca->avoidPos;
	}
	if ( AICast_CodFindCover( cs, threatPos, cs->enemyNum, mode, &ca->cover, avoid ? *avoid : NULL, refPos ) ) {
		CodTakeCover( cs, ca, mode );
		return qtrue;
	}
	ca->searchFails++;
	ca->nextSearchTime = level.time + 400 + 400 * ( ca->searchFails < 4 ? ca->searchFails : 4 );
	return qfalse;
}

//===========================================================================
// sub-states
//===========================================================================

/*
============
CodOpenMove

strafe across the enemy's view while firing. Skipped for weapons that cannot fire on the move, while a
crouch burst is on, and when the side step would run into a wall or off a ledge.
============
*/
static void CodOpenMove( cast_state_t *cs, codAI_t *ca, vec3_t threatPos, qboolean shooting ) {
	vec3_t toThreat, perp, dir, end, eye, threatEye;
	trace_t tr;
	static vec3_t mins = { -18, -18, -24 }, maxs = { 18, 18, 48 };
	int tries, d;

	if ( ( shooting && !AICast_CanMoveWhileFiringWeapon( cs->weaponNum ) ) || ca->crouchUntil > level.time ) {
		return;
	}

	VectorSubtract( threatPos, cs->bs->origin, toThreat );
	toThreat[2] = 0;
	if ( VectorNormalize( toThreat ) < 1.0f ) {
		return;
	}
	perp[0] = -toThreat[1];
	perp[1] = toThreat[0];
	perp[2] = 0;

	VectorCopy( threatPos, threatEye );
	threatEye[2] += DEFAULT_VIEWHEIGHT;

	for ( tries = 0; tries < 2; tries++ ) {
		if ( !ca->strafeDir || level.time >= ca->strafeUntil || tries > 0 ) {
			if ( tries == 0 ) {
				ca->strafeDir = ( rand() & 1 ) ? 1 : -1;
				ca->strafeUntil = level.time + CodRandMs( bg_cod.aiStrafeMin, bg_cod.aiStrafeMax );
			} else {
				ca->strafeDir = -ca->strafeDir;     // blocked: try the other way
			}
		}
		d = ca->strafeDir;
		VectorScale( perp, d, dir );

		// room to step: no wall within 48 u, ground below the end of the step
		VectorMA( cs->bs->origin, 48, dir, end );
		trap_Trace( &tr, cs->bs->origin, mins, maxs, end, cs->entityNum, MASK_DEADSOLID );
		if ( tr.fraction < 1.0f || tr.startsolid ) {
			continue;
		}
		VectorCopy( end, tr.endpos );
		tr.endpos[2] -= 56;
		trap_Trace( &tr, end, mins, maxs, tr.endpos, cs->entityNum, MASK_DEADSOLID );
		if ( tr.startsolid || tr.fraction >= 1.0f ) {
			continue;
		}
		// keeps the line of fire
		VectorCopy( end, eye );
		eye[2] += DEFAULT_VIEWHEIGHT;
		if ( !AICast_CodLineClear( eye, threatEye ) ) {
			continue;
		}
		trap_EA_Move( cs->entityNum, dir, 400 );
		cs->speedScale = 0.8f;
		return;
	}
	ca->strafeUntil = level.time + 250;     // nowhere to go: stand and shoot, look again shortly
}

// occasional short crouch bursts, weighted by ATTACK_CROUCH, only where the shot still works from down there
static void CodOpenCrouch( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	vec3_t eye, threatEye;

	if ( !AICast_CodCanCrouch( cs ) || cs->attributes[ATTACK_CROUCH] <= 0 ) {
		ca->crouchUntil = 0;
		return;
	}
	if ( ca->crouchUntil > level.time || level.time < ca->nextCrouchRoll ) {
		return;
	}
	ca->nextCrouchRoll = level.time + CodRandMs( 1200, 3000 );
	if ( random() < cs->attributes[ATTACK_CROUCH] * 0.6f ) {
		VectorCopy( cs->bs->origin, eye );
		eye[2] += CROUCH_VIEWHEIGHT;
		VectorCopy( threatPos, threatEye );
		threatEye[2] += DEFAULT_VIEWHEIGHT;
		if ( AICast_CodLineClear( eye, threatEye ) ) {
			ca->crouchUntil = level.time + CodRandMs( 600, 1400 );
		}
	}
}

/*
============
CodSuppressFire

codmod: Phase 2. Fire at the threat's last known position without a clean shot: aim where a trace towards it hits
and only fire when that is within 64 u of the position (the cover edge in front of it), and no teammate stands in
the way. Fires in bursts (about 1 s on, 0.35 s off). Returns qfalse when the soldier is out of ammo for good.
============
*/
static qboolean CodSuppressFire( cast_state_t *cs, codAI_t *ca, vec3_t at ) {
	vec3_t eye, target, aim, dir;
	trace_t tr;

	if ( !CodWeaponTick( cs, qfalse ) ) {
		return qfalse;
	}
	if ( cs->noAttackTime >= level.time || cs->castScriptStatus.scriptNoAttackTime >= level.time
		 || cs->bs->cur_ps.weaponstate == WEAPON_RELOADING || CodNeedsReload( cs ) ) {
		return qtrue;
	}
	if ( !AICast_CanMoveWhileFiringWeapon( cs->weaponNum ) && CodFlatDist( cs, ca->cover.type == COVER_LOW ? ca->cover.pos : ca->cover.peekPos ) > 12.0f ) {
		return qtrue;       // still stepping out
	}
	VectorCopy( cs->bs->origin, eye );
	eye[2] += cs->bs->cur_ps.viewheight;
	VectorCopy( at, target );
	target[2] += 24;
	trap_Trace( &tr, eye, NULL, NULL, target, cs->entityNum, MASK_SHOT );
	if ( tr.fraction < 1.0f ) {
		if ( tr.entityNum < MAX_CLIENTS && tr.entityNum != cs->enemyNum ) {
			return qtrue;       // a teammate in the line of fire
		}
		if ( Distance( tr.endpos, target ) > 64.0f ) {
			return qtrue;       // that would only shoot a wall far from the target
		}
		VectorCopy( tr.endpos, aim );
	} else {
		VectorCopy( target, aim );
	}
	VectorSubtract( aim, eye, dir );
	vectoangles( dir, cs->ideal_viewangles );
	if ( cs->ideal_viewangles[PITCH] > 180 ) {
		cs->ideal_viewangles[PITCH] -= 360;
	}
	if ( fabs( AngleDifference( cs->viewangles[YAW], cs->ideal_viewangles[YAW] ) ) < 15.0f
		 && fabs( AngleDifference( cs->viewangles[PITCH], cs->ideal_viewangles[PITCH] ) ) < 20.0f
		 && ( level.time / 350 + cs->entityNum ) % 4 != 3 ) {
		trap_EA_Attack( cs->bs->client );
		cs->bFlags |= BFL_ATTACKED;
	}
	return qtrue;
}

// leave HIDE for PEEK
static char *CodEnterPeek( cast_state_t *cs, codAI_t *ca, int durationMs, qboolean suppress, const char *why ) {
	ca->peekUntil = level.time + durationMs;
	ca->lastSawEnemy = level.time;
	ca->peekCycles++;
	ca->crouchUntil = 0;
	ca->suppressPeek = suppress;
	ca->tokenWaitStart = 0;
	cs->attackcrouch_time = 0;
	CodSetState( cs, ca, COD_PEEK, why );
	return NULL;
}

//===========================================================================
// Phase 2b: push for an angle
//===========================================================================

/*
============
CodNoteAttempt

codmod: called while a token holder tries to shoot (PEEK, OPEN). Asks the same question the weapon code asks
(AICast_CheckAttack, cached per frame): is there a real shot at the enemy right now? The answer feeds this
soldier's no-shot clock, the squad's, and the arrival check of a push. Time only counts while trying (capped at
250 ms per call), never while hiding.
============
*/
static qboolean CodNoteAttempt( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	qboolean shot = AICast_CheckAttack( cs, cs->enemyNum, qfalse ) ? qtrue : qfalse;
	int dt = level.time - ca->lastTry;

	if ( dt < 0 || dt > 6000 ) {
		ca->noShotMs = 0;       // long silence: a different situation
		dt = 0;
	} else if ( dt > 250 ) {
		dt = 250;
	}
	ca->lastTry = level.time;
	if ( Distance( threatPos, ca->noShotPos ) > 128.0f ) {
		VectorCopy( threatPos, ca->noShotPos );     // the player moved: start counting again
		ca->noShotMs = 0;
	}
	AICast_CodSquadShotReport( cs, shot );
	if ( shot ) {
		ca->noShotMs = 0;
		ca->pushLastShot = level.time;
		if ( ca->pushMode == 2 && !ca->pushShotOk ) {
			ca->pushShotOk = qtrue;
			if ( bg_cod.aiDebug >= 1 ) {
				G_Printf( "[ai] %s sq%d/%s PUSH arrived, shot ok (%d ms)\n", CodName( cs ), AICast_CodSquadId( cs ),
						  AICast_CodSquadRoleName( cs ), level.time - ca->pushArrive );
			}
		}
	} else {
		ca->noShotMs += dt;
		if ( ca->pushMode == 2 && !ca->pushShotOk ) {
			ca->pushTryMs += dt;
		}
	}
	return shot;
}

// the enemy's position is known fresh (seen within 3 s, or within 8 s and it has barely moved since)
static qboolean CodThreatFresh( cast_state_t *cs ) {
	cast_visibility_t *vis = &cs->vislist[cs->enemyNum];

	if ( AICast_CodSquadId( cs ) ) {
		return AICast_CodSquadThreatFresh( cs );
	}
	if ( !vis->real_visible_timestamp ) {
		return qfalse;
	}
	if ( vis->real_visible_timestamp > level.time - 3000 ) {
		return qtrue;
	}
	return ( vis->real_visible_timestamp > level.time - 8000
			 && Distance( g_entities[cs->enemyNum].r.currentOrigin, vis->real_visible_pos ) < 96.0f ) ? qtrue : qfalse;
}

// is this soldier supposed to push now (ordered by the squad, or a solo soldier whose own clock ran out)?
static qboolean CodPushWanted( cast_state_t *cs, codAI_t *ca ) {
	if ( !bg_cod.aiPush || ca->pushMode ) {
		return qfalse;
	}
	if ( AICast_CodSquadId( cs ) ) {
		return AICast_CodSquadPushOrder( cs );
	}
	return ( ca->noShotMs >= (int)bg_cod.aiNoShotTime && level.time >= ca->nextPush && CodThreatFresh( cs ) ) ? qtrue : qfalse;
}

// does the stand eye at the chosen angle spot still have a line to the enemy's body?
static qboolean CodSpotShot( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	vec3_t pts[3], eye;
	int num = AICast_CodBodyPoints( cs->enemyNum, threatPos, pts );

	if ( !num ) {
		return qtrue;
	}
	VectorCopy( ca->cover.type == COVER_LOW ? ca->cover.pos : ca->cover.peekPos, eye );
	eye[2] += ( ca->cover.crouchShot && AICast_CodCanCrouch( cs ) ) ? CROUCH_VIEWHEIGHT : DEFAULT_VIEWHEIGHT;
	return AICast_CodBodyShot( eye, pts, num );
}

static const char *CodPushKind( codCover_t *c ) {
	switch ( c->type ) {
	case COVER_FIRE:
		return c->crouchShot ? "crouch-shot" : "stand-shot";
	case COVER_LOW:
		return "low-cover";
	default:
		return "corner";
	}
}

static void CodPushStartedDebug( cast_state_t *cs, codAI_t *ca, vec3_t threatPos, const char *what ) {
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s sq%d/PUSH no shot for %d ms -> %s d=%d %s travel=%d\n", CodName( cs ), AICast_CodSquadId( cs ), ca->noShotMs,
				  what, (int)Distance( ca->cover.pos, threatPos ), CodPushKind( &ca->cover ), ca->cover.travel );
	}
}

/*
============
CodTryPush

codmod: the push order (or the solo no-shot clock) is due. Takes a mover slot, searches COVER_ANGLE and runs to
the spot. Fallbacks, in this order: a grenade flush when the vanilla conditions allow one, a COVER_ADVANCE hop
(ignoring cod_aiAdvanceMinDist). Returns COD_PUSH_MOVING (state is MOVE), COD_PUSH_GRENADE (the caller starts the
flush) or COD_PUSH_NONE (not now; the order stays, or was dropped on a failed search).
============
*/
static int CodTryPush( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	vec3_t *avoid;
	qboolean squad, visible;

	if ( !CodPushWanted( cs, ca ) || level.time < ca->nextPushTry ) {
		return COD_PUSH_NONE;
	}
	squad = AICast_CodSquadId( cs ) ? qtrue : qfalse;
	if ( !AICast_CodMoverAcquire( cs, 5000, qfalse ) ) {
		ca->nextPushTry = level.time + 500;     // all mover slots busy, the order stays pending
		return COD_PUSH_NONE;
	}
	if ( !AICast_CodCoverSearchAllowed( qfalse ) ) {
		AICast_CodMoverRelease( cs );
		ca->nextPushTry = level.time + 100;
		return COD_PUSH_NONE;
	}
	avoid = ( ca->avoidUntil > level.time ) ? &ca->avoidPos : NULL;

	// 1. a spot with a shot at the body
	if ( AICast_CodFindCover( cs, threatPos, cs->enemyNum, COVER_ANGLE, &ca->cover, avoid ? *avoid : NULL, NULL ) ) {
		CodTakeCover( cs, ca, COVER_ANGLE );
		ca->pushMode = 1;
		ca->relocKind = COD_RELOC_PUSH;
		ca->pushShotOk = qfalse;
		ca->pushTryMs = 0;
		AICast_CodMoverAcquire( cs, ca->moveTimeout - level.time + 500, qtrue );
		AICast_CodSquadPushStarted( cs, ca->cover.travel * 10 );
		CodPushStartedDebug( cs, ca, threatPos, "angle spot" );
		return COD_PUSH_MOVING;
	}

	// 2. flush the player out with a grenade (the vanilla conditions decide whether this soldier can)
	visible = CodEnemyVisible( cs );
	if ( AICast_WantsGrenadeFlush( cs, visible ? qfalse : qtrue ) || AICast_WantsGrenadeFlush( cs, visible ? qtrue : qfalse ) ) {
		AICast_CodMoverRelease( cs );
		if ( squad ) {
			AICast_CodSquadPushDone( cs, qtrue );
		} else {
			ca->nextPush = level.time + (int)bg_cod.aiPushCooldown;
		}
		if ( bg_cod.aiDebug >= 1 ) {
			G_Printf( "[ai] %s sq%d/PUSH no shot for %d ms, no angle, grenade\n", CodName( cs ), AICast_CodSquadId( cs ), ca->noShotMs );
		}
		ca->noShotMs = 0;
		return COD_PUSH_GRENADE;
	}

	// 3. an advance hop, however close the player already is (not into his face: COVER_ADVANCE keeps 192 u)
	if ( AICast_CodCoverSearchAllowed( qfalse ) && AICast_CodFindCover( cs, threatPos, cs->enemyNum, COVER_ADVANCE, &ca->cover, avoid ? *avoid : NULL, NULL ) ) {
		CodTakeCover( cs, ca, COVER_ADVANCE );
		ca->pushMode = 1;
		ca->relocKind = COD_RELOC_PUSH;
		ca->pushShotOk = qfalse;
		ca->pushTryMs = 0;
		AICast_CodMoverAcquire( cs, ca->moveTimeout - level.time + 500, qtrue );
		AICast_CodSquadPushStarted( cs, ca->cover.travel * 10 );
		CodPushStartedDebug( cs, ca, threatPos, "no angle, advance" );
		return COD_PUSH_MOVING;
	}

	// nothing: drop the order for now
	AICast_CodMoverRelease( cs );
	ca->nextPushTry = level.time + 1000;
	if ( squad ) {
		AICast_CodSquadPushResult( cs, qfalse );
	} else {
		ca->nextPush = level.time + 5000;
	}
	if ( bg_cod.aiDebug >= 1 && level.time >= ca->nextPushDbg ) {
		ca->nextPushDbg = level.time + 2000;
		G_Printf( "[ai] %s sq%d/PUSH no shot for %d ms, found no angle, no advance, no grenade\n", CodName( cs ), AICast_CodSquadId( cs ), ca->noShotMs );
	}
	return COD_PUSH_NONE;
}

/*
============
CodPushArrive

codmod: the pusher stands on its spot. An exposed firing spot (FIRE) starts in PEEK, firing at once (crouched when
the crouched eye has the shot); a LOW / corner spot starts in HIDE with a short hide, and its peeks are validated
against the body points.
============
*/
static void CodPushArrive( cast_state_t *cs, codAI_t *ca ) {
	ca->pushMode = 2;
	ca->pushArrive = level.time;
	ca->pushTryMs = 0;
	ca->pushShotOk = qfalse;
	ca->pushLastShot = 0;
	ca->hideStart = level.time;
	ca->hideCheck = level.time + 250;
	ca->settledAt = 0;
	ca->reloadIssued = qfalse;
	if ( ca->cover.type == COVER_FIRE ) {
		ca->peekUntil = level.time + 2500;
		ca->lastSawEnemy = level.time;
		ca->suppressPeek = qfalse;
		ca->peekCycles = 0;
		ca->crouchUntil = 0;
		CodSetState( cs, ca, COD_PEEK, "push arrived" );
	} else {
		ca->hideUntil = level.time + 250;
		CodSetState( cs, ca, COD_HIDE, "push arrived" );
	}
}

/*
============
CodPushWatch

codmod: runs every think while a push stands on its spot. Fails it (spot avoided for 8 s, back to the normal cover
search) when 2.5 s of trying brought no shot, the spot has no line to the body any more, or 9 s passed. Once the
shot is there a LOW / corner spot just becomes ordinary cover and the push is over. Returns qtrue when it ended
the push by dropping the cover (state is OPEN now).
============
*/
static qboolean CodPushWatch( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	int age = level.time - ca->pushArrive;
	const char *why = NULL;

	if ( ca->pushShotOk ) {
		if ( ca->cover.type != COVER_FIRE ) {
			CodPushEnd( cs, ca, qtrue );
		}
		return qfalse;
	}
	if ( ca->pushTryMs >= COD_PUSH_GIVEUP_MS ) {
		why = "push: no shot";
	} else if ( age > COD_PUSH_MAX_MS ) {
		why = "push: timed out";
	} else if ( ca->state == COD_HIDE && ca->cover.angle && age > 800 && !CodSpotShot( cs, ca, threatPos ) ) {
		why = "push: spot has no line";
	}
	if ( !why ) {
		return qfalse;
	}
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s sq%d/PUSH arrived, shot none (%d ms) -> spot avoided\n", CodName( cs ), AICast_CodSquadId( cs ), age );
	}
	CodCoverFailed( cs, ca, qfalse, why );
	ca->avoidUntil = level.time + 8000;
	return qtrue;
}

/*
============
CodTryRelocate

codmod: Phase 2, called from HIDE when the hide time is up. Voluntary relocations of a squad member:
  - the flank order (COVER_FLANK), as soon as a mover slot is free
  - an advance hop (COVER_ADVANCE) towards a far threat, when a teammate stays in cover to watch, or when this
    soldier has been waiting for a token for a while
Only members of a squad do this; each needs a mover slot, so at most 1-2 relocate at a time. Returns qtrue when the
soldier is now on its way (state MOVE).
============
*/
static qboolean CodTryRelocate( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	vec3_t centre;
	qboolean waitedLong;
	float aggr;

	if ( !AICast_CodSquadId( cs ) ) {
		return qfalse;
	}

	// 1. flank order
	if ( AICast_CodSquadFlankOrder( cs ) ) {
		if ( !AICast_CodMoverAcquire( cs, 4000, qfalse ) || !AICast_CodSquadCentre( cs, centre ) ) {
			return qfalse;      // all movers busy, the order stays pending
		}
		if ( !AICast_CodCoverSearchAllowed( qfalse ) ) {
			AICast_CodMoverRelease( cs );
			return qfalse;
		}
		if ( CodSearch( cs, ca, threatPos, COVER_FLANK, NULL, centre ) ) {
			AICast_CodSquadFlankResult( cs, qtrue );
			ca->relocKind = COD_RELOC_FLANK;
			AICast_CodMoverAcquire( cs, ca->moveTimeout - level.time + 500, qtrue );
			AICast_CodSquadMoverStarted( cs, qtrue );
			return qtrue;
		}
		AICast_CodMoverRelease( cs );
		AICast_CodSquadFlankResult( cs, qfalse );
		return qfalse;
	}

	// 2. advance hop
	if ( level.time < ca->nextAdvance ) {
		return qfalse;
	}
	ca->nextAdvance = level.time + 1000;
	waitedLong = ( ca->tokenWaitStart && level.time - ca->tokenWaitStart > 2500 ) ? qtrue : qfalse;
	if ( !waitedLong && ca->peekCycles < 1 ) {
		return qfalse;
	}
	if ( Distance( cs->bs->origin, threatPos ) < bg_cod.aiAdvanceMinDist || !AICast_CodSquadCanAdvance( cs ) ) {
		return qfalse;
	}
	aggr = 0.25f + 0.4f * cs->attributes[AGGRESSION];
	if ( !waitedLong && random() > aggr ) {
		return qfalse;
	}
	if ( !AICast_CodMoverAcquire( cs, 4000, qfalse ) ) {
		return qfalse;
	}
	if ( !AICast_CodCoverSearchAllowed( qfalse ) ) {
		AICast_CodMoverRelease( cs );
		return qfalse;
	}
	if ( CodSearch( cs, ca, threatPos, COVER_ADVANCE, NULL, NULL ) ) {
		ca->relocKind = COD_RELOC_ADVANCE;
		AICast_CodMoverAcquire( cs, ca->moveTimeout - level.time + 500, qtrue );
		AICast_CodSquadMoverStarted( cs, qfalse );
		return qtrue;
	}
	AICast_CodMoverRelease( cs );
	ca->nextAdvance = level.time + 4000;
	return qfalse;
}

static char *CodStateOpen( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	int mode;
	qboolean urgent, hasToken, shoot;

	// Phase 2: without a token this soldier does not shoot, it gets to cover. With no cover to be found it fights anyway.
	hasToken = AICast_CodTokenAcquire( cs, 1200 );
	shoot = ( hasToken || ca->searchFails >= 3 ) ? qtrue : qfalse;

	// cover search (rationed). An empty or nearly empty magazine looks for a hidden spot to reload in.
	if ( level.time >= ca->nextSearchTime ) {
		mode = ( CodNeedsReload( cs ) || ( CodClipBelow( cs, 1, 4 ) && CodNeedsReload( cs ) ) ) ? COVER_HIDE : COVER_ATTACK;
		urgent = ( ca->urgent || mode == COVER_HIDE ) ? qtrue : qfalse;
		if ( AICast_CodCoverSearchAllowed( urgent ) ) {
			ca->urgent = qfalse;
			if ( CodSearch( cs, ca, threatPos, mode, NULL, NULL ) ) {
				return NULL;
			}
			if ( !hasToken && ca->nextSearchTime > level.time + 600 ) {
				ca->nextSearchTime = level.time + 600;      // keep looking
			}
		}
	}

	// Phase 2b: nobody can hit the player: a pusher goes for an angle instead of searching ordinary cover
	if ( hasToken && !CodNeedsReload( cs ) ) {
		CodNoteAttempt( cs, ca, threatPos );
		if ( CodPushWanted( cs, ca ) ) {
			int pr = CodTryPush( cs, ca, threatPos );

			if ( pr == COD_PUSH_MOVING ) {
				return NULL;
			}
			if ( pr == COD_PUSH_GRENADE ) {
				CodExit( cs, ca, 0 );
				return AIFunc_GrenadeFlushStart( cs );
			}
		}
	}

	if ( shoot ) {
		CodOpenCrouch( cs, ca, threatPos );
		if ( ca->crouchUntil > level.time ) {
			cs->attackcrouch_time = ca->crouchUntil;    // AICast_Think only crouches while this runs
		}
	} else {
		ca->crouchUntil = 0;
		cs->attackcrouch_time = 0;
		CodFaceThreat( cs, threatPos );
	}
	CodOpenMove( cs, ca, threatPos, shoot );
	if ( !CodWeaponTick( cs, shoot ) ) {
		CodExit( cs, ca, 2000 );
		return AIFunc_DefaultStart( cs );
	}
	return NULL;
}

static char *CodStateMove( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	float dist;
	bot_moveresult_t *mr;

	dist = CodFlatDist( cs, ca->cover.pos );

	if ( dist <= 12.0f ) {
		CodRelocEnd( cs, ca, qtrue );
		if ( ca->pushMode == 1 ) {
			CodPushArrive( cs, ca );
			return NULL;
		}
		ca->hideStart = level.time;
		ca->hideUntil = level.time + (int)( CodRandMs( bg_cod.aiHideMin, bg_cod.aiHideMax ) * ( 1.15f - 0.5f * cs->attributes[AGGRESSION] ) );
		ca->hideCheck = level.time + 250;
		ca->settledAt = 0;
		ca->reloadIssued = qfalse;
		CodSetState( cs, ca, COD_HIDE, "arrived" );
		return NULL;
	}
	if ( level.time > ca->moveTimeout ) {
		CodCoverFailed( cs, ca, qfalse, "move timeout" );
		return NULL;
	}
	if ( level.time >= ca->nextVisCheck ) {
		ca->nextVisCheck = level.time + 250;
		if ( dist > 150.0f && !ca->pushMode && CodExposed( cs, ca, threatPos ) ) {
			CodCoverFailed( cs, ca, qtrue, "cover exposed" );
			return NULL;
		}
	}

	ca->crouchUntil = 0;
	cs->attackcrouch_time = 0;
	if ( dist > 200.0f ) {
		AICast_CodTrySprint( cs );
	}
	mr = AICast_MoveToPos( cs, ca->cover.pos, -1 );
	if ( mr ) {
		if ( mr->failure ) {
			trap_BotResetAvoidReach( cs->bs->ms );
			CodCoverFailed( cs, ca, qfalse, "no path" );
			return NULL;
		}
		if ( mr->blocked ) {
			CodCoverFailed( cs, ca, qfalse, "blocked" );
			return NULL;
		}
	}
	// run and gun where the weapon allows it (AICast_ProcessAttack aims after the movement was issued); Phase 2:
	// only a token holder shoots on the way
	if ( !CodWeaponTick( cs, AICast_CodHasToken( cs ) ) ) {
		CodExit( cs, ca, 2000 );
		return AIFunc_DefaultStart( cs );
	}
	return NULL;
}

static char *CodStateHide( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	qboolean reloading;
	vec3_t supPos;

	// posture: duck behind low cover
	if ( ca->cover.type == COVER_LOW && AICast_CodCanCrouch( cs ) ) {
		ca->crouchUntil = level.time + 300;
		cs->attackcrouch_time = ca->crouchUntil;
	} else {
		ca->crouchUntil = 0;
		cs->attackcrouch_time = 0;
	}

	// back onto the hidden spot (we may be returning from a peek)
	if ( CodFlatDist( cs, ca->cover.pos ) > 12.0f ) {
		CodMoveDirect( cs, ca->cover.pos, 0.8f );
	}
	if ( !ca->settledAt && ( CodFlatDist( cs, ca->cover.pos ) <= 16.0f || level.time > ca->stateTime + 1500 ) ) {
		ca->settledAt = level.time;
	}
	CodFaceThreat( cs, threatPos );

	// reload here when the magazine is below half, and stay until it is done
	reloading = ( cs->bs->cur_ps.weaponstate == WEAPON_RELOADING ) ? qtrue : qfalse;
	if ( !reloading && !ca->reloadIssued && ( CodNeedsReload( cs ) || ( CodClipBelow( cs, 1, 2 ) && CodNeedsReload( cs ) ) ) ) {
		trap_EA_Reload( cs->entityNum );
		ca->reloadIssued = qtrue;
	}
	if ( reloading && ca->hideUntil < level.time + 100 && level.time - ca->hideStart < 4500 ) {
		ca->hideUntil = level.time + 100;
	}

	// compromised: hit, or the threat can now see us. Only once back on the spot: the sight list lags a few
	// hundred ms, so a sighting must be newer than the moment we settled.
	if ( ca->settledAt && level.time > ca->settledAt + 100 ) {
		qboolean hit = ( cs->lastPain > ca->stateTime ) ? qtrue : qfalse;
		qboolean seen = ( CodEnemyVisible( cs ) && cs->vislist[cs->enemyNum].real_visible_timestamp > ca->settledAt + 50 ) ? qtrue : qfalse;

		if ( hit || seen ) {
			CodCoverFailed( cs, ca, qtrue, hit ? "hit while hiding" : "seen while hiding" );
			return NULL;
		}
		if ( level.time >= ca->hideCheck ) {
			ca->hideCheck = level.time + 150;
			if ( CodExposed( cs, ca, threatPos ) ) {
				CodCoverFailed( cs, ca, qtrue, "compromised" );
				return NULL;
			}
		}
	}

	// Phase 2b: ordered to push for an angle (or a solo soldier with nothing to shoot at for too long)
	if ( !reloading && CodPushWanted( cs, ca ) ) {
		int pr = CodTryPush( cs, ca, threatPos );

		if ( pr == COD_PUSH_MOVING ) {
			return NULL;
		}
		if ( pr == COD_PUSH_GRENADE ) {
			CodExit( cs, ca, 0 );
			return AIFunc_GrenadeFlushStart( cs );
		}
	}

	// Phase 2: a teammate is on the move. A token holder leaves cover early to pin the threat down
	// (suppressive fire) instead of waiting out its hide time.
	if ( !reloading && ca->settledAt && level.time < ca->hideUntil && level.time >= ca->nextSuppress
		 && !CodNeedsReload( cs ) && AICast_CodSquadSuppress( cs, supPos ) && AICast_CodTokenAcquire( cs, 900 ) ) {
		ca->nextSuppress = level.time + 3000;
		return CodEnterPeek( cs, ca, CodRandMs( 1000, 2000 ), qtrue, "suppress" );
	}

	if ( level.time < ca->hideUntil || reloading ) {
		return NULL;
	}

	// Phase 2: flank order or an advance hop (needs a mover slot)
	if ( !ca->pushMode && CodTryRelocate( cs, ca, threatPos ) ) {
		return NULL;
	}

	// time to look again. Many peeks, or a threat that moved a lot: find better cover first.
	if ( ca->peekCycles >= ca->maxPeekCycles || Distance( threatPos, ca->cover.threatPos ) > 256.0f ) {
		if ( level.time >= ca->nextSearchTime ) {
			if ( AICast_CodCoverSearchAllowed( qfalse ) ) {
				vec3_t old;

				VectorCopy( ca->cover.pos, old );
				// a worn-out spot is excluded, a moved threat may well pick the same one again
				if ( CodSearch( cs, ca, threatPos, COVER_ATTACK, ca->peekCycles >= ca->maxPeekCycles ? old : NULL, NULL ) ) {
					return NULL;
				}
				// nothing better: keep this one and count again
				ca->peekCycles = 0;
				VectorCopy( threatPos, ca->cover.threatPos );
			} else if ( level.time < ca->hideUntil + 1500 ) {
				return NULL;        // rationed this frame, wait a moment
			}
		}
	}

	// Phase 2b: at a push spot, only peek while the stand eye still has a line to the body
	if ( ca->pushMode == 2 && ca->cover.angle && !CodSpotShot( cs, ca, threatPos ) ) {
		ca->hideUntil = level.time + 250;
		return NULL;
	}

	// Phase 2: only a token holder peeks and shoots. Without one, stay down a little longer; after a long wait
	// look for a different spot.
	if ( !AICast_CodTokenAcquire( cs, 900 ) ) {
		if ( !ca->tokenWaitStart ) {
			ca->tokenWaitStart = level.time;
		}
		ca->hideUntil = level.time + CodRandMs( 250, 650 );
		if ( level.time - ca->tokenWaitStart > 6000 && level.time >= ca->nextSearchTime && AICast_CodCoverSearchAllowed( qfalse ) ) {
			vec3_t old;

			VectorCopy( ca->cover.pos, old );
			ca->tokenWaitStart = level.time;
			if ( CodSearch( cs, ca, threatPos, COVER_ATTACK, old, NULL ) ) {
				return NULL;
			}
		}
		return NULL;
	}

	return CodEnterPeek( cs, ca, CodRandMs( bg_cod.aiPeekMin, bg_cod.aiPeekMax ) + ( ca->cover.type == COVER_LOW ? 250 : 450 ), qfalse, "peek" );
}

static char *CodStateEndPeek( cast_state_t *cs, codAI_t *ca, qboolean hit, const char *why ) {
	AICast_CodTokenRelease( cs );       // Phase 2: the next waiting squad member may shoot
	ca->suppressPeek = qfalse;
	ca->hideStart = level.time;
	ca->hideUntil = level.time + (int)( CodRandMs( bg_cod.aiHideMin, bg_cod.aiHideMax ) * ( 1.15f - 0.5f * cs->attributes[AGGRESSION] ) * ( ca->pushMode ? 0.5f : 1.0f ) );
	if ( hit ) {
		ca->hideUntil += 400;       // keep the head down a little longer after being hit
	}
	ca->hideCheck = level.time + 250;
	ca->settledAt = 0;
	ca->reloadIssued = qfalse;
	CodSetState( cs, ca, COD_HIDE, why );
	return NULL;
}

static char *CodStatePeek( cast_state_t *cs, codAI_t *ca, qboolean visible ) {
	float dist;
	vec3_t *spot;
	vec3_t supPos, threatNow;
	qboolean tokenOk, pushFire;

	pushFire = ( ca->pushMode == 2 && ca->cover.type == COVER_FIRE ) ? qtrue : qfalse;

	// LOW: stand up on the spot. CORNER / FULL: step out to peekPos.
	spot = ( ca->cover.type == COVER_LOW ) ? &ca->cover.pos : &ca->cover.peekPos;
	dist = CodFlatDist( cs, *spot );
	if ( dist > 12.0f && ca->cover.type != COVER_LOW ) {
		CodMoveDirect( cs, *spot, 0.6f );
	}
	if ( pushFire && ca->cover.crouchShot && AICast_CodCanCrouch( cs ) ) {
		ca->crouchUntil = level.time + 300;     // the crouched eye has the shot: fire from down there
		cs->attackcrouch_time = ca->crouchUntil;
	} else {
		ca->crouchUntil = 0;
		cs->attackcrouch_time = 0;
	}

	if ( visible ) {
		ca->lastSawEnemy = level.time;
	}

	if ( cs->lastPain > ca->stateTime ) {
		return CodStateEndPeek( cs, ca, qtrue, "hit while peeking" );
	}
	// Phase 2: keep the token lease; a long hold gives way to a waiting squad member
	tokenOk = AICast_CodTokenAcquire( cs, 700 );
	if ( !tokenOk && !pushFire ) {
		return CodStateEndPeek( cs, ca, qfalse, "token rotated" );
	}
	CodThreatPos( cs, threatNow );
	if ( !ca->pushMode && CodPushWanted( cs, ca ) ) {
		return CodStateEndPeek( cs, ca, qfalse, "push ordered" );       // go for the angle from cover
	}
	if ( tokenOk && !ca->suppressPeek && !CodNeedsReload( cs ) && cs->bs->cur_ps.weaponstate != WEAPON_RELOADING ) {
		CodNoteAttempt( cs, ca, threatNow );
	}
	if ( !tokenOk ) {
		CodFaceThreat( cs, threatNow );     // pusher waiting for its turn on an exposed spot
	} else if ( !visible && AICast_CodSquadSuppress( cs, supPos ) ) {
		// suppressive fire on the last known position while a teammate moves
		if ( !CodSuppressFire( cs, ca, supPos ) ) {
			CodExit( cs, ca, 2000 );
			return AIFunc_DefaultStart( cs );
		}
	} else if ( !CodWeaponTick( cs, qtrue ) ) {
		CodExit( cs, ca, 2000 );
		return AIFunc_DefaultStart( cs );
	}
	if ( CodNeedsReload( cs ) || cs->bs->cur_ps.weaponstate == WEAPON_RELOADING ) {
		if ( pushFire ) {
			CodPushEnd( cs, ca, ca->pushShotOk );
			CodCoverFailed( cs, ca, qfalse, "push: reload" );       // an exposed spot is no place to reload
			return NULL;
		}
		return CodStateEndPeek( cs, ca, qfalse, "empty magazine" );
	}
	if ( level.time - ca->lastSawEnemy > COD_PEEK_LOST && !ca->suppressPeek && !pushFire ) {
		// the enemy is not where we looked: let the vanilla chase find them (Phase 4 replaces this)
		CodExit( cs, ca, 3000 );
		return AIFunc_BattleChaseStart( cs );
	}
	if ( level.time >= ca->peekUntil ) {
		if ( pushFire ) {
			// an exposed push spot: keep firing for as long as the shot lasts, leave when it is gone
			if ( !ca->pushShotOk ) {
				ca->peekUntil = level.time + 500;           // CodPushWatch gives up after COD_PUSH_GIVEUP_MS
			} else if ( ca->pushLastShot > level.time - 1200 ) {
				ca->peekUntil = level.time + 1000;
			} else {
				CodPushEnd( cs, ca, qtrue );
				CodCoverFailed( cs, ca, qfalse, "push: angle lost" );
			}
			return NULL;
		}
		return CodStateEndPeek( cs, ca, qfalse, ca->suppressPeek ? "suppression done" : "burst done" );
	}
	return NULL;
}

//===========================================================================
// AIFunc_CodCombat
//===========================================================================

/*
============
AIFunc_CodCombat

codmod: the tactical combat function. See the file header and docs/ai-overhaul-plan.md Phase 1b.
============
*/
char *AIFunc_CodCombat( cast_state_t *cs ) {
	codAI_t *ca;
	vec3_t threatPos;
	int elig;
	qboolean visible;
	char *rval;

	ca = CodGet( cs );
	ca->lastThink = level.time;
	AICast_CodSquadThink();     // codmod: Phase 2, regroups squads about once a second

	// ---- the checks of the vanilla functions we replace -----------------------------------------------
	if ( cs->aiState < AISTATE_COMBAT ) {
		AICast_StateChange( cs, AISTATE_COMBAT );
	}
	// do we need to avoid a danger?
	if ( cs->dangerEntityValidTime >= level.time ) {
		if ( !AICast_GetTakeCoverPos( cs, cs->dangerEntity, cs->dangerEntityPos, cs->takeCoverPos ) ) {
			// shit??
		}
		cs->takeCoverTime = cs->dangerEntityValidTime + 1000;
		CodExit( cs, ca, 0 );
		return AIFunc_AvoidDangerStart( cs );
	}
	// are we waiting for a door?
	if ( cs->doorMarkerTime > level.time - 100 ) {
		CodExit( cs, ca, 0 );
		return AIFunc_DoorMarkerStart( cs, cs->doorMarkerDoor, cs->doorMarkerNum );
	}
	if ( cs->enemyNum < 0 ) {
		CodExit( cs, ca, 0 );
		return AIFunc_DefaultStart( cs );
	}
	if ( g_entities[cs->enemyNum].health <= 0 ) {
		CodExit( cs, ca, 0 );
		if ( g_entities[cs->entityNum].aiTeam == AITEAM_NAZI ) {
			return AIFunc_InspectBodyStart( cs );
		}
		return AIFunc_DefaultStart( cs );
	}
	elig = CodEligibility( cs );
	// hiding / peeking does not see the enemy on purpose; PEEK has its own lost-contact timer (COD_PEEK_LOST)
	if ( elig == COD_ELIG_LOST && ( ca->state == COD_HIDE || ca->state == COD_PEEK ) ) {
		elig = COD_ELIG_OK;
	}
	if ( elig == COD_ELIG_LOST ) {
		// lost contact: the vanilla chase takes over
		CodExit( cs, ca, 3000 );
		if ( bg_cod.aiDebug >= 1 ) {
			G_Printf( "[ai] %s tactics->vanilla (enemy not seen for %d ms)\n", CodName( cs ), (int)bg_cod.aiMemoryTime );
		}
		return AIFunc_BattleChaseStart( cs );
	}
	if ( elig != COD_ELIG_OK ) {
		CodExit( cs, ca, 1000 );
		if ( bg_cod.aiDebug >= 1 ) {
			G_Printf( "[ai] %s tactics->vanilla (not eligible)\n", CodName( cs ) );
		}
		return AIFunc_DefaultStart( cs );
	}
	// not in a good attacking position, keep chasing
	if ( !AICast_StopAndAttack( cs ) ) {
		CodExit( cs, ca, 1500 );
		return AIFunc_BattleChaseStart( cs );
	}

	visible = CodEnemyVisible( cs );
	if ( visible ) {
		AICast_CodSquadReportThreat( cs, g_entities[cs->enemyNum].r.currentOrigin );
	}
	AICast_CodSquadPublish( cs, ( ca->state == COD_HIDE || ca->state == COD_PEEK ) ? qtrue : qfalse );
	CodThreatPos( cs, threatPos );

	// the vanilla grenade flush (shared helper); not while running to cover
	if ( ca->state != COD_MOVE && AICast_WantsGrenadeFlush( cs, visible ? qfalse : qtrue ) ) {
		CodExit( cs, ca, 0 );
		return AIFunc_GrenadeFlushStart( cs );
	}

	// being hit asks for a quick re-think of the cover
	if ( cs->lastPain > ca->lastPainSeen ) {
		ca->lastPainSeen = cs->lastPain;
		if ( level.time - cs->lastPain < 500 ) {
			ca->urgent = qtrue;
		}
	}

	if ( ca->pushMode == 2 && CodPushWatch( cs, ca, threatPos ) ) {
		CodDebugDraw( cs, ca );
		return NULL;
	}

	switch ( ca->state ) {
	case COD_MOVE:
		rval = CodStateMove( cs, ca, threatPos );
		break;
	case COD_HIDE:
		rval = CodStateHide( cs, ca, threatPos );
		break;
	case COD_PEEK:
		rval = CodStatePeek( cs, ca, visible );
		break;
	default:
		rval = CodStateOpen( cs, ca, threatPos );
		break;
	}

	CodDebugDraw( cs, ca );
	return rval;
}

/*
============
AIFunc_CodCombatStart
============
*/
char *AIFunc_CodCombatStart( cast_state_t *cs ) {
	codAI_t *ca;

	ca = CodGet( cs );
	// fresh start; keep nothing from a previous run except a still-valid back-off (which is zero here)
	AICast_CodReleaseCover( cs->entityNum );
	ca->hasCover = qfalse;
	ca->state = COD_OPEN;
	ca->stateTime = level.time;
	ca->nextSearchTime = 0;
	ca->searchFails = 0;
	ca->urgent = qfalse;
	ca->lastPainSeen = cs->lastPain;
	ca->strafeDir = 0;
	ca->crouchUntil = 0;
	ca->relocKind = COD_RELOC_NONE;
	ca->tokenWaitStart = 0;
	ca->suppressPeek = qfalse;
	ca->nextAdvance = level.time + CodRandMs( 2000, 4000 );
	ca->nextSuppress = 0;
	ca->nextCrouchRoll = level.time + CodRandMs( 500, 1500 );
	ca->pushMode = 0;
	ca->noShotMs = 0;
	ca->lastTry = 0;
	ca->nextPushTry = 0;
	ca->lastThink = level.time;

	// what AIFunc_BattleStart sets up that still applies; we do our own crouching
	cs->attackcrouch_time = 0;
	cs->aiFlags &= ~AIFL_ATTACK_CROUCH;
	cs->lastEnemy = cs->enemyNum;
	cs->crouchHideFlag = qfalse;
	cs->aiFlags &= ~AIFL_TALKING;
	AICast_UpdateBattleInventory( cs, cs->enemyNum );

	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s vanilla->tactics\n", CodName( cs ) );
	}
	cs->aifunc = AIFunc_CodCombat;
	return "AIFunc_CodCombat";
}
