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
// Name:			ai_cast_cod_squad.c
// Function:		codmod tactical AI -- squads, attack tokens, movers, flanking, suppression and callouts
// Tab Size:		4 (real tabs)
//
// docs/ai-overhaul-plan.md, Phase 2. Everything here is shared state between the soldiers that currently run
// AIFunc_CodCombat; the per-soldier behaviour that uses it lives in ai_cast_cod.c.
//
//   Squads        regrouped about once a second by whichever soldier thinks first (AICast_CodSquadThink): alive
//                 soldiers in AIFunc_CodCombat, same aiTeam, same enemy, within cod_aiSquadRange of a squad
//                 member and in its PVS. Squad state that has to survive a regroup (threat blackboard, hold
//                 timer, flank order, bark timer) is inherited by the new squad holding most of the old members.
//                 A soldier on its own is in no squad and is never restricted.
//   Blackboard    the last position of the enemy that any member saw, with its time.
//   Tokens        short leases. At most N members (cod_aiAttackTokens, auto 2/3/4 by g_gameskill) may hold one;
//                 a lease is requested to shoot (OPEN, PEEK) and renewed while shooting. A holder gives way after
//                 cod_aiTokenTime when somebody else is waiting; waiters are served in the order they asked.
//   Movers        voluntary relocations (advance, flank) take a mover slot: 1 per squad, 2 for 4+ members.
//   Flanking      the player held one spot (cod_aiFlankMove) for cod_aiFlankHold: one member is ordered to
//                 relocate with COVER_FLANK; the order is picked up when that soldier is next in cover.
//   Push (2b)     the squad keeps trying (token holders peek / shoot) for cod_aiNoShotTime without a single real
//                 shot at a position it knows fresh: the player sits behind cover nobody can hit. One member
//                 (two with 4+) is ordered to push for an angle (COVER_ANGLE), the rest keep shooting at the cover
//                 (suppression window). Cooldown cod_aiPushCooldown after a push ends.
//   Phase 3       grenade thrower choice + squad cooldown, the opportunity token (+1 while the player is weak) and
//                 morale (a squad that lost half its members falls back); see ai_cast_cod.c for the reactions.
//   Suppression   starting a move opens a cod_aiSuppressMs window in which token holders fire at the threat's
//                 last known position (see CodSuppressFire in ai_cast_cod.c).
//
// Nothing here goes into cast_state_t. A savegame load leaves these arrays stale; every use validates level.startTime
// and the time of the last regroup, so they restart from scratch.
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

#define COD_MAX_SQUADS      12
#define COD_SQUAD_MAX       24
#define COD_UPDATE_MS       1000
#define COD_SQUAD_STALE_MS  3000        // no regroup for this long: the squad data is not trusted
#define COD_WAIT_MS         1500        // a denied soldier counts as waiting this long
#define COD_FLANK_ORDER_MS  8000        // an order nobody picked up expires
#define COD_FLANK_COOLDOWN  15000
#define COD_DBG_MS          1500        // per-soldier rate limit of token debug lines
#define COD_PUSH_ORDER_MS   8000        // a push order nobody picked up expires
#define COD_PUSH_MAX_MS     20000       // a pusher that never reports back is dropped

typedef struct {
	qboolean active;
	int id;
	int team;
	int enemy;
	int num;
	int members[COD_SQUAD_MAX];
	vec3_t centre;
	int maxMovers;
	int lastDbgNum;
	int nextDbg;

	// blackboard
	vec3_t threatPos;
	int threatTime;

	// hold detection and flanking
	vec3_t anchor;
	int holdSince;
	int flanker;                // entity number or -1
	qboolean flankPending;      // ordered, not picked up yet
	int flankIssued;
	int nextFlank;

	int suppressUntil;
	int nextBark;

	// Phase 2b: no-shot detection and push orders
	int tryMs;                  // accumulated ms a token holder tried to shoot without any member having a shot
	int lastTryAcc;
	int lastShot;               // newest time any member had a real shot
	int pusher[2];              // entity numbers or -1
	qboolean pushPending[2];    // ordered, not picked up yet
	int pushIssued;
	int nextPush;

	// Phase 3
	int grenadeThrower;         // entity number or -1 (best member to throw, re-evaluated every 600 ms)
	int nextGrenadeEval;
	int nextGrenade;            // squad cooldown after a throw
	int peak;                   // most members alive at the same time since the last morale break
	int killed;                 // members that died since then
	int nextMorale;
	int oppUntil;               // +1 attack token until then (the player reloads, switches weapons, slides...)
} codSquad_t;

typedef struct {
	int squad;                  // index into squads[], valid while stamp == sqStamp
	int stamp;
	qboolean inCover;           // published by the soldier every think
	int publishTime;

	int tokenUntil;
	int holdSince;
	int waitSince;              // 0 = not waiting
	int lastDeny;
	int noTokenUntil;           // gave way: not again for a moment
	int moverUntil;
	int nextDbg;

	// Phase 3 (survives a regroup, so a lone survivor of a broken squad still falls back)
	int moraleUntil;
	int moraleLevel;            // level.startTime the morale fields belong to
	int moraleSerial;           // changes with every break, compared by the soldier (AICast_CodSquadMoraleFresh)
	int moraleSeen;
	int nextBarkSolo;           // callout rate limit of a soldier outside any squad
} codMember_t;

static codSquad_t squads[COD_MAX_SQUADS];
static codMember_t members[MAX_CLIENTS];
static int sqLevelStart;
static int sqNextUpdate;
static int sqLastUpdate;
static int sqStamp;
static int sqSerial;
static qboolean sqDirty;

//===========================================================================
// lookup
//===========================================================================

static const char *SqName( int entNum ) {
	char *name = g_entities[entNum].aiName;

	return name ? name : "?";
}

static void SqReset( void ) {
	memset( squads, 0, sizeof( squads ) );
	memset( members, 0, sizeof( members ) );
	sqLevelStart = level.startTime;
	sqNextUpdate = 0;
	sqLastUpdate = 0;
	sqStamp++;
	sqDirty = qfalse;
}

static qboolean SqUsable( void ) {
	if ( !bg_cod.enable || !bg_cod.aiTactics || !bg_cod.aiSquad ) {
		return qfalse;
	}
	if ( sqLevelStart != level.startTime || sqLastUpdate > level.time || sqLastUpdate < level.time - COD_SQUAD_STALE_MS ) {
		return qfalse;
	}
	return qtrue;
}

static codSquad_t *SquadOf( cast_state_t *cs ) {
	codMember_t *m;
	codSquad_t *sq;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS || !SqUsable() ) {
		return NULL;
	}
	m = &members[cs->entityNum];
	if ( m->stamp != sqStamp || m->squad < 0 || m->squad >= COD_MAX_SQUADS ) {
		return NULL;
	}
	sq = &squads[m->squad];
	return sq->active ? sq : NULL;
}

int AICast_CodSquadId( cast_state_t *cs ) {
	codSquad_t *sq = SquadOf( cs );

	return sq ? sq->id : 0;
}

/*
============
AICast_CodSquadRoleName

codmod: role for the debug lines: SOLO, FLANK (ordered or on its way), MOVE (relocating), FIRE (holds an attack
token) or COVER (waiting for its turn)
============
*/
const char *AICast_CodSquadRoleName( cast_state_t *cs ) {
	codSquad_t *sq = SquadOf( cs );
	codMember_t *m = &members[cs->entityNum];

	if ( !sq ) {
		return "SOLO";
	}
	if ( sq->flanker == cs->entityNum ) {
		return "FLANK";
	}
	if ( sq->pusher[0] == cs->entityNum || sq->pusher[1] == cs->entityNum ) {
		return "PUSH";
	}
	if ( m->moverUntil > level.time ) {
		return "MOVE";
	}
	if ( m->tokenUntil > level.time ) {
		return "FIRE";
	}
	return "COVER";
}

/*
============
AICast_CodSquadForget

codmod: a soldier spawned or died: drop its tokens and slots and regroup soon
============
*/
static void SqMoraleDeath( int entNum );

void AICast_CodSquadForget( int entNum ) {
	codSquad_t *sq;

	if ( entNum < 0 || entNum >= MAX_CLIENTS ) {
		return;
	}
	SqMoraleDeath( entNum );        // Phase 3: before the member's squad index is wiped
	memset( &members[entNum], 0, sizeof( members[entNum] ) );
	members[entNum].squad = -1;
	for ( sq = squads; sq < squads + COD_MAX_SQUADS; sq++ ) {
		if ( sq->active && sq->flanker == entNum ) {
			sq->flanker = -1;
			sq->flankPending = qfalse;
		}
		if ( sq->active ) {
			int j;

			for ( j = 0; j < 2; j++ ) {
				if ( sq->pusher[j] == entNum ) {
					sq->pusher[j] = -1;
					sq->pushPending[j] = qfalse;
				}
			}
		}
	}
	sqDirty = qtrue;
}

void AICast_CodSquadPublish( cast_state_t *cs, qboolean inCover ) {
	codMember_t *m;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS ) {
		return;
	}
	m = &members[cs->entityNum];
	m->inCover = inCover;
	m->publishTime = level.time;
}

//===========================================================================
// regroup
//===========================================================================

typedef struct {
	int ent;
	cast_state_t *cs;
	gentity_t *gent;
	int group;
} sqCand_t;

static int SqTokenLimit( codSquad_t *sq ) {
	int n = bg_cod.aiAttackTokens;

	if ( n <= 0 ) {
		n = g_gameskill.integer + 2;        // easy / medium / hard -> 2 / 3 / 4
	}
	if ( sq && sq->oppUntil > level.time ) {
		n++;                                // Phase 3: the player is weak right now, one more may shoot
	}
	return n < 1 ? 1 : n;
}

static int SqTokensHeld( codSquad_t *sq ) {
	int i, n = 0;

	for ( i = 0; i < sq->num; i++ ) {
		if ( members[sq->members[i]].tokenUntil > level.time ) {
			n++;
		}
	}
	return n;
}

/*
============
SqPickFlanker

the player has held one spot long enough: choose a member to go around. Preference: not a mover, already in
cover, not currently shooting. Needs at least one other member to keep him busy.
============
*/
static void SqPickFlanker( codSquad_t *sq ) {
	int i, best = -1;
	float bestScore = -1.0f, score;
	codMember_t *m;

	for ( i = 0; i < sq->num; i++ ) {
		m = &members[sq->members[i]];
		if ( m->moverUntil > level.time ) {
			continue;
		}
		score = 1.0f + random();
		if ( m->inCover && level.time - m->publishTime < 1000 ) {
			score += 2.0f;
		}
		if ( m->tokenUntil <= level.time ) {
			score += 1.0f;
		}
		if ( score > bestScore ) {
			bestScore = score;
			best = sq->members[i];
		}
	}
	if ( best < 0 ) {
		return;
	}
	sq->flanker = best;
	sq->flankPending = qtrue;
	sq->flankIssued = level.time;
	sq->nextFlank = level.time + COD_FLANK_COOLDOWN;
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] sq%d FLANK ordered: %s (player held %d ms, %d members)\n", sq->id, SqName( best ),
				  level.time - sq->holdSince, sq->num );
	}
}

//===========================================================================
// push for an angle (Phase 2b)
//===========================================================================

static qboolean SqPushActive( codSquad_t *sq ) {
	return ( sq->pusher[0] >= 0 || sq->pusher[1] >= 0 ) ? qtrue : qfalse;
}

static int SqPusherIndex( codSquad_t *sq, int ent ) {
	if ( sq->pusher[0] == ent ) {
		return 0;
	}
	if ( sq->pusher[1] == ent ) {
		return 1;
	}
	return -1;
}

// the enemy's position is known fresh: seen or reported within 3 s, or seen within 8 s and still within 96 u of there
static qboolean SqThreatFresh( codSquad_t *sq ) {
	if ( !sq->threatTime ) {
		return qfalse;
	}
	if ( sq->threatTime > level.time - 3000 ) {
		return qtrue;
	}
	return ( sq->threatTime > level.time - 8000 && sq->enemy >= 0 && sq->enemy < MAX_CLIENTS
			 && Distance( g_entities[sq->enemy].r.currentOrigin, sq->threatPos ) < 96.0f ) ? qtrue : qfalse;
}

/*
============
SqUpdatePush

once per regroup: expire stale orders, and order a push when the squad has tried to shoot for cod_aiNoShotTime
without a single real shot at a position it knows fresh. One pusher (two with 4+ members): preferably in cover,
not holding a token, close to the threat.
============
*/
static void SqUpdatePush( codSquad_t *sq ) {
	int i, j, want, ent, best, picked = 0;
	float score, bestScore;
	codMember_t *m;

	for ( j = 0; j < 2; j++ ) {
		if ( sq->pusher[j] < 0 ) {
			continue;
		}
		if ( ( sq->pushPending[j] && level.time - sq->pushIssued > COD_PUSH_ORDER_MS ) || level.time - sq->pushIssued > COD_PUSH_MAX_MS
			 || sq->pushIssued > level.time ) {
			sq->pusher[j] = -1;
			sq->pushPending[j] = qfalse;
			sq->nextPush = level.time + 3000;
		}
	}
	if ( !bg_cod.aiPush || SqPushActive( sq ) || level.time < sq->nextPush || sq->flanker >= 0 ) {
		return;
	}
	if ( sq->tryMs < (int)( bg_cod.aiNoShotTime * AICast_CodSkill( COD_SK_PUSH ) ) || !SqThreatFresh( sq ) ) {
		return;
	}

	want = ( sq->num >= 4 ) ? 2 : 1;
	for ( j = 0; j < want; j++ ) {
		best = -1;
		bestScore = -1.0f;
		for ( i = 0; i < sq->num; i++ ) {
			ent = sq->members[i];
			m = &members[ent];
			if ( SqPusherIndex( sq, ent ) >= 0 || m->moverUntil > level.time ) {
				continue;
			}
			score = 1.0f + random();
			if ( m->inCover && level.time - m->publishTime < 1000 ) {
				score += 3.0f;
			}
			if ( m->tokenUntil <= level.time ) {
				score += 2.0f;
			}
			score += 2.0f - Distance( g_entities[ent].r.currentOrigin, sq->threatPos ) / 700.0f;
			if ( score > bestScore ) {
				bestScore = score;
				best = ent;
			}
		}
		if ( best < 0 ) {
			break;
		}
		sq->pusher[j] = best;
		sq->pushPending[j] = qtrue;
		picked++;
		if ( bg_cod.aiDebug >= 1 ) {
			G_Printf( "[ai] sq%d PUSH ordered: %s (no shot for %d ms, %d members)\n", sq->id, SqName( best ), sq->tryMs, sq->num );
		}
	}
	if ( picked ) {
		sq->pushIssued = level.time;
	}
}

static void SqRebuild( void ) {
	sqCand_t cands[MAX_CLIENTS];
	int numCands = 0;
	codSquad_t old[COD_MAX_SQUADS];
	qboolean oldUsed[COD_MAX_SQUADS];
	int oldOf[MAX_CLIENTS];
	int i, j, k, g, n, oldStamp, bestOld, votes[COD_MAX_SQUADS];
	float range = bg_cod.aiSquadRange;
	codSquad_t *sq;
	vec3_t sum;

	// remember the previous grouping
	memcpy( old, squads, sizeof( old ) );
	memset( oldUsed, 0, sizeof( oldUsed ) );
	oldStamp = sqStamp;
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		oldOf[i] = ( members[i].stamp == oldStamp && members[i].squad >= 0 ) ? members[i].squad : -1;
	}

	// candidates: alive soldiers running the tactical combat function
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		gentity_t *ent = &g_entities[i];
		cast_state_t *cs;

		if ( !ent->inuse || !ent->client || ent->health <= 0 || !( ent->r.svFlags & SVF_CASTAI ) ) {
			continue;
		}
		cs = AICast_GetCastState( i );
		if ( !cs || cs->aifunc != AIFunc_CodCombat || cs->enemyNum < 0 ) {
			continue;
		}
		if ( AICast_CodPolishIsCamper( cs ) ) {
			continue;       // codmod: Phase 5, a camper holds its post on its own (no orders, flanks or pushes)
		}
		cands[numCands].ent = i;
		cands[numCands].cs = cs;
		cands[numCands].gent = ent;
		cands[numCands].group = -1;
		numCands++;
	}

	sqStamp++;
	memset( squads, 0, sizeof( squads ) );
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		members[i].squad = -1;
	}

	// greedy flood fill: same team, same enemy, in range and PVS of any member already in the group
	n = 0;
	for ( i = 0; i < numCands && n < COD_MAX_SQUADS; i++ ) {
		int size = 1;
		int list[COD_SQUAD_MAX];

		if ( cands[i].group >= 0 ) {
			continue;
		}
		cands[i].group = n;
		list[0] = i;
		for ( j = 0; j < size; j++ ) {
			for ( k = 0; k < numCands && size < COD_SQUAD_MAX; k++ ) {
				if ( cands[k].group >= 0 ) {
					continue;
				}
				if ( cands[k].gent->aiTeam != cands[i].gent->aiTeam || cands[k].cs->enemyNum != cands[i].cs->enemyNum ) {
					continue;
				}
				if ( Distance( cands[k].gent->r.currentOrigin, cands[list[j]].gent->r.currentOrigin ) > range ) {
					continue;
				}
				if ( !trap_InPVS( cands[k].gent->r.currentOrigin, cands[list[j]].gent->r.currentOrigin ) ) {
					continue;
				}
				cands[k].group = n;
				list[size++] = k;
			}
		}
		if ( size < 2 ) {
			cands[i].group = -2;        // alone: no squad
			continue;
		}

		sq = &squads[n];
		sq->active = qtrue;
		sq->team = cands[i].gent->aiTeam;
		sq->enemy = cands[i].cs->enemyNum;
		sq->num = size;
		VectorClear( sum );
		for ( j = 0; j < size; j++ ) {
			sq->members[j] = cands[list[j]].ent;
			VectorAdd( sum, cands[list[j]].gent->r.currentOrigin, sum );
			members[sq->members[j]].squad = n;
			members[sq->members[j]].stamp = sqStamp;
		}
		VectorScale( sum, 1.0f / size, sq->centre );
		sq->maxMovers = ( size >= 4 ) ? 2 : 1;

		// inherit the persistent state from the old squad that held most of these soldiers
		memset( votes, 0, sizeof( votes ) );
		for ( j = 0; j < size; j++ ) {
			if ( oldOf[sq->members[j]] >= 0 ) {
				votes[oldOf[sq->members[j]]]++;
			}
		}
		bestOld = -1;
		for ( g = 0; g < COD_MAX_SQUADS; g++ ) {
			if ( votes[g] > 0 && !oldUsed[g] && old[g].active && old[g].enemy == sq->enemy && ( bestOld < 0 || votes[g] > votes[bestOld] ) ) {
				bestOld = g;
			}
		}
		if ( bestOld >= 0 ) {
			oldUsed[bestOld] = qtrue;
			sq->id = old[bestOld].id;
			VectorCopy( old[bestOld].threatPos, sq->threatPos );
			sq->threatTime = old[bestOld].threatTime;
			VectorCopy( old[bestOld].anchor, sq->anchor );
			sq->holdSince = old[bestOld].holdSince;
			sq->flanker = old[bestOld].flanker;
			sq->flankPending = old[bestOld].flankPending;
			sq->flankIssued = old[bestOld].flankIssued;
			sq->nextFlank = old[bestOld].nextFlank;
			sq->suppressUntil = old[bestOld].suppressUntil;
			sq->nextBark = old[bestOld].nextBark;
			sq->lastDbgNum = old[bestOld].lastDbgNum;
			sq->nextDbg = old[bestOld].nextDbg;
			sq->tryMs = old[bestOld].tryMs;
			sq->lastTryAcc = old[bestOld].lastTryAcc;
			sq->lastShot = old[bestOld].lastShot;
			sq->pusher[0] = old[bestOld].pusher[0];
			sq->pusher[1] = old[bestOld].pusher[1];
			sq->pushPending[0] = old[bestOld].pushPending[0];
			sq->pushPending[1] = old[bestOld].pushPending[1];
			sq->pushIssued = old[bestOld].pushIssued;
			sq->nextPush = old[bestOld].nextPush;
			sq->nextGrenade = old[bestOld].nextGrenade;
			sq->peak = old[bestOld].peak;
			sq->killed = old[bestOld].killed;
			sq->nextMorale = old[bestOld].nextMorale;
			sq->oppUntil = old[bestOld].oppUntil;
		} else {
			sq->pusher[0] = sq->pusher[1] = -1;
			sq->nextPush = level.time + 2000;
			sq->id = ++sqSerial;
			sq->flanker = -1;
			sq->holdSince = level.time;
			VectorCopy( g_entities[sq->enemy].r.currentOrigin, sq->anchor );
			sq->nextFlank = level.time + 2000;
			sq->lastDbgNum = 0;
		}
		sq->grenadeThrower = -1;        // Phase 3: chosen again at the next throw decision
		sq->nextGrenadeEval = 0;
		if ( size > sq->peak ) {
			sq->peak = size;
		}
		// a flanker who left the squad loses the order
		if ( sq->flanker >= 0 ) {
			qboolean found = qfalse;

			for ( j = 0; j < size; j++ ) {
				if ( sq->members[j] == sq->flanker ) {
					found = qtrue;
				}
			}
			if ( !found ) {
				sq->flanker = -1;
				sq->flankPending = qfalse;
			}
		}
		// a pusher who left the squad loses the order
		for ( g = 0; g < 2; g++ ) {
			qboolean here = qfalse;

			if ( sq->pusher[g] < 0 ) {
				continue;
			}
			for ( j = 0; j < size; j++ ) {
				if ( sq->members[j] == sq->pusher[g] ) {
					here = qtrue;
				}
			}
			if ( !here ) {
				sq->pusher[g] = -1;
				sq->pushPending[g] = qfalse;
			}
		}
		n++;
	}
	sqLastUpdate = level.time;

	// per-squad upkeep
	for ( i = 0; i < n; i++ ) {
		sq = &squads[i];
		if ( sq->flankPending && level.time - sq->flankIssued > COD_FLANK_ORDER_MS ) {
			sq->flankPending = qfalse;
			sq->flanker = -1;
		}
		SqUpdatePush( sq );
		if ( sq->flanker < 0 && !SqPushActive( sq ) && level.time >= sq->nextFlank && sq->threatTime > level.time - 3000
			 && sq->holdSince && level.time - sq->holdSince > (int)( bg_cod.aiFlankHold * AICast_CodSkill( COD_SK_FLANK ) ) ) {
			SqPickFlanker( sq );
		}
		if ( bg_cod.aiDebug >= 1 && sq->num != sq->lastDbgNum && level.time >= sq->nextDbg ) {
			G_Printf( "[ai] sq%d members=%d tokens=%d/%d movers<=%d\n", sq->id, sq->num, SqTokensHeld( sq ), SqTokenLimit( sq ), sq->maxMovers );
			sq->lastDbgNum = sq->num;
			sq->nextDbg = level.time + 2000;
		}
	}
}

/*
============
AICast_CodSquadThink

codmod: called at the top of AIFunc_CodCombat. The first soldier to think after COD_UPDATE_MS (or after a
spawn / death) regroups everybody; the others find it done.
============
*/
void AICast_CodSquadThink( void ) {
	if ( !bg_cod.enable || !bg_cod.aiTactics || !bg_cod.aiSquad ) {
		return;
	}
	if ( sqLevelStart != level.startTime || sqNextUpdate > level.time + 2 * COD_UPDATE_MS ) {
		SqReset();
	}
	if ( level.time < sqNextUpdate && !sqDirty ) {
		return;
	}
	sqNextUpdate = level.time + COD_UPDATE_MS;
	sqDirty = qfalse;
	SqRebuild();
}

//===========================================================================
// blackboard
//===========================================================================

void AICast_CodSquadReportThreat( cast_state_t *cs, vec3_t pos ) {
	codSquad_t *sq = SquadOf( cs );

	if ( !sq ) {
		return;
	}
	VectorCopy( pos, sq->threatPos );
	sq->threatTime = level.time;
	if ( !sq->holdSince || Distance( pos, sq->anchor ) > bg_cod.aiFlankMove ) {
		VectorCopy( pos, sq->anchor );
		sq->holdSince = level.time;
		sq->tryMs = 0;          // Phase 2b: the player moved, the no-shot clock starts again
	}
}

qboolean AICast_CodSquadThreat( cast_state_t *cs, vec3_t out, int *time ) {
	codSquad_t *sq = SquadOf( cs );

	if ( !sq || !sq->threatTime || sq->enemy != cs->enemyNum ) {
		return qfalse;
	}
	VectorCopy( sq->threatPos, out );
	*time = sq->threatTime;
	return qtrue;
}

qboolean AICast_CodSquadCentre( cast_state_t *cs, vec3_t out ) {
	codSquad_t *sq = SquadOf( cs );

	if ( !sq ) {
		return qfalse;
	}
	VectorCopy( sq->centre, out );
	return qtrue;
}

//===========================================================================
// attack tokens
//===========================================================================

qboolean AICast_CodHasToken( cast_state_t *cs ) {
	codSquad_t *sq = SquadOf( cs );

	if ( !sq || sq->num <= SqTokenLimit( sq ) ) {
		return qtrue;
	}
	return ( members[cs->entityNum].tokenUntil > level.time ) ? qtrue : qfalse;
}

void AICast_CodTokenRelease( cast_state_t *cs ) {
	if ( cs->entityNum >= 0 && cs->entityNum < MAX_CLIENTS ) {
		members[cs->entityNum].tokenUntil = 0;
	}
}

// members that asked within COD_WAIT_MS and do not hold a token; 'ahead' counts those who asked before 'since'
static int SqWaiting( codSquad_t *sq, int self, int since, int *ahead ) {
	int i, n = 0;
	codMember_t *o;

	*ahead = 0;
	for ( i = 0; i < sq->num; i++ ) {
		if ( sq->members[i] == self ) {
			continue;
		}
		o = &members[sq->members[i]];
		if ( o->tokenUntil > level.time || !o->waitSince || o->lastDeny < level.time - COD_WAIT_MS ) {
			continue;
		}
		n++;
		if ( sq->flanker == sq->members[i] || SqPusherIndex( sq, sq->members[i] ) >= 0 || o->waitSince < since ) {
			( *ahead )++;
		}
	}
	return n;
}

/*
============
AICast_CodTokenAcquire

codmod: may this soldier shoot at the enemy right now? Grants or renews a lease of leaseMs. Without a squad, or
while the squad has no more members than tokens, always yes.
============
*/
qboolean AICast_CodTokenAcquire( cast_state_t *cs, int leaseMs ) {
	codSquad_t *sq = SquadOf( cs );
	codMember_t *m;
	int limit, held, ahead, waiting, ent = cs->entityNum;

	if ( !sq ) {
		return qtrue;
	}
	limit = SqTokenLimit( sq );
	if ( sq->num <= limit ) {
		return qtrue;
	}
	m = &members[ent];

	if ( m->tokenUntil > level.time ) {
		// renew; give way after a long hold when somebody is waiting
		waiting = SqWaiting( sq, ent, level.time, &ahead );
		if ( waiting > 0 && level.time - m->holdSince > (int)bg_cod.aiTokenTime && sq->flanker != ent && SqPusherIndex( sq, ent ) < 0 ) {
			m->tokenUntil = 0;
			m->noTokenUntil = level.time + 1500;
			if ( bg_cod.aiDebug >= 1 && level.time >= m->nextDbg ) {
				m->nextDbg = level.time + COD_DBG_MS;
				G_Printf( "[ai] %s sq%d token rotated out (%d waiting)\n", SqName( ent ), sq->id, waiting );
			}
			return qfalse;
		}
		m->tokenUntil = level.time + leaseMs;
		return qtrue;
	}

	// ask: at most 'limit' leases, and the longest-waiting soldiers first
	if ( !m->waitSince || m->lastDeny < level.time - 2500 ) {
		m->waitSince = level.time;
	}
	held = SqTokensHeld( sq );
	SqWaiting( sq, ent, m->waitSince, &ahead );
	if ( sq->flanker == ent || SqPusherIndex( sq, ent ) >= 0 ) {
		ahead = 0;
	}
	if ( level.time >= m->noTokenUntil && limit - held > ahead ) {
		m->tokenUntil = level.time + leaseMs;
		m->holdSince = level.time;
		m->waitSince = 0;
		if ( bg_cod.aiDebug >= 1 && level.time >= m->nextDbg ) {
			m->nextDbg = level.time + COD_DBG_MS;
			G_Printf( "[ai] %s sq%d token granted (%d/%d)\n", SqName( ent ), sq->id, held + 1, limit );
		}
		return qtrue;
	}
	if ( bg_cod.aiDebug >= 1 && level.time >= m->lastDeny + COD_WAIT_MS && level.time >= m->nextDbg ) {
		m->nextDbg = level.time + COD_DBG_MS;
		G_Printf( "[ai] %s sq%d token denied (%d/%d held)\n", SqName( ent ), sq->id, held, limit );
	}
	m->lastDeny = level.time;
	return qfalse;
}

//===========================================================================
// movers, flanking, suppression
//===========================================================================

qboolean AICast_CodMoverAcquire( cast_state_t *cs, int leaseMs, qboolean force ) {
	codSquad_t *sq = SquadOf( cs );
	codMember_t *m;
	int i, active = 0;

	if ( !sq ) {
		return qtrue;
	}
	m = &members[cs->entityNum];
	if ( m->moverUntil > level.time ) {
		m->moverUntil = level.time + leaseMs;
		return qtrue;
	}
	for ( i = 0; i < sq->num; i++ ) {
		if ( sq->members[i] != cs->entityNum && members[sq->members[i]].moverUntil > level.time ) {
			active++;
		}
	}
	if ( !force && active >= sq->maxMovers ) {
		return qfalse;
	}
	m->moverUntil = level.time + leaseMs;
	return qtrue;
}

void AICast_CodMoverRelease( cast_state_t *cs ) {
	if ( cs->entityNum >= 0 && cs->entityNum < MAX_CLIENTS ) {
		members[cs->entityNum].moverUntil = 0;
	}
}

/*
============
AICast_CodSquadCanAdvance

codmod: an advance is only worth it when somebody else sits in cover to watch the threat meanwhile
============
*/
qboolean AICast_CodSquadCanAdvance( cast_state_t *cs ) {
	codSquad_t *sq = SquadOf( cs );
	int i, active = 0;
	qboolean overwatch = qfalse;
	codMember_t *o;

	if ( !sq ) {
		return qfalse;
	}
	for ( i = 0; i < sq->num; i++ ) {
		if ( sq->members[i] == cs->entityNum ) {
			continue;
		}
		o = &members[sq->members[i]];
		if ( o->moverUntil > level.time ) {
			active++;
		} else if ( o->inCover && level.time - o->publishTime < 1000 ) {
			overwatch = qtrue;
		}
	}
	return ( overwatch && active < sq->maxMovers ) ? qtrue : qfalse;
}

void AICast_CodSquadMoverStarted( cast_state_t *cs, qboolean flank ) {
	codSquad_t *sq = SquadOf( cs );

	if ( !sq ) {
		return;
	}
	sq->suppressUntil = level.time + (int)bg_cod.aiSuppressMs;
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] %s sq%d %s starts moving, suppress %d ms\n", SqName( cs->entityNum ), sq->id, flank ? "FLANK" : "ADVANCE", (int)bg_cod.aiSuppressMs );
	}
	AICast_CodSquadBark( cs, flank ? COD_BARK_ORDERS : COD_BARK_ATTACK );
}

qboolean AICast_CodSquadFlankOrder( cast_state_t *cs ) {
	codSquad_t *sq = SquadOf( cs );

	return ( sq && sq->flanker == cs->entityNum && sq->flankPending ) ? qtrue : qfalse;
}

void AICast_CodSquadFlankResult( cast_state_t *cs, qboolean started ) {
	codSquad_t *sq = SquadOf( cs );

	if ( !sq || sq->flanker != cs->entityNum ) {
		return;
	}
	sq->flankPending = qfalse;
	if ( !started ) {
		sq->flanker = -1;
		sq->nextFlank = level.time + 8000;      // no way around from here, try again later
		if ( bg_cod.aiDebug >= 1 ) {
			G_Printf( "[ai] %s sq%d FLANK found no route\n", SqName( cs->entityNum ), sq->id );
		}
	}
}

void AICast_CodSquadFlankDone( cast_state_t *cs ) {
	codSquad_t *sq = SquadOf( cs );

	if ( sq && sq->flanker == cs->entityNum ) {
		sq->flanker = -1;
		if ( bg_cod.aiDebug >= 1 ) {
			G_Printf( "[ai] %s sq%d FLANK in position\n", SqName( cs->entityNum ), sq->id );
		}
	}
}

/*
============
AICast_CodSquadSuppress

codmod: qtrue (and the point to fire at) while the suppression window of a teammate's move is open
============
*/
qboolean AICast_CodSquadSuppress( cast_state_t *cs, vec3_t out ) {
	codSquad_t *sq = SquadOf( cs );
	int i;

	if ( !sq || sq->suppressUntil <= level.time || !sq->threatTime || sq->threatTime < level.time - 8000 || sq->enemy != cs->enemyNum ) {
		return qfalse;
	}
	for ( i = 0; i < sq->num; i++ ) {
		if ( sq->members[i] != cs->entityNum && members[sq->members[i]].moverUntil > level.time ) {
			VectorCopy( sq->threatPos, out );
			return qtrue;
		}
	}
	return qfalse;
}

//===========================================================================
// callouts
//===========================================================================

/*
============
AICast_CodSquadBark

codmod: one callout per squad per cod_aiBarkMs, with the character's own sound script (ORDERSSOUNDSCRIPT
for a flank / advance order, ATTACKSOUNDSCRIPT when a mover starts), the way the vanilla code plays them
============
*/
void AICast_CodSquadBark( cast_state_t *cs, int kind ) {
	codSquad_t *sq = SquadOf( cs );
	gentity_t *ent = &g_entities[cs->entityNum];
	char *script;
	int *next;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS ) {
		return;
	}
	// Phase 3: a soldier outside any squad (lone survivor, solo grenade thrower) has its own rate limit
	next = sq ? &sq->nextBark : &members[cs->entityNum].nextBarkSolo;
	if ( level.time < *next ) {
		return;
	}
	if ( ( cs->aiFlags & AIFL_DENYACTION ) || cs->castScriptStatus.scriptNoAttackTime >= level.time ) {
		return;
	}
	script = aiDefaults[ent->aiCharacter].soundScripts[kind == COD_BARK_ORDERS ? ORDERSSOUNDSCRIPT : ATTACKSOUNDSCRIPT];
	if ( !script || !script[0] ) {
		return;
	}
	*next = level.time + (int)( bg_cod.aiBarkMs * ( 0.8f + 0.5f * random() ) );
	G_AddEvent( ent, EV_GENERAL_SOUND, G_SoundIndex( script ) );
}

//===========================================================================
// push for an angle (Phase 2b): API used by ai_cast_cod.c
//===========================================================================

/*
============
AICast_CodSquadShotReport

codmod: a token holder tried to shoot (OPEN / PEEK): did it have a real shot (AICast_CheckAttack)? A shot
resets the squad's no-shot clock. Without one the clock advances by the time since the last report, capped at
250 ms, so hiding time is not counted, only time somebody actually tried.
============
*/
void AICast_CodSquadShotReport( cast_state_t *cs, qboolean shot ) {
	codSquad_t *sq = SquadOf( cs );
	int dt;

	if ( !sq ) {
		return;
	}
	if ( shot ) {
		sq->lastShot = level.time;
		sq->tryMs = 0;
		sq->lastTryAcc = level.time;
		return;
	}
	dt = level.time - sq->lastTryAcc;
	if ( dt > 6000 || dt < 0 ) {
		sq->tryMs = 0;      // long silence: a different situation
		dt = 0;
	} else if ( dt > 250 ) {
		dt = 250;
	}
	sq->lastTryAcc = level.time;
	sq->tryMs += dt;
}

qboolean AICast_CodSquadThreatFresh( cast_state_t *cs ) {
	codSquad_t *sq = SquadOf( cs );

	return ( sq && SqThreatFresh( sq ) ) ? qtrue : qfalse;
}

qboolean AICast_CodSquadPushOrder( cast_state_t *cs ) {
	codSquad_t *sq = SquadOf( cs );
	int idx;

	if ( !sq || !bg_cod.aiPush ) {
		return qfalse;
	}
	idx = SqPusherIndex( sq, cs->entityNum );
	return ( idx >= 0 && sq->pushPending[idx] ) ? qtrue : qfalse;
}

qboolean AICast_CodSquadPushing( cast_state_t *cs ) {
	codSquad_t *sq = SquadOf( cs );

	return ( sq && SqPusherIndex( sq, cs->entityNum ) >= 0 ) ? qtrue : qfalse;
}

/*
============
AICast_CodSquadPushStarted

codmod: the pusher is on its way. Opens the suppression window for the whole run (the travel time, capped at
4 s, at least cod_aiSuppressMs) and calls it out (ORDERSSOUNDSCRIPT).
============
*/
void AICast_CodSquadPushStarted( cast_state_t *cs, int travelMs ) {
	codSquad_t *sq = SquadOf( cs );
	int idx, window, until;

	if ( !sq ) {
		return;
	}
	idx = SqPusherIndex( sq, cs->entityNum );
	if ( idx >= 0 ) {
		sq->pushPending[idx] = qfalse;
	}
	window = travelMs + 600;
	if ( window < (int)bg_cod.aiSuppressMs ) {
		window = (int)bg_cod.aiSuppressMs;
	}
	if ( window > 4000 ) {
		window = 4000;
	}
	until = level.time + window;
	if ( until > sq->suppressUntil ) {
		sq->suppressUntil = until;
	}
	AICast_CodSquadBark( cs, COD_BARK_ORDERS );
}

void AICast_CodSquadPushResult( cast_state_t *cs, qboolean started ) {
	codSquad_t *sq = SquadOf( cs );
	int idx;

	if ( !sq || started ) {
		return;
	}
	idx = SqPusherIndex( sq, cs->entityNum );
	if ( idx >= 0 ) {
		sq->pusher[idx] = -1;
		sq->pushPending[idx] = qfalse;
	}
	sq->nextPush = level.time + 5000;       // nowhere to push to from here, look again later
}

/*
============
AICast_CodSquadPushDone

codmod: the push is over (shot found, no shot, died). Starts the squad cooldown (shorter after a failure) and
restarts the no-shot clock.
============
*/
void AICast_CodSquadPushDone( cast_state_t *cs, qboolean ok ) {
	codSquad_t *sq = SquadOf( cs );
	int idx;

	if ( !sq ) {
		return;
	}
	idx = SqPusherIndex( sq, cs->entityNum );
	if ( idx < 0 ) {
		return;
	}
	sq->pusher[idx] = -1;
	sq->pushPending[idx] = qfalse;
	sq->nextPush = level.time + (int)( bg_cod.aiPushCooldown * ( ok ? 1.0f : 0.5f ) );
	sq->tryMs = 0;
}

//===========================================================================
// Phase 3: threat reactions
//===========================================================================

/*
============
AICast_CodSquadGrenadeThrower

codmod: may this soldier throw a grenade at threatPos? The squad cooldown must be over and this soldier must be
the member that scores best (AICast_CodGrenadeScore: has a grenade, distance and arc), re-evaluated every 600 ms so
only one member throws. Always qtrue for a soldier outside a squad (the caller keeps its own cooldown).
============
*/
qboolean AICast_CodSquadGrenadeThrower( cast_state_t *cs, vec3_t threatPos ) {
	codSquad_t *sq = SquadOf( cs );
	int i, best = -1, score, bestScore = 0;

	if ( !sq ) {
		return qtrue;
	}
	if ( level.time < sq->nextGrenade ) {
		return qfalse;
	}
	if ( level.time >= sq->nextGrenadeEval ) {
		sq->nextGrenadeEval = level.time + 600;
		for ( i = 0; i < sq->num; i++ ) {
			cast_state_t *mcs = AICast_GetCastState( sq->members[i] );

			if ( !mcs ) {
				continue;
			}
			score = AICast_CodGrenadeScore( mcs, threatPos );
			if ( score > bestScore ) {
				bestScore = score;
				best = sq->members[i];
			}
		}
		sq->grenadeThrower = best;
	}
	return ( sq->grenadeThrower == cs->entityNum ) ? qtrue : qfalse;
}

/*
============
AICast_CodSquadGrenadeThrown

codmod: a grenade is on its way: squad cooldown (cod_aiGrenadeCooldown, +-15 %) and a callout
============
*/
void AICast_CodSquadGrenadeThrown( cast_state_t *cs ) {
	codSquad_t *sq = SquadOf( cs );

	if ( sq ) {
		sq->nextGrenade = level.time + (int)( bg_cod.aiGrenadeCooldown * AICast_CodSkill( COD_SK_GRENADE_CD ) * ( 0.85f + 0.3f * random() ) );
		sq->grenadeThrower = -1;
	}
	AICast_CodSquadBark( cs, COD_BARK_ATTACK );
}

int AICast_CodSquadLastShot( cast_state_t *cs ) {
	codSquad_t *sq = SquadOf( cs );

	return sq ? sq->lastShot : 0;
}

qboolean AICast_CodSquadPushActive( cast_state_t *cs ) {
	codSquad_t *sq = SquadOf( cs );

	return ( sq && SqPushActive( sq ) ) ? qtrue : qfalse;
}

void AICast_CodSquadOpportunity( cast_state_t *cs, int ms ) {
	codSquad_t *sq = SquadOf( cs );

	if ( sq && level.time + ms > sq->oppUntil ) {
		sq->oppUntil = level.time + ms;
	}
}

qboolean AICast_CodSquadMoraleActive( cast_state_t *cs ) {
	codMember_t *m;

	if ( cs->entityNum < 0 || cs->entityNum >= MAX_CLIENTS ) {
		return qfalse;
	}
	m = &members[cs->entityNum];
	return ( m->moraleLevel == level.startTime && m->moraleUntil > level.time ) ? qtrue : qfalse;
}

// qtrue once per soldier for each morale break
qboolean AICast_CodSquadMoraleFresh( cast_state_t *cs ) {
	codMember_t *m;

	if ( !AICast_CodSquadMoraleActive( cs ) ) {
		return qfalse;
	}
	m = &members[cs->entityNum];
	if ( m->moraleSeen == m->moraleSerial ) {
		return qfalse;
	}
	m->moraleSeen = m->moraleSerial;
	return qtrue;
}

/*
============
SqMoraleDeath

codmod: a squad member died. When the squad has lost half of its peak strength (3 or more at its peak), every
survivor, also those that will be alone after this, breaks: it falls back to cover farther from the player and
stays cautious for 9 s (AICast_CodSquadMoraleFresh / MoraleActive). Squad cooldown 15 s.
============
*/
static void SqMoraleDeath( int entNum ) {
	codMember_t *dm = &members[entNum];
	codSquad_t *sq;
	int i, alive = 0;

	if ( !bg_cod.enable || !bg_cod.aiTactics || !bg_cod.aiMorale || !SqUsable() ) {
		return;
	}
	if ( dm->stamp != sqStamp || dm->squad < 0 || dm->squad >= COD_MAX_SQUADS ) {
		return;
	}
	sq = &squads[dm->squad];
	if ( !sq->active ) {
		return;
	}
	sq->killed++;
	if ( sq->peak < 3 || sq->killed * 2 < sq->peak || level.time < sq->nextMorale ) {
		return;
	}
	for ( i = 0; i < sq->num; i++ ) {
		gentity_t *ent = &g_entities[sq->members[i]];
		codMember_t *m = &members[sq->members[i]];

		if ( sq->members[i] == entNum || !ent->inuse || ent->health <= 0 ) {
			continue;
		}
		m->moraleUntil = level.time + 9000;
		m->moraleLevel = level.startTime;
		m->moraleSerial++;
		alive++;
	}
	if ( bg_cod.aiDebug >= 1 ) {
		G_Printf( "[ai] sq%d MORALE broken (lost %d of %d, %d survivors fall back)\n", sq->id, sq->killed, sq->peak, alive );
	}
	sq->nextMorale = level.time + 15000;
	sq->peak = alive;
	sq->killed = 0;
}
