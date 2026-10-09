// codmod: new file (phase 2.0) -- cg_cod.c
//
// Debug HUD and dump command for the cod_* tuning values. See docs/architecture.md
// section 6. Extended in later phases (ADS, slide, prone).

#include "cg_local.h"
#include "../client/keycodes.h" // codmod: K_SPACE for toggle-crouch auto-stand (phase 5d)

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
CG_CodSlideFrac

codmod: eased 0..1 slide camera/gun fraction (phase 6). Tracks remaining slide time with
smoothstep, and fades in over ~120 ms from the PMF_COD_SLIDING edge.
================
*/
float CG_CodSlideFrac( void ) {
	playerState_t *ps;
	float s, fadeIn, slideTime;

	ps = &cg.predictedPlayerState;
	if ( !( bg_cod.enable && bg_cod.slideEnable )
		 || !( ps->pm_flags & PMF_COD_SLIDING )
		 || ps->stats[STAT_COD_SLIDE] <= 0 ) {
		cg.codSlideStartTime = 0;
		return 0.0f;
	}

	if ( cg.codSlideStartTime <= 0 ) {
		cg.codSlideStartTime = cg.time;
	}

	slideTime = bg_cod.slideTime;
	if ( slideTime < 1.0f ) {
		slideTime = 1.0f;
	}
	s = (float)ps->stats[STAT_COD_SLIDE] / slideTime;
	if ( s < 0.0f ) {
		s = 0.0f;
	} else if ( s > 1.0f ) {
		s = 1.0f;
	}
	s = BG_CodSmoothstep( s );

	fadeIn = (float)( cg.time - cg.codSlideStartTime ) / 120.0f;
	if ( fadeIn < 0.0f ) {
		fadeIn = 0.0f;
	} else if ( fadeIn > 1.0f ) {
		fadeIn = 1.0f;
	}
	fadeIn = BG_CodSmoothstep( fadeIn );

	return s * fadeIn;
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
section 6 (RUN|WALK|SPRINT|CROUCH|PRONE|CRAWL|SLIDE|AIR).
================
*/
static const char *CG_CodStateName( void ) {
	playerState_t *ps;
	usercmd_t cmd;

	ps = &cg.predictedPlayerState;

	if ( ps->groundEntityNum == ENTITYNUM_NONE ) {
		return "AIR";
	}
	if ( ps->pm_flags & PMF_COD_SLIDING ) {
		return "SLIDE";
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

	// codmod: landing slowdown timer (phase 5)
	if ( ps->stats[STAT_COD_LAND] > 0 ) {
		Com_sprintf( line, sizeof( line ), "landSlow %d ms", ps->stats[STAT_COD_LAND] );
		CG_DrawSmallStringColor( 4, y, line, color );
		y += SMALLCHAR_HEIGHT;
	}

	// codmod: slide remaining / cooldown (phase 6)
	if ( ps->stats[STAT_COD_SLIDE] > 0 ) {
		Com_sprintf( line, sizeof( line ), "slide %d ms  frac %.2f",
					 ps->stats[STAT_COD_SLIDE], CG_CodSlideFrac() );
		CG_DrawSmallStringColor( 4, y, line, color );
		y += SMALLCHAR_HEIGHT;
	} else if ( ps->stats[STAT_COD_SLIDE] < 0 ) {
		Com_sprintf( line, sizeof( line ), "slide cooldown %d ms", -ps->stats[STAT_COD_SLIDE] );
		CG_DrawSmallStringColor( 4, y, line, color );
		y += SMALLCHAR_HEIGHT;
	}

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

/*
================
CG_ToggleCrouch_f / CG_CodUpdateToggleCrouch

codmod: sticky crouch toggle (phase 5d). Alternates +movedown/-movedown. Jump
(SPACE) auto-stands so upmove is not cancelled (jump and crouch share one axis).
================
*/
void CG_ToggleCrouch_f( void ) {
	cg.codCrouchHeld = !cg.codCrouchHeld;
	if ( cg.codCrouchHeld ) {
		trap_SendConsoleCommand( "+movedown\n" );
	} else {
		trap_SendConsoleCommand( "-movedown\n" );
	}
}

void CG_CodUpdateToggleCrouch( void ) {
	if ( !cg.codCrouchHeld ) {
		return;
	}
	// Default jump is SPACE; clear sticky crouch so jump works.
	if ( trap_Key_IsDown( K_SPACE ) ) {
		cg.codCrouchHeld = qfalse;
		trap_SendConsoleCommand( "-movedown\n" );
	}
}

/*
================
CG_ToggleFlashlight_f / CG_CodAddFlashlight

codmod: client-local view flashlight (phase 5d). RTCW SP has no player lamp.
================
*/
void CG_ToggleFlashlight_f( void ) {
	if ( !( bg_cod.enable && bg_cod.flashlightEnable ) ) {
		cg.codFlashlightOn = qfalse;
		CG_Printf( "Flashlight disabled (cod_flashlightEnable 0).\n" );
		return;
	}
	cg.codFlashlightOn = !cg.codFlashlightOn;
	CG_Printf( "Flashlight %s\n", cg.codFlashlightOn ? "ON" : "OFF" );
}

void CG_CodAddFlashlight( void ) {
	vec3_t start, end, lightPos;
	trace_t tr;
	float intensity;

	if ( !cg.codFlashlightOn ) {
		return;
	}
	if ( !( bg_cod.enable && bg_cod.flashlightEnable ) ) {
		return;
	}
	if ( !cg.snap || cg.renderingThirdPerson ) {
		return;
	}
	if ( cg.snap->ps.pm_type == PM_INTERMISSION || cg.snap->ps.stats[STAT_HEALTH] <= 0 ) {
		return;
	}

	intensity = bg_cod.flashlightIntensity;
	if ( intensity < 1.0f ) {
		intensity = 350.0f;
	}

	// Trace ahead so the beam lights walls (point light at the eye alone is easy to miss).
	// REF_FORCE_DLIGHT: wolfconfig often has r_dynamiclight 0 — without FORCE the light is dropped.
	VectorCopy( cg.refdef.vieworg, start );
	VectorMA( start, 768.0f, cg.refdef.viewaxis[0], end );
	CG_Trace( &tr, start, NULL, NULL, end, cg.snap->ps.clientNum, MASK_SHOT );
	VectorMA( start, 32.0f + 200.0f * tr.fraction, cg.refdef.viewaxis[0], lightPos );

	trap_R_AddLightToScene( lightPos, intensity * ( 0.55f + 0.45f * tr.fraction ),
							bg_cod.flashlightR, bg_cod.flashlightG, bg_cod.flashlightB,
							REF_FORCE_DLIGHT );
	// Soft fill near the view so close geometry is lit too.
	VectorMA( start, 24.0f, cg.refdef.viewaxis[0], lightPos );
	trap_R_AddLightToScene( lightPos, intensity * 0.35f,
							bg_cod.flashlightR, bg_cod.flashlightG, bg_cod.flashlightB,
							REF_FORCE_DLIGHT );
}

/*
================
CG_CodResetBinds_f

codmod: console helper for a full bind reset (phase 5d). Same as execing
codmod_binds.cfg (unbindall → default.cfg → codmod deltas).
================
*/
void CG_CodResetBinds_f( void ) {
	trap_SendConsoleCommand( "exec codmod_binds.cfg\n" );
}

// codmod: quickknife state machine (phase 5.5). Values match cg.codQkState.
#define COD_QK_IDLE       0
#define COD_QK_SWITCHING  1
#define COD_QK_SWINGING   2
#define COD_QK_RETURNING  3

static void CG_CodQuickKnifeClear( void ) {
	cg.codQkState = COD_QK_IDLE;
	cg.codQkPrev = WP_NONE;
	cg.codQkStartTime = 0;
	cg.codQkAttackHeld = qfalse;
	cg.codQkSawFire = qfalse;
	cg.codQkStayOnKnife = qfalse;
}

static qboolean CG_CodHasKnife( void ) {
	if ( !cg.snap ) {
		return qfalse;
	}
	return COM_BitCheck( cg.snap->ps.weapons, WP_KNIFE ) ? qtrue : qfalse;
}

/*
================
CG_QuickKnife_f

codmod: CoD-style quick knife (phase 5.5). Bind: `bind v quickknife`. Switches to
WP_KNIFE, auto-swings once when READY, then returns to the previous weapon. Not +kick.
================
*/
void CG_QuickKnife_f( void ) {
	int cur;

	if ( !cg.snap ) {
		return;
	}
	if ( !( bg_cod.enable && bg_cod.quickKnifeEnable ) ) {
		return;
	}
	if ( cg.codQkState != COD_QK_IDLE ) {
		return;
	}
	if ( cg.snap->ps.pm_type == PM_DEAD || cg.snap->ps.stats[STAT_HEALTH] <= 0 ) {
		return;
	}
	if ( !CG_CodHasKnife() ) {
		return;
	}

	trap_SendConsoleCommand( "-ads\n" );

	cur = cg.weaponSelect;
	if ( cur == WP_KNIFE || cg.snap->ps.weapon == WP_KNIFE ) {
		cg.codQkStayOnKnife = qtrue;
		cg.codQkPrev = WP_NONE;
		cg.codQkState = COD_QK_SWINGING;
		cg.codQkStartTime = cg.time;
		cg.codQkAttackHeld = qfalse;
		cg.codQkSawFire = qfalse;
		return;
	}

	cg.codQkStayOnKnife = qfalse;
	cg.codQkPrev = cur;
	cg.codQkStartTime = cg.time;
	cg.codQkAttackHeld = qfalse;
	cg.codQkSawFire = qfalse;
	cg.codQkState = COD_QK_SWITCHING;
	CG_FinishWeaponChange( cur, WP_KNIFE );
}

/*
================
CG_CodUpdateQuickKnife

codmod: advance quickknife each real frame (phase 5.5). Called from CG_DrawActiveFrame.
================
*/
void CG_CodUpdateQuickKnife( void ) {
	playerState_t *ps;
	int timeout;

	if ( cg.codQkState == COD_QK_IDLE ) {
		return;
	}
	if ( !cg.snap ) {
		CG_CodQuickKnifeClear();
		return;
	}
	if ( !( bg_cod.enable && bg_cod.quickKnifeEnable ) ) {
		if ( cg.codQkAttackHeld ) {
			trap_SendConsoleCommand( "-attack\n" );
		}
		CG_CodQuickKnifeClear();
		return;
	}

	ps = &cg.snap->ps;
	if ( ps->pm_type == PM_DEAD || ps->stats[STAT_HEALTH] <= 0 ) {
		if ( cg.codQkAttackHeld ) {
			trap_SendConsoleCommand( "-attack\n" );
		}
		CG_CodQuickKnifeClear();
		return;
	}

	timeout = (int)bg_cod.quickKnifeTimeout;
	if ( timeout < 400 ) {
		timeout = 400;
	}
	if ( cg.time - cg.codQkStartTime > timeout ) {
		if ( cg.codQkAttackHeld ) {
			trap_SendConsoleCommand( "-attack\n" );
			cg.codQkAttackHeld = qfalse;
		}
		if ( !cg.codQkStayOnKnife && cg.codQkPrev > WP_NONE && cg.codQkPrev < WP_NUM_WEAPONS
			 && COM_BitCheck( ps->weapons, cg.codQkPrev ) ) {
			CG_FinishWeaponChange( cg.weaponSelect, cg.codQkPrev );
		}
		CG_CodQuickKnifeClear();
		return;
	}

	// manual weapon change mid-sequence — abort, keep their choice
	if ( cg.codQkState == COD_QK_SWITCHING || cg.codQkState == COD_QK_SWINGING ) {
		if ( cg.weaponSelect != WP_KNIFE ) {
			if ( cg.codQkAttackHeld ) {
				trap_SendConsoleCommand( "-attack\n" );
			}
			CG_CodQuickKnifeClear();
			return;
		}
	}

	if ( cg.codQkState == COD_QK_SWITCHING ) {
		if ( ps->weapon == WP_KNIFE && ps->weaponstate == WEAPON_READY ) {
			cg.codQkState = COD_QK_SWINGING;
		}
		return;
	}

	if ( cg.codQkState == COD_QK_SWINGING ) {
		if ( ps->weapon != WP_KNIFE ) {
			if ( cg.codQkAttackHeld ) {
				trap_SendConsoleCommand( "-attack\n" );
			}
			CG_CodQuickKnifeClear();
			return;
		}

		if ( !cg.codQkAttackHeld && !cg.codQkSawFire && ps->weaponstate == WEAPON_READY ) {
			trap_SendConsoleCommand( "+attack\n" );
			cg.codQkAttackHeld = qtrue;
			return;
		}

		if ( cg.codQkAttackHeld ) {
			if ( ps->weaponstate == WEAPON_FIRING || ps->weaponTime > 0 ) {
				// Swing registered — release attack and return immediately (don't wait out
				// knife nextShotTime / READY). Keeps V feeling tied to the press.
				cg.codQkSawFire = qtrue;
				trap_SendConsoleCommand( "-attack\n" );
				cg.codQkAttackHeld = qfalse;
				if ( cg.codQkStayOnKnife || cg.codQkPrev <= WP_NONE ) {
					CG_CodQuickKnifeClear();
					return;
				}
				cg.codQkState = COD_QK_RETURNING;
				CG_FinishWeaponChange( WP_KNIFE, cg.codQkPrev );
			}
			return;
		}
		return;
	}

	if ( cg.codQkState == COD_QK_RETURNING ) {
		if ( ps->weapon == cg.codQkPrev && ps->weaponstate == WEAPON_READY ) {
			CG_CodQuickKnifeClear();
		} else if ( cg.weaponSelect != WP_KNIFE && ps->weapon != WP_KNIFE ) {
			CG_CodQuickKnifeClear();
		}
	}
}

/*
================
CG_CodThreatColor

codmod: shared white → yellow → orange → red ramp for heat / low health (phase 5c).
t in [0,1]: 0 = fine (white), 1 = critical (red).
================
*/
void CG_CodThreatColor( float t, vec4_t out ) {
	float u;

	if ( t < 0.0f ) {
		t = 0.0f;
	} else if ( t > 1.0f ) {
		t = 1.0f;
	}

	if ( t < 0.33f ) {
		u = t / 0.33f;
		out[0] = 1.0f;
		out[1] = 1.0f;
		out[2] = 1.0f - u;
	} else if ( t < 0.66f ) {
		u = ( t - 0.33f ) / 0.33f;
		out[0] = 1.0f;
		out[1] = 1.0f - 0.45f * u;
		out[2] = 0.0f;
	} else {
		u = ( t - 0.66f ) / 0.34f;
		out[0] = 1.0f;
		out[1] = 0.55f * ( 1.0f - u );
		out[2] = 0.0f;
	}
	out[3] = 1.0f;
}

/*
================
CG_CodColorForHealth

codmod: crosshair tint from health / STAT_MAX_HEALTH (phase 5c). Uses cod_maxHealth as
fallback when max is unset. Ignores armor — missing HP only.
================
*/
void CG_CodColorForHealth( vec4_t hcolor ) {
	int health;
	int maxH;
	float t;

	if ( !cg.snap ) {
		CG_CodThreatColor( 0.0f, hcolor );
		return;
	}

	health = cg.snap->ps.stats[STAT_HEALTH];
	if ( health <= 0 ) {
		VectorClear( hcolor );
		hcolor[3] = 1.0f;
		return;
	}

	maxH = cg.snap->ps.stats[STAT_MAX_HEALTH];
	if ( maxH < 1 ) {
		maxH = ( bg_cod.maxHealth > 0.0f ) ? (int)( bg_cod.maxHealth + 0.5f ) : 100;
	}
	if ( maxH < 1 ) {
		maxH = 1;
	}

	t = 1.0f - (float)health / (float)maxH;
	if ( t < 0.0f ) {
		t = 0.0f;
	} else if ( t > 1.0f ) {
		t = 1.0f;
	}
	CG_CodThreatColor( t, hcolor );
}

/*
================
CG_CodDrawLowHealthBlood

codmod: persistent dark-red vignette while health/max is below cod_hudLowHealthStart
(phase 5c). Drawn behind HUD. Respects cg_blood.
================
*/
void CG_CodDrawLowHealthBlood( void ) {
	int health;
	int maxH;
	float ratio;
	float start;
	float alpha;
	vec4_t col;

	if ( !cg.snap ) {
		return;
	}
	if ( !( bg_cod.enable && bg_cod.hudEnable && bg_cod.hudLowHealthBlood ) ) {
		return;
	}
	if ( !cg_blood.integer ) {
		return;
	}
	if ( cg.cameraMode ) {
		return;
	}

	health = cg.snap->ps.stats[STAT_HEALTH];
	if ( health <= 0 ) {
		return;
	}

	maxH = cg.snap->ps.stats[STAT_MAX_HEALTH];
	if ( maxH < 1 ) {
		maxH = ( bg_cod.maxHealth > 0.0f ) ? (int)( bg_cod.maxHealth + 0.5f ) : 100;
	}
	if ( maxH < 1 ) {
		maxH = 1;
	}

	ratio = (float)health / (float)maxH;
	start = bg_cod.hudLowHealthStart;
	if ( start <= 0.0f || start > 1.0f ) {
		start = 0.4f;
	}
	if ( ratio > start ) {
		return;
	}

	alpha = ( start - ratio ) / start;
	if ( alpha > 1.0f ) {
		alpha = 1.0f;
	}
	alpha *= bg_cod.hudLowHealthMaxAlpha;
	if ( alpha < 0.0f ) {
		return;
	}

	// gentle pulse when critically low so regen is visible
	if ( ratio < 0.2f ) {
		alpha *= 0.85f + 0.15f * (float)sin( (float)cg.time * 0.008f );
	}

	VectorSet( col, 0.35f, 0.0f, 0.0f );
	col[3] = alpha;

	if ( cg_fixedAspect.integer ) {
		CG_SetScreenPlacement( PLACE_STRETCH, PLACE_STRETCH );
		CG_FillRect( -10, -10, 650, 490, col );
		CG_SetScreenPlacement( PLACE_CENTER, PLACE_CENTER );
	} else {
		CG_FillRect( -10, -10, 650, 490, col );
	}
}

/*
================
CG_CodDrawCrosshairBar

codmod: one post (or outline) for the CoD 4-post reticle. Coords are 640×480 virtual.
================
*/
static void CG_CodDrawCrosshairBar( float x, float y, float w, float h, const vec4_t color ) {
	if ( w < 0.5f || h < 0.5f || color[3] <= 0.01f ) {
		return;
	}
	CG_FillRect( x, y, w, h, color );
}

/*
================
CG_CodDrawCrosshair

codmod: CoD-style 4-post hip reticle (phase 5c follow-up).
- Gap blooms with aimSpreadScale (fire) + horizontal speed (walk/sprint).
- Crouch tightens the gap; sprint expands and fades the posts.
- ADS fades the posts out; center dot stays while cod_adsCrosshair 1 (interim until
  every weapon's iron sights are tuned — then mode 2 hides the dot too).
================
*/
void CG_CodDrawCrosshair( const vec4_t hcolor ) {
	float cx, cy;
	float gap, len, th, halfTh;
	float spreadT, speed, sprintMax, moveT, sprintF, ads, crouchMul;
	float postAlpha, dotAlpha;
	vec4_t postColor, outlineColor, dotColor;
	vec3_t vel;
	playerState_t *ps;
	qboolean drawPosts;
	qboolean drawDot;

	if ( !cg.snap ) {
		return;
	}

	ps = &cg.predictedPlayerState;
	ads = ( bg_cod.enable && bg_cod.adsEnable ) ? CG_CodAdsFrac() : 0.0f;
	ads = BG_CodSmoothstep( ads );

	// adsCrosshair 2: hide everything once mostly ADS (same gate as the interim path).
	if ( bg_cod.adsCrosshair >= 2 && ads > 0.5f ) {
		return;
	}

	spreadT = (float)cg.snap->ps.aimSpreadScale / 255.0f;
	if ( spreadT < 0.0f ) {
		spreadT = 0.0f;
	} else if ( spreadT > 1.0f ) {
		spreadT = 1.0f;
	}
	// Match FireWeapon hip floor so the reticle gap reads the CoD cone while hip.
	if ( bg_cod.enable && bg_cod.adsEnable && bg_cod.hipSpreadMin > 0.0f ) {
		float hipVis = bg_cod.hipSpreadMin * ( 1.0f - ads );
		if ( spreadT < hipVis ) {
			spreadT = hipVis;
		}
	}

	VectorCopy( ps->velocity, vel );
	vel[2] = 0.0f;
	speed = VectorLength( vel );
	sprintMax = bg_cod.runSpeed * bg_cod.sprintScale;
	if ( sprintMax < 1.0f ) {
		sprintMax = 1.0f;
	}
	moveT = speed / sprintMax;
	if ( moveT < 0.0f ) {
		moveT = 0.0f;
	} else if ( moveT > 1.0f ) {
		moveT = 1.0f;
	}

	sprintF = CG_CodSprintFrac();
	crouchMul = 1.0f;
	if ( ps->eFlags & EF_CROUCHING ) {
		crouchMul = bg_cod.crosshairCrouchScale;
		if ( crouchMul < 0.1f ) {
			crouchMul = 0.1f;
		} else if ( crouchMul > 1.0f ) {
			crouchMul = 1.0f;
		}
	}

	gap = bg_cod.crosshairGap
		+ bg_cod.crosshairBloom * spreadT
		+ bg_cod.crosshairMoveBloom * moveT
		+ bg_cod.crosshairMoveBloom * 0.35f * sprintF;
	gap *= crouchMul;
	// ADS contracts the gap as posts fade (reads as accuracy tightening).
	if ( bg_cod.adsCrosshair > 0 ) {
		gap *= 1.0f - 0.65f * ads;
	}
	if ( gap < 0.0f ) {
		gap = 0.0f;
	}

	len = bg_cod.crosshairLength;
	th = bg_cod.crosshairWidth;
	if ( len < 1.0f ) {
		len = 1.0f;
	}
	if ( th < 1.0f ) {
		th = 1.0f;
	}
	halfTh = th * 0.5f;

	cx = 320.0f + (float)cg_crosshairX.integer;
	cy = 240.0f + (float)cg_crosshairY.integer;

	postAlpha = hcolor[3];
	if ( bg_cod.adsCrosshair > 0 ) {
		postAlpha *= ( 1.0f - ads );
	}
	postAlpha *= 1.0f - ( 1.0f - bg_cod.crosshairSprintAlpha ) * sprintF;
	if ( postAlpha < 0.0f ) {
		postAlpha = 0.0f;
	} else if ( postAlpha > 1.0f ) {
		postAlpha = 1.0f;
	}

	dotAlpha = hcolor[3];
	// Mode 2 fades the dot with ADS; mode 1 keeps it (interim iron-sight aid).
	if ( bg_cod.adsCrosshair >= 2 ) {
		dotAlpha *= ( 1.0f - ads );
	}
	if ( dotAlpha < 0.0f ) {
		dotAlpha = 0.0f;
	} else if ( dotAlpha > 1.0f ) {
		dotAlpha = 1.0f;
	}

	drawPosts = ( postAlpha > 0.01f ) ? qtrue : qfalse;
	drawDot = qfalse;
	if ( bg_cod.crosshairDot && dotAlpha > 0.01f ) {
		drawDot = qtrue;
	}
	// Even with hip dot off, keep the interim ADS center dot.
	if ( !drawDot && bg_cod.adsCrosshair == 1 && ads > 0.05f && dotAlpha > 0.01f ) {
		drawDot = qtrue;
	}

	VectorCopy( hcolor, postColor );
	postColor[3] = postAlpha;
	VectorCopy( hcolor, dotColor );
	dotColor[3] = dotAlpha;

	if ( drawPosts ) {
		if ( bg_cod.crosshairOutline ) {
			VectorSet( outlineColor, 0.0f, 0.0f, 0.0f );
			outlineColor[3] = postAlpha * 0.85f;
			// top / bottom / left / right — 1px larger behind
			CG_CodDrawCrosshairBar( cx - halfTh - 1.0f, cy - gap - len - 1.0f, th + 2.0f, len + 2.0f, outlineColor );
			CG_CodDrawCrosshairBar( cx - halfTh - 1.0f, cy + gap - 1.0f,       th + 2.0f, len + 2.0f, outlineColor );
			CG_CodDrawCrosshairBar( cx - gap - len - 1.0f, cy - halfTh - 1.0f, len + 2.0f, th + 2.0f, outlineColor );
			CG_CodDrawCrosshairBar( cx + gap - 1.0f,       cy - halfTh - 1.0f, len + 2.0f, th + 2.0f, outlineColor );
		}
		CG_CodDrawCrosshairBar( cx - halfTh, cy - gap - len, th, len, postColor );
		CG_CodDrawCrosshairBar( cx - halfTh, cy + gap,       th, len, postColor );
		CG_CodDrawCrosshairBar( cx - gap - len, cy - halfTh, len, th, postColor );
		CG_CodDrawCrosshairBar( cx + gap,       cy - halfTh, len, th, postColor );
	}

	if ( drawDot ) {
		if ( bg_cod.crosshairOutline ) {
			VectorSet( outlineColor, 0.0f, 0.0f, 0.0f );
			outlineColor[3] = dotAlpha * 0.85f;
			CG_CodDrawCrosshairBar( cx - 2.0f, cy - 2.0f, 4.0f, 4.0f, outlineColor );
		}
		CG_CodDrawCrosshairBar( cx - 1.0f, cy - 1.0f, 2.0f, 2.0f, dotColor );
	}
}

/*
================
CG_CodTriggerHitMarker

codmod: E9 — show X + play body/headshot tick. Same-frame debounce: a later headshot
upgrade replaces a body tick; duplicate body ticks in one frame are ignored.
================
*/
void CG_CodTriggerHitMarker( qboolean headshot ) {
	sfxHandle_t sfx;

	if ( !bg_cod.enable || !bg_cod.hitMarkerEnable ) {
		return;
	}

	// Already flashed this frame: only upgrade body → headshot.
	if ( cg.codHitMarkerTime == cg.time ) {
		if ( !headshot || cg.codHitMarkerHeadshot ) {
			return;
		}
	}

	cg.codHitMarkerTime = cg.time;
	cg.codHitMarkerHeadshot = headshot;

	if ( !bg_cod.hitMarkerSound ) {
		return;
	}

	sfx = 0;
	if ( headshot && cgs.media.codHitMarkerHeadSound ) {
		sfx = cgs.media.codHitMarkerHeadSound;
	} else if ( cgs.media.codHitMarkerSound ) {
		sfx = cgs.media.codHitMarkerSound;
	}
	if ( !sfx ) {
		// Lazy re-register if init missed the pk3 (should not happen).
		if ( headshot ) {
			cgs.media.codHitMarkerHeadSound = trap_S_RegisterSound( "sound/codmod/hitmarker_head.wav" );
			sfx = cgs.media.codHitMarkerHeadSound;
		} else {
			cgs.media.codHitMarkerSound = trap_S_RegisterSound( "sound/codmod/hitmarker.wav" );
			sfx = cgs.media.codHitMarkerSound;
		}
	}
	if ( sfx ) {
		// CHAN_ANNOUNCER: weapon fire must not steal/cut the UI tick.
		trap_S_StartLocalSound( sfx, CHAN_ANNOUNCER );
	}
}

/*
================
CG_CodCheckHitMarker

codmod: E9 — PERS_HITS path (knife/explosives + headshot upgrade). Bullet flesh hits
also call CG_CodTriggerHitMarker from CG_Bullet for immediate feedback.
================
*/
void CG_CodCheckHitMarker( playerState_t *ps, playerState_t *ops ) {
	qboolean headshot;

	if ( !ps || !ops ) {
		return;
	}
	if ( ps->persistant[PERS_HITS] <= ops->persistant[PERS_HITS] ) {
		return;
	}

	headshot = ( ps->persistant[PERS_GAUNTLET_FRAG_COUNT]
				 > ops->persistant[PERS_GAUNTLET_FRAG_COUNT] ) ? qtrue : qfalse;
	CG_CodTriggerHitMarker( headshot );
}

/*
================
CG_CodDrawHitMarkerDiag

codmod: one short diagonal tick (axis-aligned square steps ≈ a 45° line).
================
*/
static void CG_CodDrawHitMarkerDiag( float x0, float y0, float x1, float y1,
									 float th, const vec4_t color ) {
	float dx, dy, steps, t, x, y;
	int n, k;

	dx = x1 - x0;
	dy = y1 - y0;
	steps = (float)sqrt( dx * dx + dy * dy );
	if ( steps < 1.0f ) {
		return;
	}
	n = (int)( steps + 0.5f );
	if ( n < 1 ) {
		n = 1;
	}
	for ( k = 0; k <= n; k++ ) {
		t = (float)k / (float)n;
		x = x0 + dx * t;
		y = y0 + dy * t;
		CG_FillRect( x - th * 0.5f, y - th * 0.5f, th, th, color );
	}
}

/*
================
CG_CodDrawHitMarker

codmod: E9 — four short diagonal ticks forming an X around the crosshair.
================
*/
void CG_CodDrawHitMarker( void ) {
	float cx, cy, gap, len, th, age, dur, alpha;
	vec4_t color;

	if ( !bg_cod.enable || !bg_cod.hitMarkerEnable ) {
		return;
	}
	if ( !cg.codHitMarkerTime ) {
		return;
	}

	dur = bg_cod.hitMarkerTime;
	if ( dur < 50.0f ) {
		dur = 50.0f;
	}
	age = (float)( cg.time - cg.codHitMarkerTime );
	if ( age < 0.0f || age >= dur ) {
		return;
	}

	// Quick pop then fade (CoD-ish).
	alpha = 1.0f - ( age / dur );
	alpha = BG_CodSmoothstep( alpha );
	if ( age < 40.0f ) {
		alpha *= age / 40.0f;
	}
	if ( alpha <= 0.01f ) {
		return;
	}

	cx = 320.0f + (float)cg_crosshairX.integer;
	cy = 240.0f + (float)cg_crosshairY.integer;
	gap = bg_cod.hitMarkerGap;
	len = bg_cod.hitMarkerLength;
	th = bg_cod.hitMarkerWidth;
	if ( gap < 1.0f ) {
		gap = 1.0f;
	}
	if ( len < 2.0f ) {
		len = 2.0f;
	}
	if ( th < 1.0f ) {
		th = 1.0f;
	}

	// White X; headshots get a slight warm tint.
	if ( cg.codHitMarkerHeadshot ) {
		VectorSet( color, 1.0f, 0.92f, 0.75f );
	} else {
		VectorSet( color, 1.0f, 1.0f, 1.0f );
	}
	color[3] = alpha;

	// NW \  NE /  SW /  SE \  — gap keeps the center clear for the reticle.
	CG_CodDrawHitMarkerDiag( cx - gap - len, cy - gap - len, cx - gap, cy - gap, th, color );
	CG_CodDrawHitMarkerDiag( cx + gap + len, cy - gap - len, cx + gap, cy - gap, th, color );
	CG_CodDrawHitMarkerDiag( cx - gap - len, cy + gap + len, cx - gap, cy + gap, th, color );
	CG_CodDrawHitMarkerDiag( cx + gap + len, cy + gap + len, cx + gap, cy + gap, th, color );
}
