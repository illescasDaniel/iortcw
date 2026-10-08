// codmod: new file (phase 2.0) -- cg_cod.c
//
// Debug HUD and dump command for the cod_* tuning values. See docs/architecture.md
// section 6. Extended in later phases (ADS, slide, prone).

#include "cg_local.h"

// codmod: Phase 3.5 per-weapon iron-sight ADS. Primary pose is computed every frame from
// tag_flash (see CG_CodApplyIronSightAds) so the rear sight sits on the screen center like
// CoD -- no new models required. Table fields:
//   sightHeight / barrelBack / sightDist  -- geometry of the sight line (0 sightDist => cvar)
//   fwd/right/up/pitch/yaw/roll           -- fine bias after auto-align (tune these live)
// Not const: cod_adsTune overwrites the fine-bias fields while tuning.
codAdsView_t cg_codAdsView[WP_NUM_WEAPONS] = {
	// Geometry biases are 0 -- live defaults live in cod_adsSightHeight / BarrelBack /
	// SightDist cvars so console tuning works. Nonzero here = extra per-weapon bias.
	//                          sightH  barrelB  sightD   fwd   right  up   pitch yaw roll
	[WP_LUGER]          = { 0, 0, 0,   0, 0, 0,  0, 0, 0 },
	[WP_SILENCER]       = { 0, 2, 0,   0, 0, 0,  0, 0, 0 },
	[WP_COLT]           = { 0, 0, 0,   0, 0, 0,  0, 0, 0 },

	[WP_MP40]           = { 0.5f, 4, 2,   0, 0, 0,  0, 0, 0 },
	[WP_THOMPSON]       = { 0.5f, 4, 2,   0, 0, 0,  0, 0, 0 },
	[WP_STEN]           = { 0.3f, 3, 2,   0, 0, 0,  0, 0, 0 },

	[WP_MAUSER]         = { 1.0f, 12, 4,   0, 0, 0,  0, 0, 0 },
	[WP_GARAND]         = { 1.0f, 12, 4,   0, 0, 0,  0, 0, 0 },
	[WP_FG42]           = { 0.5f,  8, 3,   0, 0, 0,  0, 0, 0 },
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
CG_CodSprintFrac

codmod: eased 0..1 sprint gun-lower fraction (phase 4.1). Visual only — firing is gated in
pmove by clearing COD_SF_SPRINTING on BUTTON_ATTACK, not by this value. Raises/lowers over
cod_sprintRaiseTime. Suppressed while ADS so iron sights don't fight the sprint pose.
================
*/
float CG_CodSprintFrac( void ) {
	float target;
	float raiseMs;
	float dt;
	int now;

	if ( !( bg_cod.enable && bg_cod.gaitEnable ) ) {
		cg.codSprintFrac = 0.0f;
		cg.codSprintFracTime = cg.time;
		return 0.0f;
	}

	if ( CG_CodAdsFrac() > 0.01f ) {
		target = 0.0f;
	} else if ( cg.predictedPlayerState.stats[STAT_COD_FLAGS] & COD_SF_SPRINTING ) {
		target = 1.0f;
	} else {
		target = 0.0f;
	}

	raiseMs = bg_cod.sprintRaiseTime;
	if ( raiseMs < 1.0f ) {
		raiseMs = 1.0f;
	}

	now = cg.time;
	if ( cg.codSprintFracTime <= 0 || now < cg.codSprintFracTime ) {
		cg.codSprintFracTime = now;
	}
	dt = (float)( now - cg.codSprintFracTime ) / raiseMs;
	cg.codSprintFracTime = now;
	if ( dt < 0.0f ) {
		dt = 0.0f;
	} else if ( dt > 1.0f ) {
		dt = 1.0f;
	}

	if ( cg.codSprintFrac < target ) {
		cg.codSprintFrac += dt;
		if ( cg.codSprintFrac > target ) {
			cg.codSprintFrac = target;
		}
	} else if ( cg.codSprintFrac > target ) {
		cg.codSprintFrac -= dt;
		if ( cg.codSprintFrac < target ) {
			cg.codSprintFrac = target;
		}
	}

	return cg.codSprintFrac;
}

/*
================
CG_CodGaitBobAmp

codmod: walk / jog / sprint bob amplitude multiplier for weapon and camera (phase 4.2).
================
*/
float CG_CodGaitBobAmp( void ) {
	usercmd_t cmd;
	int cmdNum;

	if ( !( bg_cod.enable && bg_cod.gaitEnable ) ) {
		return 1.0f;
	}

	if ( cg.predictedPlayerState.stats[STAT_COD_FLAGS] & COD_SF_SPRINTING ) {
		return bg_cod.bobSprintAmp;
	}

	cmdNum = trap_GetCurrentCmdNumber();
	trap_GetUserCmd( cmdNum, &cmd );
	if ( cmd.buttons & BUTTON_WALKING ) {
		return bg_cod.bobWalkAmp;
	}

	return bg_cod.bobRunAmp;
}

/*
================
CG_CodResolveFlashTag

codmod: helper for iron-sight ADS. Fills world-space muzzle origin and bore forward from
the hand+gun pose. Returns qfalse if tag_flash is missing.
================
*/
static qboolean CG_CodResolveFlashTag( refEntity_t *hand, refEntity_t *gun, vec3_t flashOrigin, vec3_t flashFwd ) {
	orientation_t flash;
	vec3_t flashAxis[3];
	int i;

	CG_PositionEntityOnTag( gun, hand, "tag_weapon", 0, NULL );
	if ( trap_R_LerpTag( &flash, gun, "tag_flash", 0 ) < 0 ) {
		return qfalse;
	}

	VectorCopy( gun->origin, flashOrigin );
	for ( i = 0; i < 3; i++ ) {
		VectorMA( flashOrigin, flash.origin[i], gun->axis[i], flashOrigin );
	}
	MatrixMultiply( flash.axis, gun->axis, flashAxis );
	VectorCopy( flashAxis[0], flashFwd );
	VectorNormalize( flashFwd );
	return qtrue;
}

/*
================
CG_CodSightPoint

codmod: rear-sight estimate = back along the bore from the muzzle, then up toward view-up
(projected off the bore). View-up is used instead of flaky tag Z so the sight line clears
the slide instead of diving into the mesh.
================
*/
static void CG_CodSightPoint( const vec3_t flashOrigin, const vec3_t flashFwd,
							  float barrelBack, float sightHeight, vec3_t sightPos ) {
	vec3_t boreUp;
	float d;

	VectorCopy( cg.refdef.viewaxis[2], boreUp );
	d = DotProduct( boreUp, flashFwd );
	VectorMA( boreUp, -d, flashFwd, boreUp );
	if ( VectorNormalize( boreUp ) < 0.001f ) {
		CrossProduct( cg.refdef.viewaxis[1], flashFwd, boreUp );
		VectorNormalize( boreUp );
	}
	if ( DotProduct( boreUp, cg.refdef.viewaxis[2] ) < 0.0f ) {
		VectorInverse( boreUp );
	}

	VectorMA( flashOrigin, -barrelBack, flashFwd, sightPos );
	VectorMA( sightPos, sightHeight, boreUp, sightPos );
}

/*
================
CG_CodApplyIronSightAds

codmod: CoD-style iron-sight ADS without new models (phase 3.5).

Must be called with the same hands animation frame that will be drawn -- tag_weapon (and
therefore the bore) moves with the anim; aligning against frame 0 left a permanent diagonal.

Align:
  1) iteratively straighten pitch/yaw so tag_flash forward matches view forward (tag
     hierarchy is not a 1:1 euler map, so one shot is not enough)
  2) level roll to the view (drops idle/bob tilt)
  3) translate so the rear sight lands on the optical axis at sightDist
Blend the fully-aligned pose back onto the hip pose by e. Fine bias from cg_codAdsView and
cod_adsGunFovComp is applied on top. Falls back to bias-only if tags are missing.
================
*/
void CG_CodApplyIronSightAds( vec3_t handOrigin, vec3_t angles, int weaponNum, float e,
							  int frame, int oldframe, float backlerp ) {
	weaponInfo_t *weapon;
	codAdsView_t *ads;
	refEntity_t hand;
	refEntity_t gun;
	vec3_t flashOrigin;
	vec3_t flashFwd;
	vec3_t sightPos;
	vec3_t desired;
	vec3_t delta;
	vec3_t flashAngles, viewAngles;
	vec3_t alignedOrigin, alignedAngles;
	float sightHeight, barrelBack, sightDist;
	float fovScale, fovComp;
	float deltaLen;
	int i, iter;

	if ( e <= 0.0f || weaponNum <= WP_NONE || weaponNum >= WP_NUM_WEAPONS ) {
		return;
	}

	weapon = &cg_weapons[weaponNum];
	ads = &cg_codAdsView[weaponNum];

	// codmod: live cvars are the knobs (set/seta in console). Per-weapon table values are
	// ADDED as a bias on top -- previously a nonzero table entry replaced the cvar entirely,
	// so `seta cod_adsSightHeight` looked like a no-op on the Luger.
	sightHeight = bg_cod.adsSightHeight + ads->sightHeight;
	barrelBack  = bg_cod.adsBarrelBack  + ads->barrelBack;
	sightDist   = bg_cod.adsSightDist   + ads->sightDist;

	// codmod: too small = looking inside the mesh; too large = floating mid-screen arms.
	// Pistol defaults from codmod_tune.log (Dist 20 / Height 2.5 / Yaw -3.5 / Roll 6.5).
	if ( sightDist < 8.0f ) {
		sightDist = 8.0f;
	}
	if ( sightDist > 22.0f ) {
		sightDist = 22.0f;
	}

	fovScale = ( bg_cod.adsTune && weaponNum == cg.weaponSelect )
			   ? bg_cod.adsTuneFov : BG_CodAdsFovScale( weaponNum );
	fovComp = bg_cod.adsGunFovComp * ( 1.0f - fovScale );

	VectorCopy( handOrigin, alignedOrigin );
	VectorCopy( angles, alignedAngles );

	memset( &hand, 0, sizeof( hand ) );
	VectorCopy( handOrigin, hand.origin );
	hand.hModel = weapon->handsModel;
	// codmod: MUST match the drawn pose -- see call site in CG_AddViewWeapon.
	hand.frame = frame;
	hand.oldframe = oldframe;
	hand.backlerp = backlerp;

	memset( &gun, 0, sizeof( gun ) );
	gun.hModel = weapon->weaponModel[W_FP_MODEL];
	gun.frame = gun.oldframe = 0;
	gun.backlerp = 0;

	if ( hand.hModel && gun.hModel ) {
		vectoangles( cg.refdef.viewaxis[0], viewAngles );

		// Pass 1: iterative bore straighten. Hand euler deltas only approximate the tag
		// chain, so re-measure flashFwd after each correction until it matches the view.
		for ( iter = 0; iter < 4; iter++ ) {
			VectorCopy( alignedOrigin, hand.origin );
			AnglesToAxis( alignedAngles, hand.axis );
			if ( !CG_CodResolveFlashTag( &hand, &gun, flashOrigin, flashFwd ) ) {
				break;
			}
			vectoangles( flashFwd, flashAngles );
			alignedAngles[PITCH] += AngleDelta( viewAngles[PITCH], flashAngles[PITCH] );
			alignedAngles[YAW]   += AngleDelta( viewAngles[YAW],   flashAngles[YAW] );
			if ( fabs( AngleDelta( viewAngles[PITCH], flashAngles[PITCH] ) ) < 0.05f &&
				 fabs( AngleDelta( viewAngles[YAW],   flashAngles[YAW] ) )   < 0.05f ) {
				break;
			}
		}

		// Level the gun to the view (kills idle/bob roll that reads as a crooked ADS).
		alignedAngles[ROLL] += AngleDelta( cg.refdefViewAngles[ROLL], alignedAngles[ROLL] );

		// Mesh hold bias BEFORE sight centering -- applying yaw/roll after translate left the
		// sights off-axis and kept ADS looking diagonal vs CoD.
		alignedAngles[PITCH] += bg_cod.adsPitch + ads->pitch;
		alignedAngles[YAW]   += bg_cod.adsYaw   + ads->yaw;
		alignedAngles[ROLL]  += bg_cod.adsRoll  + ads->roll;

		// Pass 2: at the biased orientation, put the rear sight on the optical axis.
		VectorCopy( alignedOrigin, hand.origin );
		AnglesToAxis( alignedAngles, hand.axis );
		if ( CG_CodResolveFlashTag( &hand, &gun, flashOrigin, flashFwd ) ) {
			CG_CodSightPoint( flashOrigin, flashFwd, barrelBack, sightHeight, sightPos );
			VectorMA( cg.refdef.vieworg, sightDist + fovComp, cg.refdef.viewaxis[0], desired );
			VectorSubtract( desired, sightPos, delta );

			// Cap wild tag deltas so a bad resolve can't fling the gun mid-scene.
			deltaLen = VectorLength( delta );
			if ( deltaLen > 40.0f ) {
				VectorScale( delta, 40.0f / deltaLen, delta );
			}
			VectorAdd( alignedOrigin, delta, alignedOrigin );
		}
	}

	// Blend hip pose -> fully aligned pose by ADS fraction.
	for ( i = 0; i < 3; i++ ) {
		handOrigin[i] += ( alignedOrigin[i] - handOrigin[i] ) * e;
		angles[i] += AngleDelta( alignedAngles[i], angles[i] ) * e;
	}

	// Position-only fine bias. Angle bias is already in alignedAngles.
	// cod_adsRight shifts the whole gun horizontally (plus per-weapon ads->right).
	VectorMA( handOrigin, ads->fwd   * e, cg.refdef.viewaxis[0], handOrigin );
	VectorMA( handOrigin, ( bg_cod.adsRight + ads->right ) * e, cg.refdef.viewaxis[1], handOrigin );
	VectorMA( handOrigin, ads->up    * e, cg.refdef.viewaxis[2], handOrigin );
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
		CG_Printf( "[%s] = { %gf, %gf, %gf,  %gf, %gf, %gf,  %gf, %gf, %gf },\n",
				   name,
				   ads->sightHeight, ads->barrelBack, ads->sightDist,
				   ads->fwd, ads->right, ads->up,
				   ads->pitch, ads->yaw, ads->roll );
	} else {
		CG_Printf( "[%d] = { %gf, %gf, %gf,  %gf, %gf, %gf,  %gf, %gf, %gf },\n",
				   weapon,
				   ads->sightHeight, ads->barrelBack, ads->sightDist,
				   ads->fwd, ads->right, ads->up,
				   ads->pitch, ads->yaw, ads->roll );
	}
}

// codmod: phase 3.8 scope-hold edge/queue tracking. File-static is fine -- CG_DrawActiveFrame
// calls CG_CodUpdateScopeHold() exactly once per real (unpredicted) frame, never rewound.
static qboolean codScopeLastHeld;    // last frame's WBUTTON_COD_ADS state, for edge detection
static int codScopeQueuedWant = -1;  // -1 = nothing queued, 0 = queued release, 1 = queued press

/*
================
CG_CodUpdateScopeHold

codmod: phase 3.8 -- "every weapon aims with right-click" extends to the three scoped
rifles. Holding WBUTTON_COD_ADS on an unscoped rifle whose scope is selectable switches to
the real scope the same way `weapalt` does (CG_CodAltWeapon); releasing switches back.
`weapalt` itself still works independently (CG_AltWeapon_f). Called every frame from
CG_DrawActiveFrame, before the usercmd weapon selection is read, so cg.weaponSelect (and the
usercmd it feeds) reflects the hold this frame. See docs/phase-3-ads.md section 3.8.
================
*/
void CG_CodUpdateScopeHold( void ) {
	usercmd_t cmd;
	qboolean adsHeld;
	int weapon;
	qboolean isScopeWeapon;
	qboolean reloading;

	if ( !cg.snap ) {
		return;
	}

	if ( !( bg_cod.enable && bg_cod.adsEnable ) ) {
		cg.codScopeByAds = qfalse;
		codScopeQueuedWant = -1;
		return;
	}

	isScopeWeapon = BG_CodIsScopeWeapon( cg.weaponSelect );

	// codmod: unexpected weapon change (empty-mag auto-switch in cg_event.c, the
	// speed-based auto-drop in CG_DrawActiveFrame, a manual number-key switch, or death) --
	// something switched us away from the scope without going through our own release path
	// below. Clear the bookkeeping; nothing to undo, the weapon is already gone.
	if ( cg.codScopeByAds && !isScopeWeapon ) {
		cg.codScopeByAds = qfalse;
		codScopeQueuedWant = -1;
	}

	trap_GetUserCmd( trap_GetCurrentCmdNumber(), &cmd );
	adsHeld = ( cmd.wbuttons & WBUTTON_COD_ADS ) ? qtrue : qfalse;

	reloading = (qboolean)( cg.snap->ps.weaponstate == WEAPON_RELOADING );

	// apply a queued edge once the weapon settles -- never while reloading (vanilla rule,
	// docs/phase-3-ads.md section 3.9)
	if ( codScopeQueuedWant >= 0 && cg.snap->ps.weaponstate == WEAPON_READY ) {
		CG_CodAltWeapon( qtrue );
		cg.codScopeByAds = ( codScopeQueuedWant == 1 ) ? qtrue : qfalse;
		codScopeQueuedWant = -1;
	}

	if ( adsHeld == codScopeLastHeld ) {
		return;   // no press/release edge this frame
	}
	codScopeLastHeld = adsHeld;

	if ( reloading ) {
		return;   // never touch alt-fire mid reload (vanilla rule)
	}

	weapon = cg.weaponSelect;

	if ( adsHeld ) {
		// press: only an unscoped rifle with a selectable scope alt is eligible. Plain
		// iron-sight ADS (PM_CodUpdateADS, bg_pmove.c) handles every other weapon.
		if ( isScopeWeapon || !CG_CodHasSelectableAlt( weapon ) ) {
			return;
		}

		if ( cg.snap->ps.weaponstate == WEAPON_RAISING || cg.snap->ps.weaponstate == WEAPON_RAISING_TORELOAD
			 || cg.snap->ps.weaponstate == WEAPON_DROPPING || cg.snap->ps.weaponstate == WEAPON_DROPPING_TORELOAD ) {
			codScopeQueuedWant = 1;   // mid weapon-change, apply once WEAPON_READY
		} else {
			CG_CodAltWeapon( qtrue );
			cg.codScopeByAds = qtrue;
		}
	} else {
		// release: only undo a scope *we* entered. If the player scoped with `weapalt`
		// (cg.codScopeByAds not set), releasing ADS does nothing -- the vanilla workflow
		// keeps working.
		if ( !cg.codScopeByAds ) {
			return;
		}

		if ( cg.snap->ps.weaponstate == WEAPON_RAISING || cg.snap->ps.weaponstate == WEAPON_RAISING_TORELOAD
			 || cg.snap->ps.weaponstate == WEAPON_DROPPING || cg.snap->ps.weaponstate == WEAPON_DROPPING_TORELOAD ) {
			codScopeQueuedWant = 0;
		} else {
			CG_CodAltWeapon( qtrue );
			cg.codScopeByAds = qfalse;
		}
	}
}

/*
================
CG_CodStateName

codmod: picks the debug HUD state label. Priority matches the order in architecture.md
section 6 (RUN|WALK|SPRINT|CROUCH|AIR); PRONE/CRAWL/SLIDE are added in phases 5/5b.
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

	// codmod: ads frac + current fov (phase 3.7)
	Com_sprintf( line, sizeof( line ), "ads %.2f  fov %.1f", ps->stats[STAT_COD_ADS] / 1000.0f, cg.refdef.fov_x );
	CG_DrawSmallStringColor( 4, y, line, color );
	y += SMALLCHAR_HEIGHT;

	// codmod: visual sprint gun-lower fraction + gait bob amp (phase 4)
	Com_sprintf( line, sizeof( line ), "sprint %.2f  bobAmp %.2f", CG_CodSprintFrac(), CG_CodGaitBobAmp() );
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

/*
================
CG_ToggleAds_f

codmod: sticky ADS toggle (phase 3.1). ioRTCW SP has no console `alias`/`vstr` toggle
trick, so this command alternates +ads / -ads via the console. Bind with
`bind MOUSE2 toggleads` instead of the default hold bind.
================
*/
void CG_ToggleAds_f( void ) {
	static qboolean held;

	held = !held;
	if ( held ) {
		trap_SendConsoleCommand( "+ads\n" );
	} else {
		trap_SendConsoleCommand( "-ads\n" );
	}
}
