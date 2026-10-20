/*
===========================================================================

Return to Castle Wolfenstein single player GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.

This file is part of the Return to Castle Wolfenstein single player GPL Source Code (RTCW SP Source Code).

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

// codmod: new file -- ai_cast_cod_polish.c
//
//===========================================================================
// Name:			ai_cast_cod_polish.c
// Function:		codmod tactical AI, Phase 5 -- movement and aim polish
// Tab Size:		4 (real tabs)
//
// docs/ai-overhaul-plan.md, Phase 5. Small behaviours the combat state machine (ai_cast_cod.c) asks for:
//   backpedal    the player charges (closing fast, 140-400 u): back away, still shooting, instead of standing
//   crouch walk  a move from one low cover to another whose route stays hidden from the threat is done crouched
//   jink         hit or suppressed: short sideways direction changes (0.3-0.6 s) while moving or strafing
//   kick         the player stands within melee range in front: a kick (the soldier's own "kick" animation)
//   accuracy     first shots of a sight go wide, accuracy grows with the time in sight (scaled by skill), drops
//                while the target sprints or slides and while the shooter moves
//   campers      the anchor (post) of a CAMPER soldier
//
// Every entry point is gated by bg_cod.enable && bg_cod.aiTactics && bg_cod.aiPolish (plus its own switch) and is
// only ever called from AIFunc_CodCombat / AICast_GetAccuracy, i.e. for soldiers that run the tactical combat.
// Nothing is stored in cast_state_t: the records are static, stamped with level.startTime, and reset when they look
// stale (a savegame load), like codAI[] in ai_cast_cod.c.
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

#define POL_ON( sw )        ( bg_cod.enable && bg_cod.aiTactics && bg_cod.aiPolish && bg_cod.sw )

#define POL_BACK_MIN        140.0f      // backpedal band (flat distance to the enemy)
#define POL_BACK_MAX        400.0f
#define POL_BACK_STEP       64.0f       // room checked behind the soldier
#define POL_SIGHT_GAP       4000        // unseen for this long: the engagement is over, the next sight starts cold
#define POL_KICK_RANGE      64.0f       // flat distance from which a soldier kicks
#define POL_KICK_REACH      84.0f       // length of the hit trace
#define POL_KICK_HIT_MS     350         // kick animation: contact
#define POL_KICK_END_MS     750         // kick animation: cut here, the soldier moves on

typedef struct {
	int levelStart;
	int lastThink;

	// accuracy
	int sightMs;                // time the enemy was in sight in this engagement
	int lastSight;
	qboolean accDbg;

	// backpedal
	int backUntil;
	int backCheck;
	int nextBack;

	// jink
	int jinkSide;
	int jinkUntil;
	int jinkCheck;
	qboolean jinkOk;
	int nextJinkDbg;
	int lastJinkMove;

	// kick
	int kickStart;              // 0 = not kicking
	qboolean kickHit;
	int nextKick;

	// camper
	qboolean anchorSet;
	vec3_t anchor;
} polish_t;

static polish_t pols[MAX_CLIENTS];

static const char *PolName( cast_state_t *cs ) {
	char *name = g_entities[cs->entityNum].aiName;

	return name ? name : "?";
}

static void PolDebug( cast_state_t *cs, const char *what ) {
	G_Printf( "[ai] %s sq%d/%s %s\n", PolName( cs ), AICast_CodSquadId( cs ), AICast_CodSquadRoleName( cs ), what );
}

/*
============
PolGet

the soldier's record, cleared when it belongs to another level or to the future (savegame load)
============
*/
static polish_t *PolGet( cast_state_t *cs ) {
	polish_t *p = &pols[cs->entityNum];

	if ( p->levelStart != level.startTime || p->lastThink > level.time ) {
		memset( p, 0, sizeof( *p ) );
		p->levelStart = level.startTime;
	}
	return p;
}

void AICast_CodPolishReset( int entNum ) {
	if ( entNum < 0 || entNum >= MAX_CLIENTS ) {
		return;
	}
	memset( &pols[entNum], 0, sizeof( pols[entNum] ) );
	pols[entNum].levelStart = level.startTime;
}

//===========================================================================
// accuracy model
//===========================================================================

/*
============
AICast_CodPolishThink

codmod: once per AIFunc_CodCombat think. Keeps the clock of the time the enemy has been in sight: it only runs while
the enemy is visible and starts again from zero after POL_SIGHT_GAP of not seeing it (a new engagement).
============
*/
void AICast_CodPolishThink( cast_state_t *cs, qboolean visible ) {
	polish_t *p;
	int dt;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS || !POL_ON( aiAccuracy ) ) {
		return;
	}
	p = PolGet( cs );
	dt = p->lastThink ? level.time - p->lastThink : 0;
	if ( dt < 0 ) {
		dt = 0;
	} else if ( dt > 200 ) {
		dt = 200;
	}
	if ( visible ) {
		if ( p->lastSight < level.time - POL_SIGHT_GAP ) {
			p->sightMs = 0;
			p->accDbg = qfalse;
		}
		p->lastSight = level.time;
		p->sightMs += dt;
		if ( !p->accDbg ) {
			p->accDbg = qtrue;
			if ( bg_cod.aiDebug >= 1 ) {
				float mul = AICast_CodSkill( COD_SK_WARMUP );

				PolDebug( cs, va( "ACC warmup start x%.2f over %d ms (skill %d)", bg_cod.aiWarmupAcc, (int)( bg_cod.aiWarmup * mul ), g_gameskill.integer ) );
			}
		}
	}
	p->lastThink = level.time;
}

/*
============
AICast_CodPolishAccuracy

codmod: multiplied into AICast_GetAccuracy (together with the Phase 3 suppression factor). 1.0 for everybody that
does not run the tactical combat. Never above 1, never below 0.15:
  - warm-up: the first third of the warm-up time at cod_aiWarmupAcc, then rising to 1 (warm-up time scaled by skill:
    easy 1.4x, medium 1x, hard 0.7x)
  - the target sprints x0.8 or slides x0.6 (player flags)
  - the shooter moves: up to x0.7 at 200 u/s
============
*/
float AICast_CodPolishAccuracy( int entNum ) {
	polish_t *p;
	cast_state_t *cs;
	gentity_t *ent, *enemy;
	float f = 1.0f, warm, t, a, speed;

	if ( entNum < 0 || entNum >= MAX_CLIENTS || !POL_ON( aiAccuracy ) ) {
		return 1.0f;
	}
	p = &pols[entNum];
	if ( p->levelStart != level.startTime || p->lastThink < level.time - 1000 ) {
		return 1.0f;
	}
	ent = &g_entities[entNum];
	cs = AICast_GetCastState( entNum );
	if ( !cs || !ent->client ) {
		return 1.0f;
	}

	// warm-up
	warm = bg_cod.aiWarmup * AICast_CodSkill( COD_SK_WARMUP );
	if ( warm >= 1.0f && p->sightMs < warm ) {
		a = bg_cod.aiWarmupAcc;
		if ( a < 0.05f ) {
			a = 0.05f;
		} else if ( a > 1.0f ) {
			a = 1.0f;
		}
		t = ( (float)p->sightMs / warm - 0.35f ) / 0.65f;       // flat start, then a smooth rise
		if ( t < 0 ) {
			t = 0;
		}
		t = t * t * ( 3.0f - 2.0f * t );
		f *= a + ( 1.0f - a ) * t;
	}

	// the target
	if ( cs->enemyNum >= 0 && cs->enemyNum < MAX_CLIENTS ) {
		enemy = &g_entities[cs->enemyNum];
		if ( enemy->client ) {
			if ( enemy->client->ps.pm_flags & PMF_COD_SLIDING ) {
				f *= 0.6f;
			} else if ( enemy->client->ps.stats[STAT_COD_FLAGS] & COD_SF_SPRINTING ) {
				f *= 0.8f;
			}
		}
	}

	// the shooter
	speed = sqrt( ent->client->ps.velocity[0] * ent->client->ps.velocity[0] + ent->client->ps.velocity[1] * ent->client->ps.velocity[1] );
	if ( speed > 60.0f ) {
		f *= 1.0f - 0.3f * ( speed >= 200.0f ? 1.0f : speed / 200.0f );
	}

	if ( f < 0.15f ) {
		f = 0.15f;
	}
	return f > 1.0f ? 1.0f : f;
}

//===========================================================================
// room checks
//===========================================================================

/*
============
PolPathOk

room to move len units along dir (flat): the hull is not blocked, there is ground below the end and, with
checkAAS, the end lies in an AAS area the soldier can walk to
============
*/
static qboolean PolPathOk( cast_state_t *cs, vec3_t dir, float len, qboolean checkAAS ) {
	static vec3_t mins = { -18, -18, -24 }, maxs = { 18, 18, 48 };
	vec3_t end, down;
	trace_t tr;
	int area;

	VectorMA( cs->bs->origin, len, dir, end );
	trap_Trace( &tr, cs->bs->origin, mins, maxs, end, cs->entityNum, MASK_DEADSOLID );
	if ( tr.fraction < 1.0f || tr.startsolid ) {
		return qfalse;
	}
	VectorCopy( end, down );
	down[2] -= 56;
	trap_Trace( &tr, end, mins, maxs, down, cs->entityNum, MASK_DEADSOLID );
	if ( tr.startsolid || tr.fraction >= 1.0f ) {
		return qfalse;          // a ledge
	}
	if ( checkAAS ) {
		area = BotPointAreaNum( tr.endpos );
		if ( !area ) {
			return qfalse;
		}
		if ( area != cs->bs->areanum && trap_AAS_AreaTravelTimeToGoalArea( cs->bs->areanum, cs->bs->origin, area, cs->travelflags ) <= 0 ) {
			return qfalse;
		}
	}
	return qtrue;
}

//===========================================================================
// backpedal
//===========================================================================

/*
============
AICast_CodPolishBackpedal

codmod: the player runs at the soldier (closing speed >= cod_aiBackpedalSpeed, 140-400 u away) while the soldier has
a shot: it backs away from the threat for 0.5-0.9 s, still firing (weapons that cannot fire on the move excluded), then
a cooldown of 3 s. The room behind is hull-, ledge- and AAS-checked when it starts and every 250 ms. Returns qtrue
when it issued the move this think (the caller then skips its strafe).
============
*/
qboolean AICast_CodPolishBackpedal( cast_state_t *cs, vec3_t threatPos ) {
	polish_t *p;
	gentity_t *enemy;
	vec3_t away, rel;
	float dist, closing;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS || !POL_ON( aiBackpedal ) ) {
		return qfalse;
	}
	p = PolGet( cs );
	p->lastThink = level.time;
	if ( !AICast_CanMoveWhileFiringWeapon( cs->weaponNum ) || cs->enemyNum < 0 ) {
		p->backUntil = 0;
		return qfalse;
	}
	enemy = &g_entities[cs->enemyNum];
	VectorSubtract( cs->bs->origin, enemy->r.currentOrigin, away );
	away[2] = 0;
	dist = VectorNormalize( away );
	if ( dist < 1.0f || cs->bs->cur_ps.groundEntityNum == ENTITYNUM_NONE ) {
		p->backUntil = 0;
		return qfalse;
	}

	if ( p->backUntil > level.time ) {
		// keep going while the room lasts and the enemy is still on its way
		if ( dist < POL_BACK_MIN * 0.6f || dist > POL_BACK_MAX * 1.3f ) {
			p->backUntil = 0;
			return qfalse;
		}
		if ( level.time >= p->backCheck ) {
			p->backCheck = level.time + 250;
			if ( !PolPathOk( cs, away, POL_BACK_STEP, qtrue ) ) {
				p->backUntil = 0;
				p->nextBack = level.time + 1500;
				return qfalse;
			}
		}
		trap_EA_Move( cs->entityNum, away, 400 );
		cs->speedScale = 1.0f;
		return qtrue;
	}

	if ( level.time < p->nextBack || dist < POL_BACK_MIN || dist > POL_BACK_MAX || !enemy->client ) {
		return qfalse;
	}
	// closing speed: how fast the distance shrinks (the enemy's velocity minus ours, along the line between us)
	VectorSubtract( enemy->client->ps.velocity, cs->bs->cur_ps.velocity, rel );
	rel[2] = 0;
	closing = DotProduct( rel, away );
	if ( closing < bg_cod.aiBackpedalSpeed ) {
		return qfalse;
	}
	p->nextBack = level.time + 600;     // retry rate when the room check fails
	if ( !PolPathOk( cs, away, POL_BACK_STEP, qtrue ) ) {
		return qfalse;
	}
	p->backUntil = level.time + 500 + rand() % 400;
	p->backCheck = level.time + 250;
	p->nextBack = p->backUntil + 3000;
	if ( bg_cod.aiDebug >= 1 ) {
		PolDebug( cs, va( "BACKPEDAL player charges at %d u/s, d=%d", (int)closing, (int)dist ) );
	}
	trap_EA_Move( cs->entityNum, away, 400 );
	cs->speedScale = 1.0f;
	return qtrue;
}

//===========================================================================
// jinks
//===========================================================================

/*
============
AICast_CodPolishJinkLegMs

codmod: OPEN strafe leg length while under fire: 0.3-0.6 s instead of the normal 0.6-1.4 s (0 = not under fire)
============
*/
int AICast_CodPolishJinkLegMs( cast_state_t *cs, qboolean underFire ) {
	polish_t *p;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS || !underFire || !POL_ON( aiJink ) ) {
		return 0;
	}
	p = PolGet( cs );
	p->lastThink = level.time;
	if ( bg_cod.aiDebug >= 1 && level.time >= p->nextJinkDbg ) {
		p->nextJinkDbg = level.time + 2500;
		PolDebug( cs, "JINK strafing in short legs under fire" );
	}
	return 300 + rand() % 301;
}

/*
============
AICast_CodPolishJink

codmod: a soldier running to cover that is hit or suppressed zigzags: the movement direction chosen by the pathing
(moveDir) gets a sideways component that flips every 0.3-0.6 s. The side step is checked for room and a ledge when
it changes and every 200 ms; with no room the move is left alone. Returns qtrue when it replaced the movement.
============
*/
qboolean AICast_CodPolishJink( cast_state_t *cs, vec3_t moveDir, qboolean underFire ) {
	polish_t *p;
	vec3_t fwd, perp, dir;
	int tries;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS || !POL_ON( aiJink ) ) {
		return qfalse;
	}
	p = PolGet( cs );
	p->lastThink = level.time;
	if ( !underFire ) {
		p->jinkUntil = 0;
		return qfalse;
	}
	VectorCopy( moveDir, fwd );
	fwd[2] = 0;
	if ( VectorNormalize( fwd ) < 0.5f ) {
		return qfalse;
	}
	perp[0] = -fwd[1];
	perp[1] = fwd[0];
	perp[2] = 0;

	if ( level.time >= p->jinkUntil ) {
		p->jinkSide = ( p->jinkSide == 0 ) ? ( ( rand() & 1 ) ? 1 : -1 ) : -p->jinkSide;
		p->jinkUntil = level.time + 300 + rand() % 301;
		p->jinkCheck = 0;
		if ( bg_cod.aiDebug >= 1 && level.time >= p->nextJinkDbg ) {
			p->nextJinkDbg = level.time + 2500;
			PolDebug( cs, va( "JINK %s while running", p->jinkSide > 0 ? "left" : "right" ) );
		}
	}
	for ( tries = 0; tries < 2; tries++ ) {
		VectorScale( fwd, 0.75f, dir );
		VectorMA( dir, 0.65f * p->jinkSide, perp, dir );
		VectorNormalize( dir );
		if ( level.time < p->jinkCheck ) {
			break;
		}
		p->jinkOk = PolPathOk( cs, dir, 48.0f, qfalse );
		if ( p->jinkOk ) {
			p->jinkCheck = level.time + 200;
			break;
		}
		p->jinkSide = -p->jinkSide;     // no room that way: try the other
		p->jinkCheck = 0;
		if ( tries == 1 ) {
			p->jinkCheck = level.time + 200;
		}
	}
	if ( !p->jinkOk ) {
		return qfalse;
	}
	trap_EA_Move( cs->entityNum, dir, 400 );
	return qtrue;
}

//===========================================================================
// crouch walk
//===========================================================================

/*
============
AICast_CodPolishCrouchWalk

codmod: should a move from 'from' to 'to' (both low-cover spots, checked by the caller) be crouch-walked? Short (40-300 u
flat), a straight walkable line between them, and at least 3 of 5 points along it hidden from the threat's eye for a
crouched head (so standing up would show it, crouching does not).
============
*/
qboolean AICast_CodPolishCrouchWalk( cast_state_t *cs, vec3_t from, vec3_t to, vec3_t threatPos ) {
	static vec3_t mins = { -18, -18, -24 }, maxs = { 18, 18, 48 };
	vec3_t d, p, threatEye;
	trace_t tr;
	float len;
	int i, hidden = 0;

	if ( !POL_ON( aiCrouchWalk ) || !AICast_CodCanCrouch( cs ) ) {
		return qfalse;
	}
	VectorSubtract( to, from, d );
	d[2] = 0;
	len = VectorLength( d );
	if ( len < 40.0f || len >= 300.0f ) {
		return qfalse;
	}
	trap_Trace( &tr, from, mins, maxs, to, cs->entityNum, MASK_DEADSOLID );
	if ( tr.fraction < 1.0f || tr.startsolid ) {
		return qfalse;          // the route winds around something
	}
	VectorCopy( threatPos, threatEye );
	threatEye[2] += DEFAULT_VIEWHEIGHT;
	for ( i = 0; i < 5; i++ ) {
		VectorSubtract( to, from, d );
		VectorMA( from, 0.1f + 0.2f * i, d, p );
		p[2] += CROUCH_VIEWHEIGHT;
		if ( !AICast_CodLineClear( p, threatEye ) ) {
			hidden++;
		}
	}
	if ( hidden < 3 ) {
		return qfalse;
	}
	if ( bg_cod.aiDebug >= 1 ) {
		PolDebug( cs, va( "CROUCHWALK low cover to low cover, %d u, %d/5 hidden", (int)len, hidden ) );
	}
	return qtrue;
}

//===========================================================================
// kick
//===========================================================================

// qtrue when the character's animation set has an animation of that name (BG_PlayAnimName would error out otherwise)
animModelInfo_t *BG_ModelInfoForClient( int client );

static qboolean PolHasAnim( int entNum, const char *name ) {
	animModelInfo_t *mi = BG_ModelInfoForClient( entNum );
	int i;

	if ( !mi ) {
		return qfalse;
	}
	for ( i = 0; i < mi->numAnimations; i++ ) {
		if ( !Q_stricmp( mi->animations[i].name, name ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
============
AICast_CodPolishKick

codmod: a kick for a player who stands within 64 u in front of the soldier. The soldier's own "kick" animation (the
one the Black Guard uses; characters without it never kick) plays, at contact (350 ms) a short hit trace decides: the
damage is cod_aiKickDamage with MOD_KICKED plus a small shove, with the player kick's flesh / miss event. The anim is
cut at 750 ms. Cooldown cod_aiKickCooldown. Only against a human-controlled enemy. 'busy' (a dodge in progress) blocks
a new kick. Returns qtrue while the kick runs: the caller does nothing else that think.
============
*/
qboolean AICast_CodPolishKick( cast_state_t *cs, qboolean busy ) {
	polish_t *p;
	gentity_t *ent, *enemy;
	vec3_t dir, eye, fwd;
	trace_t *tr;
	float dist, yaw;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS || !POL_ON( aiKick ) ) {
		return qfalse;
	}
	p = PolGet( cs );
	p->lastThink = level.time;
	ent = &g_entities[cs->entityNum];
	if ( cs->enemyNum < 0 || cs->enemyNum >= MAX_CLIENTS ) {
		p->kickStart = 0;
		return qfalse;
	}
	enemy = &g_entities[cs->enemyNum];

	VectorCopy( cs->bs->origin, eye );
	eye[2] += cs->bs->cur_ps.viewheight;
	VectorSubtract( enemy->r.currentOrigin, cs->bs->origin, dir );

	if ( p->kickStart ) {
		if ( level.time - p->kickStart > POL_KICK_END_MS || ent->health <= 0 || enemy->health <= 0 ) {
			p->kickStart = 0;
			return qfalse;
		}
		// keep facing the target while the leg swings
		VectorSubtract( enemy->r.currentOrigin, eye, dir );
		dir[2] += 20;
		vectoangles( dir, cs->ideal_viewangles );
		if ( cs->ideal_viewangles[PITCH] > 180 ) {
			cs->ideal_viewangles[PITCH] -= 360;
		}
		if ( !p->kickHit && level.time - p->kickStart >= POL_KICK_HIT_MS ) {
			p->kickHit = qtrue;
			AngleVectors( cs->bs->cur_ps.viewangles, fwd, NULL, NULL );
			tr = CheckMeleeAttack( ent, POL_KICK_REACH, qfalse );
			if ( tr && tr->entityNum == cs->enemyNum && enemy->takedamage ) {
				gentity_t *tent = G_TempEntity( tr->endpos, EV_WOLFKICK_HIT_FLESH );

				tent->s.eventParm = enemy->s.number;
				tent->s.otherEntityNum = ent->s.number;
				G_Damage( enemy, ent, ent, fwd, tr->endpos, (int)bg_cod.aiKickDamage, 0, MOD_KICKED );
				fwd[2] = 0.3f;
				VectorMA( enemy->client->ps.velocity, 180, fwd, enemy->client->ps.velocity );
				if ( bg_cod.aiDebug >= 1 ) {
					PolDebug( cs, va( "KICK hit, %d damage", (int)bg_cod.aiKickDamage ) );
				}
			} else {
				gentity_t *tent = G_TempEntity( cs->bs->origin, EV_WOLFKICK_MISS );

				tent->s.eventParm = ent->s.number;
				if ( bg_cod.aiDebug >= 1 ) {
					PolDebug( cs, "KICK missed" );
				}
			}
		}
		return qtrue;
	}

	if ( busy || level.time < p->nextKick || !enemy->client || ( enemy->r.svFlags & SVF_CASTAI ) || enemy->health <= 0 ) {
		return qfalse;
	}
	// close, level with us, in front, no other animation running, standing on the ground
	if ( fabs( dir[2] ) > 50.0f ) {
		return qfalse;
	}
	dir[2] = 0;
	dist = VectorNormalize( dir );
	if ( dist > POL_KICK_RANGE ) {
		return qfalse;
	}
	if ( ent->client->ps.legsTimer || ent->client->ps.pm_type != PM_NORMAL
		 || cs->bs->cur_ps.groundEntityNum == ENTITYNUM_NONE ) {
		return qfalse;
	}
	vectoangles( dir, fwd );
	yaw = fabs( AngleDifference( cs->viewangles[YAW], fwd[YAW] ) );
	if ( yaw > 50.0f ) {
		return qfalse;
	}
	if ( !PolHasAnim( cs->entityNum, "kick" ) ) {
		p->nextKick = level.time + 10000;       // this character has no kick animation
		return qfalse;
	}

	p->kickStart = level.time;
	p->kickHit = qfalse;
	p->nextKick = level.time + (int)bg_cod.aiKickCooldown * ( 90 + rand() % 21 ) / 100;
	BG_PlayAnimName( &ent->client->ps, "kick", ANIM_BP_BOTH, qtrue, qfalse, qtrue );
	if ( ent->client->ps.legsTimer > POL_KICK_END_MS ) {
		ent->client->ps.legsTimer = POL_KICK_END_MS;
	}
	if ( ent->client->ps.torsoTimer > POL_KICK_END_MS ) {
		ent->client->ps.torsoTimer = POL_KICK_END_MS;
	}
	if ( bg_cod.aiDebug >= 1 ) {
		PolDebug( cs, va( "KICK player in melee range d=%d", (int)dist ) );
	}
	return qtrue;
}

//===========================================================================
// campers
//===========================================================================

qboolean AICast_CodPolishIsCamper( cast_state_t *cs ) {
	return ( POL_ON( aiCamper ) && cs->attributes[CAMPER] >= 0.5f ) ? qtrue : qfalse;
}

/*
============
AICast_CodPolishCamperAnchor

the camper's post: where it stood when the tactical combat first saw it (usually where it spawned). Returns qfalse for a
soldier that is not a camper.
============
*/
qboolean AICast_CodPolishCamperAnchor( cast_state_t *cs, vec3_t out ) {
	polish_t *p;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS || !AICast_CodPolishIsCamper( cs ) ) {
		return qfalse;
	}
	p = PolGet( cs );
	p->lastThink = level.time;
	if ( !p->anchorSet ) {
		p->anchorSet = qtrue;
		VectorCopy( cs->bs->origin, p->anchor );
		if ( bg_cod.aiDebug >= 1 ) {
			PolDebug( cs, va( "CAMPER holds its post at %d %d %d", (int)p->anchor[0], (int)p->anchor[1], (int)p->anchor[2] ) );
		}
	}
	VectorCopy( p->anchor, out );
	return qtrue;
}

void AICast_CodPolishCamperMoved( cast_state_t *cs, vec3_t pos ) {
	polish_t *p;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS ) {
		return;
	}
	p = PolGet( cs );
	p->anchorSet = qtrue;
	VectorCopy( pos, p->anchor );
}
