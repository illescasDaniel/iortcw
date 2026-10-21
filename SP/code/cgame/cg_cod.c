// codmod: new file (phase 2.0) -- cg_cod.c
//
// Debug HUD and dump command for the cod_* tuning values. See docs/architecture.md
// section 6. Extended in later phases (ADS, slide, prone).

#include "cg_local.h"
#include "../client/keycodes.h" // codmod: K_SPACE for toggle-crouch auto-stand (phase 5d)
#include "../ui/ui_lang.h"

// codmod: Phase 3.5 per-weapon iron-sight ADS. Primary pose is computed every frame from
// tag_flash (see CG_CodApplyIronSightAds) so the rear sight sits on the screen center like
// CoD -- no new models required. Table fields:
//   sightHeight / barrelBack / sightDist  -- geometry of the sight line (0 sightDist => cvar)
//   fwd/right/up/pitch/yaw/roll           -- fine bias after auto-align (tune these live)
// Not const: cod_adsTune overwrites all 9 fields while tuning (absolute pose).
codAdsView_t cg_codAdsView[WP_NUM_WEAPONS] = {
	// Absolute iron-sight poses baked from codmod_ads_tune.log (2026-10-09, final pass).
	// Global iron-sight cvars default to 0 (optional master offset). Re-tune anytime
	// with /cod_adsTuneStart → Save (tooling kept).
	//                          sightH  barrelB  sightD   fwd   right  up   pitch yaw roll
	[WP_LUGER]          = { 2.3f, 13.0f, 20.0f,   0.0f,  1.1f,  0.0f,  -1.0f, -3.7f,  6.5f },
	[WP_SILENCER]       = { 2.2f, 23.5f, 21.5f,   0.0f,  0.8f,  0.0f,  -0.9f, -3.6f,  6.1f },
	[WP_COLT]           = { 2.5f, 13.0f, 20.0f,   0.0f,  1.1f,  0.0f,  -1.0f, -1.5f, -0.3f },

	[WP_MP40]           = { 3.0f, 15.7f, 22.0f,   0.0f,  0.2f,  0.3f,  -4.2f,  0.7f,  4.0f },
	[WP_THOMPSON]       = { 3.0f,  8.0f, 22.0f,   0.0f,  0.0f,  0.8f,  -5.0f,  0.3f,  0.0f },
	[WP_STEN]           = { 3.1f, 17.5f, 23.0f,   0.5f,  0.3f, -2.1f,  -6.3f,  3.8f, -2.4f },

	[WP_MAUSER]         = { 3.5f, 27.5f, 24.0f,  -0.5f,  1.0f,  2.0f,   1.9f, -2.4f, -1.5f },
	[WP_GARAND]         = { 3.5f, 25.0f, 24.0f,   0.0f,  0.6f,  1.2f,   0.0f, -0.6f,  1.0f },
	[WP_FG42]           = { 3.0f, 21.0f, 23.0f,   0.0f, -0.1f,  0.0f,  -1.0f,  0.5f, -1.5f },
};

// codmod: ADS-capable iron-sight weapons for the tune session (scopes excluded).
static const int cg_codAdsTuneWeapons[] = {
	WP_LUGER, WP_SILENCER, WP_COLT,
	WP_MP40, WP_THOMPSON, WP_STEN,
	WP_MAUSER, WP_GARAND, WP_FG42
};
static const int cg_codAdsTuneWeaponCount =
	(int)( sizeof( cg_codAdsTuneWeapons ) / sizeof( cg_codAdsTuneWeapons[0] ) );

// Bitmask: weapon table row has been converted to absolute (global+bias folded in) this map.
static unsigned int cg_codAdsTuneAbsMask;

#define COD_ADS_TUNE_LOG_FILE "codmod_ads_tune.log"

static const char *CG_CodWeaponName( int weapon );

/*
================
CG_CodTuneLog

codmod: append one line to a file under fs_homepath/fs_game and echo to the console.
Shared by the tune menu (codmod_tune.log) and ADS SAVE/DUMP (codmod_ads_tune.log).
================
*/
void CG_CodTuneLog( const char *filename, const char *line ) {
	fileHandle_t f;
	char buf[512];
	int len;

	if ( !filename || !filename[0] || !line ) {
		return;
	}

	CG_Printf( "%s\n", line );

	Com_sprintf( buf, sizeof( buf ), "%s\n", line );
	len = (int)strlen( buf );
	if ( trap_FS_FOpenFile( filename, &f, FS_APPEND ) >= 0 && f ) {
		trap_FS_Write( buf, len, f );
		trap_FS_FCloseFile( f );
	}
}

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

	// codmod: normally live cvars are the knobs and table values are ADDED as a bias.
	// While cod_adsTune is on for the selected weapon, the table holds an absolute pose
	// (globals already folded in) so tweaking one gun cannot shift another mid-session.
	if ( bg_cod.adsTune && weaponNum == cg.weaponSelect ) {
		sightHeight = ads->sightHeight;
		barrelBack  = ads->barrelBack;
		sightDist   = ads->sightDist;
	} else {
		sightHeight = bg_cod.adsSightHeight + ads->sightHeight;
		barrelBack  = bg_cod.adsBarrelBack  + ads->barrelBack;
		sightDist   = bg_cod.adsSightDist   + ads->sightDist;
	}

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
		if ( bg_cod.adsTune && weaponNum == cg.weaponSelect ) {
			alignedAngles[PITCH] += ads->pitch;
			alignedAngles[YAW]   += ads->yaw;
			alignedAngles[ROLL]  += ads->roll;
		} else {
			alignedAngles[PITCH] += bg_cod.adsPitch + ads->pitch;
			alignedAngles[YAW]   += bg_cod.adsYaw   + ads->yaw;
			alignedAngles[ROLL]  += bg_cod.adsRoll  + ads->roll;
		}

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
	// cod_adsRight shifts the whole gun horizontally (plus per-weapon ads->right),
	// except while adsTune (table right is already absolute).
	VectorMA( handOrigin, ads->fwd * e, cg.refdef.viewaxis[0], handOrigin );
	if ( bg_cod.adsTune && weaponNum == cg.weaponSelect ) {
		VectorMA( handOrigin, ads->right * e, cg.refdef.viewaxis[1], handOrigin );
	} else {
		VectorMA( handOrigin, ( bg_cod.adsRight + ads->right ) * e, cg.refdef.viewaxis[1], handOrigin );
	}
	VectorMA( handOrigin, ads->up * e, cg.refdef.viewaxis[2], handOrigin );
}

/*
================
CG_CodAdsTuneEnsureAbsolute

codmod: fold global iron-sight cvars into the per-weapon table once per weapon per map
so adsTune can treat the row as an absolute pose without double-counting globals.
================
*/
static void CG_CodAdsTuneEnsureAbsolute( int weapon ) {
	codAdsView_t *ads;
	unsigned int bit;

	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS || weapon >= 32 ) {
		return;
	}

	bit = 1u << weapon;
	if ( cg_codAdsTuneAbsMask & bit ) {
		return;
	}

	ads = &cg_codAdsView[weapon];
	ads->sightHeight += bg_cod.adsSightHeight;
	ads->barrelBack  += bg_cod.adsBarrelBack;
	ads->sightDist   += bg_cod.adsSightDist;
	ads->pitch       += bg_cod.adsPitch;
	ads->yaw         += bg_cod.adsYaw;
	ads->roll        += bg_cod.adsRoll;
	ads->right       += bg_cod.adsRight;
	cg_codAdsTuneAbsMask |= bit;
}

/*
================
CG_CodUpdateAdsTune

codmod: cod_adsTune 1 live-overwrites the current weapon's cg_codAdsView entry from all
cod_adsTune* cvars every frame (geometry + fine bias). Called once per real frame from
CG_DrawActiveFrame (cg_view.c).
================
*/
void CG_CodUpdateAdsTune( void ) {
	int weapon;
	codAdsView_t *ads;

	if ( !bg_cod.adsTune ) {
		return;
	}

	// Use weaponSelect (not predicted weapon) so Next/Load stay in sync during switch anims.
	weapon = cg.weaponSelect;
	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS ) {
		return;
	}

	// Tune* cvars are already absolute; mark the row so later loads do not re-fold globals.
	if ( weapon < 32 ) {
		cg_codAdsTuneAbsMask |= ( 1u << weapon );
	}

	ads = &cg_codAdsView[weapon];
	ads->sightHeight = bg_cod.adsTuneHeight;
	ads->barrelBack  = bg_cod.adsTuneBarrel;
	ads->sightDist   = bg_cod.adsTuneDist;
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

codmod: "cod_adsTuneLoad" — copies the current weapon's effective iron-sight pose into
cod_adsTune* cvars. While adsTune is on, folds globals into the table once per weapon
so the knobs match what you currently see. While adsTune is off, seeds from
global+table without mutating the table.
================
*/
void CG_CodAdsTuneLoad_f( void ) {
	int weapon;
	codAdsView_t *ads;
	const char *name;
	float height, barrel, dist, fwd, right, up, pitch, yaw, roll;

	weapon = cg.weaponSelect;
	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS ) {
		CG_Printf( "cod_adsTuneLoad: no weapon selected\n" );
		return;
	}

	ads = &cg_codAdsView[weapon];
	if ( bg_cod.adsTune ) {
		CG_CodAdsTuneEnsureAbsolute( weapon );
		height = ads->sightHeight;
		barrel = ads->barrelBack;
		dist   = ads->sightDist;
		fwd    = ads->fwd;
		right  = ads->right;
		up     = ads->up;
		pitch  = ads->pitch;
		yaw    = ads->yaw;
		roll   = ads->roll;
	} else {
		height = bg_cod.adsSightHeight + ads->sightHeight;
		barrel = bg_cod.adsBarrelBack  + ads->barrelBack;
		dist   = bg_cod.adsSightDist   + ads->sightDist;
		fwd    = ads->fwd;
		right  = bg_cod.adsRight + ads->right;
		up     = ads->up;
		pitch  = bg_cod.adsPitch + ads->pitch;
		yaw    = bg_cod.adsYaw   + ads->yaw;
		roll   = bg_cod.adsRoll  + ads->roll;
	}

	trap_Cvar_Set( "cod_adsTuneHeight", va( "%g", height ) );
	trap_Cvar_Set( "cod_adsTuneBarrel", va( "%g", barrel ) );
	trap_Cvar_Set( "cod_adsTuneDist", va( "%g", dist ) );
	trap_Cvar_Set( "cod_adsTuneFwd", va( "%g", fwd ) );
	trap_Cvar_Set( "cod_adsTuneRight", va( "%g", right ) );
	trap_Cvar_Set( "cod_adsTuneUp", va( "%g", up ) );
	trap_Cvar_Set( "cod_adsTunePitch", va( "%g", pitch ) );
	trap_Cvar_Set( "cod_adsTuneYaw", va( "%g", yaw ) );
	trap_Cvar_Set( "cod_adsTuneRoll", va( "%g", roll ) );
	trap_Cvar_Set( "cod_adsTuneFov", va( "%g", BG_CodAdsFovScale( weapon ) ) );

	name = CG_CodWeaponName( weapon );
	CG_Printf( "cod_adsTuneLoad: loaded %s (%d)\n", name ? name : "?", weapon );
}

// Only the ADS-relevant weapons are named; anything else returns NULL.
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
CG_CodAdsTuneFormatCLine

codmod: pasteable cg_codAdsView initializer for one weapon.
================
*/
static void CG_CodAdsTuneFormatCLine( int weapon, const codAdsView_t *ads, char *out, int outSize ) {
	const char *name;

	name = CG_CodWeaponName( weapon );
	if ( name ) {
		Com_sprintf( out, outSize,
					 "[%s] = { %gf, %gf, %gf,  %gf, %gf, %gf,  %gf, %gf, %gf },",
					 name,
					 ads->sightHeight, ads->barrelBack, ads->sightDist,
					 ads->fwd, ads->right, ads->up,
					 ads->pitch, ads->yaw, ads->roll );
	} else {
		Com_sprintf( out, outSize,
					 "[%d] = { %gf, %gf, %gf,  %gf, %gf, %gf,  %gf, %gf, %gf },",
					 weapon,
					 ads->sightHeight, ads->barrelBack, ads->sightDist,
					 ads->fwd, ads->right, ads->up,
					 ads->pitch, ads->yaw, ads->roll );
	}
}

/*
================
CG_CodAdsTuneDump_f

codmod: "cod_adsTuneDump" — prints a pasteable C initializer and appends it to
codmod_ads_tune.log (no SAVE marker; recoverable scratch dump).
================
*/
void CG_CodAdsTuneDump_f( void ) {
	int weapon;
	codAdsView_t *ads;
	char line[256];

	weapon = cg.weaponSelect;
	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS ) {
		CG_Printf( "cod_adsTuneDump: no weapon selected\n" );
		return;
	}

	if ( bg_cod.adsTune ) {
		CG_CodAdsTuneEnsureAbsolute( weapon );
	}

	ads = &cg_codAdsView[weapon];
	CG_CodAdsTuneFormatCLine( weapon, ads, line, sizeof( line ) );
	CG_CodTuneLog( COD_ADS_TUNE_LOG_FILE, line );
}

/*
================
CG_CodAdsTuneSave_f

codmod: "cod_adsTuneSave" — append a machine-parseable SAVE + C initializer for the
current weapon to codmod_ads_tune.log. Latest SAVE per weapon wins at bake time.
================
*/
void CG_CodAdsTuneSave_f( void ) {
	int weapon;
	codAdsView_t *ads;
	const char *name;
	char saveLine[512];
	char cLine[256];
	float fov;

	weapon = cg.weaponSelect;
	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS ) {
		CG_Printf( "cod_adsTuneSave: no weapon selected\n" );
		return;
	}

	if ( bg_cod.adsTune ) {
		CG_CodAdsTuneEnsureAbsolute( weapon );
	}

	ads = &cg_codAdsView[weapon];
	name = CG_CodWeaponName( weapon );
	fov = bg_cod.adsTune ? bg_cod.adsTuneFov : BG_CodAdsFovScale( weapon );

	Com_sprintf( saveLine, sizeof( saveLine ),
				 "ads_tune SAVE weapon=%s id=%d height=%g barrel=%g dist=%g fwd=%g right=%g up=%g pitch=%g yaw=%g roll=%g fov=%g",
				 name ? name : "?",
				 weapon,
				 ads->sightHeight, ads->barrelBack, ads->sightDist,
				 ads->fwd, ads->right, ads->up,
				 ads->pitch, ads->yaw, ads->roll,
				 fov );
	CG_CodTuneLog( COD_ADS_TUNE_LOG_FILE, saveLine );

	CG_CodAdsTuneFormatCLine( weapon, ads, cLine, sizeof( cLine ) );
	CG_CodTuneLog( COD_ADS_TUNE_LOG_FILE, cLine );
}

/*
================
CG_CodAdsTunePrintChecklist

codmod: print the 9 iron-sight weapons for the tune session.
================
*/
static void CG_CodAdsTunePrintChecklist( void ) {
	int i, w;
	const char *name;

	CG_Printf( "ads_tune checklist (%d iron-sight weapons):\n", cg_codAdsTuneWeaponCount );
	for ( i = 0; i < cg_codAdsTuneWeaponCount; i++ ) {
		w = cg_codAdsTuneWeapons[i];
		name = CG_CodWeaponName( w );
		CG_Printf( "  %d) %s (id %d)\n", i + 1, name ? name : "?", w );
	}
}

/*
================
CG_CodAdsTuneStart_f

codmod: "cod_adsTuneStart" — enable adsLock + adsTune, load current weapon, print
checklist. Run `give all` (cheats) so every ADS weapon is available.
================
*/
void CG_CodAdsTuneStart_f( void ) {
	cg_codAdsTuneAbsMask = 0;

	trap_Cvar_Set( "cod_adsLock", "1" );
	trap_Cvar_Set( "cod_adsCrosshairForce", "1" ); // full reticle while ADS for center align
	trap_Cvar_Set( "cod_adsTune", "0" ); // load effective before overwrite starts
	CG_CodAdsTuneLoad_f();
	trap_Cvar_Set( "cod_adsTune", "1" );

	CG_Printf( "cod_adsTuneStart: adsLock=1 adsTune=1 adsCrosshairForce=1\n" );
	CG_CodAdsTunePrintChecklist();
	CG_Printf( "Next: /give all   then tune each gun and /cod_adsTuneSave\n" );
	CG_Printf( "Cycle: /cod_adsTuneNext   Log: run/home/codmod/codmod_ads_tune.log\n" );
	CG_Printf( "Crosshair stays on while ADS (cod_adsCrosshairForce). Set 0 when done.\n" );
	CG_CodTuneLog( COD_ADS_TUNE_LOG_FILE, "ads_tune START session" );
}

/*
================
CG_CodAdsTuneNext_f

codmod: "cod_adsTuneNext" — cycle to the next of the 9 ADS weapons and TuneLoad.
================
*/
void CG_CodAdsTuneNext_f( void ) {
	int cur, i, idx, next, last;

	if ( !cg.snap ) {
		return;
	}

	cur = cg.weaponSelect;
	idx = 0;
	for ( i = 0; i < cg_codAdsTuneWeaponCount; i++ ) {
		if ( cg_codAdsTuneWeapons[i] == cur ) {
			idx = i;
			break;
		}
	}

	next = cg_codAdsTuneWeapons[( idx + 1 ) % cg_codAdsTuneWeaponCount];
	last = cg.weaponSelect;

	if ( !( COM_BitCheck( cg.predictedPlayerState.weapons, next ) ) ) {
		CG_Printf( "cod_adsTuneNext: %s not owned — run /give all\n",
				   CG_CodWeaponName( next ) ? CG_CodWeaponName( next ) : "?" );
		return;
	}

	CG_FinishWeaponChange( last, next );
	CG_CodAdsTuneLoad_f();
	CG_Printf( "cod_adsTuneNext: now %s — tweak then /cod_adsTuneSave\n",
			   CG_CodWeaponName( next ) ? CG_CodWeaponName( next ) : "?" );
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
	if ( ps->stats[STAT_COD_FLAGS] & COD_SF_PRONE ) {
		if ( ps->stats[STAT_COD_FLAGS] & COD_SF_PRONE_MOVING ) {
			return "CRAWL";
		}
		return "PRONE";
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

	// codmod: prone stance timer (phase 6b)
	if ( ps->stats[STAT_COD_FLAGS] & COD_SF_PRONE
		 || ps->stats[STAT_COD_STANCE] != 0 ) {
		Com_sprintf( line, sizeof( line ), "stance %d  viewh %d",
					 ps->stats[STAT_COD_STANCE], ps->viewheight );
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

// CoD hold time: tap crouch, hold ~0.4s → prone (pad Circle/B via +crouchprone).
#define COD_CROUCH_HOLD_PRONE_MS 400

static void CG_CodPulseProne( void ) {
	cg.codPronePulse = 2;
}

void CG_CrouchProneDown_f( void ) {
	playerState_t *ps;

	cg.codCrouchProneDown = qtrue;
	cg.codCrouchProneStart = cg.time;
	cg.codCrouchProneWentProne = qfalse;
	cg.codCrouchProneExitProne = qfalse;
	cg.codCrouchProneWasCrouched = qfalse;

	if ( !bg_cod.enable ) {
		CG_ToggleCrouch_f();
		return;
	}

	ps = &cg.predictedPlayerState;
	if ( bg_cod.proneEnable && ( ps->stats[STAT_COD_FLAGS] & COD_SF_PRONE ) ) {
		CG_CodPulseProne();
		cg.codCrouchProneExitProne = qtrue;
		return;
	}

	// Remember stance at press. Do NOT crouch yet — tap crouches on release;
	// hold goes straight to prone (no crouch-then-prone).
	cg.codCrouchProneWasCrouched = cg.codCrouchHeld;
}

void CG_CrouchProneUp_f( void ) {
	if ( !cg.codCrouchProneDown ) {
		return;
	}
	cg.codCrouchProneDown = qfalse;

	if ( cg.codCrouchProneExitProne || cg.codCrouchProneWentProne ) {
		return;
	}

	// Tap: toggle crouch (stand→crouch or crouch→stand).
	if ( cg.codCrouchProneWasCrouched && cg.codCrouchHeld ) {
		cg.codCrouchHeld = qfalse;
		trap_SendConsoleCommand( "-movedown\n" );
	} else if ( !cg.codCrouchProneWasCrouched && !cg.codCrouchHeld ) {
		cg.codCrouchHeld = qtrue;
		trap_SendConsoleCommand( "+movedown\n" );
	}
}

void CG_CodUpdateCrouchProne( void ) {
	playerState_t *ps;

	if ( cg.codPronePulse > 0 ) {
		if ( cg.codPronePulse == 2 ) {
			trap_SendConsoleCommand( "+prone\n" );
		} else if ( cg.codPronePulse == 1 ) {
			trap_SendConsoleCommand( "-prone\n" );
		}
		cg.codPronePulse--;
	}

	if ( !cg.codCrouchProneDown || cg.codCrouchProneWentProne || cg.codCrouchProneExitProne ) {
		return;
	}
	if ( !( bg_cod.enable && bg_cod.proneEnable ) ) {
		return;
	}
	if ( cg.time - cg.codCrouchProneStart < COD_CROUCH_HOLD_PRONE_MS ) {
		return;
	}

	ps = &cg.predictedPlayerState;
	if ( ps->pm_flags & PMF_COD_SLIDING ) {
		return;
	}
	if ( ps->stats[STAT_COD_FLAGS] & COD_SF_PRONE ) {
		cg.codCrouchProneWentProne = qtrue;
		return;
	}
	if ( ps->stats[STAT_COD_STANCE] > 0 ) {
		return;
	}
	if ( ps->groundEntityNum == ENTITYNUM_NONE ) {
		return;
	}

	// Hold from crouch: clear sticky crouch so prone owns the stance.
	if ( cg.codCrouchHeld ) {
		cg.codCrouchHeld = qfalse;
		trap_SendConsoleCommand( "-movedown\n" );
	}
	CG_CodPulseProne();
	cg.codCrouchProneWentProne = qtrue;
}

static qboolean CG_CodJumpKeyHeld( void ) {
	int k;

	if ( trap_Key_IsDown( K_SPACE ) || trap_Key_IsDown( K_PAD0_A ) ) {
		return qtrue;
	}
	k = trap_Key_GetKey( "+moveup" );
	if ( k >= 0 && trap_Key_IsDown( k ) ) {
		return qtrue;
	}
	return qfalse;
}

void CG_CodUpdateToggleCrouch( void ) {
	static qboolean wasSliding;
	qboolean sliding;

	// codmod: CoD stands after a slide — clear sticky toggle crouch so the next CheckDuck
	// does not immediately re-duck (phase 6 follow-up).
	sliding = ( cg.predictedPlayerState.pm_flags & PMF_COD_SLIDING ) ? qtrue : qfalse;
	if ( wasSliding && !sliding && cg.codCrouchHeld ) {
		cg.codCrouchHeld = qfalse;
		trap_SendConsoleCommand( "-movedown\n" );
	}
	wasSliding = sliding;

	if ( !cg.codCrouchHeld ) {
		return;
	}
	// Jump (SPACE / pad A / +moveup) clears sticky crouch so the player stands (and can jump).
	if ( CG_CodJumpKeyHeld() ) {
		cg.codCrouchHeld = qfalse;
		trap_SendConsoleCommand( "-movedown\n" );
	}
}

// CoD: tap Square/X = reload; hold = interact (+activate).
#define COD_RELOAD_USE_HOLD_MS 250

void CG_ReloadUseDown_f( void ) {
	cg.codReloadUseDown = qtrue;
	cg.codReloadUseStart = cg.time;
	cg.codReloadUseActivated = qfalse;
}

void CG_ReloadUseUp_f( void ) {
	if ( !cg.codReloadUseDown ) {
		return;
	}
	cg.codReloadUseDown = qfalse;
	if ( cg.codReloadUseActivated ) {
		trap_SendConsoleCommand( "-activate\n" );
		return;
	}
	// Tap → reload
	cg.codReloadPulse = 2;
}

void CG_CodUpdateReloadUse( void ) {
	if ( cg.codReloadPulse > 0 ) {
		if ( cg.codReloadPulse == 2 ) {
			trap_SendConsoleCommand( "+reload\n" );
		} else if ( cg.codReloadPulse == 1 ) {
			trap_SendConsoleCommand( "-reload\n" );
		}
		cg.codReloadPulse--;
	}

	if ( !cg.codReloadUseDown || cg.codReloadUseActivated ) {
		return;
	}
	if ( cg.time - cg.codReloadUseStart < COD_RELOAD_USE_HOLD_MS ) {
		return;
	}
	trap_SendConsoleCommand( "+activate\n" );
	cg.codReloadUseActivated = qtrue;
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
	// They found the key themselves: the tip is not needed any more.
	if ( !cod_tipFlashlightShown.integer ) {
		trap_Cvar_Set( "cod_tipFlashlightShown", "1" );
	}
}

/*
CG_CodUpdateTips

codmod: one-time flashlight key tip, once per install (cod_tipFlashlightShown, CVAR_ARCHIVE in wolfconfig.cfg;
`cod_tipFlashlightShown 0` re-arms it). Shown as a bottom-screen center print after the player has had control
in a level for 4 s. The template string goes through Lang_Translate before the key name is formatted in
(the centerprint lookup afterwards finds no match for the formatted text and leaves it alone).
*/
void CG_CodUpdateTips( void ) {
	static int eligibleSince;
	int k;
	char key[32];

	if ( cod_tipFlashlightShown.integer ) {
		return;
	}
	if ( !( bg_cod.enable && bg_cod.flashlightEnable ) || !cg.snap || cg.cameraMode || cg.demoPlayback ||
		 cg.snap->ps.pm_type != PM_NORMAL || cg.snap->ps.stats[STAT_HEALTH] <= 0 ||
		 trap_Key_GetCatcher() != 0 ) {
		eligibleSince = 0;
		return;
	}
	if ( !eligibleSince || cg.time < eligibleSince ) {
		eligibleSince = cg.time;
		return;
	}
	if ( cg.time - eligibleSince < 4000 ) {
		return;
	}
	k = trap_Key_GetKey( "toggleflashlight" );
	if ( k < 0 ) {
		return;     // unbound: stay armed
	}
	trap_Key_KeynumToStringBuf( k, key, sizeof( key ) );
	if ( key[0] >= 'a' && key[0] <= 'z' && key[1] == '\0' ) {
		key[0] -= 'a' - 'A';    // single letters upper-case
	}
	CG_CenterPrint( va( (char *)Lang_Translate( "Toggle Flashlight with %s" ), key ), SCREEN_HEIGHT - ( SCREEN_HEIGHT * 0.25 ), SMALLCHAR_WIDTH );
	trap_Cvar_Set( "cod_tipFlashlightShown", "1" );
}

void CG_CodAddFlashlight( void ) {
	vec3_t start, dir;
	float intensity, radius;

	if ( !cg.codFlashlightOn ) {
		return;
	}
	// Cutscene eye is the spline camera — do not light from there.
	if ( cg.cameraMode ) {
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
		intensity = 500.0f;
	}
	// Beam reach (dlight radius). Intensity cvar doubles as a range scale.
	radius = intensity * 2.2f;
	if ( radius < 512.0f ) {
		radius = 512.0f;
	}
	if ( radius > 1800.0f ) {
		radius = 1800.0f;
	}

	// Origin slightly forward of the eye so the gun mesh / near plane don't bathe the world.
	VectorCopy( cg.refdef.vieworg, start );
	VectorMA( start, 10.0f, cg.refdef.viewaxis[0], start );
	VectorCopy( cg.refdef.viewaxis[0], dir );

	// Eye-origin spot cones (rend2). No impact point-light on the aimed wall — that was the
	// "each wall texture gets individually bright" look. REF_FORCE_DLIGHT: wolfconfig often
	// has r_dynamiclight 0.
	trap_R_AddSpotLightToScene( start, radius * 0.55f,
								bg_cod.flashlightR, bg_cod.flashlightG, bg_cod.flashlightB,
								dir, 16.0f, REF_FORCE_DLIGHT | REF_SHADOW_DLIGHT );
	trap_R_AddSpotLightToScene( start, radius,
								bg_cod.flashlightR * 0.65f, bg_cod.flashlightG * 0.65f, bg_cod.flashlightB * 0.65f,
								dir, 28.0f, REF_FORCE_DLIGHT | REF_SHADOW_DLIGHT );
}

/*
================
CG_CodDrawFlashlightHudIcon

codmod: small on-indicator next to the weapon icon (phase 5d). No world beam overlay.
================
*/
void CG_CodDrawFlashlightHudIcon( float weapX, float weapY, float weapW, float weapH ) {
	float x, y, s;
	vec4_t body, tip;
	float pulse;

	if ( !cg.codFlashlightOn ) {
		return;
	}
	if ( !( bg_cod.enable && bg_cod.flashlightEnable ) ) {
		return;
	}

	s = weapH * 0.42f;
	if ( s < 11.0f ) {
		s = 11.0f;
	}
	x = weapX - s - 5.0f;
	y = weapY + ( weapH - s ) * 0.5f;

	pulse = 0.88f + 0.12f * (float)sin( (double)cg.time * 0.005 );
	body[0] = 1.0f * pulse;
	body[1] = 0.85f * pulse;
	body[2] = 0.25f * pulse;
	body[3] = 0.95f;
	tip[0] = 1.0f;
	tip[1] = 0.95f;
	tip[2] = 0.55f;
	tip[3] = 0.85f;

	CG_FillRect( x + s * 0.05f, y + s * 0.32f, s * 0.42f, s * 0.36f, body );
	CG_FillRect( x + s * 0.42f, y + s * 0.22f, s * 0.28f, s * 0.56f, body );
	CG_FillRect( x + s * 0.68f, y + s * 0.12f, s * 0.28f, s * 0.76f, tip );
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

// CoD: tap R3 = quick knife; hold = kick (slower, hits harder). Kick is pulsed, not held.
#define COD_QUICKKICK_HOLD_MS 250
#define COD_QUICKKICK_PULSE_MS 120

void CG_QuickKickDown_f( void ) {
	cg.codQkickDown = qtrue;
	cg.codQkickStart = cg.time;
	cg.codQkickState = 0;
}

void CG_QuickKickUp_f( void ) {
	if ( !cg.codQkickDown ) {
		return;
	}
	cg.codQkickDown = qfalse;
	if ( cg.codQkickState == 1 ) {
		trap_SendConsoleCommand( "-kick\n" );
	} else if ( cg.codQkickState == 0 ) {
		CG_QuickKnife_f();  // tap
	}
	cg.codQkickState = 0;
}

void CG_CodUpdateQuickKick( void ) {
	if ( !cg.codQkickDown ) {
		return;
	}
	if ( cg.codQkickState == 0 && cg.time - cg.codQkickStart >= COD_QUICKKICK_HOLD_MS ) {
		trap_SendConsoleCommand( "+kick\n" );
		cg.codQkickState = 1;
	} else if ( cg.codQkickState == 1 && cg.time - cg.codQkickStart >= COD_QUICKKICK_HOLD_MS + COD_QUICKKICK_PULSE_MS ) {
		trap_SendConsoleCommand( "-kick\n" );
		cg.codQkickState = 2;
	}
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

	// Debug: keep the full reticle while ADS so iron sights can be aligned to center.
	if ( bg_cod.adsCrosshairForce ) {
		ads = 0.0f;
	}

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
	if ( th < 0.25f ) {
		th = 0.25f;
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
			// top / bottom / left / right — 0.5 unit larger behind (thin outline for thin posts)
			CG_CodDrawCrosshairBar( cx - halfTh - 0.5f, cy - gap - len - 0.5f, th + 1.0f, len + 1.0f, outlineColor );
			CG_CodDrawCrosshairBar( cx - halfTh - 0.5f, cy + gap - 0.5f,       th + 1.0f, len + 1.0f, outlineColor );
			CG_CodDrawCrosshairBar( cx - gap - len - 0.5f, cy - halfTh - 0.5f, len + 1.0f, th + 1.0f, outlineColor );
			CG_CodDrawCrosshairBar( cx + gap - 0.5f,       cy - halfTh - 0.5f, len + 1.0f, th + 1.0f, outlineColor );
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
	// One horizontal run per physical pixel row: rows never overlap (no alpha
	// build-up) and the edge steps are 1 px, not 1 virtual unit.
	float dx, dy, rowH, ya, yb, y, t, xc, halfW, slope;

	dx = x1 - x0;
	dy = y1 - y0;
	if ( dy == 0.0f ) {
		return;
	}
	rowH = 1.0f / ( cgs.screenYScale > 0.0f ? cgs.screenYScale : 1.0f );
	slope = dx / dy;
	// Horizontal width of a line of perpendicular thickness th.
	halfW = th * 0.5f * (float)sqrt( 1.0f + slope * slope );

	ya = dy < 0.0f ? y1 : y0;
	yb = dy < 0.0f ? y0 : y1;
	for ( y = ya; y < yb; y += rowH ) {
		t = ( y + rowH * 0.5f - y0 ) / dy;
		xc = x0 + dx * t;
		CG_FillRect( xc - halfW, y, halfW * 2.0f, rowH, color );
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
	if ( th < 0.25f ) {
		th = 0.25f;
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

/*
=================
CG_CodPadAimAssist

Controller-only. Always-on target friction when last input is pad (LOS required).
Optional Auto-Aim (cod_padAutoAim) snaps onto the closest visible enemy while ADS
only — hipfire never snaps/locks.

Writes ROM cvars for CL_JoystickMove:
  cod_padAimSlowYaw / SlowPitch — per-axis look multipliers (friction)
  cod_padAimPullYaw / Pitch     — degrees this frame (magnetism / auto-aim)
=================
*/
void CG_CodPadAimAssist( void ) {
	char lastIn[16];
	int i;
	float bestScore;
	float maxAng;
	centity_t *best;
	vec3_t viewOrg, viewAng, forward;
	qboolean ads;
	qboolean autoAds;
	float slowYaw, slowPitch;
	float pullYaw, pullPitch;

	trap_Cvar_Set( "cod_padAimSlow", "1" );
	trap_Cvar_Set( "cod_padAimSlowYaw", "1" );
	trap_Cvar_Set( "cod_padAimSlowPitch", "1" );
	trap_Cvar_Set( "cod_padAimPullYaw", "0" );
	trap_Cvar_Set( "cod_padAimPullPitch", "0" );

	if ( !bg_cod.enable || !cg.snap ) {
		return;
	}

	trap_Cvar_VariableStringBuffer( "in_lastInput", lastIn, sizeof( lastIn ) );
	if ( Q_stricmp( lastIn, "pad" ) ) {
		return;
	}

	VectorCopy( cg.refdef.vieworg, viewOrg );
	VectorCopy( cg.refdefViewAngles, viewAng );
	AngleVectors( viewAng, forward, NULL, NULL );

	ads = ( CG_CodAdsFrac() > 0.2f ) ? qtrue : qfalse;
	autoAds = ( bg_cod.padAutoAim && ads ) ? qtrue : qfalse;
	// Wider cone while Auto-Aim + ADS so the closest enemy can be acquired.
	maxAng = autoAds ? 32.0f : 18.0f;

	best = NULL;
	bestScore = 9999.0f;

	for ( i = 0; i < MAX_GENTITIES; i++ ) {
		centity_t *cent = &cg_entities[i];
		entityState_t *es;
		vec3_t chest, dir, wantAng, delta;
		trace_t tr;
		float dist, yawErr, pitchErr, angErr, score;

		if ( !cent->currentValid ) {
			continue;
		}
		es = &cent->currentState;
		if ( es->eType != ET_PLAYER ) {
			continue;
		}
		if ( es->eFlags & EF_DEAD ) {
			continue;
		}
		if ( es->number == cg.snap->ps.clientNum ) {
			continue;
		}
		// Skip same-team (friendly AI / teammates). TEAM_FREE = no filter.
		{
			clientInfo_t *ci = &cgs.clientinfo[es->clientNum];
			int myTeam = cgs.clientinfo[cg.snap->ps.clientNum].team;
			if ( ci->infoValid && myTeam != TEAM_FREE && ci->team == myTeam ) {
				continue;
			}
		}

		VectorCopy( cent->lerpOrigin, chest );
		chest[2] += 20.0f; // chest-ish above origin
		VectorSubtract( chest, viewOrg, dir );
		dist = VectorNormalize( dir );
		if ( dist < 32.0f || dist > 1500.0f ) {
			continue;
		}
		if ( DotProduct( dir, forward ) < ( autoAds ? 0.10f : 0.35f ) ) {
			continue;
		}

		// No assist through walls — world solid must be clear to the chest.
		CG_Trace( &tr, viewOrg, NULL, NULL, chest, cg.snap->ps.clientNum, MASK_SOLID );
		if ( tr.fraction < 1.0f ) {
			continue;
		}

		vectoangles( dir, wantAng );
		AnglesSubtract( wantAng, viewAng, delta );
		yawErr = fabs( AngleNormalize180( delta[YAW] ) );
		pitchErr = fabs( AngleNormalize180( delta[PITCH] ) );
		angErr = yawErr + pitchErr;
		if ( angErr > maxAng ) {
			continue;
		}

		// Auto-Aim + ADS: prefer closest visible enemy in the cone.
		if ( autoAds ) {
			score = dist + angErr * 2.0f;
		} else {
			score = angErr + dist * 0.01f;
		}
		if ( score < bestScore ) {
			bestScore = score;
			best = cent;
		}
	}

	slowYaw = 1.0f;
	slowPitch = 1.0f;
	pullYaw = 0.0f;
	pullPitch = 0.0f;

	if ( best ) {
		vec3_t chest, dir, wantAng, delta;
		float dist, yawSigned, pitchSigned, yawErr, pitchErr, angErr, t, friction;
		float frametime, magRate, magStep;

		VectorCopy( best->lerpOrigin, chest );
		chest[2] += 20.0f;
		VectorSubtract( chest, viewOrg, dir );
		dist = VectorNormalize( dir );
		vectoangles( dir, wantAng );
		AnglesSubtract( wantAng, viewAng, delta );
		yawSigned = AngleNormalize180( delta[YAW] );
		pitchSigned = AngleNormalize180( delta[PITCH] );
		yawErr = fabs( yawSigned );
		pitchErr = fabs( pitchSigned );
		angErr = yawErr + pitchErr;

		// Strength 0..1 from centered + close.
		t = 1.0f - ( angErr / maxAng );
		if ( t < 0.0f ) {
			t = 0.0f;
		}
		if ( dist < 400.0f ) {
			t += ( 1.0f - dist / 400.0f ) * 0.35f;
		}
		if ( t > 1.0f ) {
			t = 1.0f;
		}

		// Target friction (always-on, LOS already required). Soft track — not a snap.
		friction = t * ( ads ? 0.62f : 0.42f );
		slowYaw = 1.0f - friction * 0.55f;
		slowPitch = 1.0f - friction * 0.80f;
		if ( yawErr > pitchErr + 2.0f ) {
			slowPitch = 1.0f - friction * 0.92f;
		} else if ( pitchErr > yawErr + 2.0f ) {
			slowYaw = 1.0f - friction * 0.85f;
		}
		if ( slowYaw < 0.28f ) {
			slowYaw = 0.28f;
		}
		if ( slowPitch < 0.22f ) {
			slowPitch = 0.22f;
		}

		frametime = cg.frametime * 0.001f;
		if ( frametime <= 0.0f ) {
			frametime = 0.016f;
		}

		if ( autoAds && dist < 1200.0f ) {
			// Console-style ADS snap onto closest visible enemy.
			float rate, step;

			rate = 1440.0f;
			step = rate * frametime;
			if ( yawErr > 0.02f ) {
				pullYaw = ( yawErr <= step ) ? yawSigned : ( ( yawSigned > 0.0f ) ? step : -step );
			}
			if ( pitchErr > 0.02f ) {
				pullPitch = ( pitchErr <= step ) ? pitchSigned : ( ( pitchSigned > 0.0f ) ? step : -step );
			}
		} else if ( !bg_cod.padAutoAim ) {
			// Light magnetism only when Auto-Aim is off (never hipfire-lock with Auto-Aim on).
			magRate = ( ads ? 45.0f : 28.0f ) * t;
			magStep = magRate * frametime;
			if ( yawErr > 0.08f ) {
				pullYaw = ( yawSigned > 0.0f ) ? magStep : -magStep;
				if ( fabs( pullYaw ) > yawErr ) {
					pullYaw = yawSigned;
				}
			}
			if ( pitchErr > 0.08f ) {
				pullPitch = ( pitchSigned > 0.0f ) ? magStep : -magStep;
				if ( fabs( pullPitch ) > pitchErr ) {
					pullPitch = pitchSigned;
				}
			}
		}
	}

	trap_Cvar_Set( "cod_padAimSlow", va( "%g", ( slowYaw < slowPitch ) ? slowYaw : slowPitch ) );
	trap_Cvar_Set( "cod_padAimSlowYaw", va( "%g", slowYaw ) );
	trap_Cvar_Set( "cod_padAimSlowPitch", va( "%g", slowPitch ) );
	trap_Cvar_Set( "cod_padAimPullYaw", va( "%g", pullYaw ) );
	trap_Cvar_Set( "cod_padAimPullPitch", va( "%g", pullPitch ) );
}
