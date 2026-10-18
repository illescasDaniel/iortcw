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

// codmod: new file -- ai_cast_cod.h
//
// Tactical AI (docs/ai-overhaul-plan.md): shared types and prototypes for ai_cast_cod.c (eligibility, combat
// state machine) and ai_cast_cod_cover.c (cover finder and cover claims). Include after g_local.h / ai_cast.h.
// Game module only; none of this is saved in cast_state_t (savegames stay compatible).

#ifndef __AI_CAST_COD_H__
#define __AI_CAST_COD_H__

// what a cover search is looking for
typedef enum {
	COVER_ATTACK,       // a spot it can shoot from (low cover, corner)
	COVER_HIDE,         // fully hidden, for reloading / retreating
	COVER_ADVANCE,      // Phase 2: attack spot clearly closer to the threat (bounding overwatch hop)
	COVER_FLANK,        // Phase 2: attack spot with a very different bearing to the threat than the squad's
	COVER_ANGLE         // Phase 2b: a spot with a real shot at the enemy's body (push for an angle)
} codCoverMode_t;

// how a cover spot works
typedef enum {
	COVER_NONE,
	COVER_LOW,          // crouch to hide, stand to shoot
	COVER_CORNER_L,     // hidden; step left (as seen facing the threat) to shoot
	COVER_CORNER_R,     // hidden; step right to shoot
	COVER_FULL,         // hidden; peekPos is a looser step-out spot (ATTACK) or unused (HIDE)
	COVER_FIRE          // Phase 2b: exposed firing spot (COVER_ANGLE only), shoot from here without hiding
} codCoverType_t;

typedef struct {
	vec3_t pos;             // where to stand (player origin, i.e. hull centre)
	vec3_t peekPos;         // where to stand to shoot (CORNER/FULL); equals pos for LOW
	vec3_t threatPos;       // threat position the spot was chosen against
	codCoverType_t type;
	float score;
	int travel;             // AAS travel time, 1/100 s
	qboolean angle;         // Phase 2b: chosen by a COVER_ANGLE search (body-point shot)
	qboolean crouchShot;    // Phase 2b: the crouched eye already has the shot
} codCover_t;

// ai_cast_cod_cover.c
qboolean AICast_CodFindCover( cast_state_t *cs, vec3_t threatPos, int threatNum, int mode, codCover_t *out, vec3_t avoidPos, vec3_t refPos );
qboolean AICast_CodCoverSearchAllowed( qboolean urgent );
void AICast_CodClaimCover( int entNum, vec3_t pos );
void AICast_CodReleaseCover( int entNum );
qboolean AICast_CodLineClear( vec3_t from, vec3_t to );
const char *AICast_CodCoverName( int type );
int AICast_CodBodyPoints( int enemyNum, vec3_t at, vec3_t pts[3] );     // Phase 2b: points on the enemy's current bbox
qboolean AICast_CodBodyShot( vec3_t eye, vec3_t pts[3], int num );      // clear line from eye to any of them
extern int codCoverLastCandidates;      // debug: candidates generated / validated in the last search
extern int codCoverLastValid;

// ai_cast_cod.c
qboolean AICast_CodCanCrouch( cast_state_t *cs );

// ai_cast_cod_squad.c (Phase 2: squads, attack tokens, movers, flanking, callouts)
#define COD_BARK_ATTACK     0       // callout kinds
#define COD_BARK_ORDERS     1
void AICast_CodSquadThink( void );                                      // once per AIFunc_CodCombat; regroups ~1/s
void AICast_CodSquadForget( int entNum );                               // spawn / death
int AICast_CodSquadId( cast_state_t *cs );                              // 0 = not in a squad
const char *AICast_CodSquadRoleName( cast_state_t *cs );
void AICast_CodSquadPublish( cast_state_t *cs, qboolean inCover );
void AICast_CodSquadReportThreat( cast_state_t *cs, vec3_t pos );       // a member sees the enemy at pos
qboolean AICast_CodSquadThreat( cast_state_t *cs, vec3_t out, int *time );
qboolean AICast_CodSquadCentre( cast_state_t *cs, vec3_t out );
qboolean AICast_CodTokenAcquire( cast_state_t *cs, int leaseMs );       // may shoot at the enemy now
void AICast_CodTokenRelease( cast_state_t *cs );
qboolean AICast_CodHasToken( cast_state_t *cs );
qboolean AICast_CodMoverAcquire( cast_state_t *cs, int leaseMs, qboolean force );
void AICast_CodMoverRelease( cast_state_t *cs );
qboolean AICast_CodSquadCanAdvance( cast_state_t *cs );                 // squad >= 2, someone in cover to overwatch
void AICast_CodSquadMoverStarted( cast_state_t *cs, qboolean flank );   // callout + opens the suppression window
qboolean AICast_CodSquadFlankOrder( cast_state_t *cs );                 // this soldier was picked to flank
void AICast_CodSquadFlankResult( cast_state_t *cs, qboolean started );  // search done: moving (qtrue) / failed
void AICast_CodSquadFlankDone( cast_state_t *cs );                      // arrived
qboolean AICast_CodSquadSuppress( cast_state_t *cs, vec3_t out );       // a teammate is moving: fire at out
void AICast_CodSquadBark( cast_state_t *cs, int kind );

// Phase 2b: push for an angle (the player sits behind cover and nobody has a hittable shot)
void AICast_CodSquadShotReport( cast_state_t *cs, qboolean shot );      // a token holder tried to shoot: did it have a shot
qboolean AICast_CodSquadPushOrder( cast_state_t *cs );                  // this soldier was picked to push
qboolean AICast_CodSquadPushing( cast_state_t *cs );                    // picked or on its way / in position
void AICast_CodSquadPushStarted( cast_state_t *cs, int travelMs );      // moving: bark + suppression window
void AICast_CodSquadPushResult( cast_state_t *cs, qboolean started );   // search done (qfalse: nothing found)
void AICast_CodSquadPushDone( cast_state_t *cs, qboolean ok );          // push over: cooldown
qboolean AICast_CodSquadThreatFresh( cast_state_t *cs );                // the enemy's position is known fresh

#endif // __AI_CAST_COD_H__
