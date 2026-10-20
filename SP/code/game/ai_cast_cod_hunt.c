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

//===========================================================================
//
// Name:			ai_cast_cod_hunt.c
// Function:		codmod tactical AI, Phase 4 -- search and hunt after contact is lost
// Tab Size:		4 (real tabs)
//
// docs/ai-overhaul-plan.md, Phase 4. AIFunc_CodCombat calls AICast_CodHuntThink() instead of handing a soldier
// that lost its enemy to the vanilla chase (AIFunc_BattleChase). The hunt is a small state machine per soldier on
// top of a record per enemy that all hunters of that enemy share:
//
//   HUNT phase   a target (last seen position + last velocity * time, at most cod_aiHuntPredict away, dropped to
//                walkable ground) is approached by bounding overwatch: one or two soldiers hop (cover to cover when
//                a COVER_ADVANCE spot exists, else a timed run along the AAS route) while the others HOLD an angle
//                (view only, aimed at where the route from the target first becomes visible, or at the last seen
//                point). While hopping the soldier aims at the corner the route turns around and, close to it,
//                crouch-walks ("slice the pie"). The phase ends when somebody reaches the target or after
//                cod_aiHuntTime.
//   SWEEP phase  a few AAS areas the player could have reached (by travel time from the last known position) are
//                shared out between the soldiers; each visits its point and looks around. Ends after
//                cod_aiSweepTime.
//   ALERT        the soldiers forget the chase but not the fight: enemyNum is cleared, they go to the vanilla
//                idle/alert state and react to the next sighting twice as fast for cod_aiAlertTime.
//
// Seeing the enemy at any point makes AICast_CodHuntThink() unnecessary (CodEligibility is OK again), so the
// soldier is back in the normal CodCombat at once; a holder shoots as soon as the vision code reports the player
// (reaction time is halved while hunting / alerted, AICast_CodAlertActive).
//
// Nothing here is stored in cast_state_t. The records are static arrays stamped with level.startTime and checked
// for age, so a savegame load just starts them from scratch.
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

#define HUNT_MAX_PART       24
#define HUNT_MAX_SWEEP      5
#define HUNT_STALE_MS       600         // a soldier that did not think for this long left the hunt
#define HUNT_REC_STALE_MS   5000        // a record nobody touched for this long is a leftover
#define HUNT_DONE_KEEP_MS   15000       // a finished hunt is forgotten after this long (a later loss starts a new one)
#define HUNT_REACH_DIST     90.0f       // horizontal distance that counts as "at the target"
#define HUNT_COVER_DIST     14.0f       // arrived at a hop cover spot
#define HUNT_SWEEP_DIST     48.0f       // arrived at a sweep point
#define HUNT_MAX_FAILS      3           // path failures before the soldier gives the hunt up
#define HUNT_DBG_MS         1500

typedef enum {
	HP_HUNT,
	HP_SWEEP,
	HP_DONE
} huntPhase_t;

typedef enum {
	HM_NONE,
	HM_HOLD,        // standing, aiming at an angle
	HM_HOP,         // running to the next spot
	HM_SWEEP,       // walking to a sweep point
	HM_LOOK         // at a sweep point (or nothing left to visit): looking around
} huntMode_t;

static const char *huntModeNames[] = { "NONE", "HOLD", "HOP", "SWEEP", "LOOK" };

typedef struct {
	int ent;
	int time;                   // last think
	int waitSince;              // holding since (0 = not holding)
	qboolean ready;             // its hold time is over: it wants a hop
} huntPart_t;

// per enemy: what is known about where it went, and the hunt of all soldiers that lost it
typedef struct {
	int levelStart;
	int lastUse;

	// sighting track (any soldier that sees the enemy feeds it)
	int seenTime;
	vec3_t seenPos;
	vec3_t seenVel;

	// the hunt
	qboolean active;
	int serial;
	int phase;
	int start;
	int doneTime;
	vec3_t target;
	int targetArea;
	int sweepStart;
	int numSweep;
	vec3_t sweepPos[HUNT_MAX_SWEEP];
	int sweepOwner[HUNT_MAX_SWEEP];     // entity number or -1
	int sweepClaim[HUNT_MAX_SWEEP];     // when it was claimed
	qboolean sweepDone[HUNT_MAX_SWEEP];
	int hopEnt[2];                      // soldiers that may be moving right now
	int hopUntil[2];
	huntPart_t part[HUNT_MAX_PART];
} huntRec_t;

typedef struct {
	int levelStart;
	int lastThink;
	int serial;                 // the record's serial this state belongs to
	int mode;
	int modeTime;
	int alertUntil;             // heightened alert (survives a hunt reset)

	vec3_t goal;
	qboolean goalIsTarget;
	qboolean hasCover;
	codCover_t cover;
	int hopTimeout;
	int holdUntil;
	int holdSince;
	int fails;
	qboolean noHop;             // routing failed: this soldier only holds (and sweeps)

	vec3_t aim;
	int aimKind;                // 0 target, 1 corridor / corner, 2 last seen point
	int nextAim;
	qboolean clearing;          // corner clearing: last stretch before the aim point

	int sweepIdx;
	int lookUntil;
	float lookBase;
	int pickTime;

	vec3_t lastPos;
	int lastPosTime;
	int stuck;

	int crouchUntil;
	int nextDbg;
} huntSoldier_t;

static huntRec_t recs[MAX_CLIENTS];
static huntSoldier_t hunts[MAX_CLIENTS];
static int huntSerial;
static int huntBudgetTime;
static int huntBudgetUsed;

// player hull, same box the movement code uses
static vec3_t huntMins = { -18, -18, -24 };
static vec3_t huntMaxs = { 18, 18, 48 };

//===========================================================================
// small helpers
//===========================================================================

static const char *HuntName( cast_state_t *cs ) {
	char *name = g_entities[cs->entityNum].aiName;

	return name ? name : "?";
}

static int HuntRandMs( float lo, float hi ) {
	return (int)( lo + random() * ( hi - lo ) );
}

// AAS queries per server frame: the route-first-visible and area-travel calls are the expensive ones
static qboolean HuntBudget( void ) {
	if ( huntBudgetTime != level.time ) {
		huntBudgetTime = level.time;
		huntBudgetUsed = 0;
	}
	if ( huntBudgetUsed >= 2 ) {
		return qfalse;
	}
	huntBudgetUsed++;
	return qtrue;
}

static huntRec_t *HuntRec( int enemy ) {
	huntRec_t *rec = &recs[enemy];

	if ( rec->levelStart != level.startTime || rec->lastUse > level.time || rec->lastUse < level.time - HUNT_REC_STALE_MS ) {
		// keep the sighting track across a quiet spell: only the hunt itself goes stale
		if ( rec->levelStart == level.startTime && rec->lastUse <= level.time ) {
			rec->active = qfalse;
		} else {
			memset( rec, 0, sizeof( *rec ) );
			rec->levelStart = level.startTime;
		}
	}
	rec->lastUse = level.time;
	return rec;
}

static huntSoldier_t *HuntSoldier( int n ) {
	huntSoldier_t *hs = &hunts[n];

	if ( hs->levelStart != level.startTime || hs->lastThink > level.time ) {
		memset( hs, 0, sizeof( *hs ) );
		hs->levelStart = level.startTime;
	}
	return hs;
}

static float HuntDist( cast_state_t *cs, vec3_t pos ) {
	vec3_t v;

	VectorSubtract( pos, cs->bs->origin, v );
	v[2] *= 0.2f;       // same weighting the cover code uses
	return VectorLength( v );
}

static float HuntFlatDist( vec3_t a, vec3_t b ) {
	vec3_t v;

	VectorSubtract( a, b, v );
	v[2] = 0;
	return VectorLength( v );
}

// turn the view to a point (at standing eye height above it), pitch limited
static void HuntAimAt( cast_state_t *cs, vec3_t pt ) {
	vec3_t eye, dir, p;

	VectorCopy( cs->bs->origin, eye );
	eye[2] += cs->bs->cur_ps.viewheight;
	VectorCopy( pt, p );
	p[2] += DEFAULT_VIEWHEIGHT;
	VectorSubtract( p, eye, dir );
	vectoangles( dir, cs->ideal_viewangles );
	if ( cs->ideal_viewangles[PITCH] > 180 ) {
		cs->ideal_viewangles[PITCH] -= 360;
	}
	if ( cs->ideal_viewangles[PITCH] > 35 ) {
		cs->ideal_viewangles[PITCH] = 35;
	} else if ( cs->ideal_viewangles[PITCH] < -35 ) {
		cs->ideal_viewangles[PITCH] = -35;
	}
}

static void HuntSetCrouch( cast_state_t *cs, huntSoldier_t *hs, qboolean on ) {
	if ( on && AICast_CodCanCrouch( cs ) ) {
		hs->crouchUntil = level.time + 300;
		cs->attackcrouch_time = hs->crouchUntil;
	} else {
		hs->crouchUntil = 0;
		cs->attackcrouch_time = 0;
	}
}

// the rate-limited variant for lines printed from a per-think path
static qboolean HuntDbgOk( huntSoldier_t *hs ) {
	if ( bg_cod.aiDebug < 1 || level.time < hs->nextDbg ) {
		return qfalse;
	}
	hs->nextDbg = level.time + HUNT_DBG_MS;
	return qtrue;
}

//===========================================================================
// participants and hop slots
//===========================================================================

static huntPart_t *HuntPartOf( huntRec_t *rec, int ent ) {
	int i, free = -1;

	for ( i = 0; i < HUNT_MAX_PART; i++ ) {
		if ( rec->part[i].ent == ent && rec->part[i].time ) {
			return &rec->part[i];
		}
		if ( free < 0 && ( !rec->part[i].time || rec->part[i].time < level.time - HUNT_STALE_MS ) ) {
			free = i;
		}
	}
	if ( free < 0 ) {
		return NULL;
	}
	memset( &rec->part[free], 0, sizeof( huntPart_t ) );
	rec->part[free].ent = ent;
	return &rec->part[free];
}

static int HuntPartCount( huntRec_t *rec ) {
	int i, num = 0;

	for ( i = 0; i < HUNT_MAX_PART; i++ ) {
		if ( rec->part[i].time && rec->part[i].time >= level.time - HUNT_STALE_MS && rec->part[i].time <= level.time ) {
			num++;
		}
	}
	return num;
}

static int HuntSlotOf( huntRec_t *rec, int ent ) {
	int i;

	for ( i = 0; i < 2; i++ ) {
		if ( rec->hopEnt[i] == ent && rec->hopUntil[i] > level.time ) {
			return i;
		}
	}
	return -1;
}

static void HuntSlotRelease( huntRec_t *rec, int ent ) {
	int i;

	for ( i = 0; i < 2; i++ ) {
		if ( rec->hopEnt[i] == ent ) {
			rec->hopUntil[i] = 0;
		}
	}
}

/*
============
HuntSlotAcquire

at most two soldiers move at a time (one for a squad of up to three) and always at least one stays behind to
cover; of the soldiers whose hold time is over the one that has been holding longest goes first
============
*/
static qboolean HuntSlotAcquire( huntRec_t *rec, int ent, int leaseMs, int numPart ) {
	int i, used = 0, max, free = -1;
	huntPart_t *me = HuntPartOf( rec, ent );

	i = HuntSlotOf( rec, ent );
	if ( i >= 0 ) {
		rec->hopUntil[i] = level.time + leaseMs;
		return qtrue;
	}
	max = ( numPart >= 4 ) ? 2 : 1;
	if ( numPart >= 2 && max > numPart - 1 ) {
		max = numPart - 1;
	}
	for ( i = 0; i < 2; i++ ) {
		if ( rec->hopUntil[i] > level.time ) {
			used++;
		} else if ( free < 0 ) {
			free = i;
		}
	}
	if ( used >= max || free < 0 ) {
		return qfalse;
	}
	if ( me ) {
		for ( i = 0; i < HUNT_MAX_PART; i++ ) {
			huntPart_t *p = &rec->part[i];

			if ( p == me || !p->time || p->time < level.time - HUNT_STALE_MS || !p->ready || !p->waitSince ) {
				continue;
			}
			if ( p->waitSince < me->waitSince ) {
				return qfalse;      // somebody has been waiting longer
			}
		}
	}
	rec->hopEnt[free] = ent;
	rec->hopUntil[free] = level.time + leaseMs;
	return qtrue;
}

//===========================================================================
// the enemy's track and the hunt target
//===========================================================================

/*
============
AICast_CodHuntTrack

codmod: Phase 4. A soldier of the tactical combat sees the enemy: remember where and how fast it moved, and end
the hunt of everybody (the next soldier that loses the enemy starts a new one from this sighting).
============
*/
void AICast_CodHuntTrack( cast_state_t *cs ) {
	huntRec_t *rec;
	gentity_t *enemy;

	if ( !bg_cod.aiHunt || cs->enemyNum < 0 || cs->enemyNum >= MAX_CLIENTS ) {
		return;
	}
	enemy = &g_entities[cs->enemyNum];
	if ( !enemy->inuse || !enemy->client ) {
		return;
	}
	rec = HuntRec( cs->enemyNum );
	rec->seenTime = level.time;
	VectorCopy( enemy->r.currentOrigin, rec->seenPos );
	VectorCopy( enemy->client->ps.velocity, rec->seenVel );
	rec->seenVel[2] = 0;
	rec->active = qfalse;
}

/*
============
HuntStart

the target: where the enemy was last seen plus its last velocity times the time since (at most
cod_aiHuntPredict away, clamped by a hull trace so it never lies inside a wall, dropped to the floor). qfalse when
nothing usable is known (never seen or heard, or no AAS area at the spot).
============
*/
static qboolean HuntStart( huntRec_t *rec, cast_state_t *cs ) {
	cast_visibility_t *vis = &cs->vislist[cs->enemyNum];
	vec3_t off, wanted, pos, floor;
	trace_t tr;
	float len, max;
	int dt, area;

	// the best sighting we have: ours (or a squad mate's, via the track) or what we only heard
	if ( vis->real_visible_timestamp > rec->seenTime ) {
		rec->seenTime = vis->real_visible_timestamp;
		VectorCopy( vis->real_visible_pos, rec->seenPos );
		VectorClear( rec->seenVel );
	}
	if ( !rec->seenTime ) {
		if ( VectorCompare( vis->visible_pos, vec3_origin ) ) {
			return qfalse;
		}
		rec->seenTime = level.time;
		VectorCopy( vis->visible_pos, rec->seenPos );
		VectorClear( rec->seenVel );
	}

	dt = level.time - rec->seenTime;
	if ( dt < 0 ) {
		dt = 0;
	} else if ( dt > 3000 ) {
		dt = 3000;
	}
	VectorScale( rec->seenVel, dt * 0.001f, off );
	off[2] = 0;
	len = VectorLength( off );
	max = bg_cod.aiHuntPredict;
	if ( max < 0 ) {
		max = 0;
	} else if ( max > 512 ) {
		max = 512;
	}
	if ( len > max && len > 0.001f ) {
		VectorScale( off, max / len, off );
	}
	VectorAdd( rec->seenPos, off, wanted );
	trap_Trace( &tr, rec->seenPos, huntMins, huntMaxs, wanted, ENTITYNUM_NONE, MASK_PLAYERSOLID );
	if ( tr.startsolid || tr.allsolid ) {
		VectorCopy( rec->seenPos, pos );
	} else {
		VectorCopy( tr.endpos, pos );
	}
	if ( !AICast_CodDropToFloor( pos, floor ) ) {
		VectorCopy( rec->seenPos, floor );
	}
	area = BotPointAreaNum( floor );
	if ( !area ) {
		VectorCopy( rec->seenPos, floor );
		area = BotPointAreaNum( floor );
		if ( !area ) {
			return qfalse;
		}
	}

	rec->active = qtrue;
	rec->serial = ++huntSerial;
	rec->phase = HP_HUNT;
	rec->start = level.time;
	rec->doneTime = 0;
	VectorCopy( floor, rec->target );
	rec->targetArea = area;
	rec->numSweep = 0;
	rec->hopEnt[0] = rec->hopEnt[1] = -1;
	rec->hopUntil[0] = rec->hopUntil[1] = 0;
	memset( rec->part, 0, sizeof( rec->part ) );

	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s sq%d/HUNT start: last seen %d ms ago, target %d u from the sighting, %d u from me\n", HuntName( cs ),
				  AICast_CodSquadId( cs ), level.time - rec->seenTime, (int)Distance( rec->target, rec->seenPos ),
				  (int)Distance( rec->target, cs->bs->origin ) );
	}
	AICast_CodSquadBark( cs, COD_BARK_ORDERS );
	return qtrue;
}

//===========================================================================
// sweep points
//===========================================================================

/*
============
HuntBeginSweep

the target was reached (or the hunt ran out of time): pick up to HUNT_MAX_SWEEP spots in AAS areas the player could
have reached from where it was last seen. Candidates are ring points around the sighting, kept when the travel time
from the sighting's area is positive and under about 0.4 s per 100 u of cod_aiSearchRadius, then chosen nearest
first with at least 200 u between any two.
============
*/
static void HuntBeginSweep( huntRec_t *rec, cast_state_t *cs, const char *why ) {
	struct {
		vec3_t pos;
		int area;
		int travel;
	} cands[48];
	int numCands = 0, i, j, k, startArea, numPart, want, cap;
	float radius, ang, base, r;
	vec3_t pt, floor, dir;

	rec->phase = HP_SWEEP;
	rec->sweepStart = level.time;
	rec->numSweep = 0;
	numPart = HuntPartCount( rec );
	want = numPart + 1;
	if ( want < 2 ) {
		want = 2;
	} else if ( want > HUNT_MAX_SWEEP ) {
		want = HUNT_MAX_SWEEP;
	}

	radius = bg_cod.aiSearchRadius;
	if ( radius < 200 ) {
		radius = 200;
	} else if ( radius > 1500 ) {
		radius = 1500;
	}
	cap = (int)( radius * 0.4f );

	startArea = BotPointAreaNum( rec->seenPos );
	if ( !startArea ) {
		startArea = rec->targetArea;
	}
	base = random() * 36.0f;
	for ( k = 0; k < 4 && numCands < 48; k++ ) {
		r = radius * ( 0.25f + 0.25f * k );
		for ( i = 0; i < 10 && numCands < 48; i++ ) {
			int area, travel;

			ang = DEG2RAD( base + i * 36.0f );
			VectorSet( dir, cos( ang ), sin( ang ), 0 );
			VectorMA( rec->seenPos, r, dir, pt );
			if ( !AICast_CodDropToFloor( pt, floor ) ) {
				continue;
			}
			area = BotPointAreaNum( floor );
			if ( !area ) {
				continue;
			}
			for ( j = 0; j < numCands; j++ ) {
				if ( cands[j].area == area ) {
					break;
				}
			}
			if ( j < numCands ) {
				continue;
			}
			travel = trap_AAS_AreaTravelTimeToGoalArea( startArea, rec->seenPos, area, cs->travelflags );
			if ( travel <= 0 || travel > cap ) {
				continue;
			}
			VectorCopy( floor, cands[numCands].pos );
			cands[numCands].area = area;
			cands[numCands].travel = travel;
			numCands++;
		}
	}

	// nearest by travel time first, at least 200 u apart (and not on top of the target)
	while ( rec->numSweep < want ) {
		int best = -1;

		for ( i = 0; i < numCands; i++ ) {
			if ( cands[i].area < 0 ) {
				continue;
			}
			if ( best < 0 || cands[i].travel < cands[best].travel ) {
				best = i;
			}
		}
		if ( best < 0 ) {
			break;
		}
		for ( j = 0; j < rec->numSweep; j++ ) {
			if ( Distance( rec->sweepPos[j], cands[best].pos ) < 200.0f ) {
				break;
			}
		}
		if ( j == rec->numSweep && Distance( rec->target, cands[best].pos ) >= 120.0f ) {
			VectorCopy( cands[best].pos, rec->sweepPos[rec->numSweep] );
			rec->sweepOwner[rec->numSweep] = -1;
			rec->sweepClaim[rec->numSweep] = 0;
			rec->sweepDone[rec->numSweep] = qfalse;
			rec->numSweep++;
		}
		cands[best].area = -1;
	}

	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s sq%d/SEARCH sweep starts (%s): %d points of %d candidates, %d hunters, %d ms to look\n", HuntName( cs ),
				  AICast_CodSquadId( cs ), why, rec->numSweep, numCands, numPart, (int)bg_cod.aiSweepTime );
	}
	AICast_CodSquadBark( cs, COD_BARK_ORDERS );
}

//===========================================================================
// leaving
//===========================================================================

static void HuntForget( cast_state_t *cs, huntSoldier_t *hs ) {
	int i;

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( recs[i].levelStart == level.startTime ) {
			int j;

			HuntSlotRelease( &recs[i], cs->entityNum );
			for ( j = 0; j < HUNT_MAX_SWEEP; j++ ) {
				if ( recs[i].sweepOwner[j] == cs->entityNum && !recs[i].sweepDone[j] ) {
					recs[i].sweepOwner[j] = -1;
				}
			}
		}
	}
	if ( hs->hasCover ) {
		AICast_CodReleaseCover( cs->entityNum );
	}
	if ( hs->crouchUntil ) {
		cs->attackcrouch_time = 0;
	}
	hs->hasCover = qfalse;
	hs->crouchUntil = 0;
	hs->mode = HM_NONE;
	hs->clearing = qfalse;
}

/*
============
AICast_CodHuntLeave

codmod: Phase 4. Contact (or any other reason to stop hunting): drop the slot, the cover claim and the crouch.
Cheap when the soldier was not hunting.
============
*/
void AICast_CodHuntLeave( cast_state_t *cs ) {
	huntSoldier_t *hs;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS ) {
		return;
	}
	hs = &hunts[cs->entityNum];
	if ( hs->levelStart != level.startTime || hs->mode == HM_NONE ) {
		return;
	}
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s sq%d/%s HUNT over: contact again after %d ms\n", HuntName( cs ), AICast_CodSquadId( cs ),
				  huntModeNames[hs->mode], level.time - hs->modeTime );
	}
	HuntForget( cs, hs );
}

void AICast_CodHuntReset( int entNum ) {
	if ( entNum < 0 || entNum >= MAX_CLIENTS ) {
		return;
	}
	memset( &hunts[entNum], 0, sizeof( hunts[entNum] ) );
	hunts[entNum].levelStart = level.startTime;
}

qboolean AICast_CodHuntCrouch( cast_state_t *cs ) {
	huntSoldier_t *hs;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS ) {
		return qfalse;
	}
	hs = &hunts[cs->entityNum];
	if ( hs->levelStart != level.startTime || hs->mode == HM_NONE || hs->lastThink < level.time - 300 ) {
		return qfalse;
	}
	return ( hs->crouchUntil >= level.time ) ? qtrue : qfalse;
}

const char *AICast_CodHuntModeName( cast_state_t *cs ) {
	huntSoldier_t *hs;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS ) {
		return "NONE";
	}
	hs = &hunts[cs->entityNum];
	return ( hs->levelStart == level.startTime ) ? huntModeNames[hs->mode] : "NONE";
}

/*
============
AICast_CodAlertActive

codmod: Phase 4. qtrue while this soldier hunts or is on heightened alert after a hunt: AICast_EntityVisible halves
its reaction time.
============
*/
qboolean AICast_CodAlertActive( cast_state_t *cs ) {
	huntSoldier_t *hs;

	if ( !bg_cod.enable || !bg_cod.aiTactics || !bg_cod.aiHunt || cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS ) {
		return qfalse;
	}
	hs = &hunts[cs->entityNum];
	if ( hs->levelStart != level.startTime ) {
		return qfalse;
	}
	if ( hs->mode != HM_NONE && hs->lastThink <= level.time && hs->lastThink >= level.time - 600 ) {
		return qtrue;
	}
	return ( hs->alertUntil > level.time && hs->alertUntil <= level.time + (int)( bg_cod.aiAlertTime * 1.5f ) + 1000 ) ? qtrue : qfalse;
}

/*
============
HuntAlert

the hunt found nothing: forget the chase (enemy and last enemy), go to the vanilla idle, which drops the aiState to
ALERT, and react twice as fast for cod_aiAlertTime
============
*/
static int HuntAlert( cast_state_t *cs, huntRec_t *rec, huntSoldier_t *hs, char **rval ) {
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s sq%d/ALERT hunt over after %d ms: heightened alert for %d ms\n", HuntName( cs ), AICast_CodSquadId( cs ),
				  level.time - rec->start, (int)bg_cod.aiAlertTime );
	}
	HuntForget( cs, hs );
	hs->alertUntil = level.time + (int)( bg_cod.aiAlertTime * AICast_CodSkill( COD_SK_HUNT ) );
	cs->enemyNum = -1;
	cs->lastEnemy = -1;
	*rval = AIFunc_DefaultStart( cs );
	return COD_HUNT_ALERT;
}

//===========================================================================
// aim
//===========================================================================

/*
============
HuntUpdateAim

where this soldier looks. The first point on the route from the target to the soldier that the soldier can see is the
corner / door / corridor mouth the player would come around; a holder alternates it with the last seen point.
Recomputed about twice a second (AAS budget).
============
*/
static void HuntUpdateAim( cast_state_t *cs, huntRec_t *rec, huntSoldier_t *hs, qboolean moving ) {
	vec3_t vp;

	if ( level.time < hs->nextAim ) {
		return;
	}
	if ( !HuntBudget() ) {
		return;
	}
	hs->nextAim = level.time + 450;

	if ( trap_AAS_GetRouteFirstVisPos( rec->target, cs->bs->origin, cs->travelflags, vp )
		 && HuntFlatDist( vp, cs->bs->origin ) > 70.0f && Distance( vp, rec->target ) > 96.0f ) {
		VectorCopy( vp, hs->aim );
		hs->aimKind = 1;
	} else {
		VectorCopy( rec->target, hs->aim );
		hs->aimKind = 0;
	}
	if ( !moving && ( cs->entityNum & 1 ) ) {
		VectorCopy( rec->seenPos, hs->aim );
		hs->aimKind = 2;
	}
}

//===========================================================================
// HUNT phase
//===========================================================================

static void HuntEnterHold( cast_state_t *cs, huntRec_t *rec, huntSoldier_t *hs, int holdMs ) {
	hs->mode = HM_HOLD;
	hs->modeTime = level.time;
	hs->holdSince = level.time;
	hs->holdUntil = level.time + holdMs;
	hs->clearing = qfalse;
	hs->nextAim = 0;
	HuntUpdateAim( cs, rec, hs, qfalse );
	HuntSetCrouch( cs, hs, ( hs->hasCover && hs->cover.type == COVER_LOW ) ? qtrue : qfalse );
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s sq%d/HOLD angle -> %s d=%d for %d ms\n", HuntName( cs ), AICast_CodSquadId( cs ),
				  hs->aimKind == 1 ? "corridor" : ( hs->aimKind == 2 ? "last seen point" : "target" ),
				  (int)Distance( hs->aim, cs->bs->origin ), holdMs );
	}
}

static void HuntEndHop( cast_state_t *cs, huntRec_t *rec, huntSoldier_t *hs, qboolean arrived, const char *why ) {
	HuntSlotRelease( rec, cs->entityNum );
	if ( !hs->goalIsTarget ) {
		// keep the claim of the spot we stand on; a failed one is released
		if ( !arrived && hs->hasCover ) {
			AICast_CodReleaseCover( cs->entityNum );
			hs->hasCover = qfalse;
		}
	} else if ( hs->hasCover ) {
		AICast_CodReleaseCover( cs->entityNum );
		hs->hasCover = qfalse;
	}
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s sq%d/HOP end (%s) after %d ms, d=%d from the target\n", HuntName( cs ), AICast_CodSquadId( cs ), why,
				  level.time - hs->modeTime, (int)Distance( cs->bs->origin, rec->target ) );
	}
	if ( rec->phase == HP_HUNT && HuntFlatDist( cs->bs->origin, rec->target ) <= HUNT_REACH_DIST * 1.5f ) {
		HuntBeginSweep( rec, cs, "target reached" );
		return;
	}
	HuntEnterHold( cs, rec, hs, (int)( bg_cod.aiHoldTime * ( 0.8f + 0.5f * random() ) ) );
}

/*
============
HuntPlanHop

choose where this soldier goes next: a COVER_ADVANCE spot against the target when one exists (cover to cover), else
straight along the AAS route to the target for a limited time. 1 when planned, 0 when the target cannot be reached, -1 to try again (AAS / cover search budget).
============
*/
static int HuntPlanHop( cast_state_t *cs, huntRec_t *rec, huntSoldier_t *hs ) {
	int area, travel, timeout;
	float dist;

	dist = Distance( cs->bs->origin, rec->target );
	area = cs->bs->areanum;
	if ( !area ) {
		area = BotPointAreaNum( cs->bs->origin );
	}
	if ( !area ) {
		return 0;
	}
	if ( !HuntBudget() ) {
		return -1;
	}
	travel = trap_AAS_AreaTravelTimeToGoalArea( area, cs->bs->origin, rec->targetArea, cs->travelflags );
	if ( travel <= 0 && area != rec->targetArea ) {
		return 0;           // no route
	}

	hs->hasCover = qfalse;
	hs->goalIsTarget = qfalse;
	if ( dist > 320.0f ) {
		vec3_t avoid;
		qboolean haveAvoid = qfalse;

		if ( !AICast_CodCoverSearchAllowed( qfalse ) ) {
			return -1;
		}

		if ( hs->cover.pos[0] || hs->cover.pos[1] || hs->cover.pos[2] ) {
			VectorCopy( hs->cover.pos, avoid );
			haveAvoid = qtrue;      // not the spot it just came from
		}
		if ( AICast_CodFindCover( cs, rec->target, cs->enemyNum, COVER_ADVANCE, &hs->cover, haveAvoid ? avoid : NULL, NULL ) ) {
			hs->hasCover = qtrue;
			VectorCopy( hs->cover.pos, hs->goal );
			AICast_CodClaimCover( cs->entityNum, hs->cover.pos );
			timeout = hs->cover.travel * 20 + 1500;
			if ( timeout < 2500 ) {
				timeout = 2500;
			} else if ( timeout > 9000 ) {
				timeout = 9000;
			}
			hs->hopTimeout = level.time + timeout;
			hs->mode = HM_HOP;
			hs->modeTime = level.time;
			hs->stuck = 0;
			VectorCopy( cs->bs->origin, hs->lastPos );
			hs->lastPosTime = level.time;
			if ( bg_cod.aiDebug >= 1 ) {
				G_Printf( "[ai] %s sq%d/HUNT hop -> cover %s d=%d travel=%d\n", HuntName( cs ), AICast_CodSquadId( cs ),
						  AICast_CodCoverName( hs->cover.type ), (int)Distance( hs->cover.pos, rec->target ), hs->cover.travel );
			}
			return 1;
		}
	}
	// no cover to hop to: run for the target, but only for a few seconds when it is far (the next one covers us)
	VectorCopy( rec->target, hs->goal );
	hs->goalIsTarget = qtrue;
	hs->hopTimeout = level.time + ( dist > 400.0f ? 4500 : 7000 );
	hs->mode = HM_HOP;
	hs->modeTime = level.time;
	hs->stuck = 0;
	VectorCopy( cs->bs->origin, hs->lastPos );
	hs->lastPosTime = level.time;
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s sq%d/HUNT hop -> target d=%d (no cover hop)\n", HuntName( cs ), AICast_CodSquadId( cs ), (int)dist );
	}
	return 1;
}

static void HuntStepHold( cast_state_t *cs, huntRec_t *rec, huntSoldier_t *hs, huntPart_t *part, int numPart ) {
	int plan;

	HuntUpdateAim( cs, rec, hs, qfalse );
	HuntAimAt( cs, hs->aim );
	HuntSetCrouch( cs, hs, ( hs->hasCover && hs->cover.type == COVER_LOW ) ? qtrue : qfalse );

	if ( part ) {
		part->ready = qfalse;
		part->waitSince = hs->holdSince;
	}
	if ( level.time < hs->holdUntil || hs->noHop ) {
		return;
	}
	if ( part ) {
		part->ready = qtrue;
	}
	if ( !HuntSlotAcquire( rec, cs->entityNum, 3000, numPart ) ) {
		return;
	}
	plan = HuntPlanHop( cs, rec, hs );
	if ( plan <= 0 ) {
		HuntSlotRelease( rec, cs->entityNum );
		if ( plan == 0 ) {
			hs->fails++;
			hs->holdUntil = level.time + 1500;
			if ( hs->fails >= 2 ) {
				hs->noHop = qtrue;
			}
			if ( part ) {
				part->ready = qfalse;
			}
			if ( HuntDbgOk( hs ) ) {
				G_Printf( "[ai] %s sq%d/HOLD HUNT no route to the target (fail %d)\n", HuntName( cs ), AICast_CodSquadId( cs ), hs->fails );
			}
		}
		return;
	}
	if ( part ) {
		part->ready = qfalse;
		part->waitSince = 0;
	}
	if ( numPart >= 2 ) {
		AICast_CodSquadBark( cs, COD_BARK_ATTACK );
	}
}

static void HuntStepHop( cast_state_t *cs, huntRec_t *rec, huntSoldier_t *hs ) {
	bot_moveresult_t *mr;
	float dist, cornerDist;
	qboolean arrived, crouch;

	HuntUpdateAim( cs, rec, hs, qtrue );
	// the lease is renewed every think, so a soldier that dies or leaves frees its slot within 1.5 s
	if ( !HuntSlotAcquire( rec, cs->entityNum, 1500, HuntPartCount( rec ) ) ) {
		HuntEndHop( cs, rec, hs, qfalse, "slot lost" );
		return;
	}
	if ( hs->goalIsTarget ) {
		dist = HuntFlatDist( cs->bs->origin, hs->goal );
		arrived = ( dist <= HUNT_REACH_DIST ) ? qtrue : qfalse;
	} else {
		dist = HuntDist( cs, hs->goal );
		arrived = ( dist <= HUNT_COVER_DIST ) ? qtrue : qfalse;
	}
	if ( arrived ) {
		HuntEndHop( cs, rec, hs, qtrue, "arrived" );
		return;
	}
	if ( level.time > hs->hopTimeout ) {
		HuntEndHop( cs, rec, hs, qfalse, "timeout" );
		return;
	}
	// not getting anywhere
	if ( level.time >= hs->lastPosTime + 1000 ) {
		if ( Distance( cs->bs->origin, hs->lastPos ) < 24.0f ) {
			hs->stuck++;
		} else {
			hs->stuck = 0;
		}
		VectorCopy( cs->bs->origin, hs->lastPos );
		hs->lastPosTime = level.time;
		if ( hs->stuck >= 2 ) {
			hs->fails++;
			HuntEndHop( cs, rec, hs, qfalse, "stuck" );
			return;
		}
	}

	// corner clearing: the aim point is the corner / door edge; closing in on it (or on the target) the soldier
	// slows down and crouch-walks, keeping the view on the edge while the path takes it around
	cornerDist = ( hs->aimKind == 1 ) ? HuntFlatDist( hs->aim, cs->bs->origin ) : 99999.0f;
	crouch = ( ( cornerDist < 190.0f ) || ( hs->goalIsTarget && HuntFlatDist( rec->target, cs->bs->origin ) < 170.0f ) ) ? qtrue : qfalse;
	if ( crouch && !hs->clearing && AICast_CodCanCrouch( cs ) ) {
		hs->clearing = qtrue;
		if ( bg_cod.aiDebug >= 1 && HuntDbgOk( hs ) ) {
			G_Printf( "[ai] %s sq%d/HOP CLEAR corner d=%d, crouch-walking the last stretch\n", HuntName( cs ), AICast_CodSquadId( cs ),
					  cornerDist < 99999.0f ? (int)cornerDist : (int)HuntFlatDist( rec->target, cs->bs->origin ) );
		}
	} else if ( !crouch ) {
		hs->clearing = qfalse;
	}
	HuntSetCrouch( cs, hs, hs->clearing );

	mr = AICast_MoveToPos( cs, hs->goal, -1 );
	if ( mr ) {
		if ( mr->failure || mr->blocked ) {
			trap_BotResetAvoidReach( cs->bs->ms );
			hs->fails++;
			HuntEndHop( cs, rec, hs, qfalse, mr->failure ? "no path" : "blocked" );
			return;
		}
	}
	if ( hs->clearing ) {
		cs->speedScale = 0.55f;
	} else {
		cs->speedScale = 1.0f;
		if ( dist > 250.0f ) {
			AICast_CodTrySprint( cs );
		}
	}
	// the view stays on the corner / target unless the movement code needs it (ladders, swimming)
	if ( !mr || !( mr->flags & ( MOVERESULT_MOVEMENTVIEW | MOVERESULT_SWIMVIEW ) ) ) {
		HuntAimAt( cs, hs->aim );
	}
}

//===========================================================================
// SWEEP phase
//===========================================================================

static void HuntEnterLook( cast_state_t *cs, huntSoldier_t *hs, int ms ) {
	hs->mode = HM_LOOK;
	hs->modeTime = level.time;
	hs->lookUntil = level.time + ms;
	hs->lookBase = cs->viewangles[YAW];
	HuntSetCrouch( cs, hs, qfalse );
}

// the nearest sweep point nobody else is walking to
static qboolean HuntPickSweep( cast_state_t *cs, huntRec_t *rec, huntSoldier_t *hs ) {
	int i, best = -1;
	float d, bestD = 0;

	for ( i = 0; i < rec->numSweep; i++ ) {
		if ( rec->sweepDone[i] ) {
			continue;
		}
		if ( rec->sweepOwner[i] >= 0 && rec->sweepOwner[i] != cs->entityNum && rec->sweepClaim[i] > level.time - 9000 ) {
			continue;
		}
		d = Distance( cs->bs->origin, rec->sweepPos[i] );
		if ( best < 0 || d < bestD ) {
			best = i;
			bestD = d;
		}
	}
	if ( best < 0 ) {
		return qfalse;
	}
	rec->sweepOwner[best] = cs->entityNum;
	rec->sweepClaim[best] = level.time;
	hs->sweepIdx = best;
	VectorCopy( rec->sweepPos[best], hs->goal );
	hs->mode = HM_SWEEP;
	hs->modeTime = level.time;
	hs->hopTimeout = level.time + 8000;
	hs->stuck = 0;
	VectorCopy( cs->bs->origin, hs->lastPos );
	hs->lastPosTime = level.time;
	HuntSetCrouch( cs, hs, qfalse );
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s sq%d/SEARCH area %d/%d at d=%d\n", HuntName( cs ), AICast_CodSquadId( cs ), best + 1, rec->numSweep, (int)bestD );
	}
	return qtrue;
}

static void HuntStepSweep( cast_state_t *cs, huntRec_t *rec, huntSoldier_t *hs ) {
	bot_moveresult_t *mr;
	qboolean skip = qfalse;
	float ang;
	vec3_t dir;

	// the sweep starts: leave whatever the hunt phase was doing
	if ( hs->mode != HM_SWEEP && hs->mode != HM_LOOK ) {
		HuntSlotRelease( rec, cs->entityNum );
		if ( hs->hasCover ) {
			AICast_CodReleaseCover( cs->entityNum );
			hs->hasCover = qfalse;
		}
		hs->modeTime = level.time;
		hs->sweepIdx = -1;
		if ( !HuntPickSweep( cs, rec, hs ) ) {
			HuntEnterLook( cs, hs, 2500 );
		}
	}

	if ( hs->mode == HM_LOOK ) {
		// turn slowly left and right around the heading at arrival
		ang = sin( ( level.time - hs->modeTime ) * 0.0018f + cs->entityNum ) * 75.0f;
		VectorSet( dir, 0, AngleNormalize360( hs->lookBase + ang ), 0 );
		cs->ideal_viewangles[YAW] = dir[1];
		cs->ideal_viewangles[PITCH] = 0;
		cs->ideal_viewangles[ROLL] = 0;
		if ( level.time >= hs->lookUntil ) {
			if ( !HuntPickSweep( cs, rec, hs ) ) {
				HuntEnterLook( cs, hs, 2500 );
			}
		}
		return;
	}

	// SWEEP: walking to the point
	if ( HuntDist( cs, hs->goal ) <= HUNT_SWEEP_DIST ) {
		rec->sweepDone[hs->sweepIdx] = qtrue;
		if ( bg_cod.aiDebug >= 1 ) {
			G_Printf( "[ai] %s sq%d/SEARCH area %d/%d reached, looking around\n", HuntName( cs ), AICast_CodSquadId( cs ), hs->sweepIdx + 1, rec->numSweep );
		}
		HuntEnterLook( cs, hs, HuntRandMs( 2000, 3000 ) );
		return;
	}
	if ( level.time > hs->hopTimeout ) {
		skip = qtrue;
	}
	if ( !skip && level.time >= hs->lastPosTime + 1000 ) {
		if ( Distance( cs->bs->origin, hs->lastPos ) < 24.0f ) {
			hs->stuck++;
		} else {
			hs->stuck = 0;
		}
		VectorCopy( cs->bs->origin, hs->lastPos );
		hs->lastPosTime = level.time;
		if ( hs->stuck >= 2 ) {
			skip = qtrue;
		}
	}
	mr = skip ? NULL : AICast_MoveToPos( cs, hs->goal, -1 );
	if ( mr && ( mr->failure || mr->blocked ) ) {
		trap_BotResetAvoidReach( cs->bs->ms );
		skip = qtrue;
	}
	if ( skip ) {
		// cannot get there: counts as visited, take the next one
		rec->sweepDone[hs->sweepIdx] = qtrue;
		hs->fails++;
		if ( bg_cod.aiDebug >= 1 ) {
			G_Printf( "[ai] %s sq%d/SEARCH area %d/%d unreachable, skipped\n", HuntName( cs ), AICast_CodSquadId( cs ), hs->sweepIdx + 1, rec->numSweep );
		}
		if ( !HuntPickSweep( cs, rec, hs ) ) {
			HuntEnterLook( cs, hs, 2500 );
		}
		return;
	}
	cs->speedScale = 0.8f;      // searching, not charging
}

//===========================================================================
// AICast_CodHuntThink
//===========================================================================

/*
============
AICast_CodHuntThink

codmod: Phase 4. One think of a soldier that lost its enemy (CodEligibility said LOST); called from
AIFunc_CodCombat after it dropped its cover state. Returns COD_HUNT_BUSY (this think is done), COD_HUNT_ALERT
(*rval is the vanilla function the soldier now runs) or COD_HUNT_VANILLA (nothing to hunt: the caller does the old
chase hand-over, with its back-off).
============
*/
int AICast_CodHuntThink( cast_state_t *cs, char **rval ) {
	huntRec_t *rec;
	huntSoldier_t *hs;
	huntPart_t *part;
	int n = cs->entityNum, numPart;

	*rval = NULL;
	if ( !bg_cod.enable || !bg_cod.aiTactics || !bg_cod.aiHunt ) {
		return COD_HUNT_VANILLA;
	}
	if ( n < 0 || n >= MAX_CLIENTS || cs->enemyNum < 0 || cs->enemyNum >= MAX_CLIENTS ) {
		return COD_HUNT_VANILLA;
	}
	hs = HuntSoldier( n );
	rec = HuntRec( cs->enemyNum );

	if ( !rec->active || ( rec->phase == HP_DONE && level.time - rec->doneTime > HUNT_DONE_KEEP_MS ) ) {
		if ( !HuntStart( rec, cs ) ) {
			if ( bg_cod.aiDebug >= 1 ) {
				G_Printf( "[ai] %s HUNT impossible (no known position), vanilla chase\n", HuntName( cs ) );
			}
			return COD_HUNT_VANILLA;
		}
	}

	// phase changes (the first soldier to notice does them)
	if ( rec->phase == HP_HUNT && level.time - rec->start > (int)( bg_cod.aiHuntTime * AICast_CodSkill( COD_SK_HUNT ) ) ) {
		HuntBeginSweep( rec, cs, "hunt time over" );
	}
	if ( rec->phase == HP_SWEEP && level.time - rec->sweepStart > (int)( bg_cod.aiSweepTime * AICast_CodSkill( COD_SK_HUNT ) ) ) {
		rec->phase = HP_DONE;
		rec->doneTime = level.time;
	}
	if ( rec->phase == HP_DONE ) {
		return HuntAlert( cs, rec, hs, rval );
	}

	// a soldier that comes back after an interruption (grenade dodge, a dive) starts its part again
	if ( hs->serial != rec->serial || hs->lastThink < level.time - HUNT_STALE_MS ) {
		int keepAlert = hs->alertUntil;

		if ( hs->mode != HM_NONE ) {
			HuntForget( cs, hs );
		}
		memset( hs, 0, sizeof( *hs ) );
		hs->levelStart = level.startTime;
		hs->alertUntil = keepAlert;
		hs->serial = rec->serial;
		hs->sweepIdx = -1;
		hs->modeTime = level.time;
	}
	hs->lastThink = level.time;

	part = HuntPartOf( rec, n );
	if ( part ) {
		part->time = level.time;
	}
	numPart = HuntPartCount( rec );
	AICast_CodSquadPublish( cs, ( hs->mode == HM_HOLD ) ? qtrue : qfalse );

	if ( hs->fails >= HUNT_MAX_FAILS && numPart <= 1 ) {
		// a lone soldier that cannot get anywhere: the vanilla chase is the last resort, and the hunt is over for
		// good (back from the chase's back-off it goes to alert instead of starting over)
		HuntForget( cs, hs );
		rec->phase = HP_DONE;
		rec->doneTime = level.time;
		return COD_HUNT_VANILLA;
	}

	if ( rec->phase == HP_SWEEP ) {
		HuntStepSweep( cs, rec, hs );
		return COD_HUNT_BUSY;
	}

	if ( hs->mode == HM_NONE ) {
		HuntEnterHold( cs, rec, hs, HuntRandMs( 400, 900 ) + ( n % 4 ) * 350 );
	}
	if ( hs->mode == HM_HOP ) {
		HuntStepHop( cs, rec, hs );
	} else {
		HuntStepHold( cs, rec, hs, part, numPart );
	}
	return COD_HUNT_BUSY;
}
