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
// Name:			ai_cast_cod_cover.c
// Function:		codmod tactical AI -- cover finder and cover claims
// Tab Size:		4 (real tabs)
//
// docs/ai-overhaul-plan.md, Phase 1a. Pure game-side sampling, no map data needed:
//   1. candidates: wall-hug rays + ring samples (at most MAX_COVER_CANDS)
//   2. validate: floor, water, AAS area, not claimed by another soldier
//   3. classify against the threat's eye: LOW (crouch to hide, stand to shoot), CORNER_L/R (hidden, step
//      sideways to shoot) or FULL (hidden only)
//   4. AAS travel time (last, because routing caches are the expensive part)
//   5. score, best wins
// Nothing here is stored in cast_state_t; claims live in a static array and are validated against the owner's
// state on every use, so a savegame load simply starts with no claims.
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

#define MAX_COVER_CANDS     72          // array size; ordinary searches stop at COVER_CANDS_STD
#define COVER_CANDS_STD     48
#define COVER_WALL_RAYS     16
#define COVER_MIN_MOVE      48.0f       // candidates closer than this to the soldier are pointless
#define COVER_CLAIM_DIST    64.0f       // keep this far from other soldiers' claims and bodies
#define COVER_LOW_BLOCKER   96.0f       // low cover: the blocking surface must be this close to the spot

// what stops a bullet or a look, ignoring actors (bodies/corpses)
#define COVER_MASK          ( ( MASK_SHOT & ~( CONTENTS_BODY | CONTENTS_CORPSE ) ) | CONTENTS_AI_NOSIGHT )

// player hull, same box the movement code uses
static vec3_t coverMins = { -18, -18, -24 };
static vec3_t coverMaxs = { 18, 18, 48 };

typedef struct {
	vec3_t pos;
	vec3_t peekPos;
	int type;
	int area;
	int travel;
	qboolean crouchShot;        // Phase 2b (COVER_ANGLE): the crouched eye has the shot
} codCand_t;

// one claim per soldier (index = entity number)
static struct {
	qboolean active;
	vec3_t pos;
} codClaims[MAX_CLIENTS];

// search ration (global per server frame)
static int searchFrameTime;
static int searchCount;

int codCoverLastCandidates;
int codCoverLastValid;

/*
============
AICast_CodCoverName
============
*/
const char *AICast_CodCoverName( int type ) {
	switch ( type ) {
	case COVER_LOW:
		return "LOW";
	case COVER_CORNER_L:
		return "CORNER_L";
	case COVER_CORNER_R:
		return "CORNER_R";
	case COVER_FULL:
		return "FULL";
	case COVER_FIRE:
		return "FIRE";
	default:
		return "NONE";
	}
}

static int codCoverTravelCap;       // Phase 3: one-shot travel cap (1/100 s) for the next AICast_CodFindCover

/*
============
AICast_CodCoverSetTravelCap

codmod: Phase 3. The next AICast_CodFindCover only accepts spots reachable within 'travel' (1/100 s); consumed by
that call. A grenade dodge has the fuse time to work with, not the cover radius.
============
*/
void AICast_CodCoverSetTravelCap( int travel ) {
	codCoverTravelCap = travel;
}

/*
============
AICast_CodCoverSearchAllowed

codmod: global ration, cod_aiCoverSearchesPerFrame searches per server frame. An urgent search (hit, empty
magazine) is always allowed but still counts against the frame's budget. Call this only when a search follows.
============
*/
qboolean AICast_CodCoverSearchAllowed( qboolean urgent ) {
	int limit;

	if ( searchFrameTime != level.time ) {
		searchFrameTime = level.time;
		searchCount = 0;
	}
	limit = bg_cod.aiCoverSearches;
	if ( limit < 1 ) {
		limit = 1;
	}
	if ( searchCount >= limit && !urgent ) {
		return qfalse;
	}
	searchCount++;
	return qtrue;
}

/*
============
AICast_CodClaimCover / AICast_CodReleaseCover
============
*/
void AICast_CodClaimCover( int entNum, vec3_t pos ) {
	if ( entNum < 0 || entNum >= MAX_CLIENTS ) {
		return;
	}
	codClaims[entNum].active = qtrue;
	VectorCopy( pos, codClaims[entNum].pos );
}

void AICast_CodReleaseCover( int entNum ) {
	if ( entNum < 0 || entNum >= MAX_CLIENTS ) {
		return;
	}
	codClaims[entNum].active = qfalse;
}

/*
============
AICast_CodLineClear

codmod: qtrue when nothing that stops bullets or sight lies between the two points. Actors are ignored.
============
*/
qboolean AICast_CodLineClear( vec3_t from, vec3_t to ) {
	trace_t tr;

	trap_Trace( &tr, from, NULL, NULL, to, ENTITYNUM_NONE, COVER_MASK );
	return ( tr.fraction == 1.0f && !tr.startsolid && !tr.allsolid ) ? qtrue : qfalse;
}

/*
============
AICast_CodBodyPoints

codmod: Phase 2b. Points that matter on the enemy at position 'at' (its origin), using its CURRENT bbox so a
crouched or prone player gives low points: head (near the top of the box), centre and legs. Returns how many
points were written (3), 0 for a bad entity.
============
*/
int AICast_CodBodyPoints( int enemyNum, vec3_t at, vec3_t pts[3] ) {
	gentity_t *e;
	float mins2, maxs2;

	if ( enemyNum < 0 || enemyNum >= MAX_CLIENTS ) {
		return 0;
	}
	e = &g_entities[enemyNum];
	if ( !e->inuse || !e->client ) {
		return 0;
	}
	mins2 = e->r.mins[2];
	maxs2 = e->r.maxs[2];
	if ( maxs2 - mins2 < 16.0f ) {
		return 0;
	}
	VectorCopy( at, pts[0] );
	pts[0][2] += maxs2 - 6.0f;                      // head
	VectorCopy( at, pts[1] );
	pts[1][2] += ( mins2 + maxs2 ) * 0.5f;          // centre of mass
	VectorCopy( at, pts[2] );
	pts[2][2] += mins2 + 10.0f;                     // legs
	return 3;
}

/*
============
AICast_CodBodyShot

codmod: qtrue when a bullet from 'eye' reaches at least one of the body points (actors ignored)
============
*/
qboolean AICast_CodBodyShot( vec3_t eye, vec3_t pts[3], int num ) {
	int i;

	for ( i = 0; i < num; i++ ) {
		if ( AICast_CodLineClear( eye, pts[i] ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
============
CodWeaponBand

preferred fighting distance per weapon class
============
*/
static void CodWeaponBand( int weapon, float *lo, float *hi ) {
	switch ( weapon ) {
	case WP_MAUSER:
	case WP_GARAND:
		*lo = 400;
		*hi = 1500;
		break;
	case WP_LUGER:
	case WP_COLT:
	case WP_SILENCER:
	case WP_AKIMBO:
		*lo = 200;
		*hi = 700;
		break;
	default:    // SMGs
		*lo = 256;
		*hi = 900;
		break;
	}
}

/*
============
CodNearOtherSoldier

qtrue when pos is within COVER_CLAIM_DIST of another living soldier's claimed cover or of its body
============
*/
static qboolean CodNearOtherSoldier( int self, vec3_t pos ) {
	int i;
	gentity_t *other;

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( i == self ) {
			continue;
		}
		other = &g_entities[i];
		if ( !other->inuse || !other->client || other->health <= 0 ) {
			continue;
		}
		if ( !( other->r.svFlags & SVF_CASTAI ) ) {
			continue;
		}
		if ( codClaims[i].active && Distance( pos, codClaims[i].pos ) < COVER_CLAIM_DIST ) {
			return qtrue;
		}
		if ( Distance( pos, other->r.currentOrigin ) < COVER_CLAIM_DIST ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
============
CodDropToFloor

hull trace down from 'in'; qfalse when there is no walkable ground within 128 u, or it is wet
============
*/
static qboolean CodDropToFloor( vec3_t in, vec3_t out ) {
	vec3_t end;
	trace_t tr;

	VectorCopy( in, end );
	end[2] -= 128;
	trap_Trace( &tr, in, coverMins, coverMaxs, end, ENTITYNUM_NONE, MASK_DEADSOLID );
	if ( tr.startsolid || tr.allsolid || tr.fraction >= 1.0f || tr.plane.normal[2] < 0.7f ) {
		return qfalse;
	}
	VectorCopy( tr.endpos, out );
	if ( trap_PointContents( out, ENTITYNUM_NONE ) & MASK_WATER ) {
		return qfalse;
	}
	return qtrue;
}

// codmod: Phase 4, for the hunt (predicted position, sweep points)
qboolean AICast_CodDropToFloor( vec3_t in, vec3_t out ) {
	return CodDropToFloor( in, out );
}

/*
============
CodAddCandidate

adds a floor-dropped candidate unless it is a near duplicate
============
*/
static void CodAddCandidate( codCand_t *cands, int *num, int maxCands, vec3_t pos ) {
	int i;

	if ( *num >= maxCands ) {
		return;
	}
	for ( i = 0; i < *num; i++ ) {
		if ( Distance( cands[i].pos, pos ) < 40.0f ) {
			return;
		}
	}
	VectorCopy( pos, cands[*num].pos );
	VectorClear( cands[*num].peekPos );
	cands[*num].type = COVER_NONE;
	cands[*num].area = 0;
	cands[*num].travel = 0;
	cands[*num].crouchShot = qfalse;
	( *num )++;
}

/*
============
CodPeekSpot

a step-out spot 'off' units from P along 'dir' (horizontal unit vector): the hull must be able to walk there,
there must be ground (no ledge), and a standing eye there must see the threat.
============
*/
static qboolean CodStepSpot( vec3_t P, vec3_t dir, float off, vec3_t out ) {
	vec3_t Q, end;
	trace_t tr;

	VectorMA( P, off, dir, Q );
	trap_Trace( &tr, P, coverMins, coverMaxs, Q, ENTITYNUM_NONE, MASK_DEADSOLID );
	if ( tr.fraction < 1.0f || tr.startsolid || tr.allsolid ) {
		return qfalse;
	}
	VectorCopy( Q, end );
	end[2] -= 40;
	trap_Trace( &tr, Q, coverMins, coverMaxs, end, ENTITYNUM_NONE, MASK_DEADSOLID );
	if ( tr.startsolid || tr.allsolid || tr.fraction >= 1.0f ) {
		return qfalse;
	}
	VectorCopy( tr.endpos, out );
	return qtrue;
}

static qboolean CodPeekSpot( vec3_t P, vec3_t dir, float off, vec3_t threatEye, vec3_t out ) {
	vec3_t eye;

	if ( !CodStepSpot( P, dir, off, out ) ) {
		return qfalse;
	}
	VectorCopy( out, eye );
	eye[2] += DEFAULT_VIEWHEIGHT;
	return AICast_CodLineClear( eye, threatEye );
}

/*
============
CodClassifyAngle

codmod: Phase 2b. Classifies spot P for a COVER_ANGLE search against the enemy's BODY points (current bbox):
  FIRE (crouchShot)  the crouched eye already has a shot: shoot from down there, harder to hit back
  LOW                only the standing eye has the shot and crouching hides us from the enemy's eye
  FIRE               the standing eye has the shot and there is no cover to speak of
  CORNER_L/R         no shot from P but a short sidestep gives one (peekPos)
  NONE               no shot at all
============
*/
static int CodClassifyAngle( vec3_t P, vec3_t pts[3], int numPts, vec3_t enemyEye, vec3_t tdirH, qboolean canCrouch, vec3_t peekOut, qboolean *crouchShot ) {
	vec3_t eyeStand, eyeCrouch, body, perp, dir, Q, eyeQ;
	int side;
	float sgn;

	*crouchShot = qfalse;
	VectorCopy( P, peekOut );
	VectorCopy( P, eyeStand );
	eyeStand[2] += DEFAULT_VIEWHEIGHT;
	VectorCopy( P, eyeCrouch );
	eyeCrouch[2] += CROUCH_VIEWHEIGHT;
	VectorCopy( P, body );
	body[2] += 8;

	if ( canCrouch && AICast_CodBodyShot( eyeCrouch, pts, numPts ) ) {
		*crouchShot = qtrue;
		return COVER_FIRE;
	}
	if ( AICast_CodBodyShot( eyeStand, pts, numPts ) ) {
		if ( canCrouch && !AICast_CodLineClear( eyeCrouch, enemyEye ) && !AICast_CodLineClear( body, enemyEye ) ) {
			return COVER_LOW;
		}
		return COVER_FIRE;
	}

	// a short sidestep gives the shot: a corner
	perp[0] = -tdirH[1];
	perp[1] = tdirH[0];
	perp[2] = 0;
	for ( side = 0; side < 2; side++ ) {
		sgn = ( side == 0 ) ? 1.0f : -1.0f;
		VectorScale( perp, sgn, dir );
		if ( !CodStepSpot( P, dir, 40.0f, Q ) ) {
			continue;
		}
		VectorCopy( Q, eyeQ );
		eyeQ[2] += DEFAULT_VIEWHEIGHT;
		if ( AICast_CodBodyShot( eyeQ, pts, numPts ) ) {
			VectorCopy( Q, peekOut );
			return ( sgn > 0 ) ? COVER_CORNER_L : COVER_CORNER_R;
		}
	}
	return COVER_NONE;
}

/*
============
CodClassify

classifies spot P against the threat. Returns the cover type and fills peekOut. For COVER_HIDE searches the
corner/step-out analysis is skipped (a hidden spot is just FULL).
============
*/
static int CodClassify( vec3_t P, vec3_t threatEye, vec3_t tdirH, qboolean canCrouch, int mode, vec3_t peekOut ) {
	vec3_t eyeStand, eyeCrouch, body, perp;
	trace_t tr;
	qboolean visStand, visCrouch, visBody;
	static const float cornerOffs[2] = { 32.0f, 48.0f };
	static const float looseOffs[2] = { 64.0f, 96.0f };
	int side, o, first;
	float sgn;

	VectorCopy( P, eyeStand );
	eyeStand[2] += DEFAULT_VIEWHEIGHT;
	VectorCopy( P, eyeCrouch );
	eyeCrouch[2] += CROUCH_VIEWHEIGHT;
	VectorCopy( P, body );
	body[2] += 8;

	visStand = AICast_CodLineClear( eyeStand, threatEye );
	trap_Trace( &tr, eyeCrouch, NULL, NULL, threatEye, ENTITYNUM_NONE, COVER_MASK );
	visCrouch = ( tr.fraction == 1.0f && !tr.startsolid );
	visBody = AICast_CodLineClear( body, threatEye );

	if ( visCrouch || visBody ) {
		return COVER_NONE;      // exposed even when crouched
	}

	if ( visStand ) {
		// only the head clears whatever is in the way: low cover, if the blocker is close and we can duck
		if ( !canCrouch ) {
			return COVER_NONE;
		}
		if ( Distance( eyeCrouch, tr.endpos ) > COVER_LOW_BLOCKER ) {
			return COVER_NONE;
		}
		VectorCopy( P, peekOut );
		return COVER_LOW;
	}

	// hidden even standing
	VectorCopy( P, peekOut );
	if ( mode == COVER_HIDE || mode == COVER_DODGE ) {
		return COVER_FULL;
	}

	// perpendicular to the threat direction: perp points to the left when facing the threat
	perp[0] = -tdirH[1];
	perp[1] = tdirH[0];
	perp[2] = 0;
	first = ( rand() & 1 ) ? 1 : -1;

	// corner: a short sideways step makes the threat visible
	for ( o = 0; o < 2; o++ ) {
		for ( side = 0; side < 2; side++ ) {
			vec3_t dir;

			sgn = ( side == 0 ) ? (float)first : (float)-first;
			VectorScale( perp, sgn, dir );
			if ( CodPeekSpot( P, dir, cornerOffs[o], threatEye, peekOut ) ) {
				return ( sgn > 0 ) ? COVER_CORNER_L : COVER_CORNER_R;
			}
		}
	}

	// loose: a wider sidestep, or stepping forward, still works for a shooter
	for ( o = 0; o < 2; o++ ) {
		for ( side = 0; side < 3; side++ ) {
			vec3_t dir;

			if ( side == 2 ) {
				VectorCopy( tdirH, dir );
			} else {
				sgn = ( side == 0 ) ? (float)first : (float)-first;
				VectorScale( perp, sgn, dir );
			}
			if ( CodPeekSpot( P, dir, side == 2 ? 48.0f : looseOffs[o], threatEye, peekOut ) ) {
				return COVER_FULL;
			}
		}
	}

	return COVER_NONE;          // hidden but cannot shoot from here: useless for an attacker
}

/*
============
AICast_CodFindCover

codmod: finds a cover spot against threatPos. mode COVER_ATTACK wants a spot it can shoot from, COVER_HIDE a
fully hidden one. avoidPos (optional) rejects candidates within 96 u of a spot that just failed.
Phase 2 modes: COVER_ADVANCE wants an attack spot clearly closer to the threat than the soldier is now (bounding
overwatch hop); COVER_FLANK wants an attack spot whose bearing to the threat differs by more than 60 degrees from
the bearing of refPos (the squad centre) and also samples ring points around the threat; its route is scored by
how much of the straight line to the spot the threat can see.
Returns qfalse when nothing usable was found; out is only valid on qtrue.
============
*/
qboolean AICast_CodFindCover( cast_state_t *cs, vec3_t threatPos, int threatNum, int mode, codCover_t *out, vec3_t avoidPos, vec3_t refPos ) {
	static const float ringRadii[4] = { 128.0f, 256.0f, 384.0f, 512.0f };
	codCand_t cands[MAX_COVER_CANDS];
	int numCands, numValid;
	vec3_t origin, tdir, tdirH, threatEye, dir, end, pt, floorPos, toSpot;
	trace_t tr;
	float radius, ang, baseAng, tdist, d, dAI, lo, hi, score, bestScore, w;
	int i, k, travel, startArea, weapon, bestIdx;
	qboolean canCrouch;
	int areaCache[MAX_COVER_CANDS];
	int travelCache[MAX_COVER_CANDS];
	int numCached;
	vec3_t refDir;
	float maxTravel, progress;
	qboolean flank;
	qboolean angle, bodyAware;
	int maxCands, numBody;
	vec3_t bodyPts[3], enemyEye;
	qboolean retreat, dodge;
	int travelCap;

	codCoverLastCandidates = 0;
	codCoverLastValid = 0;
	travelCap = codCoverTravelCap;      // one-shot
	codCoverTravelCap = 0;

	if ( !cs || !cs->bs || !out || threatNum < 0 ) {
		return qfalse;
	}

	VectorCopy( cs->bs->origin, origin );
	startArea = cs->bs->areanum;
	if ( !startArea ) {
		startArea = BotPointAreaNum( origin );
	}
	if ( !startArea ) {
		return qfalse;
	}

	radius = bg_cod.aiCoverRadius;
	if ( radius < 128 ) {
		radius = 128;
	}
	if ( radius > 1536 ) {
		radius = 1536;
	}

	VectorCopy( threatPos, threatEye );
	threatEye[2] += DEFAULT_VIEWHEIGHT;

	VectorSubtract( threatPos, origin, tdir );
	tdist = VectorNormalize( tdir );
	VectorCopy( tdir, tdirH );
	tdirH[2] = 0;
	if ( VectorNormalize( tdirH ) < 0.001f ) {
		tdirH[0] = 1;
	}

	canCrouch = AICast_CodCanCrouch( cs );
	weapon = cs->weaponNum;
	CodWeaponBand( weapon, &lo, &hi );

	// codmod: Phase 2 modes
	flank = ( mode == COVER_FLANK && refPos ) ? qtrue : qfalse;
	retreat = ( mode == COVER_RETREAT ) ? qtrue : qfalse;       // Phase 3: attack-type spot clearly farther from the threat
	dodge = ( mode == COVER_DODGE ) ? qtrue : qfalse;           // Phase 3: hidden from a grenade's landing point, quickly reachable
	// codmod: Phase 2b. COVER_ANGLE needs a shot at the enemy's body; advance / flank spots must give one too (the
	// eye test alone treats a prone player behind a table as reachable)
	angle = ( mode == COVER_ANGLE ) ? qtrue : qfalse;
	bodyAware = ( angle || mode == COVER_FLANK || mode == COVER_ADVANCE ) ? qtrue : qfalse;
	numBody = bodyAware ? AICast_CodBodyPoints( threatNum, threatPos, bodyPts ) : 0;
	maxCands = angle ? MAX_COVER_CANDS : COVER_CANDS_STD;
	VectorCopy( threatPos, enemyEye );
	enemyEye[2] += ( threatNum < MAX_CLIENTS && g_entities[threatNum].client ) ? g_entities[threatNum].client->ps.viewheight : DEFAULT_VIEWHEIGHT;
	if ( angle && !numBody ) {
		return qfalse;
	}
	maxTravel = bg_cod.aiCoverMaxTravel;
	if ( mode == COVER_ADVANCE ) {
		maxTravel *= 0.7f;      // short bounds
	} else if ( flank || angle ) {
		maxTravel *= 2.0f;      // flank and push routes are long
	} else if ( retreat ) {
		maxTravel *= 1.5f;
	}
	if ( travelCap > 0 && maxTravel > travelCap ) {
		maxTravel = travelCap;
	}
	VectorClear( refDir );
	if ( flank ) {
		VectorSubtract( refPos, threatPos, refDir );
		refDir[2] = 0;
		if ( VectorNormalize( refDir ) < 0.001f ) {
			return qfalse;
		}
	}

	// ---- 1. candidates -----------------------------------------------------------------------------
	numCands = 0;

	// (0) flank: ring points around the threat on its side and behind it, relative to the squad
	if ( flank ) {
		static const float flankAngs[8] = { 65.0f, -65.0f, 90.0f, -90.0f, 115.0f, -115.0f, 140.0f, -140.0f };
		float fd, base = RAD2DEG( atan2( refDir[1], refDir[0] ) );

		fd = Distance( threatPos, refPos );
		if ( fd < 350.0f ) {
			fd = 350.0f;
		} else if ( fd > 800.0f ) {
			fd = 800.0f;
		}
		for ( k = 0; k < 3; k++ ) {
			for ( i = 0; i < 8 && numCands < maxCands; i++ ) {
				ang = DEG2RAD( base + flankAngs[i] );
				VectorSet( dir, cos( ang ), sin( ang ), 0 );
				VectorMA( threatPos, fd * ( 0.7f + 0.3f * k ), dir, pt );
				if ( CodDropToFloor( pt, floorPos ) ) {
					CodAddCandidate( cands, &numCands, maxCands, floorPos );
				}
			}
		}
	}

	// (0b) angle: ring points around the THREAT (radius 160/288/416), 8 bearings off the one the soldier sits on,
	// so the spots lie on the soldier's side and at a different bearing from the one that has no shot
	if ( angle ) {
		static const float angleRadii[3] = { 160.0f, 288.0f, 416.0f };
		static const float angleAngs[8] = { 35.0f, -35.0f, 70.0f, -70.0f, 105.0f, -105.0f, 140.0f, -140.0f };
		float bearing = RAD2DEG( atan2( origin[1] - threatPos[1], origin[0] - threatPos[0] ) );

		for ( k = 0; k < 3; k++ ) {
			for ( i = 0; i < 8 && numCands < maxCands; i++ ) {
				ang = DEG2RAD( bearing + angleAngs[i] + ( random() - 0.5f ) * 16.0f );
				VectorSet( dir, cos( ang ), sin( ang ), 0 );
				VectorMA( threatPos, angleRadii[k], dir, pt );
				if ( CodDropToFloor( pt, floorPos ) ) {
					CodAddCandidate( cands, &numCands, maxCands, floorPos );
				}
			}
		}
	}

	// (a) wall-hug rays: where a ray ends on a near-vertical surface is where cover tends to be
	baseAng = random() * ( 360.0f / COVER_WALL_RAYS );
	for ( i = 0; i < COVER_WALL_RAYS && numCands < maxCands; i++ ) {
		ang = DEG2RAD( baseAng + i * ( 360.0f / COVER_WALL_RAYS ) );
		dir[0] = cos( ang );
		dir[1] = sin( ang );
		dir[2] = 0;
		VectorMA( origin, radius, dir, end );
		trap_Trace( &tr, origin, coverMins, coverMaxs, end, ENTITYNUM_NONE, MASK_DEADSOLID );
		if ( tr.startsolid || tr.allsolid || tr.fraction >= 1.0f ) {
			continue;
		}
		if ( fabs( tr.plane.normal[2] ) > 0.3f ) {
			continue;       // floor, ceiling or slope, not a wall
		}
		if ( Distance( origin, tr.endpos ) < COVER_MIN_MOVE ) {
			continue;
		}
		if ( CodDropToFloor( tr.endpos, floorPos ) ) {
			CodAddCandidate( cands, &numCands, maxCands, floorPos );
		}
	}

	// (b) ring samples at 128/256/384/512, 8 angles, skipping the arc that runs straight at the threat
	baseAng = random() * 45.0f;
	for ( k = 0; k < 4 && numCands < maxCands; k++ ) {
		if ( ringRadii[k] > radius ) {
			break;
		}
		for ( i = 0; i < 8 && numCands < maxCands; i++ ) {
			ang = DEG2RAD( baseAng + i * 45.0f );
			dir[0] = cos( ang );
			dir[1] = sin( ang );
			dir[2] = 0;
			if ( k > 0 && DotProduct( dir, tdirH ) > 0.85f ) {
				continue;   // biased away from the threat
			}
			VectorMA( origin, ringRadii[k], dir, pt );
			trap_Trace( &tr, origin, coverMins, coverMaxs, pt, ENTITYNUM_NONE, MASK_DEADSOLID );
			if ( tr.startsolid || tr.allsolid ) {
				continue;
			}
			if ( Distance( origin, tr.endpos ) < COVER_MIN_MOVE ) {
				continue;
			}
			if ( CodDropToFloor( tr.endpos, floorPos ) ) {
				CodAddCandidate( cands, &numCands, maxCands, floorPos );
			}
		}
	}
	codCoverLastCandidates = numCands;

	// ---- 2./3. validate and classify (cheap traces first) --------------------------------------------
	numValid = 0;
	for ( i = 0; i < numCands; i++ ) {
		if ( avoidPos && Distance( cands[i].pos, avoidPos ) < 96.0f ) {
			continue;
		}
		if ( CodNearOtherSoldier( cs->entityNum, cands[i].pos ) ) {
			continue;
		}
		if ( mode == COVER_ADVANCE ) {
			d = Distance( cands[i].pos, threatPos );
			if ( d > tdist - 96.0f || d < 192.0f ) {
				continue;       // must make real progress, but not walk into the threat's face
			}
		} else if ( flank ) {
			VectorSubtract( cands[i].pos, threatPos, toSpot );
			toSpot[2] = 0;
			VectorNormalize( toSpot );
			if ( DotProduct( toSpot, refDir ) > 0.5f ) {
				continue;       // within 60 degrees of the squad's bearing: not a flank
			}
		} else if ( retreat ) {
			d = Distance( cands[i].pos, threatPos );
			if ( d < tdist + 80.0f || Distance( cands[i].pos, origin ) < 96.0f ) {
				continue;       // has to be a real step away from the threat
			}
		} else if ( angle ) {
			d = Distance( cands[i].pos, threatPos );
			if ( d < 80.0f || Distance( cands[i].pos, origin ) < 64.0f ) {
				continue;       // not in the player's face, and it has to be a real move
			}
			if ( d < 128.0f && lo > 256.0f ) {
				continue;       // only pistols and SMGs fight this close
			}
		}
		if ( cs->dangerEntityValidTime >= level.time && Distance( cands[i].pos, cs->dangerEntityPos ) < 250.0f ) {
			continue;       // do not hide next to a live grenade
		}
		if ( angle ) {
			cands[i].type = CodClassifyAngle( cands[i].pos, bodyPts, numBody, enemyEye, tdirH, canCrouch, cands[i].peekPos, &cands[i].crouchShot );
		} else {
			cands[i].type = CodClassify( cands[i].pos, threatEye, tdirH, canCrouch, mode, cands[i].peekPos );
		}
		if ( cands[i].type == COVER_NONE ) {
			continue;
		}
		if ( bodyAware && !angle && numBody ) {
			vec3_t shootEye;

			VectorCopy( cands[i].peekPos, shootEye );
			shootEye[2] += DEFAULT_VIEWHEIGHT;
			if ( !AICast_CodBodyShot( shootEye, bodyPts, numBody ) ) {
				cands[i].type = COVER_NONE;     // sees the eye height, but cannot hit the (crouched / prone) body
				continue;
			}
		}
		numValid++;
	}
	codCoverLastValid = numValid;
	if ( !numValid ) {
		return qfalse;
	}

	// ---- 4./5. AAS reachability, score ---------------------------------------------------------------
	numCached = 0;
	bestIdx = -1;
	bestScore = -99999.0f;
	for ( i = 0; i < numCands; i++ ) {
		if ( cands[i].type == COVER_NONE ) {
			continue;
		}
		cands[i].area = BotPointAreaNum( cands[i].pos );
		if ( !cands[i].area ) {
			continue;
		}
		// one routing query per AAS area
		travel = -1;
		for ( k = 0; k < numCached; k++ ) {
			if ( areaCache[k] == cands[i].area ) {
				travel = travelCache[k];
				break;
			}
		}
		if ( travel < 0 ) {
			travel = trap_AAS_AreaTravelTimeToGoalArea( startArea, origin, cands[i].area, cs->travelflags );
			if ( numCached < MAX_COVER_CANDS ) {
				areaCache[numCached] = cands[i].area;
				travelCache[numCached] = travel;
				numCached++;
			}
		}
		if ( travel <= 0 || travel > maxTravel ) {
			continue;       // unreachable or too far
		}
		cands[i].travel = travel;

		// codmod: Phase 2b. Push for an angle: crouch shots and low cover that keep the shot first, then distance
		// progress, a new bearing and a route the player cannot watch
		if ( angle ) {
			float angDiff, dot2;
			vec3_t b1, b2;
			int sample, seen = 0;

			d = Distance( cands[i].pos, threatPos );
			score = 60.0f - travel * 0.08f;
			if ( cands[i].crouchShot ) {
				score += 18.0f;
			} else if ( cands[i].type != COVER_FIRE ) {
				score += 25.0f;     // low cover or a corner that still has the shot
			}
			if ( d < lo ) {
				score -= ( lo - d ) * 0.04f;
			} else if ( d > hi ) {
				score -= ( d - hi ) * 0.05f;
			} else {
				score += 10.0f;
			}
			if ( d < 128.0f ) {
				score -= 20.0f;
			}
			progress = tdist - d;
			if ( progress > 300.0f ) {
				progress = 300.0f;
			} else if ( progress < -150.0f ) {
				progress = -150.0f;
			}
			score += progress * 0.06f;
			VectorSubtract( origin, threatPos, b1 );
			b1[2] = 0;
			VectorNormalize( b1 );
			VectorSubtract( cands[i].pos, threatPos, b2 );
			b2[2] = 0;
			VectorNormalize( b2 );
			dot2 = DotProduct( b1, b2 );
			angDiff = RAD2DEG( acos( dot2 < -1.0f ? -1.0f : ( dot2 > 1.0f ? 1.0f : dot2 ) ) );
			score += ( angDiff > 90.0f ? 90.0f : angDiff ) * 0.15f;
			for ( sample = 1; sample <= 4; sample++ ) {
				VectorSubtract( cands[i].pos, origin, b1 );
				VectorMA( origin, sample * 0.25f, b1, b1 );
				b1[2] += 40.0f;
				if ( AICast_CodLineClear( enemyEye, b1 ) ) {
					seen++;
				}
			}
			score -= seen * 8.0f;
			if ( cands[i].pos[2] > threatPos[2] && cands[i].pos[2] - threatPos[2] <= 96.0f ) {
				score += 10.0f;
			}
			score += random() * 6.0f;
			if ( score > bestScore ) {
				bestScore = score;
				bestIdx = i;
			}
			continue;
		}

		// score (higher is better)
		if ( mode == COVER_HIDE || dodge ) {
			w = ( cands[i].type == COVER_FULL ) ? 1.0f : 0.6f;
		} else {
			w = ( cands[i].type == COVER_FULL ) ? 0.3f : 1.0f;
		}
		score = w * 100.0f - travel * ( dodge ? 0.4f : 0.1f );     // a dodge is a race against the fuse

		d = Distance( cands[i].pos, threatPos );
		if ( d < 192.0f ) {
			score -= 40.0f + ( 192.0f - d ) * 0.2f;
		}
		if ( mode != COVER_HIDE && !dodge ) {
			if ( d < lo ) {
				score -= ( lo - d ) * 0.08f;
			} else if ( d > hi ) {
				score -= ( d - hi ) * 0.05f;
			} else {
				score += 15.0f;
			}
		}
		// do not run at the threat to get there
		dAI = tdist;
		VectorSubtract( cands[i].pos, origin, toSpot );
		VectorNormalize( toSpot );
		if ( mode == COVER_ADVANCE ) {
			progress = dAI - d;
			score += ( progress > 300.0f ? 300.0f : progress ) * 0.12f;
		} else if ( flank ) {
			// the larger the bearing difference the better, and a route the threat cannot see is better still
			float dot;
			int sample, seen = 0;
			vec3_t sp;

			VectorSubtract( cands[i].pos, threatPos, sp );
			sp[2] = 0;
			VectorNormalize( sp );
			dot = DotProduct( sp, refDir );
			score += RAD2DEG( acos( dot < -1.0f ? -1.0f : dot ) ) * 0.1f;
			for ( sample = 1; sample <= 4; sample++ ) {
				VectorSubtract( cands[i].pos, origin, sp );
				VectorMA( origin, sample * 0.25f, sp, sp );
				sp[2] += 40.0f;
				if ( AICast_CodLineClear( threatEye, sp ) ) {
					seen++;
				}
			}
			score -= seen * 8.0f;
		} else if ( retreat ) {
			// the farther from the threat the better (capped), and a route the threat cannot watch is better still
			int sample, seen = 0;
			vec3_t sp;

			progress = d - dAI;
			score += ( progress > 300.0f ? 300.0f : progress ) * 0.1f;
			for ( sample = 1; sample <= 4; sample++ ) {
				VectorSubtract( cands[i].pos, origin, sp );
				VectorMA( origin, sample * 0.25f, sp, sp );
				sp[2] += 40.0f;
				if ( AICast_CodLineClear( threatEye, sp ) ) {
					seen++;
				}
			}
			score -= seen * 8.0f;
		} else if ( dodge ) {
			// away from the blast: the farther the better (capped); running at the grenade is penalised below
			score += ( d > 450.0f ? 450.0f : d ) * 0.08f;
			if ( d < dAI - 64.0f && DotProduct( toSpot, tdir ) > 0.5f ) {
				score -= 40.0f;
			}
		} else if ( d < dAI - 64.0f && DotProduct( toSpot, tdir ) > 0.5f ) {
			score -= 25.0f;
		}
		// slight height advantage over the threat
		if ( cands[i].pos[2] > threatPos[2] && cands[i].pos[2] - threatPos[2] <= 96.0f ) {
			score += 10.0f;
		}
		score += random() * 6.0f;   // so a group does not pick one spot every time

		if ( score > bestScore ) {
			bestScore = score;
			bestIdx = i;
		}
	}

	if ( bestIdx < 0 ) {
		return qfalse;
	}

	VectorCopy( cands[bestIdx].pos, out->pos );
	VectorCopy( cands[bestIdx].peekPos, out->peekPos );
	VectorCopy( threatPos, out->threatPos );
	out->type = cands[bestIdx].type;
	out->angle = angle;
	out->crouchShot = cands[bestIdx].crouchShot;
	out->score = bestScore;
	out->travel = cands[bestIdx].travel;
	return qtrue;
}
