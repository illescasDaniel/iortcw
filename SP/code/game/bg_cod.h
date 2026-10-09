// codmod: new file (phase 2.0) -- bg_cod.h
//
// Shared CoD4-style tuning table for the player. Compiled into *both* qagame and cgame
// (see bg_pmove.o entries in SP/Makefile) -- each module gets its own copy of bg_cod,
// kept in sync every frame because g_main.c and cg_main.c both read the same cvars.
//
// See docs/architecture.md section 5 (outer repo) for the design rationale.

#ifndef __BG_COD_H__
#define __BG_COD_H__

#include "../qcommon/q_shared.h"

// codmod: all CoD-style tuning in one struct, filled in by BG_CodUpdateCvars() every frame.
// Never add playerState_t fields for this -- see the golden rules in architecture.md.
typedef struct {
	int enable;             // master switch: 0 = vanilla behaviour everywhere
	int moveEnable;         // speed/sprint/walk changes (2.1)
	int leanEnable;         // lean changes (2.3)

	float runSpeed;         // u/s, player jog
	float sprintScale;      // x runSpeed
	float walkScale;        // x runSpeed when BUTTON_WALKING
	float crouchScale;      // x runSpeed
	float backScale;        // x runSpeed when moving backward
	float strafeScale;      // x runSpeed when strafing

	float sprintTime;           // seconds, full stamina bar drain time while sprinting+moving
	float sprintRechargeTime;   // seconds, empty -> full recharge time
	float sprintMinTime;        // seconds of stamina required to *start* a new sprint
	int sprintForwardOnly;      // qboolean-ish: sprint requires cmd.forwardmove > 0
	float sprintJumpCost;       // stamina units taken on jump (vanilla default 2000)

	float duckTime;    // ms, cgame duck viewheight smoothing
	int duckEase;       // ease-out curve instead of linear
	int duckGunDip;     // small viewmodel dip following the same curve

	float leanMax;         // u, lean offset
	float leanRollScale;   // camera roll = leanf * leanRollScale
	float leanTimeIn;      // ms, time to reach full lean
	float leanTimeOut;     // ms, time to return to center
	int leanAllowFire;     // allow firing (and crosshair) while leaned
	int leanEase;          // smoothstep the displayed lean fraction (cgame-only)

	int aiSpeedMode;     // 0 = vanilla, 1 = match player speeds, 2 = scale anim-derived speeds
	float aiSpeedScale;  // multiplier used by aiSpeedMode 2

	int adsEnable;          // ADS changes (phase 3)
	float adsMoveScale;     // x runSpeed while fully in ADS; default when a weapon's own moveScale is 0
	float adsFovScale;      // x cg_fov while fully in ADS; default when a weapon's own fovScale is 0
	float adsTimeIn;        // ms, default raise time when a weapon's own timeIn is 0
	float adsTimeOut;       // ms, default lower time when a weapon's own timeOut is 0
	float adsSpreadScale;   // x aimSpreadScale while fully in ADS; default when a weapon's own spreadScale is 0
	float hipSpreadMin;     // floor on aimSpreadScale while hip-firing (CoD cone); 0 = off
	float hipSpreadScale;   // extra hip multiplier after the floor (1 = none)
	float adsSensScale;     // extra multiplier on top of the FOV-derived sensitivity scale
	float adsBobScale;      // viewmodel bob/land-dip/idle-drift are scaled by 1 - e*adsBobScale
	float adsGunFovComp;    // pushes the viewmodel forward in proportion to the FOV zoom (cgame)
	float adsSightHeight;   // default u above tag_flash bore to the rear sight (cgame auto-align)
	float adsBarrelBack;    // default u from tag_flash back to the rear sight along the bore
	float adsSightDist;     // default u ahead of the camera for the rear sight when fully ADS
	float adsPitch;         // degrees, global ADS angle bias after auto-align (cgame)
	float adsYaw;           // degrees, global ADS angle bias (neg = clockwise from above)
	float adsRoll;          // degrees, global ADS angle bias (pos ≈ clockwise looking forward)
	float adsRight;         // u, whole-weapon horizontal shift along view right/left (cgame)
	int adsCrosshair;       // 0=keep hip crosshair, 1=ADS center dot (default), 2=hide while ADS
	float scopeSwitchTime;  // ms, rifle<->scope weapon-change time when >0 (phase 3.8); 0 = vanilla 250ms

	int adsLock;      // debug: cgame treats ADS as fully in, visual only (alignment tooling)
	int adsTune;      // debug: live-overwrite cg_codAdsView fine-bias from the Tune* cvars below
	float adsTuneFwd, adsTuneRight, adsTuneUp;      // u, cgame-only fine bias
	float adsTunePitch, adsTuneYaw, adsTuneRoll;    // degrees, cgame-only fine bias
	float adsTuneFov;                               // fovScale preview while tuning, cgame-only

	// codmod: Phase 4 gait / FP movement presentation (docs/phase-4-gait.md)
	int gaitEnable;            // master for gun-lower, gait bob, sprint camera
	float sprintRaiseTime;     // ms, visual gun raise after sprint ends (does not gate fire)
	float sprintGunPitch;      // degrees, viewmodel pitch at full sprint lower
	float sprintGunRoll;       // degrees
	float sprintGunFwd;        // u along view forward (neg = toward camera)
	float sprintGunUp;         // u
	float bobWalkRate;         // pmove bobmove while walking
	float bobRunRate;          // pmove bobmove while jogging
	float bobSprintRate;       // pmove bobmove while sprinting
	float bobWalkAmp;          // cgame weapon/camera bob multiplier while walking
	float bobRunAmp;           // while jogging
	float bobSprintAmp;        // while sprinting
	float sprintRunPitch;      // x cg_runpitch contribution while sprinting
	float sprintRunRoll;       // x cg_runroll contribution while sprinting
	float sprintFovScale;      // FOV scale at full sprint (1 = none)

	// codmod: Phase 5 movement weight + fire responsiveness (docs/phase-5-feel.md)
	int weaponEnable;          // master for pistol tap-fire / fire-interval overrides
	int pistolTapFire;         // SP releasedFire early-clear for Luger/Colt/Silencer
	float pistolTapThreshold;  // ms: clear weaponTime when <= this after release
	float pistolNextShot;      // ms min between pistol shots (0 = ammoTable)
	float pistolFireDelay;     // ms first-shot delay override for pistols
	float autoFireScale;       // scale nextShotTime for SMGs/FG42 (1 = vanilla)
	int semiAutoClick;         // 1 = pistols/bolt/semi need release between shots (hold only sprays autos)
	float jumpVelocity;        // u/s vertical jump for player (vanilla JUMP_VELOCITY 270)
	float gravity;             // player ps.gravity (vanilla 800)
	float accelerate;          // player ground accelerate (vanilla pm_accelerate 10)
	float friction;            // player ground friction (vanilla pm_friction 6)
	float stopspeed;           // player friction control floor (vanilla 100)
	int landSlowEnable;        // brief wishspeed scale after landing
	float landSlowTime;        // ms
	float landSlowScale;       // × wishspeed while land timer active

	int quickKnifeEnable;      // cgame quickknife command (phase 5.5)
	float quickKnifeTimeout;   // ms failsafe to return to previous weapon
	float knifeSwitchTime;     // ms drop/raise when switching to/from knife (0 = vanilla 250)

	// codmod: Phase 5b vitals — health regen, stamina finish, HUD bars
	int vitalsEnable;          // master for regen + linear stamina + bar HUD draw
	int healthRegenEnable;     // auto regen after delay
	float healthRegenDelay;    // ms after last damage before regen
	float healthRegenRate;     // HP per second while regenerating
	int healthRegenPauseHurt;  // damage resets the delay timer
	int healthBarNumber;       // 1 = also paint HP number on the health bar
	float maxHealth;           // player STAT_MAX_HEALTH when > 0 (vanilla 100; CoD-ish 50)
	int sprintLinear;          // single-rate recharge (no 25%/idle bonuses)
	float sprintRechargeDelay; // ms after sprint ends before recharge starts
	float ammoScale;           // × pickup/start ammo + max carry (player only; 2 = double)
	int interactHint;          // cgame: "Press <key> to interact" next to cursor hint

	// codmod: Phase 5c CoD on-screen HUD (docs/phase-5c-hud.md)
	int hudEnable;             // master: clip/reserve ammo, no bars, low-HP blood/crosshair
	int hudAmmoStyle;          // 1 = clip/reserve string; 0 = vanilla split
	int hudHeatColor;          // ammo text white→red by curWeapHeat
	int hudHealthCrosshair;    // crosshair white→red by missing health/max
	int hudLowHealthBlood;     // persistent low-HP blood/vignette
	float hudLowHealthStart;   // health/max at which blood starts
	float hudLowHealthMaxAlpha;// blood overlay alpha near death

	// codmod: CoD 4-post dynamic crosshair (phase 5c follow-up)
	int crosshairStyle;        // 1 = CoD posts+bloom; 0 = vanilla shader
	float crosshairGap;        // base half-gap (640 coords)
	float crosshairLength;     // post length
	float crosshairWidth;      // post thickness
	int crosshairDot;          // 1 = hip-fire center dot
	float crosshairBloom;      // extra gap from aimSpreadScale (0..1)
	float crosshairMoveBloom;  // extra gap at full sprint speed
	float crosshairCrouchScale;// gap multiplier while crouching
	float crosshairSprintAlpha;// post alpha at full sprint (dot stays)
	int crosshairOutline;      // 1 = 1px black outline for contrast

	// codmod: E9 hit markers — X flash + tick sounds on player→enemy damage
	int hitMarkerEnable;       // master: count hits in SP + draw/sound in cgame
	float hitMarkerTime;       // ms the X stays visible
	float hitMarkerGap;        // half-gap from crosshair center to each tick (640)
	float hitMarkerLength;     // diagonal tick length
	float hitMarkerWidth;      // tick thickness
	int hitMarkerSound;        // 1 = play body/headshot ticks

	int debugHud;    // 0 off, 1 HUD, 2 HUD + nearest AI speed
	float menuScale; // /cod_menu text+panel scale (0.5..1.0); cgame UI preference
} codTuning_t;

// codmod: Luger / Colt / Silencer — SP pistol tap-fire set (phase 5).
qboolean BG_CodIsPistol( int weapon );

// codmod: pistols / bolt / semi that must click per shot (not hold-to-spray).
qboolean BG_CodNeedsClickFire( int weapon );

// codmod: player fire cone — hip floor lerps to ADS-tightened spread by adsFrac (0..1).
float BG_CodFireSpreadScale( float aimSpread, int weapon, float adsFrac );

// codmod: generic smoothstep ease, 0..1 in, 0..1 out (phase 3.5+). GCC doesn't warn about
// unused "static inline" functions, so qagame linking this header unused is fine.
static ID_INLINE float BG_CodSmoothstep( float x ) {
	if ( x <= 0.0f ) {
		return 0.0f;
	}
	if ( x >= 1.0f ) {
		return 1.0f;
	}
	return x * x * ( 3.0f - 2.0f * x );
}

// codmod: one instance per module (qagame and cgame each link their own bg_cod.o).
extern codTuning_t bg_cod;

// codmod: single source of truth for cvar name/default/flags and where it lands in bg_cod.
typedef struct {
	const char *name;
	const char *def;
	int flags;
	size_t ofs;      // offset into codTuning_t, see CODOFS() in bg_cod.c
	int isInt;       // qtrue: read cvar.integer, qfalse: read cvar.value
} codCvarDef_t;

extern const codCvarDef_t bg_codCvars[];
extern const int bg_numCodCvars;

// codmod: g_main.c / cg_main.c each keep a static vmCvar_t[BG_COD_MAX_CVARS] and pass it in.
// Bump this if bg_codCvars ever grows past it -- BG_CodRegisterCvars() clamps defensively.
// (phase 3–5c + E9; leave headroom for slide/prone.)
#define BG_COD_MAX_CVARS 128

void BG_CodRegisterCvars( vmCvar_t *cvars, int maxCvars );
void BG_CodUpdateCvars( vmCvar_t *cvars, int maxCvars );

// codmod: per-weapon ADS gameplay tuning (phase 3.2). A field of 0 means "use the matching
// cod_ads* cvar default" -- see BG_CodAdsMoveScale() etc. below. Not const-correct to edit
// at runtime: this is the gameplay table (shared qagame+cgame), separate from cgame's
// live-tunable cg_codAdsView (cg_cod.c, phase 3.5).
typedef struct {
	qboolean canADS;
	float moveScale;       // 0 => cod_adsMoveScale
	float fovScale;         // 0 => cod_adsFovScale
	int timeIn, timeOut;   // ms; 0 => cod_adsTimeIn / cod_adsTimeOut
	float spreadScale;      // 0 => cod_adsSpreadScale
} codWeaponTuning_t;

extern const codWeaponTuning_t bg_codWeapons[WP_NUM_WEAPONS];

// codmod: ADS eligibility from playerState_t alone (phase 3.2). Callers that have a
// pmove_t (PM_CodUpdateADS) additionally gate on waterlevel/ladder, which aren't part of
// playerState_t -- see docs/phase-3-ads.md section 3.2.
qboolean BG_CodCanADS( const playerState_t *ps );

// codmod: resolved per-weapon ADS values (table entry if non-zero, else the cvar default).
float BG_CodAdsMoveScale( int weapon );
float BG_CodAdsFovScale( int weapon );
float BG_CodAdsSpreadScale( int weapon );
int BG_CodAdsTimeIn( int weapon );
int BG_CodAdsTimeOut( int weapon );

// codmod: true for the three "secondary fire" scoped weapons (phase 3.8). They use the
// real RTCW scope instead of iron-sight ADS, but still get the ADS move-speed penalty.
qboolean BG_CodIsScopeWeapon( int weapon );

// codmod: player max reserve ammo after cod_ammoScale (phase 5b). AI / scale≤1 → table maxammo.
int BG_CodMaxAmmo( int weapon );

#endif // __BG_COD_H__
