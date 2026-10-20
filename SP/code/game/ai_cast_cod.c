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
// Phase 3 (threat reactions): offensive grenades (a player who holds one cover for too long, or sits behind a known
// corner), grenade dodging (and the vanilla catch / kick), suppression from near misses (a decaying value per
// soldier: stay down longer, peek shorter, aim worse, an OPEN soldier runs for cover), pain (urgent cover in the
// open, a farther retreat at low health), squad morale (a squad that lost half falls back and is cautious for a
// while) and opportunism (the player reloads, switches weapons, slides or goes prone: peek / push now).
//
// Phase 5 (ai_cast_cod_polish.c): movement and aim polish -- backpedal under a charge, crouch-walking between low covers,
// jinks under fire, a kick in melee range, the warm-up accuracy model and CAMPER soldiers that crouch-peek at their
// post. Each has its own switch (cod_aiPolish and cod_aiBackpedal / CrouchWalk / Jink / Kick / Accuracy / Camper).
//
// All state lives in the static codAI[] array (never in cast_state_t, which is saved as a raw struct). A savegame
// load just leaves it stale; CodGet() detects that (level.startTime / lastThink) and starts from scratch.
//===========================================================================

#include "g_local.h"
#include "../qcommon/q_shared.h"
#include "../botlib/botlib.h"      //bot lib interface
#include "../botlib/be_aas.h"
#include "../botlib/aasfile.h"      // codmod: Phase 5, TRAVEL_WALK
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
static const char *codModeNames[] = { "attack", "hide", "advance", "flank", "angle", "retreat", "dodge" };

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

	// Phase 3
	int nextGrenadeCheck;           // offensive grenade: next evaluation
	int nextGrenade;                // this soldier's own cooldown (a solo soldier's only one)
	vec3_t holdPos;                 // where the threat has been sitting
	int holdSince;                  // since when (0 = threat not fresh)
	qboolean dodging;               // the current MOVE is a grenade dodge
	qboolean dodgeFailed;           // no dodge cover for the grenade we reacted to: the vanilla avoid runs
	qboolean dodged;                // dived already
	int dodgeEnt;                   // the grenade we reacted to ...
	int dodgeThink;                 // ... (its explode time identifies it)
	int dodgeEnd;                   // explosion time
	float dodgeRadius;
	int retreatKind;                // 0 none, 1 low health, 2 broken morale (the current MOVE is a retreat)
	int nextRetreat;
	int oppUntil;                   // the player is weak until then
	int nextOppBark;
	qboolean oppWasProne;
	int nextDbg;                    // rate limit of the Phase 3 debug lines
	int nextSuppDbg;

	// Phase 5
	qboolean crouchWalk;            // the current MOVE is crouch-walked (low cover to low cover)
	qboolean camper;                // CAMPER attribute and cod_aiCamper: holds its post
	qboolean camperReloc;           // a camper relocation (compromised post) is under way: the arrival is the new post
	int camperPainSeen;             // camper: last pain already counted
	int camperHits;                 // camper: hits taken while hiding
	int camperHitTime;
	qboolean prevLow;               // the cover search started from a LOW cover spot we were standing at ...
	vec3_t prevPos;                 // ... this one

	int nextDraw;
} codAI_t;

#define COD_RETREAT_HEALTH  1
#define COD_RETREAT_MORALE  2

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
	AICast_CodHuntReset( entNum );      // codmod: Phase 4
	AICast_CodPolishReset( entNum );    // codmod: Phase 5
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
	if ( AICast_CodHuntCrouch( cs ) ) {
		return qtrue;       // codmod: Phase 4, crouch-walking up to a corner while hunting
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

//===========================================================================
// Phase 6: difficulty scaling (g_gameskill) and the per-frame budget counters
//===========================================================================

/*
Difficulty table (rows: codSkill_t; factor on a duration, > 1 = longer). cod_aiSkillScale 0 = all 1.0.
Attack tokens (cod_aiAttackTokens 0 = 2/3/4) and the Phase 5 accuracy warm-up come from the same table / skill value.

                         easy   medium   hard
  reaction time           1.35    1.0     0.75
  peek burst              0.80    1.0     1.25
  hide between peeks      1.30    1.0     0.80
  flank hold              1.60    1.0     0.70
  push no-shot time       1.50    1.0     0.75
  grenade hold            1.50    1.0     0.75
  grenade cooldown        1.50    1.0     0.75
  accuracy warm-up        1.40    1.0     0.70
  hunt / sweep / alert    0.70    1.0     1.30
*/
static const float codSkillTable[COD_SK_COUNT][3] = {
	{ 1.35f, 1.0f, 0.75f },     // COD_SK_REACT
	{ 0.80f, 1.0f, 1.25f },     // COD_SK_PEEK
	{ 1.30f, 1.0f, 0.80f },     // COD_SK_HIDE
	{ 1.60f, 1.0f, 0.70f },     // COD_SK_FLANK
	{ 1.50f, 1.0f, 0.75f },     // COD_SK_PUSH
	{ 1.50f, 1.0f, 0.75f },     // COD_SK_GRENADE_HOLD
	{ 1.50f, 1.0f, 0.75f },     // COD_SK_GRENADE_CD
	{ 1.40f, 1.0f, 0.70f },     // COD_SK_WARMUP
	{ 0.70f, 1.0f, 1.30f }      // COD_SK_HUNT
};

float AICast_CodSkill( int what ) {
	int sk;

	if ( !bg_cod.enable || !bg_cod.aiTactics || !bg_cod.aiSkillScale || what < 0 || what >= COD_SK_COUNT ) {
		return 1.0f;
	}
	sk = g_gameskill.integer;
	if ( sk < 0 ) {
		sk = 0;
	} else if ( sk > 2 ) {
		sk = 2;
	}
	return codSkillTable[what][sk];
}

static qboolean CodIsTacticalType( int aiCharacter );

// reaction time: only for the soldier types the tactical AI drives; monsters, bosses and civilians stay vanilla
float AICast_CodSkillReact( cast_state_t *cs ) {
	if ( !bg_cod.enable || !bg_cod.aiTactics || !bg_cod.aiSkillScale || !CodIsTacticalType( cs->aiCharacter ) || cs->aasWorldIndex != 0 ) {
		return 1.0f;
	}
	return AICast_CodSkill( COD_SK_REACT );
}

int codPerfTraces, codPerfAas;
static int codPerfSearches;

typedef struct {
	int frameTime;                  // frame being counted
	int soldiers;                   // tactical thinks this frame
	int nextPrint;
	int frames, sumTraces, maxTraces, sumAas, maxAas, searches, maxSoldiers, sumSoldiers;
} codPerf_t;

static codPerf_t codPerf;

void AICast_CodPerfCoverSearch( void ) {
	codPerfSearches++;
}

/*
============
CodPerfFrame

codmod: called once per AIFunc_CodCombat think. Rolls the counters over when the server frame changes (the counters
are bumped by the trap_ macros in ai_cast_cod.h and by the cover search ration) and, with cod_aiDebug 2, prints one
line per second: avg/max traces and AAS route queries per frame that had tactical soldiers, cover searches, and the
soldier count. Traces made by vanilla AI code or by AICast_PredictMovement are not included.
============
*/
static void CodPerfFrame( void ) {
	codPerf_t *pf = &codPerf;

	if ( pf->frameTime != level.time ) {
		if ( pf->soldiers ) {
			pf->frames++;
			pf->sumTraces += codPerfTraces;
			if ( codPerfTraces > pf->maxTraces ) {
				pf->maxTraces = codPerfTraces;
			}
			pf->sumAas += codPerfAas;
			if ( codPerfAas > pf->maxAas ) {
				pf->maxAas = codPerfAas;
			}
			pf->searches += codPerfSearches;
			pf->sumSoldiers += pf->soldiers;
			if ( pf->soldiers > pf->maxSoldiers ) {
				pf->maxSoldiers = pf->soldiers;
			}
		}
		if ( bg_cod.aiDebug >= 2 && level.time >= pf->nextPrint ) {
			if ( pf->frames ) {
				G_Printf( "[ai] perf: %d tactical frames/s, traces avg %.1f max %d, aas avg %.1f max %d, cover searches %d, soldiers avg %.1f max %d\n",
						  pf->frames, (float)pf->sumTraces / pf->frames, pf->maxTraces, (float)pf->sumAas / pf->frames, pf->maxAas,
						  pf->searches, (float)pf->sumSoldiers / pf->frames, pf->maxSoldiers );
			}
			pf->nextPrint = level.time + 1000;
			pf->frames = pf->sumTraces = pf->maxTraces = pf->sumAas = pf->maxAas = pf->searches = pf->maxSoldiers = pf->sumSoldiers = 0;
		}
		pf->frameTime = level.time;
		pf->soldiers = 0;
		codPerfTraces = codPerfAas = codPerfSearches = 0;
	}
	pf->soldiers++;
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
	// campers keep their post (Phase 5: with cod_aiCamper they fight from it as crouch-peekers instead)
	if ( cs->attributes[CAMPER] >= 0.5f && !AICast_CodPolishIsCamper( cs ) ) {
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

//===========================================================================
// Phase 3: suppression, morale
//===========================================================================

// suppression per soldier: a value 0..1 that near misses add to and that decays by cod_aiSuppressDecay per second
static struct {
	int levelStart;
	int time;
	float val;
} codSupp[MAX_CLIENTS];

static float CodSuppression( int entNum ) {
	float v;

	if ( entNum < 0 || entNum >= MAX_CLIENTS || codSupp[entNum].levelStart != level.startTime || codSupp[entNum].time > level.time ) {
		return 0;
	}
	v = codSupp[entNum].val - bg_cod.aiSuppressDecay * ( level.time - codSupp[entNum].time ) / 1000.0f;
	return v > 0 ? v : 0;
}

static qboolean CodSuppressed( int entNum ) {
	return ( CodSuppression( entNum ) >= bg_cod.aiSuppressThreshold && bg_cod.aiSuppressThreshold > 0 ) ? qtrue : qfalse;
}

/*
============
AICast_CodNoteBullet

codmod: Phase 3, called from AICast_ProcessBullet for every AI that is already in combat (the vanilla function
ignores them). A bullet of an enemy that passes within 160 u of a soldier running the tactical combat adds to its
suppression value.
============
*/
void AICast_CodNoteBullet( cast_state_t *cs, gentity_t *attacker, vec3_t start, vec3_t end ) {
	codAI_t *ca;
	vec3_t body, proj, a, dir;
	float d, v, before;
	int n = cs->entityNum;

	if ( !bg_cod.enable || !bg_cod.aiTactics || bg_cod.aiSuppressDecay <= 0 || n < 0 || n >= MAX_CLIENTS ) {
		return;
	}
	if ( !attacker || attacker->s.number >= MAX_CLIENTS || attacker->s.number == n || AICast_SameTeam( cs, attacker->s.number ) ) {
		return;
	}
	ca = &codAI[n];
	if ( ca->levelStart != level.startTime || ca->lastThink < level.time - 1500 || cs->aifunc != AIFunc_CodCombat ) {
		return;
	}
	VectorCopy( g_entities[n].r.currentOrigin, body );
	body[2] += 20;
	VectorSubtract( end, start, dir );
	ProjectPointOntoVector( body, start, end, proj );
	VectorSubtract( proj, start, a );
	if ( DotProduct( a, dir ) < 0 ) {
		return;     // behind the muzzle
	}
	VectorSubtract( proj, end, a );
	d = ( DotProduct( a, dir ) > 0 ) ? Distance( body, end ) : Distance( body, proj );
	if ( d > 160.0f ) {
		return;
	}
	before = CodSuppression( n );
	v = before + 0.12f + 0.2f * ( 1.0f - d / 160.0f );
	if ( v > 1.0f ) {
		v = 1.0f;
	}
	codSupp[n].levelStart = level.startTime;
	codSupp[n].time = level.time;
	codSupp[n].val = v;
	if ( bg_cod.aiDebug >= 1 && before < bg_cod.aiSuppressThreshold && v >= bg_cod.aiSuppressThreshold && level.time >= ca->nextSuppDbg ) {
		ca->nextSuppDbg = level.time + 2000;
		G_Printf( "[ai] %s SUPPRESSED v=%.2f (near miss d=%d)\n", CodName( cs ), v, (int)d );
	}
}

/*
============
AICast_CodAccuracyScale

codmod: Phase 3, multiplied into AICast_GetAccuracy: 1 for everyone, down to about 0.45 for a suppressed soldier;
Phase 5 adds the polish accuracy model (first-shot warm-up, sprinting / sliding target, moving shooter)
============
*/
float AICast_CodAccuracyScale( int entNum ) {
	codAI_t *ca;
	float v;

	if ( entNum < 0 || entNum >= MAX_CLIENTS ) {
		return 1.0f;
	}
	ca = &codAI[entNum];
	if ( ca->levelStart != level.startTime || ca->lastThink < level.time - 1000 ) {
		return 1.0f;
	}
	if ( !CodSuppressed( entNum ) ) {
		return AICast_CodPolishAccuracy( entNum );      // codmod: Phase 5, warm-up and movement (1.0 when off)
	}
	v = CodSuppression( entNum );
	return ( 1.0f - 0.55f * v ) * AICast_CodPolishAccuracy( entNum );
}

// aggression, lowered while the squad's morale is broken
static float CodAggression( cast_state_t *cs ) {
	float a = cs->attributes[AGGRESSION];

	if ( bg_cod.aiMorale && AICast_CodSquadMoraleActive( cs ) ) {
		a *= 0.35f;
	}
	return a;
}

// codmod: Phase 5, hit within the last 2 s or suppressed: jink instead of running straight
static qboolean CodUnderFire( cast_state_t *cs ) {
	return ( ( cs->lastPain && cs->lastPain > level.time - 2000 ) || CodSuppressed( cs->entityNum ) ) ? qtrue : qfalse;
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
	if ( !ca->camper ) {
		AICast_CodSquadPublish( cs, qfalse );
	}
	AICast_CodReleaseCover( cs->entityNum );
	ca->hasCover = qfalse;
	ca->dodging = qfalse;
	ca->retreatKind = 0;
	ca->crouchWalk = qfalse;
	ca->crouchUntil = 0;
	cs->attackcrouch_time = 0;
	ca->state = COD_OPEN;
	if ( backoffMs > 0 ) {
		ca->noTacticsUntil = level.time + backoffMs;
	}
}

// the cover we were using is no good any more: back to OPEN and search again (excluding that spot for a while)
static void CodCoverFailed( cast_state_t *cs, codAI_t *ca, qboolean urgent, const char *why ) {
	// codmod: Phase 5, a camper keeps its post: only the third compromise within 8 s drives it out (CodCamperPost)
	if ( ca->camper && urgent && !ca->camperReloc ) {
		if ( level.time - ca->camperHitTime > 8000 ) {
			ca->camperHits = 0;
		}
		ca->camperHitTime = level.time;
		if ( ++ca->camperHits >= 3 ) {
			ca->camperReloc = qtrue;
			if ( bg_cod.aiDebug >= 1 ) {
				G_Printf( "[ai] %s CAMPER post compromised (%s): relocating\n", CodName( cs ), why );
			}
		}
	}
	CodRelocEnd( cs, ca, qfalse );
	CodPushEnd( cs, ca, qfalse );
	AICast_CodTokenRelease( cs );
	if ( ca->hasCover ) {
		VectorCopy( ca->cover.pos, ca->avoidPos );
		ca->avoidUntil = level.time + 3000;
	}
	AICast_CodReleaseCover( cs->entityNum );
	ca->hasCover = qfalse;
	ca->dodging = qfalse;
	ca->retreatKind = 0;
	ca->crouchWalk = qfalse;
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
	// codmod: Phase 5, from one low cover to another over a hidden route: crouch-walk (slower, head stays down). Not for
	// a dodge, a retreat, a push or a camper's way back to its post.
	ca->crouchWalk = qfalse;
	if ( ca->prevLow && ca->cover.type == COVER_LOW && ( mode == COVER_ATTACK || mode == COVER_ADVANCE || mode == COVER_FLANK )
		 && !ca->camper ) {
		vec3_t tp;

		CodThreatPos( cs, tp );
		if ( AICast_CodPolishCrouchWalk( cs, ca->prevPos, ca->cover.pos, tp ) ) {
			ca->crouchWalk = qtrue;
			ca->moveTimeout = level.time + ca->cover.travel * 10 * 3 + 1500;      // slower
		}
	}
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
	// codmod: Phase 5, are we standing at a low cover right now? (CodTakeCover decides about crouch-walking)
	ca->prevLow = ( ca->hasCover && ca->cover.type == COVER_LOW && ( ca->state == COD_HIDE || ca->state == COD_PEEK )
					&& CodFlatDist( cs, ca->cover.pos ) < 32.0f ) ? qtrue : qfalse;
	if ( ca->prevLow ) {
		VectorCopy( ca->cover.pos, ca->prevPos );
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
	int tries, d, jl;

	if ( ( shooting && !AICast_CanMoveWhileFiringWeapon( cs->weaponNum ) ) || ca->crouchUntil > level.time ) {
		return;
	}
	// codmod: Phase 5, under fire the legs are short (0.3-0.6 s): a jink instead of a steady strafe
	jl = AICast_CodPolishJinkLegMs( cs, CodUnderFire( cs ) );
	if ( jl && ca->strafeDir && ca->strafeUntil > level.time + 600 ) {
		ca->strafeUntil = level.time + jl;
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
				ca->strafeUntil = level.time + ( jl ? jl : CodRandMs( bg_cod.aiStrafeMin, bg_cod.aiStrafeMax ) );
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
	// codmod: Phase 3, shorter peeks while suppressed, a little longer while the player is weak
	if ( CodSuppressed( cs->entityNum ) ) {
		durationMs = (int)( durationMs * ( 1.0f - 0.45f * CodSuppression( cs->entityNum ) ) );
	} else if ( ca->oppUntil > level.time ) {
		durationMs += 400;
	}
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
	if ( !bg_cod.aiPush || ca->pushMode || ca->camper ) {      // codmod: Phase 5, campers do not push
		return qfalse;
	}
	if ( bg_cod.aiMorale && AICast_CodSquadMoraleActive( cs ) ) {
		return qfalse;      // codmod: Phase 3, a broken squad does not push
	}
	if ( AICast_CodSquadId( cs ) ) {
		return AICast_CodSquadPushOrder( cs );
	}
	return ( ca->noShotMs >= (int)( bg_cod.aiNoShotTime * AICast_CodSkill( COD_SK_PUSH ) ) && level.time >= ca->nextPush && CodThreatFresh( cs ) ) ? qtrue : qfalse;
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

//===========================================================================
// Phase 3: offensive grenades
//===========================================================================

// the vanilla "can throw one" test (carried, ammo, own recharge, the script lets it switch)
static qboolean CodHasGrenade( cast_state_t *cs ) {
	if ( cs->weaponNum && ( cs->castScriptStatus.scriptFlags & SFL_NOCHANGEWEAPON ) ) {
		return qfalse;
	}
	if ( COM_BitCheck( cs->bs->cur_ps.weapons, WP_GRENADE_LAUNCHER ) && AICast_GotEnoughAmmoForWeapon( cs, WP_GRENADE_LAUNCHER )
		 && cs->weaponFireTimes[WP_GRENADE_LAUNCHER] < level.time - (int)( aicast_skillscale * 3000 ) ) {
		return qtrue;
	}
	if ( COM_BitCheck( cs->bs->cur_ps.weapons, WP_GRENADE_PINEAPPLE ) && AICast_GotEnoughAmmoForWeapon( cs, WP_GRENADE_PINEAPPLE )
		 && cs->weaponFireTimes[WP_GRENADE_PINEAPPLE] < level.time - (int)( aicast_skillscale * 3000 ) ) {
		return qtrue;
	}
	return qfalse;
}

// a lob from the eye over the middle to the target is not blocked (a cheap stand-in for the vanilla arc test, which
// AIFunc_GrenadeFlush repeats with the real trajectory before it lets go)
static qboolean CodGrenadeArc( cast_state_t *cs, vec3_t threatPos ) {
	vec3_t eye, apex, tgt;
	float d;

	VectorCopy( cs->bs->origin, eye );
	eye[2] += DEFAULT_VIEWHEIGHT;
	VectorCopy( threatPos, tgt );
	tgt[2] += 24;
	d = Distance( eye, tgt );
	VectorAdd( eye, tgt, apex );
	VectorScale( apex, 0.5f, apex );
	apex[2] = ( eye[2] > tgt[2] ? eye[2] : tgt[2] ) + 40.0f + d * 0.18f;
	return ( AICast_CodLineClear( eye, apex ) && AICast_CodLineClear( apex, tgt ) ) ? qtrue : qfalse;
}

/*
============
AICast_CodGrenadeScore

codmod: Phase 3, how good a thrower this soldier is for a target at threatPos right now (0 = cannot). The squad picks the
best one; nearer is better, and the soldier needs a lob that is not blocked.
============
*/
int AICast_CodGrenadeScore( cast_state_t *cs, vec3_t threatPos ) {
	gentity_t *ent;
	float d;

	if ( !cs || !cs->bs || cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS ) {
		return 0;
	}
	ent = &g_entities[cs->entityNum];
	if ( !ent->inuse || ent->health <= 0 || cs->aiState < AISTATE_COMBAT || cs->castScriptStatus.castScriptEventIndex >= 0
		 || cs->castScriptStatus.scriptNoAttackTime >= level.time || cs->aifunc != AIFunc_CodCombat ) {
		return 0;
	}
	if ( codAI[cs->entityNum].state == COD_MOVE ) {
		return 0;
	}
	d = Distance( cs->bs->origin, threatPos );
	if ( d < 200.0f || d > 1100.0f || !CodHasGrenade( cs ) || !CodGrenadeArc( cs, threatPos ) ) {
		return 0;
	}
	return 1 + (int)( 1100.0f - d );
}

// the squad cooldown / thrower choice, the arc and the vanilla availability (ammo, spacing) all say yes
static qboolean CodGrenadeAvailable( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	if ( level.time < ca->nextGrenade || !AICast_CodSquadGrenadeThrower( cs, threatPos ) ) {
		return qfalse;
	}
	if ( AICast_CodGrenadeScore( cs, threatPos ) <= 0 ) {
		return qfalse;
	}
	return AICast_WantsGrenadeFlush( cs, qfalse );
}

// a throw is decided: cooldowns and the callout
static void CodGrenadeCommit( cast_state_t *cs, codAI_t *ca ) {
	AICast_CodSquadGrenadeThrown( cs );
	ca->nextGrenade = level.time + (int)( bg_cod.aiGrenadeCooldown * AICast_CodSkill( COD_SK_GRENADE_CD ) * ( 0.85f + 0.3f * random() ) );
	ca->holdSince = 0;
}

/*
============
CodGrenadeWanted

codmod: Phase 3. A grenade is thrown when the threat has sat at the same spot (within 96 u) for cod_aiGrenadeHold ms and
nobody had a real shot in the last 1.5 s, or when it is out of sight behind a known corner (seen at that spot for
1.5 s, no line from here). One thrower per squad (the best, squad cooldown cod_aiGrenadeCooldown), nobody while a
teammate is pushing for an angle (the push has its own grenade fallback).
============
*/
static qboolean CodGrenadeWanted( cast_state_t *cs, codAI_t *ca, vec3_t threatPos, qboolean visible ) {
	int hold, lastShot;
	qboolean held, corner;

	if ( !CodThreatFresh( cs ) ) {
		ca->holdSince = 0;
		return qfalse;
	}
	if ( !ca->holdSince || Distance( threatPos, ca->holdPos ) > 96.0f ) {
		VectorCopy( threatPos, ca->holdPos );
		ca->holdSince = level.time;
	}
	if ( bg_cod.aiGrenadeHold <= 0 || level.time < ca->nextGrenadeCheck || ca->state == COD_MOVE ) {
		return qfalse;
	}
	ca->nextGrenadeCheck = level.time + 300 + rand() % 200;

	hold = level.time - ca->holdSince;
	lastShot = AICast_CodSquadLastShot( cs );
	if ( ca->pushLastShot > lastShot ) {
		lastShot = ca->pushLastShot;
	}
	if ( level.time - lastShot < 1500 ) {
		return qfalse;
	}
	held = ( hold >= (int)( bg_cod.aiGrenadeHold * AICast_CodSkill( COD_SK_GRENADE_HOLD ) ) ) ? qtrue : qfalse;
	corner = qfalse;
	if ( !held && !visible && hold >= 1500 ) {
		vec3_t eye, threatEye;

		VectorCopy( cs->bs->origin, eye );
		eye[2] += DEFAULT_VIEWHEIGHT;
		VectorCopy( threatPos, threatEye );
		threatEye[2] += DEFAULT_VIEWHEIGHT;
		corner = AICast_CodLineClear( eye, threatEye ) ? qfalse : qtrue;
	}
	if ( !held && !corner ) {
		return qfalse;
	}
	if ( AICast_CodSquadPushActive( cs ) || !CodGrenadeAvailable( cs, ca, threatPos ) ) {
		return qfalse;
	}
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s sq%d GRENADE %s (threat held %d ms, d=%d)\n", CodName( cs ), AICast_CodSquadId( cs ),
				  held ? "player holds cover" : "player behind a corner", hold, (int)Distance( cs->bs->origin, threatPos ) );
	}
	return qtrue;
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
	qboolean squad;

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
	// (codmod: Phase 3, the same squad cooldown / thrower choice as the proactive grenades, so they never double up)
	if ( CodGrenadeAvailable( cs, ca, threatPos ) ) {
		CodGrenadeCommit( cs, ca );
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
	if ( bg_cod.aiMorale && AICast_CodSquadMoraleActive( cs ) ) {
		return qfalse;      // codmod: Phase 3, a broken squad neither flanks nor advances
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
	waitedLong = ( ( ca->tokenWaitStart && level.time - ca->tokenWaitStart > 2500 ) || ca->oppUntil > level.time ) ? qtrue : qfalse;     // Phase 3: or the player is weak
	if ( !waitedLong && ca->peekCycles < 1 ) {
		return qfalse;
	}
	if ( Distance( cs->bs->origin, threatPos ) < bg_cod.aiAdvanceMinDist || !AICast_CodSquadCanAdvance( cs ) ) {
		return qfalse;
	}
	aggr = 0.25f + 0.4f * CodAggression( cs );
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

// codmod: Phase 5, a camper's post is its cover: a fake LOW cover at the anchor, fought from as a crouch-peeker.
// Returns qtrue when the soldier was sent to (or is at) the post.
static qboolean CodCamperPost( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	vec3_t anchor;

	if ( !ca->camper ) {
		return qfalse;
	}
	if ( ca->camperReloc ) {
		if ( level.time - ca->camperHitTime < 6000 ) {
			return qfalse;          // driven out: ordinary cover search
		}
		ca->camperReloc = qfalse;   // nothing better found: back to the old post
		ca->camperHits = 0;
	}
	if ( !AICast_CodPolishCamperAnchor( cs, anchor ) ) {
		return qfalse;
	}
	VectorCopy( anchor, ca->cover.pos );
	VectorCopy( anchor, ca->cover.peekPos );
	VectorCopy( threatPos, ca->cover.threatPos );
	ca->cover.type = COVER_LOW;
	ca->cover.score = 0;
	ca->cover.angle = qfalse;
	ca->cover.crouchShot = qfalse;
	ca->cover.travel = (int)( CodFlatDist( cs, anchor ) / 3.0f );
	ca->crouchWalk = qfalse;
	CodTakeCover( cs, ca, COVER_ATTACK );
	return qtrue;
}

static char *CodStateOpen( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	int mode;
	qboolean urgent, hasToken, shoot;

	if ( !ca->hasCover && CodCamperPost( cs, ca, threatPos ) ) {
		return NULL;
	}

	// Phase 2: without a token this soldier does not shoot, it gets to cover. With no cover to be found it fights anyway.
	hasToken = AICast_CodTokenAcquire( cs, 1200 );
	shoot = ( hasToken || ca->searchFails >= 3 ) ? qtrue : qfalse;

	// codmod: Phase 3, pinned down in the open: get to cover now (an urgent search ignores the ration)
	if ( CodSuppressed( cs->entityNum ) && !ca->hasCover ) {
		ca->urgent = qtrue;
		if ( ca->nextSearchTime > level.time + 250 ) {
			ca->nextSearchTime = level.time + 250;
		}
	}

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
	// codmod: Phase 5, the player charges: back away while shooting instead of standing / strafing
	if ( shoot && CodEnemyVisible( cs ) && AICast_CodPolishBackpedal( cs, threatPos ) ) {
		ca->crouchUntil = 0;
		cs->attackcrouch_time = 0;
	} else {
		CodOpenMove( cs, ca, threatPos, shoot );
	}
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
		ca->hideUntil = level.time + (int)( CodRandMs( bg_cod.aiHideMin, bg_cod.aiHideMax ) * ( 1.15f - 0.5f * CodAggression( cs ) ) * AICast_CodSkill( COD_SK_HIDE ) );
		ca->hideCheck = level.time + 250;
		ca->settledAt = 0;
		ca->reloadIssued = qfalse;
		ca->crouchWalk = qfalse;
		if ( ca->camperReloc ) {
			// codmod: Phase 5, a camper that was driven out of its post holds the new one
			ca->camperReloc = qfalse;
			ca->camperHits = 0;
			AICast_CodPolishCamperMoved( cs, ca->cover.pos );
		}
		// codmod: Phase 3, arrived from a retreat: stay down longer. Arrived from a dodge: at least until the
		// blast is over, and the (hidden-only) spot is replaced by an ordinary one at the first look
		if ( ca->retreatKind ) {
			ca->hideUntil = level.time + (int)( ( ca->hideUntil - level.time ) * ( ca->retreatKind == COD_RETREAT_MORALE ? 1.5f : 1.3f ) );
			ca->retreatKind = 0;
		}
		if ( ca->dodging ) {
			ca->dodging = qfalse;
			if ( ca->hideUntil < ca->dodgeEnd + 500 ) {
				ca->hideUntil = ca->dodgeEnd + 500;
			}
			ca->peekCycles = ca->maxPeekCycles;
		}
		CodSetState( cs, ca, COD_HIDE, "arrived" );
		return NULL;
	}
	if ( level.time > ca->moveTimeout ) {
		CodCoverFailed( cs, ca, qfalse, "move timeout" );
		return NULL;
	}
	if ( level.time >= ca->nextVisCheck ) {
		ca->nextVisCheck = level.time + 250;
		if ( dist > 150.0f && !ca->pushMode && !ca->dodging && CodExposed( cs, ca, threatPos ) ) {
			CodCoverFailed( cs, ca, qtrue, "cover exposed" );
			return NULL;
		}
	}

	// codmod: Phase 5, crouch-walk between low covers (a hit makes the soldier run instead)
	if ( ca->crouchWalk && cs->lastPain > ca->stateTime ) {
		ca->crouchWalk = qfalse;
	}
	if ( ca->crouchWalk ) {
		ca->crouchUntil = level.time + 300;
		cs->attackcrouch_time = ca->crouchUntil;
	} else {
		ca->crouchUntil = 0;
		cs->attackcrouch_time = 0;
		if ( dist > ( ca->dodging ? 60.0f : 200.0f ) ) {
			AICast_CodTrySprint( cs );
		}
	}
	// codmod: Phase 3, still inside the blast with the fuse nearly out: dive (a character with the dive animation)
	if ( ca->dodging && !ca->dodged && ( cs->aiFlags & AIFL_DIVE_ANIM ) && dist > 96.0f && ca->dodgeEnd - level.time < 1200
		 && Distance( cs->bs->origin, cs->dangerEntityPos ) < ca->dodgeRadius && !cs->bs->cur_ps.torsoTimer
		 && cs->lastRollMove < level.time - 800 && cs->castScriptStatus.castScriptEventIndex < 0
		 && cs->bs->cur_ps.groundEntityNum == ENTITYNUM_WORLD ) {
		vec3_t dir;
		char *r;

		VectorSubtract( ca->cover.pos, cs->bs->origin, dir );
		dir[2] = 0;
		ca->dodged = qtrue;
		if ( VectorNormalize( dir ) > 1.0f && ( r = AIFunc_BattleDiveStart( cs, dir ) ) != NULL ) {
			if ( bg_cod.aiDebug >= 1 ) {
				G_Printf( "[ai] %s DODGE dive (fuse %d ms)\n", CodName( cs ), ca->dodgeEnd - level.time );
			}
			return r;
		}
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
		// codmod: Phase 5, hit or suppressed on the way: zigzag (walking moves only, not for a dodge or a crouch-walk)
		if ( !ca->dodging && !ca->crouchWalk && dist > 100.0f && ( mr->traveltype & TRAVELTYPE_MASK ) == TRAVEL_WALK ) {
			AICast_CodPolishJink( cs, mr->movedir, CodUnderFire( cs ) );
		}
	}
	// run and gun where the weapon allows it (AICast_ProcessAttack aims after the movement was issued); Phase 2:
	// only a token holder shoots on the way
	if ( !CodWeaponTick( cs, ca->dodging ? qfalse : AICast_CodHasToken( cs ) ) ) {
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
	if ( ca->settledAt && level.time > ca->settledAt + 100 && level.time >= ca->dodgeEnd ) {
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

	// codmod: Phase 3. Under fire: keep the head down while the near misses go on (at most 3.5 s in all). The player
	// is weak (reloading, switching weapons, sliding, prone): a token holder does not wait out the rest of the hide.
	if ( !reloading && ca->settledAt && level.time >= ca->hideUntil - 50 && level.time - ca->hideStart < 3500 && CodSuppressed( cs->entityNum ) ) {
		ca->hideUntil = level.time + 300;
	} else if ( !reloading && ca->settledAt && level.time < ca->hideUntil && ca->oppUntil > level.time && !CodNeedsReload( cs )
				&& !CodSuppressed( cs->entityNum ) ) {
		ca->hideUntil = level.time;
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

	return CodEnterPeek( cs, ca, (int)( CodRandMs( bg_cod.aiPeekMin, bg_cod.aiPeekMax ) * AICast_CodSkill( COD_SK_PEEK ) ) + ( ca->cover.type == COVER_LOW ? 250 : 450 ), qfalse, "peek" );
}

static char *CodStateEndPeek( cast_state_t *cs, codAI_t *ca, qboolean hit, const char *why ) {
	AICast_CodTokenRelease( cs );       // Phase 2: the next waiting squad member may shoot
	ca->suppressPeek = qfalse;
	ca->hideStart = level.time;
	ca->hideUntil = level.time + (int)( CodRandMs( bg_cod.aiHideMin, bg_cod.aiHideMax ) * ( 1.15f - 0.5f * CodAggression( cs ) ) * ( ca->pushMode ? 0.5f : 1.0f ) * AICast_CodSkill( COD_SK_HIDE ) );
	if ( hit ) {
		ca->hideUntil += 400;       // keep the head down a little longer after being hit
	}
	// codmod: Phase 3, suppressed soldiers and broken squads stay down longer
	if ( CodSuppressed( cs->entityNum ) ) {
		ca->hideUntil = level.time + (int)( ( ca->hideUntil - level.time ) * ( 1.0f + 0.8f * CodSuppression( cs->entityNum ) ) );
	}
	if ( bg_cod.aiMorale && AICast_CodSquadMoraleActive( cs ) ) {
		ca->hideUntil = level.time + (int)( ( ca->hideUntil - level.time ) * 1.4f );
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
	// codmod: Phase 5, the player charges while we shoot from the peek: back away (the next HIDE walks back to the cover)
	if ( tokenOk && visible && !pushFire && !ca->suppressPeek && !ca->camper && AICast_CodPolishBackpedal( cs, threatNow ) ) {
		ca->crouchUntil = 0;
		cs->attackcrouch_time = 0;
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
		if ( bg_cod.aiHunt ) {
			// codmod: Phase 4, the enemy is not where we looked: leave the cover state; the next think sees the enemy
			// as lost and starts the hunt (AIFunc_CodCombat)
			CodExit( cs, ca, 0 );
			if ( bg_cod.aiDebug >= 1 ) {
				G_Printf( "[ai] %s PEEK lost the enemy: hunt\n", CodName( cs ) );
			}
			return NULL;
		}
		// the enemy is not where we looked: let the vanilla chase find them
		CodExit( cs, ca, 3000 );
		return AIFunc_BattleChaseStart( cs );
	}
	if ( level.time >= ca->peekUntil && tokenOk && !ca->suppressPeek && !pushFire && ca->oppUntil > level.time && ca->peekCycles < 6 ) {
		ca->peekUntil = level.time + 300;       // codmod: Phase 3, the player is weak: keep shooting
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
// Phase 3: grenade defence, retreat, morale, opportunism
//===========================================================================

#define COD_DANGER_VANILLA  0       // CodDanger results: hand over to the vanilla AvoidDanger
#define COD_DANGER_HANDLED  1       // *rval is the return value of this think
#define COD_DANGER_CONTINUE 2       // go on with the tactical think (a dodge is on, or this spot is safe)

/*
============
CodDanger

codmod: Phase 3. A danger entity is near. Only a grenade (or pineapple) is handled here, the rest (flames, dynamite, a
panzerfaust, ...) goes to the vanilla AvoidDanger as before. A grenade with a long fuse is caught and kicked back by a
soldier with AIFL_CATCH_GRENADE (the vanilla rule, AIFunc_GrenadeKick), otherwise the soldier stays put when the cover
here hides it from the landing point, or runs to cover out of the blast that it can reach before the fuse is out
(COVER_DODGE, urgent search, travel capped at the fuse) and dives when the fuse is nearly out (AIFL_DIVE_ANIM). No
dodge cover: the vanilla avoid. Each grenade is reacted to once.
============
*/
static int CodDanger( cast_state_t *cs, codAI_t *ca, char **rval ) {
	gentity_t *ent = &g_entities[cs->entityNum];
	gentity_t *danger;
	vec3_t kp, end;
	trace_t tr;
	int fuse, travelCap;
	float dist;

	*rval = NULL;
	if ( cs->dangerEntity < 0 || cs->dangerEntity >= MAX_GENTITIES ) {
		return COD_DANGER_VANILLA;
	}
	danger = &g_entities[cs->dangerEntity];
	if ( !danger->inuse || danger->s.eType != ET_MISSILE || ent->s.onFireEnd >= level.time
		 || ( danger->s.weapon != WP_GRENADE_LAUNCHER && danger->s.weapon != WP_GRENADE_PINEAPPLE ) ) {
		return COD_DANGER_VANILLA;
	}
	if ( ca->dodgeEnt == cs->dangerEntity && ca->dodgeThink == danger->nextthink ) {
		return ca->dodgeFailed ? COD_DANGER_VANILLA : COD_DANGER_CONTINUE;      // already reacted to this one
	}
	fuse = danger->nextthink - level.time;

	// 1. catch it and kick it back (the vanilla rules, AICast_AvoidDanger)
	if ( fuse > 1500 && level.lastGrenadeKick < level.time - 3000 && ( cs->aiFlags & AIFL_CATCH_GRENADE )
		 && !( danger->flags & FL_AI_GRENADE_KICK ) && !AICast_SameTeam( cs, danger->r.ownerNum ) ) {
		if ( G_PredictMissile( danger, fuse, kp, qfalse ) ) {
			kp[2] += -ent->r.mins[2] + 12;
			VectorCopy( kp, end );
			end[2] -= 90;
			trap_Trace( &tr, kp, ent->r.mins, ent->r.maxs, end, cs->entityNum, MASK_SOLID );
			VectorCopy( tr.endpos, kp );
			if ( !tr.startsolid && tr.fraction < 1.0f
				 && VectorDistance( cs->bs->origin, kp ) < cs->attributes[RUNNING_SPEED] * 0.0004 * ( fuse - 2000 ) ) {
				trap_Trace( &tr, cs->bs->origin, ent->r.mins, ent->r.maxs, kp, cs->entityNum, MASK_SOLID );
				if ( VectorDistance( tr.endpos, kp ) < 8 ) {
					VectorCopy( kp, cs->takeCoverPos );
					danger->flags |= FL_AI_GRENADE_KICK;
					ent->flags |= FL_AI_GRENADE_KICK;
					level.lastGrenadeKick = level.time;
					if ( bg_cod.aiDebug >= 1 ) {
						G_Printf( "[ai] %s GRENADE kick it back (fuse %d ms)\n", CodName( cs ), fuse );
					}
					CodExit( cs, ca, 0 );
					*rval = AIFunc_GrenadeKickStart( cs );
					return COD_DANGER_HANDLED;
				}
			}
		}
	}

	// 2. dodge
	ca->dodgeEnt = cs->dangerEntity;
	ca->dodgeThink = danger->nextthink;
	ca->dodgeFailed = qfalse;
	ca->dodged = qfalse;
	ca->dodgeEnd = cs->dangerEntityValidTime;
	ca->dodgeRadius = cs->dangerDist / 1.5f;
	dist = Distance( cs->bs->origin, cs->dangerEntityPos );
	if ( dist > ca->dodgeRadius * 1.15f
		 || ( dist > 60.0f && !AICast_VisibleFromPos( cs->dangerEntityPos, cs->dangerEntity, cs->bs->origin, cs->entityNum, qfalse ) ) ) {
		return COD_DANGER_CONTINUE;     // out of the blast, or covered from it: stay
	}
	travelCap = ( fuse - 500 ) / 10;    // AAS travel time is 1/100 s
	if ( travelCap < 40 ) {
		travelCap = 40;
	}
	if ( AICast_CodCoverSearchAllowed( qtrue ) ) {
		AICast_CodCoverSetTravelCap( travelCap );
		if ( AICast_CodFindCover( cs, cs->dangerEntityPos, cs->enemyNum, COVER_DODGE, &ca->cover, NULL, NULL ) ) {
			CodRelocEnd( cs, ca, qfalse );
			AICast_CodTokenRelease( cs );
			CodTakeCover( cs, ca, COVER_DODGE );
			ca->dodging = qtrue;
			ca->dodgeEnd = cs->dangerEntityValidTime;
			if ( ca->moveTimeout > level.time + fuse + 400 ) {
				ca->moveTimeout = level.time + fuse + 400;
			}
			if ( bg_cod.aiDebug >= 1 ) {
				G_Printf( "[ai] %s DODGE grenade fuse=%d d=%d -> cover d=%d travel=%d\n", CodName( cs ), fuse, (int)dist,
						  (int)Distance( ca->cover.pos, cs->dangerEntityPos ), ca->cover.travel );
			}
			return COD_DANGER_CONTINUE;
		}
	}
	ca->dodgeFailed = qtrue;
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s DODGE grenade fuse=%d d=%d, no cover in reach: vanilla avoid\n", CodName( cs ), fuse, (int)dist );
	}
	return COD_DANGER_VANILLA;
}

/*
============
CodTryRetreat

codmod: Phase 3. Low health or a broken squad: an urgent COVER_RETREAT search (an attack-type spot clearly farther from
the threat, up to 1.5x the usual travel). The soldier runs there, stays down longer and fights from there.
============
*/
static qboolean CodTryRetreat( cast_state_t *cs, codAI_t *ca, vec3_t threatPos, int kind ) {
	if ( !AICast_CodCoverSearchAllowed( qtrue ) ) {
		return qfalse;
	}
	CodRelocEnd( cs, ca, qfalse );
	if ( CodSearch( cs, ca, threatPos, COVER_RETREAT, NULL, NULL ) ) {
		AICast_CodTokenRelease( cs );
		ca->retreatKind = kind;
		ca->nextRetreat = level.time + 10000;
		if ( bg_cod.aiDebug >= 1 ) {
			G_Printf( "[ai] %s RETREAT %s -> cover d=%d travel=%d\n", CodName( cs ), kind == COD_RETREAT_MORALE ? "morale broken" : "low health",
					  (int)Distance( ca->cover.pos, threatPos ), ca->cover.travel );
		}
		return qtrue;
	}
	return qfalse;
}

// morale and low health, once per event
static void CodReactions( cast_state_t *cs, codAI_t *ca, vec3_t threatPos ) {
	gentity_t *ent = &g_entities[cs->entityNum];
	int maxHealth = ent->client->ps.stats[STAT_MAX_HEALTH];

	if ( bg_cod.aiMorale && AICast_CodSquadMoraleFresh( cs ) ) {
		AICast_CodSquadBark( cs, COD_BARK_ORDERS );
		if ( !CodTryRetreat( cs, ca, threatPos, COD_RETREAT_MORALE ) && bg_cod.aiDebug >= 1 ) {
			G_Printf( "[ai] %s MORALE broken, no farther cover: stays cautious\n", CodName( cs ) );
		}
		return;
	}
	if ( maxHealth <= 0 ) {
		maxHealth = 100;
	}
	if ( bg_cod.aiRetreatHealth > 0 && ent->health * 100 < maxHealth * (int)bg_cod.aiRetreatHealth && level.time >= ca->nextRetreat
		 && cs->lastPain > level.time - 3000 && ca->state != COD_MOVE ) {
		ca->nextRetreat = level.time + 3000;
		CodTryRetreat( cs, ca, threatPos, COD_RETREAT_HEALTH );
	}
}

/*
============
CodOpportunity

codmod: Phase 3. The player is weak for a moment: reloading, switching weapons, sliding or going prone. A soldier that saw
the player within the last second (or hears the reload, within 700 u) marks the squad opportunity: token holders peek
or advance at once, a waiting soldier may get an early token (one more token while it lasts), plus a bark.
============
*/
static void CodOpportunity( cast_state_t *cs, codAI_t *ca ) {
	gentity_t *enemy;
	playerState_t *ps;
	cast_visibility_t *vis;
	int ws, ms;
	qboolean reload, event, prone, proneNew, seen, heard;

	if ( cs->enemyNum < 0 || cs->enemyNum >= MAX_CLIENTS ) {
		return;
	}
	enemy = &g_entities[cs->enemyNum];
	if ( !enemy->client || ( enemy->r.svFlags & SVF_CASTAI ) ) {
		return;
	}
	ps = &enemy->client->ps;
	ws = ps->weaponstate;
	prone = ( ps->stats[STAT_COD_FLAGS] & COD_SF_PRONE ) ? qtrue : qfalse;
	proneNew = ( prone && !ca->oppWasProne ) ? qtrue : qfalse;
	ca->oppWasProne = prone;
	if ( !bg_cod.aiOpportunity ) {
		return;
	}
	reload = ( ws == WEAPON_RELOADING ) ? qtrue : qfalse;
	event = ( reload || ws == WEAPON_DROPPING || ws == WEAPON_RAISING || ws == WEAPON_DROPPING_TORELOAD || ws == WEAPON_RAISING_TORELOAD
			  || ( ps->pm_flags & PMF_COD_SLIDING ) || proneNew ) ? qtrue : qfalse;
	if ( !event ) {
		return;
	}
	vis = &cs->vislist[cs->enemyNum];
	seen = ( vis->real_visible_timestamp && vis->real_visible_timestamp > level.time - 1000 ) ? qtrue : qfalse;
	heard = ( reload && Distance( cs->bs->origin, ps->origin ) < 700.0f ) ? qtrue : qfalse;
	if ( !seen && !heard ) {
		return;
	}
	ms = reload ? 1500 : 900;
	if ( ca->oppUntil < level.time ) {
		if ( bg_cod.aiDebug >= 1 && level.time >= ca->nextDbg ) {
			ca->nextDbg = level.time + 2000;
			G_Printf( "[ai] %s sq%d OPPORTUNITY player %s\n", CodName( cs ), AICast_CodSquadId( cs ),
					  reload ? "reloads" : ( ps->pm_flags & PMF_COD_SLIDING ) ? "slides" : prone ? "goes prone" : "switches weapon" );
		}
		if ( level.time >= ca->nextOppBark ) {
			ca->nextOppBark = level.time + 7000;
			AICast_CodSquadBark( cs, COD_BARK_ORDERS );     // "Schnell!"
		}
	}
	if ( ca->oppUntil < level.time + ms ) {
		ca->oppUntil = level.time + ms;
	}
	AICast_CodSquadOpportunity( cs, ms );
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
	CodPerfFrame();     // codmod: Phase 6
	ca->camper = AICast_CodPolishIsCamper( cs );    // codmod: Phase 5
	AICast_CodSquadThink();     // codmod: Phase 2, regroups squads about once a second

	// ---- the checks of the vanilla functions we replace -----------------------------------------------
	if ( cs->aiState < AISTATE_COMBAT ) {
		AICast_StateChange( cs, AISTATE_COMBAT );
	}
	// do we need to avoid a danger? (Phase 3: a grenade is kicked back, dodged or ignored from cover here)
	if ( cs->dangerEntityValidTime >= level.time ) {
		int dr = CodDanger( cs, ca, &rval );

		if ( dr == COD_DANGER_HANDLED ) {
			return rval;
		}
		if ( dr == COD_DANGER_VANILLA ) {
			if ( !AICast_GetTakeCoverPos( cs, cs->dangerEntity, cs->dangerEntityPos, cs->takeCoverPos ) ) {
				// shit??
			}
			cs->takeCoverTime = cs->dangerEntityValidTime + 1000;
			CodExit( cs, ca, 0 );
			return AIFunc_AvoidDangerStart( cs );
		}
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
	// codmod: Phase 4, a dodge or a retreat in progress is finished before the hunt starts
	if ( elig == COD_ELIG_LOST && bg_cod.aiHunt && ca->state == COD_MOVE && ( ca->dodging || ca->retreatKind ) ) {
		elig = COD_ELIG_OK;
	}
	if ( elig != COD_ELIG_LOST ) {
		AICast_CodHuntLeave( cs );      // codmod: Phase 4, contact again (or not tactical any more): the hunt state goes
	}
	if ( elig == COD_ELIG_LOST && ca->camper ) {
		// codmod: Phase 5, a camper does not hunt or chase: vanilla camper behaviour until the enemy shows up again
		CodExit( cs, ca, 3000 );
		return AIFunc_DefaultStart( cs );
	}
	if ( elig == COD_ELIG_LOST && bg_cod.aiHunt ) {
		// codmod: Phase 4, lost contact: hunt (hops, held angles, corner clearing, a sweep, then heightened alert)
		int hr;

		if ( ca->state != COD_OPEN || ca->hasCover || ca->pushMode ) {
			CodExit( cs, ca, 0 );       // drops cover, token, mover slot and crouch
		}
		hr = AICast_CodHuntThink( cs, &rval );
		if ( hr == COD_HUNT_BUSY ) {
			return NULL;
		}
		if ( hr == COD_HUNT_ALERT ) {
			CodExit( cs, ca, 0 );
			return rval;
		}
		// COD_HUNT_VANILLA: the old hand-over below
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
	AICast_CodPolishThink( cs, visible );       // codmod: Phase 5, time-in-sight clock for the accuracy model
	if ( visible && !ca->camper ) {
		AICast_CodSquadReportThreat( cs, g_entities[cs->enemyNum].r.currentOrigin );
		AICast_CodHuntTrack( cs );      // codmod: Phase 4, last seen position and velocity for the hunt
	}
	if ( !ca->camper ) {
		AICast_CodSquadPublish( cs, ( ca->state == COD_HIDE || ca->state == COD_PEEK ) ? qtrue : qfalse );
	}
	CodThreatPos( cs, threatPos );

	// codmod: Phase 3. The player is weak for a moment.
	CodOpportunity( cs, ca );

	// codmod: Phase 5, melee kick when the player is right in front of the soldier
	if ( AICast_CodPolishKick( cs, ca->dodging ) ) {
		ca->crouchUntil = 0;
		cs->attackcrouch_time = 0;
		CodDebugDraw( cs, ca );
		return NULL;
	}

	// codmod: Phase 3. A grenade for a player who holds one spot or sits behind a corner (replaces the vanilla
	// grenade flush call, whose 7 s spacing ignored squads); never while running for cover.
	if ( !ca->dodging && CodGrenadeWanted( cs, ca, threatPos, visible ) ) {
		CodGrenadeCommit( cs, ca );
		CodExit( cs, ca, 0 );
		return AIFunc_GrenadeFlushStart( cs );
	}

	// being hit asks for a quick re-think of the cover
	if ( cs->lastPain > ca->lastPainSeen ) {
		ca->lastPainSeen = cs->lastPain;
		if ( level.time - cs->lastPain < 500 ) {
			ca->urgent = qtrue;
			if ( ca->state == COD_OPEN && !ca->hasCover ) {
				ca->nextSearchTime = level.time;        // codmod: Phase 3, hit in the open: cover now
				if ( bg_cod.aiDebug >= 1 ) {
					G_Printf( "[ai] %s HIT in the open: urgent cover\n", CodName( cs ) );
				}
			}
		}
	}
	// codmod: Phase 3, broken morale, low health
	if ( !ca->dodging ) {
		CodReactions( cs, ca, threatPos );
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
	ca->dodging = qfalse;
	ca->retreatKind = 0;
	ca->holdSince = 0;
	ca->nextGrenadeCheck = level.time + 1000;
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
