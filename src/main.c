/*
Raylib platformer: AABB collision + Celeste-style movement.
Ported from a Unity PlayerMovement script (run accel, coyote time, jump buffer,
variable jump height, hang time, double jump, glide, wall slide / grab / jump,
stamina, 8-way dash with hitstop, super/hyper dash jumps, crouch + crouch
slide).

Controls
  Move ........ A/D or Left/Right
  Up / Down ... W/S or Up/Down (climb on wall, hyper dash jump)
  Crouch ...... Left Shift (crouch / crouch slide / fast-fall)
  Aim gun ..... W/A/S/D (8 directions)   Fire: U or P
  Jump ........ J or Z   (hold = higher, press again in air = double jump,
                              hold after out of jumps = glide)
  Dash ........ K or X (8 directions)
  Wall grab ... hold L or Left Ctrl against a wall
  R ........... respawn      F1 ... debug view (hitboxes / probes)
*/

#include "art.h"   // all sprites / animations / tilesets live in art.c
#include "fx.h"    // ghost trail, particles, ripple, speed rings
#include "level.h" // loads levels made in LDtk
#include "raylib.h"
#include "resource_dir.h" // from raylib-quickstart: SearchAndSetResourceDir()
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// =====================================================================
//  TUNING  -- values copied from the Unity PlayerData asset
//  Unity units -> pixels:  1 unit = PPU pixels. 8 px art tile = 1 unit, drawn
//  at scale 4 -> PPU 32. Speeds are units/s, so  U(25)  = 25 units/s = 800
//  px/s. +y is DOWN in raylib.
// =====================================================================
#define PPU 32.0f
#define U(v) ((v) * PPU)

#define PHYSICS_DT (1.0f / 240.0f)

#define PLAYER_W 40.0f // hitbox: 10 x 16 art px at scale 4 (sprite is 16 x 18)
#define STAND_H 64.0f
#define CROUCH_H 30.0f // a bit under 1 tile so it fits the 1-tile tunnel

// ---- Gravity / falling ----
#define FALL_GRAV_MULT 1.5f  // fallGravityMult
#define MAX_FALL_SPEED U(25) // maxFallSpeed
#define FAST_FALL_MULT 5.0f  // fastFallGravityMult
#define MAX_FAST_FALL U(30)  // maxFastFallSpeed

// ---- Run ----
// runAccelAmount  = 50 * runAcceleration  / runMaxSpeed = 5   (1/s, rigidbody
// mass 1) runDeccelAmount = 50 * runDecceleration / runMaxSpeed = 10
#define RUN_MAX_SPEED U(25) // runMaxSpeed
#define RUN_ACCEL 5.0f
#define RUN_DECEL 10.0f
#define ACCEL_IN_AIR 1.0f      // accelInAir
#define DECEL_IN_AIR 0.201f    // deccelInAir
#define SNAP_MIN U(0.1f)       // SnapMin
#define SNAP_MAX U(0.1f)       // SnapMax
#define CONSERVE_MOMENTUM true // doConserveMomentum

// ---- Crouch ----
#define CROUCH_SLIDE_FORCE U(8)     // CrouchSlideForce
#define CROUCH_SLIDE_MIN U(15)      // MinCrouchslideSpeed
#define CROUCH_SLIDE_MAX U(12 + 15) // MaxCrouchslideSpeed = force + runMaxSpeed
#define CROUCH_FRICTION_MULT 0.2f   // crouchFrictionMultiplier
#define CROUCH_WALK_SPEED                                                      \
  80.0f // (added) slow crouch-walk so you can't get stuck under a ceiling

// ---- Jump ----
#define JUMP_HEIGHT U(3.5f)     // jumpHeight
#define JUMP_TIME_TO_APEX 0.45f // jumpTimeToApex
#define GRAVITY (2.0f * JUMP_HEIGHT / (JUMP_TIME_TO_APEX * JUMP_TIME_TO_APEX))
#define JUMP_FORCE (GRAVITY * JUMP_TIME_TO_APEX) // = 15.56 units/s
#define JUMP_SPEED_BOOST U(0.5f)                 // jumpSpeedBoost
#define AIR_JUMPS 1             // jumpCount (jumps AFTER the ground jump)
#define JUMP_CUT_GRAV_MULT 4.0f // jumpCutGravityMult
#define HANG_GRAV_MULT 0.5f     // jumpHangGravityMult
#define HANG_THRESHOLD U(0.2f)  // jumpHangTimeThreshold
#define HANG_ACCEL_MULT 0.43f   // jumpHangAccelerationMult
#define HANG_SPEED_MULT 0.97f   // jumpHangMaxSpeedMult
#define COYOTE_TIME 0.10f       // coyoteTime
#define JUMP_BUFFER 0.15f       // jumpInputBufferTime
// The Unity script only lets you jump again once you START FALLING (CanJump has
// !IsJumping). true = you may double-jump while still rising; false = original.
#define ALLOW_JUMP_WHILE_RISING true

// ---- Glide ----
#define GLIDE_GRAV_MULT 0.35f  // glideGravityMult
#define GLIDE_MAX_FALL U(7.5f) // glideMaxFallSpeed

// ---- Wall jump ----
#define WALL_JUMP_FORCE_X U(17)   // wallJumpForce.x
#define WALL_JUMP_FORCE_Y U(15)   // wallJumpForce.y
#define CLIMB_JUMP_FORCE U(10)    // climbJumpForce
#define WALL_JUMP_GRACE 0.1f      // wallJumpGraceTime
#define WALL_JUMP_RUN_LERP 0.5f   // wallJumpRunLerp
#define WALL_JUMP_TIME 0.2f       // wallJumpTime
#define DO_TURN_ON_WALL_JUMP true // doTurnOnWallJump (face away from the wall)

// ---- Wall slide / climb ----
#define WALL_SLIDE_SPEED U(13)   // slideSpeed (-13)
#define WALL_SLIDE_ACCEL 10.0f   // slideAccel
#define WALL_SLIDE_UP_DECEL 1.1f // slideUpDeccel
#define WALL_SLIDE_MIN_UP_SPEED                                                \
  U(0) // slideMinUpSpeed (upward drift below this is cut to 0)
#define MIN_SPEED_ZERO_GRAV U(10)         // Minspeedtogozerograv
#define CLIMB_UP_SPEED U(5)               // climbUpSpeed
#define CLIMB_DOWN_SPEED WALL_SLIDE_SPEED // grab + down uses slideSpeed

// ---- Stamina ----
#define MAX_STAMINA 250.0f    // maxStamina
#define LOW_STAMINA 30.0f     // below this the player flashes red
#define WALL_JUMP_COST 50.0f  // wallJumpCost
#define CLIMB_JUMP_COST 25.0f // climbJumpCost
#define GRAB_DRAIN 8.0f       // grabDrainRate  (per second)
#define CLIMB_DRAIN 15.0f     // climbDrainRate (per second)

// ---- Dash ----
#define DASH_AMOUNT 1          // dashAmount
#define DASH_SPEED U(50)       // dashSpeed
#define DASH_ATTACK_TIME 0.15f // dashAttackTime
#define DASH_END_TIME 0.1f     // dashEndTime
#define DASH_END_SPEED_X                                                       \
  U(25) // dashEndSpeed.x (multiplied per-axis with the dash direction)
#define DASH_END_SPEED_Y U(15) // dashEndSpeed.y
#define DASH_END_RUN_LERP 0.0f // dashEndRunLerp
#define DASH_SLEEP 0.1f        // dashSleepTime (hitstop)
#define DASH_BUFFER 0.309f     // dashInputBufferTime
#define DASH_REFILL_TIME 0.09f // dashRefillTime
// Dash ledge correction (the script's dashCornerCorrection /
// dashCornerCorrectionfix): dashing head-on into a wall whose top edge is low
// enough hops you ONTO the ledge.
#define DASH_CORNER_CORRECTION                                                 \
  U(-0.2f) // dashCornerCorrection (Unity sign: negative = stricter,
           // the ledge must be this far below your centre)
#define DASH_CORNER_CORRECTION_FIX                                             \
  U(0.155f) // dashCornerCorrectionfix (lift above the ledge top)
#define DASH_CORNER_SKIN U(0.1f) // skinWidth in the script

// ---- Dash jumps (super / hyper) ----
// horizontal = dashTypeMaxSpeed * dashJumpSpeedMult * preserveDashSpeedMult
#define SUPER_JUMP_VY U(17)                    // superDashVForce
#define SUPER_JUMP_SPEED U(30 * 0.91f * 0.96f) // superDashMaxSpeed
#define HYPER_JUMP_VY U(12)                    // hyperDashVForce
#define HYPER_JUMP_SPEED U(40 * 0.91f * 0.96f) // hyperDashMaxSpeed
#define DASH_REFUND_MIN_DELAY 0.05f            // dashRefundMinDelay
#define DASH_REFUND_AMOUNT 1                   // dashRefundAmount

// ---- Juice / collision ----
// ---- Gun knockback (PlayerData "Gun" header) ----
// These three were NOT in the asset values you sent me - they are guesses. Tune
// them.
#define KNOCKBACK_FORCE                                                        \
  U(25) // knockbackForce (added to your velocity, away from the cursor)
#define KNOCKBACK_STUN                                                         \
  0.15f // knockbackStunDuration (run input ignored this long)
#define KNOCKBACK_FREEZE                                                       \
  0.06f // knockbackFreezeDuration (freeze-frame on firing)

// Squash & stretch: scale goes 1 -> (X, Y) over TIME seconds, then back over
// TIME. Defaults are the ones from the Unity script (JumpSqueeze /
// LandSqueeze).
#define JUMP_SQUASH_X 0.7f
#define JUMP_SQUASH_Y 1.0f
#define JUMP_SQUASH_TIME 0.2f
#define LAND_SQUASH_X 1.25f
#define LAND_SQUASH_Y 0.8f
#define LAND_SQUASH_TIME 0.05f
// Crouch squash: plays CROUCH_TRANSITION seconds after you crouch (the script
// waits for the crouch animation to finish; its default is 1.0 s, so match your
// animation length).
#define CROUCH_SQUASH_X 1.25f
#define CROUCH_SQUASH_Y 0.8f
#define CROUCH_SQUASH_TIME 0.05f
#define CROUCH_TRANSITION 0.1f // _crouchTransitionDuration
#define UNCROUCH_SQUASH_X 1.25f
#define UNCROUCH_SQUASH_Y 0.8f
#define UNCROUCH_SQUASH_TIME 0.05f
#define UNCROUCH_TRANSITION 0.1f // _UncrouchTransitionDuration

// Camera shake when you dash (cm.Shake in the Unity script). Amplitude is in
// screen pixels. These are guesses - match them to your Cinemachine values.
#define SHAKE_AMPLITUDE 4
#define SHAKE_DURATION 0.15f
#define PROBE 2.0f    // thickness of ground / wall probe boxes
#define MAX_STEP 4.0f // collision sub-step (px) -> no tunnelling at dash speed
#define AABB_EPS 0.01f

// =====================================================================
//  INPUT BINDINGS  (primary + alternate key per action)
// =====================================================================
#define BIND_LEFT_1 KEY_A
#define BIND_LEFT_2 KEY_LEFT
#define BIND_RIGHT_1 KEY_D
#define BIND_RIGHT_2 KEY_RIGHT
#define BIND_UP_1 KEY_W // climb
#define BIND_UP_2 KEY_UP
#define BIND_DOWN_1 KEY_S // climb down / hyper dash
#define BIND_DOWN_2 KEY_DOWN
#define BIND_CROUCH KEY_LEFT_SHIFT // crouch / crouch slide / fast-fall

// gun aim (8 directions)
#define BIND_AIM_LEFT KEY_A
#define BIND_AIM_RIGHT KEY_D
#define BIND_AIM_UP KEY_W
#define BIND_AIM_DOWN KEY_S

#define BIND_JUMP_1 KEY_J
#define BIND_JUMP_2 KEY_Z
#define BIND_DASH_1 KEY_K
#define BIND_DASH_2 KEY_X
#define BIND_GRAB_1 KEY_L
#define BIND_GRAB_2 KEY_LEFT_CONTROL

#define BIND_RESPAWN KEY_R
#define BIND_DEBUG KEY_F1
#define BIND_PIXEL_MODE KEY_F2 // toggle upscale-first / pixel-first at runtime
// Pixel-perfect layer mode (F2 toggles it while playing):
//   false = draw at art resolution, upscale last  (gun rotates on the art-pixel
//           grid, like Unity's pixel-perfect camera)   <- default
//   true  = upscale first: draw at full resolution, positions still snapped to
//           the art-pixel grid (rotated sprites keep smooth full-res edges)
#define UPSCALE_FIRST_DEFAULT true
#define REF_W 1280 // the view you designed for = 320 x 180 art pixels
#define REF_H 720
#define BIND_RELOAD KEY_F5 // reload the LDtk level without restarting the game
#define BIND_FIRE                                                              \
  KEY_U // gun: fires along the aim, knocks YOU back the other way
#define BIND_FIRE_2 KEY_P

// =====================================================================
//  LEVEL  (every solid is an axis-aligned rectangle)
//
//  Everything is placed on the TILE GRID: T(x, y, w, h) takes TILE units, one
//  tile = ART_TILE_PX (32 px = one 8x8 art tile at 4x). Because every platform
//  is a whole number of tiles wide and tall, tiles never get cropped.
//  The ground's top surface is at tile row GROUND_ROW.
// =====================================================================
#define TILE ((float)ART_TILE_PX)
#define GROUND_ROW 16
// The ACTIVE level: the LDtk file if it loads, otherwise the built-in test
// level above.
#define LEVEL_FILE "levels/world.ldtk" // inside the resources folder
static Level level;
static bool levelLoaded = false;
static Rectangle *solids = NULL;
static int solidCount = 0;
#define SOLID_COUNT solidCount
static Vector2 spawnPos;       // top-left of the player when (re)spawning
static float killY = 1600.0f;  // fall below this and you respawn
static float shakeTime = 0.0f; // camera shake timer (set when dashing)

// =====================================================================
//  AABB
// =====================================================================
static bool AABBOverlap(Rectangle a, Rectangle b) {
  return a.x + AABB_EPS < b.x + b.width && a.x + a.width - AABB_EPS > b.x &&
         a.y + AABB_EPS < b.y + b.height && a.y + a.height - AABB_EPS > b.y;
}

// index of the first solid overlapping r, or -1
static int FirstSolidHit(Rectangle r) {
  for (int i = 0; i < SOLID_COUNT; i++)
    if (AABBOverlap(r, solids[i]))
      return i;
  return -1;
}

// =====================================================================
//  PLAYER
// =====================================================================
typedef struct {
  Rectangle box; // the AABB (x,y = top-left)
  Vector2 vel;

  // input (refreshed every frame)
  Vector2 in; // x: -1..1, y: +1 up / -1 down
  bool jumpHeld, grabHeld;

  // timers
  float time;
  float lastOnGround, lastOnWall, lastOnWallL, lastOnWallR;
  float lastPressedJump, lastPressedDash, lastJumpPressTime;
  float wallJumpStart;
  float dashStartTime, dashTimer, dashEndTimer, dashRefillTimer;
  float hitstop;
  float convertTimer;
  int convertDir;

  // gun
  Vector2 aim;          // unit vector from the player toward the cursor
  bool knockbackQueued; // set by ReadInput when you fire
  float knockbackTimer; // > 0: run input is ignored (_inKnockback)

  // crouch squash waiting for the crouch animation (DelayedCrouchSqueeze)
  bool csPending, csEntering;
  float csDelay;

  // state
  bool facingRight, grounded;
  bool jumping, jumpCut, jumpFalling;
  bool wallJumping, wallGrabbing, sliding, gliding;
  bool dashing, dashAttacking, dashEnding;
  bool crouching;
  bool crouchHeld; // crouch key (separate from the down input)
  int jumpsLeft, dashesLeft;
  float stamina;
  Vector2 dashDir;

  // juice: squash & stretch tween
  float sx, sy;
  float squashX, squashY, squashTime, squashT;

  // wall-jump turn lock: after a wall jump you face away from the wall and
  // can't turn back toward it until you are actually moving that way
  bool turnLock;
  int turnLockDir;
} Player;

static float Clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static float Signf(float v) {
  return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f);
}

static void Squash(Player *p, float x, float y, float time) {
  p->squashX = x;
  p->squashY = y;
  p->squashTime = time;
  p->squashT = 0.0f;
}

// Crouch / uncrouch resets the squash, then squeezes again once the animation
// is done.
static void ScheduleCrouchSquash(Player *p, bool entering) {
  p->squashTime = 0.0f;
  p->sx = p->sy = 1.0f;
  p->csPending = true;
  p->csEntering = entering;
  p->csDelay = entering ? CROUCH_TRANSITION : UNCROUCH_TRANSITION;
}

// Face the direction we're pressing, except right after a wall jump (turn
// lock).
static void CheckDirectionToFace(Player *p) {
  if (p->in.x == 0.0f)
    return;
  int req = p->in.x > 0.0f ? 1 : -1;
  if (p->turnLock && req == p->turnLockDir) {
    // asking to turn back toward the wall: only once we're really moving that
    // way
    if ((int)Signf(p->vel.x) == p->turnLockDir || fabsf(p->vel.x) < 0.01f)
      p->turnLock = false;
    else
      return;
  }
  p->facingRight = req > 0;
}

static void Respawn(Player *p) {
  memset(p, 0, sizeof(*p));
  p->box = (Rectangle){spawnPos.x, spawnPos.y, PLAYER_W, STAND_H};
  p->facingRight = true;
  p->jumpsLeft = AIR_JUMPS + 1;
  p->dashesLeft = DASH_AMOUNT;
  p->stamina = MAX_STAMINA;
  p->lastPressedJump = p->lastPressedDash = -1.0f;
  p->sx = p->sy = 1.0f;
  Fx_Reset();
}

// ---------------------------------------------------------------------
// Dash ledge correction - port of the corner-hit branch of StartDash():
//  1. only for a (nearly) head-on dash into a wall
//  2. cast a ray DOWN, just inside the wall face, from 1 unit above your head
//  to
//     1 unit below your feet to find the ledge's top surface
//  3. the ledge must be low enough: ledgeTop >= centre - dashCornerCorrection
//     (y is flipped vs Unity, hence the minus)
//  4. move you past the wall edge and (dashCornerCorrectionfix) above the ledge
//  5. only if that spot is free; otherwise the caller stops you at the wall
// Returns true if the player was moved onto the ledge.
// ---------------------------------------------------------------------
static bool TryDashLedgeCorrection(Player *p, const Rectangle *wall,
                                   float step) {
  if (fabsf(p->dashDir.x) <= 0.8f) // original: dot(dir, normal) < -0.8
    return false;

  float dirX = step > 0 ? 1.0f : -1.0f;
  float faceX = (step > 0) ? wall->x : wall->x + wall->width; // contact point
  float rayX = faceX + dirX * DASH_CORNER_SKIN;
  float startY = p->box.y - U(1.0f);               // 1 unit above the head
  float endY = p->box.y + p->box.height + U(1.0f); // 1 unit below the feet

  // first solid surface the downward ray meets (a ray starting inside a solid
  // "hits" at its start, which is above the player's centre -> rejected below)
  float ledgeTop = 0.0f;
  bool found = false;
  for (int i = 0; i < SOLID_COUNT; i++) {
    const Rectangle *s = &solids[i];
    if (rayX < s->x || rayX > s->x + s->width)
      continue;
    float hitY;
    if (s->y <= startY && startY < s->y + s->height)
      hitY = startY; // started inside it
    else if (s->y >= startY && s->y <= endY)
      hitY = s->y;
    else
      continue;
    if (!found || hitY < ledgeTop) {
      ledgeTop = hitY;
      found = true;
    }
  }
  if (!found)
    return false;

  float centerY = p->box.y + p->box.height / 2.0f;
  if (ledgeTop < centerY - DASH_CORNER_CORRECTION) // not "half above" the ledge
    return false;

  float centerX = faceX + dirX * (p->box.width / 2.0f + DASH_CORNER_SKIN);
  float newCenterY =
      ledgeTop - p->box.height / 2.0f - DASH_CORNER_CORRECTION_FIX;

  // OverlapBox(newPos, size * 0.99): is the spot free?
  float w = p->box.width * 0.99f, h = p->box.height * 0.99f;
  Rectangle check = {centerX - w / 2.0f, newCenterY - h / 2.0f, w, h};
  if (FirstSolidHit(check) >= 0)
    return false;

  p->box.x = centerX - p->box.width / 2.0f;
  p->box.y = newCenterY - p->box.height / 2.0f;
  return true;
}

// ---------------------------------------------------------------------
// Move along ONE axis, resolving against every solid.
// Returns true if something blocked us. Sub-steps keep fast movement
// (dash) from tunnelling through thin platforms.
// ---------------------------------------------------------------------
static bool MoveAxis(Player *p, float amount, bool horizontal,
                     bool cornerAssist) {
  while (amount != 0.0f) {
    float step = Clampf(amount, -MAX_STEP, MAX_STEP);
    amount -= step;

    Rectangle next = p->box;
    if (horizontal)
      next.x += step;
    else
      next.y += step;

    int i = FirstSolidHit(next);
    if (i < 0) {
      p->box = next;
      continue;
    }

    // Dash ledge correction: hop onto a ledge instead of stopping at its wall.
    if (horizontal && cornerAssist &&
        TryDashLedgeCorrection(p, &solids[i], step))
      continue;

    // Snap flush against the solid we hit and stop on this axis.
    Rectangle s = solids[i];
    if (horizontal)
      p->box.x = (step > 0) ? s.x - p->box.width : s.x + s.width;
    else
      p->box.y = (step > 0) ? s.y - p->box.height : s.y + s.height;
    return true;
  }
  return false;
}

// =====================================================================
//  ACTIONS
// =====================================================================
static void DoJump(Player *p) {
  bool onGround = p->lastOnGround > 0; //
  p->lastPressedJump = 0;
  p->lastOnGround = 0;
  if (p->jumpsLeft > 0)
    p->jumpsLeft--;
  p->jumping = true;
  p->wallJumping = false;
  p->jumpCut = false;
  p->jumpFalling = false;
  p->vel.y = -JUMP_FORCE; // for same height
  if (onGround)           // bunnyhop boost
    p->vel.x += p->in.x * JUMP_SPEED_BOOST;
  Fx_Event(FX_JUMP, p->box, (Vector2){0, 0}, p->facingRight);
  Squash(p, JUMP_SQUASH_X, JUMP_SQUASH_Y, JUMP_SQUASH_TIME);
}

static void DoWallJump(Player *p, int dir, bool climbJump, float cost) {
  p->lastPressedJump = 0;
  p->lastOnGround = 0;
  p->lastOnWall = p->lastOnWallL = p->lastOnWallR = 0;
  p->stamina = fmaxf(0.0f, p->stamina - cost);

  p->wallJumping = true;
  p->jumping = false;
  p->jumpCut = false;
  p->jumpFalling = false;
  p->wallJumpStart = p->time;

  if (climbJump) {
    // straight up, stays on the wall side; may convert into a real wall jump
    p->vel.y = (p->vel.y > 0) ? -CLIMB_JUMP_FORCE : p->vel.y - CLIMB_JUMP_FORCE;
    p->convertTimer = WALL_JUMP_GRACE;
    p->convertDir = dir;
  } else {
    float fx = WALL_JUMP_FORCE_X * dir;
    p->vel.x = (Signf(p->vel.x) != Signf(fx)) ? fx : p->vel.x + fx;
    p->vel.y =
        (p->vel.y > 0) ? -WALL_JUMP_FORCE_Y : p->vel.y - WALL_JUMP_FORCE_Y;
    if (DO_TURN_ON_WALL_JUMP) {
      p->facingRight = dir > 0;
      p->turnLock = true;
      p->turnLockDir = -dir; // the direction back toward the wall
    }
    Fx_Event(FX_WALL_JUMP, p->box, (Vector2){(float)dir, 0}, p->facingRight);
  }
}

static void StartDash(Player *p) {
  p->hitstop = DASH_SLEEP;

  Vector2 d = {p->in.x, -p->in.y}; // world y is flipped
  float len = sqrtf(d.x * d.x + d.y * d.y);
  if (len < 0.01f)
    d = (Vector2){p->facingRight ? 1.0f : -1.0f, 0.0f};
  else
    d = (Vector2){d.x / len, d.y / len};
  p->dashDir = d;
  if (d.x != 0.0f)
    p->facingRight = d.x > 0.0f;

  p->dashing = true;
  p->dashAttacking = true;
  p->dashEnding = false;
  p->jumping = p->wallJumping = p->jumpCut = p->jumpFalling = false;
  p->wallGrabbing = false;
  p->gliding = false;
  p->dashesLeft--;
  p->lastOnGround = 0;
  p->lastPressedDash = 0;
  p->dashTimer = DASH_ATTACK_TIME;
  p->dashStartTime = p->time;
  shakeTime = SHAKE_DURATION; // camera shake
  Fx_Event(FX_DASH_START, p->box, p->dashDir,
           p->facingRight); // ghosts + ripple
}

static void EndDashAttack(Player *p) {
  p->dashAttacking = false;
  p->dashEnding = true;
  p->dashEndTimer = DASH_END_TIME;
  p->vel.x = p->dashDir.x * DASH_END_SPEED_X; // Vector2 * Vector2 = per-axis
  p->vel.y = p->dashDir.y * DASH_END_SPEED_Y;
}

// Jump during the dash while standing on the ground: super (or hyper if holding
// down)
static void DoDashJump(Player *p) {
  bool hyper = p->in.y < 0.0f;
  float vy = hyper ? HYPER_JUMP_VY : SUPER_JUMP_VY;
  float vx = hyper ? HYPER_JUMP_SPEED : SUPER_JUMP_SPEED;
  bool late = p->lastJumpPressTime > p->dashStartTime + DASH_REFUND_MIN_DELAY;

  p->lastPressedJump = 0;
  p->lastOnGround = 0;
  if (p->jumpsLeft > 0)
    p->jumpsLeft--;
  p->vel.y = -vy;
  // Direction: held direction wins (so you can dash right, then press left +
  // jump to dash-jump the OTHER way); with no input, keep the dash direction.
  float dirX;
  if (p->in.x != 0.0f)
    dirX = Signf(p->in.x);
  else if (p->dashDir.x != 0.0f)
    dirX = Signf(p->dashDir.x);
  else
    dirX = p->facingRight ? 1.0f : -1.0f;
  p->facingRight = dirX > 0.0f;
  p->vel.x = dirX * vx;
  p->dashing = p->dashAttacking = p->dashEnding = false;
  p->jumping = true;
  p->jumpCut = false;
  if (late) // buffered = refund
    p->dashesLeft = (int)fminf((float)DASH_AMOUNT,
                               (float)(p->dashesLeft + DASH_REFUND_AMOUNT));
  Fx_Event(FX_JUMP, p->box, (Vector2){0, 0}, p->facingRight);
  Squash(p, JUMP_SQUASH_X, JUMP_SQUASH_Y, JUMP_SQUASH_TIME);
}

// Gun knockback (PlayerMovement.ApplyKnockback): fire at the cursor and you are
// thrown the opposite way. Freeze-frame + a short lockout of run input.
static void ApplyKnockback(Player *p) {
  Vector2 kd = {-p->aim.x, -p->aim.y}; // knock back = away from the aim
  p->hitstop = KNOCKBACK_FREEZE;       // Sleep(knockbackFreezeDuration)
  Fx_Event(FX_GUN, p->box, p->aim, p->facingRight);

  // interrupt states that would fight the knockback
  p->wallGrabbing = false;
  p->sliding = false;
  if (p->dashing) // StopCoroutine(StartDash)
    p->dashing = p->dashAttacking = p->dashEnding = false;
  p->jumpCut = p->jumping = p->jumpFalling = false; // no gravity leak

  // remove velocity that opposes the knockback, per axis
  if ((kd.x > 0 && p->vel.x < 0) || (kd.x < 0 && p->vel.x > 0))
    p->vel.x = 0.0f;
  if ((kd.y > 0 && p->vel.y < 0) || (kd.y < 0 && p->vel.y > 0))
    p->vel.y = 0.0f;

  p->vel.x += kd.x * KNOCKBACK_FORCE; // AddForce(..., Impulse)
  p->vel.y += kd.y * KNOCKBACK_FORCE;

  p->knockbackTimer = KNOCKBACK_STUN; // _inKnockback
}

static void Run(Player *p, float lerpAmount, float dt) {
  if (p->knockbackTimer > 0.0f)
    return; // _inKnockback: skip run input during the stun
  float target = p->in.x * (p->crouching ? CROUCH_WALK_SPEED : RUN_MAX_SPEED);
  target = p->vel.x + (target - p->vel.x) * lerpAmount;

  bool hasTarget = fabsf(target) > 0.01f;
  float rate;
  if (p->crouching)
    rate = RUN_DECEL * CROUCH_FRICTION_MULT;
  else if (p->lastOnGround > 0)
    rate = hasTarget ? RUN_ACCEL : RUN_DECEL;
  else
    rate = hasTarget ? RUN_ACCEL * ACCEL_IN_AIR : RUN_DECEL * DECEL_IN_AIR;

  // bonus control at the apex of a jump
  if ((p->jumping || p->wallJumping || p->jumpFalling) &&
      fabsf(p->vel.y) < HANG_THRESHOLD) {
    rate *= HANG_ACCEL_MULT;
    target *= HANG_SPEED_MULT;
  }

  // conserve momentum: don't brake in the air if already faster than max in our
  // direction
  if (CONSERVE_MOMENTUM && fabsf(p->vel.x) > fabsf(target) &&
      Signf(p->vel.x) == Signf(target) && hasTarget && p->lastOnGround < 0)
    rate = 0.0f;

  p->vel.x += (target - p->vel.x) * fminf(1.0f, rate * dt);
}

// =====================================================================
//  UPDATE  (one fixed physics step)
// =====================================================================
static void UpdatePlayer(Player *p, float dt) {
  // ---------- timers ----------
  p->lastOnGround -= dt;
  p->lastOnWall -= dt;
  p->lastOnWallL -= dt;
  p->lastOnWallR -= dt;
  p->lastPressedJump -= dt;
  p->lastPressedDash -= dt;
  p->knockbackTimer -= dt;

  // ---------- gun knockback (queued by ReadInput) ----------
  if (p->knockbackQueued) {
    p->knockbackQueued = false;
    ApplyKnockback(p);
  }

  // ---------- facing ----------
  CheckDirectionToFace(p);

  // ---------- collision probes (thin AABBs hugging the player's box)
  // ----------
  Rectangle gp = {p->box.x + 3, p->box.y + p->box.height, p->box.width - 6,
                  PROBE};
  Rectangle lp = {p->box.x - PROBE, p->box.y + 6, PROBE, p->box.height - 12};
  Rectangle rp = {p->box.x + p->box.width, p->box.y + 6, PROBE,
                  p->box.height - 12};
  p->grounded = FirstSolidHit(gp) >= 0;

  bool checks = (!p->dashing || p->dashAttacking) && !p->jumping;
  if (checks) {
    if (p->grounded) {
      if (p->lastOnGround < -0.1f)
        Squash(p, LAND_SQUASH_X, LAND_SQUASH_Y, LAND_SQUASH_TIME); // landing
      p->lastOnGround = COYOTE_TIME;
      p->stamina = MAX_STAMINA;
      p->jumpsLeft = AIR_JUMPS + 1;
    }
    if (!p->wallJumping) {
      if (FirstSolidHit(rp) >= 0)
        p->lastOnWallR = COYOTE_TIME;
      if (FirstSolidHit(lp) >= 0)
        p->lastOnWallL = COYOTE_TIME;
    }
    p->lastOnWall = fmaxf(p->lastOnWallL, p->lastOnWallR);
    if (p->lastOnWall > 0 && p->lastOnGround <= 0 && p->stamina > 0.0f)
      p->jumpsLeft = AIR_JUMPS; // touching a wall refreshes air jumps (not when
                                // out of stamina)
  }

  // ---------- dash refill (on the ground) ----------
  if (!p->dashing && p->dashesLeft < DASH_AMOUNT && p->lastOnGround > 0) {
    p->dashRefillTimer += dt;
    if (p->dashRefillTimer >= DASH_REFILL_TIME) {
      p->dashesLeft++;
      p->dashRefillTimer = 0;
    }
  } else
    p->dashRefillTimer = 0;

  // ---------- dash ----------
  if (p->lastPressedDash > 0 && p->dashesLeft > 0 && !p->dashing)
    StartDash(p);

  if (p->dashAttacking) {
    if (p->grounded && p->lastPressedJump > 0 &&
        p->lastJumpPressTime >= p->dashStartTime)
      DoDashJump(p);
    else {
      p->dashTimer -= dt;
      if (p->dashTimer <= 0.0f)
        EndDashAttack(p);
    }
  } else if (p->dashEnding) {
    p->dashEndTimer -= dt;
    if (p->dashEndTimer <= 0.0f) {
      p->dashEnding = false;
      p->dashing = false;
    }
  }

  // ---------- jump state flags ----------
  if (p->jumping && p->vel.y > 0) {
    p->jumping = false;
    p->jumpFalling = true;
  }
  if (p->wallJumping && p->time - p->wallJumpStart > WALL_JUMP_TIME)
    p->wallJumping = false;
  if (p->lastOnGround > 0 && !p->jumping && !p->wallJumping) {
    p->turnLock = false;
    p->jumpCut = false;
    p->jumpFalling = false;
  }
  // walked off a ledge without jumping -> the ground jump is gone
  if (p->lastOnGround <= 0 && !p->jumping && !p->wallJumping &&
      p->jumpsLeft == AIR_JUMPS + 1)
    p->jumpsLeft = AIR_JUMPS;

  // climb-jump -> wall-jump conversion (push away from the wall shortly after
  // climb-jumping)
  if (p->convertTimer > 0.0f) {
    p->convertTimer -= dt;
    if (p->in.x != 0.0f && (int)Signf(p->in.x) == p->convertDir) {
      p->stamina = Clampf(p->stamina + CLIMB_JUMP_COST - WALL_JUMP_COST, 0.0f,
                          MAX_STAMINA);
      float fx = WALL_JUMP_FORCE_X * p->convertDir;
      p->vel.x = (Signf(p->vel.x) != Signf(fx)) ? fx : p->vel.x + fx;
      if (DO_TURN_ON_WALL_JUMP) {
        p->facingRight = p->convertDir > 0;
        p->turnLock = true;
        p->turnLockDir = -p->convertDir;
      }
      Fx_Event(FX_WALL_JUMP, p->box, (Vector2){(float)p->convertDir, 0},
               p->facingRight);
      p->convertTimer = 0.0f;
    }
  }

  // ---------- wall jump / jump ----------
  if (!p->dashing && p->lastPressedJump > 0) {
    int dir = (p->lastOnWallR > 0) ? -1 : 1; // direction AWAY from the wall
    bool inputToward = p->in.x != 0.0f && (int)Signf(p->in.x) == -dir;
    bool climbJump = p->wallGrabbing && p->in.y > 0.0f && inputToward;
    float cost = climbJump ? CLIMB_JUMP_COST : WALL_JUMP_COST;

    if (p->lastOnWall > 0 && p->lastOnGround <= 0) // works even with no stamina
      DoWallJump(p, dir, climbJump, cost);
    else if (p->jumpsLeft > 0 && !p->gliding &&
             (ALLOW_JUMP_WHILE_RISING || !p->jumping))
      DoJump(p);
  }

  // ---------- glide ----------
  if (!p->dashing && !p->wallGrabbing && !p->wallJumping &&
      p->lastPressedJump > 0 && p->lastOnGround <= 0 && p->jumpsLeft <= 0 &&
      p->vel.y > 0 && !p->gliding) {
    p->gliding = true;
    p->lastPressedJump = 0;
  }
  if (p->gliding &&
      (!p->jumpHeld || p->lastOnGround > 0 || p->dashing || p->wallGrabbing))
    p->gliding = false;

  // ---------- wall grab ----------
  bool canGrab = p->lastOnWall > 0 && !p->dashing && !p->jumping &&
                 !p->wallJumping && p->lastOnGround <= 0 && p->stamina > 0.0f;
  p->wallGrabbing = p->grabHeld && canGrab;
  if (p->wallGrabbing) {
    float rate = fabsf(p->in.y) > 0.0f ? CLIMB_DRAIN : GRAB_DRAIN;
    p->stamina = Clampf(p->stamina - rate * dt, 0.0f, MAX_STAMINA);
    if (p->stamina <= 0.0f)
      p->wallGrabbing = false;
  }

  // ---------- crouch ----------
  bool canStand = true;
  if (p->crouching) {
    Rectangle stand = {p->box.x, p->box.y + p->box.height - STAND_H,
                       p->box.width, STAND_H};
    canStand = FirstSolidHit(stand) < 0;
  }
  bool wantCrouch = p->lastOnGround > 0 && !p->dashing && !p->wallGrabbing &&
                    (p->crouchHeld || !canStand);
  if (wantCrouch && !p->crouching) {
    float ax = fabsf(p->vel.x);
    if (ax > CROUCH_SLIDE_MIN && ax < CROUCH_SLIDE_MAX) // crouch slide burst
      p->vel.x += Signf(p->vel.x) * CROUCH_SLIDE_FORCE;
    p->box.y += STAND_H - CROUCH_H;
    p->box.height = CROUCH_H;
    p->crouching = true;
    ScheduleCrouchSquash(p, true);
  } else if (!wantCrouch && p->crouching && canStand) {
    p->box.y -= STAND_H - CROUCH_H;
    p->box.height = STAND_H;
    p->crouching = false;
    ScheduleCrouchSquash(p, false);
  }

  // ---------- wall slide ----------
  bool canSlide = p->lastOnWall > 0 && !p->jumping && !p->wallJumping &&
                  !p->dashing && p->lastOnGround <= 0;
  p->sliding = canSlide && !p->grabHeld &&
               ((p->lastOnWallL > 0 && p->in.x < 0) ||
                (p->lastOnWallR > 0 && p->in.x > 0));

  // ---------- velocity ----------
  float maxFall = MAX_FALL_SPEED;

  if (p->dashAttacking) {
    p->vel.x = p->dashDir.x * DASH_SPEED; // no gravity during the dash
    p->vel.y = p->dashDir.y * DASH_SPEED;
  } else if (p->wallGrabbing) {
    p->vel.x = 0.0f;
    p->vel.y =
        p->in.y > 0 ? -CLIMB_UP_SPEED : (p->in.y < 0 ? CLIMB_DOWN_SPEED : 0.0f);
  } else {
    if (!p->dashing)
      Run(p, p->wallJumping ? WALL_JUMP_RUN_LERP : 1.0f, dt);
    else if (p->dashEnding)
      Run(p, DASH_END_RUN_LERP, dt);

    // pick the gravity multiplier (same priority order as the Unity script)
    float g = 1.0f;
    if (p->gliding) {
      g = GLIDE_GRAV_MULT;
      maxFall = GLIDE_MAX_FALL;
    } else if (p->sliding && fabsf(p->vel.y) > MIN_SPEED_ZERO_GRAV) {
      g = 0.0f;
    } // fast on wall: slide force takes over
    else if (p->vel.y > 0 && p->crouchHeld) { // fast-fall = crouch key
      g = FAST_FALL_MULT;
      maxFall = MAX_FAST_FALL;
    } else if (p->jumpCut) {
      g = JUMP_CUT_GRAV_MULT;
    } else if ((p->jumping || p->wallJumping || p->jumpFalling) &&
               fabsf(p->vel.y) < HANG_THRESHOLD) {
      g = HANG_GRAV_MULT;
    } else if (p->vel.y > 0) {
      g = FALL_GRAV_MULT;
    }

    p->vel.y += GRAVITY * g * dt;

    if (p->sliding) {
      if (p->vel.y >= 0) // falling / still: ease toward the slide speed
        p->vel.y +=
            (WALL_SLIDE_SPEED - p->vel.y) * fminf(1.0f, WALL_SLIDE_ACCEL * dt);
      else // moving up the wall: bleed the upward speed off
        p->vel.y -= p->vel.y * fminf(1.0f, WALL_SLIDE_UP_DECEL * dt);
      // slideMinUpSpeed: cut a tiny upward drift so you don't float up the wall
      if (p->vel.y < 0.0f && -p->vel.y <= WALL_SLIDE_MIN_UP_SPEED &&
          !p->wallGrabbing)
        p->vel.y = 0.0f;
    }

    if (p->vel.y > maxFall)
      p->vel.y = maxFall;

    // velocity snapping
    if (fabsf(p->in.x) < 0.01f && fabsf(p->vel.x) < SNAP_MIN)
      p->vel.x = 0.0f;
    // Snap to exactly RUN_MAX_SPEED while holding a direction and within
    // SNAP_MAX of it (ground AND air; from above only on the ground so air
    // momentum is kept).
    float diff = fabsf(p->vel.x) - RUN_MAX_SPEED;
    if (!p->crouching && p->in.x != 0.0f && Signf(p->vel.x) == Signf(p->in.x) &&
        ((diff < 0.0f && diff > -SNAP_MAX) ||
         (diff > 0.0f && diff < SNAP_MAX && p->lastOnGround > 0)))
      p->vel.x = Signf(p->vel.x) * RUN_MAX_SPEED;
  }

  // ---------- move + collide (X then Y) ----------
  bool hitX = MoveAxis(p, p->vel.x * dt, true, p->dashAttacking);
  bool hitY = MoveAxis(p, p->vel.y * dt, false, false);
  if (hitX)
    p->vel.x = 0.0f;
  if (hitY)
    p->vel.y = 0.0f;

  // a head-on dash into a wall / floor / ceiling ends the attack phase early
  if (p->dashAttacking && ((hitX && fabsf(p->dashDir.x) > 0.9f) ||
                           (hitY && fabsf(p->dashDir.y) > 0.9f)))
    EndDashAttack(p);

  // ---------- crouch squash (waits for the crouch animation) ----------
  if (p->csPending) {
    p->csDelay -= dt;
    if (p->csDelay <= 0.0f) {
      p->csPending = false;
      if (p->csEntering == p->crouching) { // still in that state
        if (p->csEntering)
          Squash(p, CROUCH_SQUASH_X, CROUCH_SQUASH_Y, CROUCH_SQUASH_TIME);
        else
          Squash(p, UNCROUCH_SQUASH_X, UNCROUCH_SQUASH_Y, UNCROUCH_SQUASH_TIME);
      }
    }
  }

  // ---------- juice: squash & stretch (1 -> target -> 1) ----------
  if (p->squashTime > 0.0f && p->squashT < 2.0f) {
    p->squashT += dt / p->squashTime;
    float t = fminf(p->squashT, 2.0f);
    float amt = t < 1.0f ? t : 2.0f - t; // 0 -> 1 -> 0
    p->sx = 1.0f + (p->squashX - 1.0f) * amt;
    p->sy = 1.0f + (p->squashY - 1.0f) * amt;
  } else {
    p->sx = p->sy = 1.0f;
  }

  // ---------- visual effects ----------
  FxPlayerState fxs = {p->box, p->vel, p->facingRight, p->dashAttacking,
                       p->dashEnding};
  Fx_Tick(dt, &fxs);

  // fell out of the world
  if (p->box.y > killY)
    Respawn(p);

  p->time += dt;
}

// =====================================================================
//  INPUT
// =====================================================================
static void ReadInput(Player *p) {
  float x = 0.0f, y = 0.0f;
  if (IsKeyDown(BIND_LEFT_1) || IsKeyDown(BIND_LEFT_2))
    x -= 1.0f;
  if (IsKeyDown(BIND_RIGHT_1) || IsKeyDown(BIND_RIGHT_2))
    x += 1.0f;
  if (IsKeyDown(BIND_UP_1) || IsKeyDown(BIND_UP_2))
    y += 1.0f;
  if (IsKeyDown(BIND_DOWN_1) || IsKeyDown(BIND_DOWN_2))
    y -= 1.0f;
  p->in = (Vector2){x, y};
  p->crouchHeld = IsKeyDown(BIND_CROUCH);

  p->jumpHeld = IsKeyDown(BIND_JUMP_1) || IsKeyDown(BIND_JUMP_2);
  p->grabHeld = IsKeyDown(BIND_GRAB_1) || IsKeyDown(BIND_GRAB_2);

  if (IsKeyPressed(BIND_JUMP_1) || IsKeyPressed(BIND_JUMP_2)) {
    p->lastPressedJump = JUMP_BUFFER;
    p->lastJumpPressTime = p->time;
  }
  // releasing jump while still rising -> "jump cut" (higher gravity)
  if ((IsKeyReleased(BIND_JUMP_1) || IsKeyReleased(BIND_JUMP_2)) &&
      !p->jumpHeld)
    if ((p->jumping || p->wallJumping) && p->vel.y < 0)
      p->jumpCut = true;

  if (IsKeyPressed(BIND_DASH_1) || IsKeyPressed(BIND_DASH_2))
    p->lastPressedDash = DASH_BUFFER;

  // gun: aim with W/A/S/D (8 directions, keeps the last direction when
  // released); firing queues a knockback the other way
  Vector2 a = {0.0f, 0.0f};
  if (IsKeyDown(BIND_AIM_LEFT))
    a.x -= 1.0f;
  if (IsKeyDown(BIND_AIM_RIGHT))
    a.x += 1.0f;
  if (IsKeyDown(BIND_AIM_UP))
    a.y -= 1.0f; // screen/world y points down
  if (IsKeyDown(BIND_AIM_DOWN))
    a.y += 1.0f;
  if (a.x != 0.0f || a.y != 0.0f) {
    float len = sqrtf(a.x * a.x + a.y * a.y);
    p->aim = (Vector2){a.x / len, a.y / len};
  } else if (p->aim.x == 0.0f && p->aim.y == 0.0f) {
    p->aim = (Vector2){p->facingRight ? 1.0f : -1.0f, 0.0f};
  }
  if (IsKeyPressed(BIND_FIRE) || IsKeyPressed(BIND_FIRE_2))
    p->knockbackQueued = true;
}

// =====================================================================
//  DRAW
// =====================================================================
// Pick which animation to show. Order = priority (first match wins).
static AnimId ChooseAnim(const Player *p) {
  if (p->dashing)
    return ANIM_DASH;
  if (p->wallGrabbing)
    return p->in.y != 0.0f ? ANIM_WALL_CLIMB : ANIM_WALL_GRAB;
  if (p->sliding)
    return ANIM_WALL_SLIDE;
  if (p->crouching)
    return ANIM_CROUCH;
  if (!p->grounded) {
    if (p->gliding)
      return ANIM_GLIDE;
    return p->vel.y < 0.0f ? ANIM_JUMP : ANIM_FALL;
  }
  if (fabsf(p->vel.x) > 20.0f)
    return ANIM_RUN;
  return ANIM_IDLE;
}

static ArtPlayer MakeArtPlayer(const Player *p) {
  ArtPlayer a;
  a.box = p->box;
  a.sx = p->sx;
  a.sy = p->sy;
  a.facingRight = p->facingRight;
  a.noDash = p->dashesLeft <= 0;
  a.lowStamina = p->stamina <= 0.0f ||
                 (p->stamina < LOW_STAMINA && sinf(p->time * 40.0f) > 0.0f);
  a.gliding = p->gliding;
  a.stamina01 = p->stamina / MAX_STAMINA;
  return a;
}

static void DrawDebug(const Player *p) {
  for (int i = 0; i < SOLID_COUNT; i++)
    DrawRectangleLinesEx(solids[i], 1, (Color){255, 255, 255, 90});
  DrawRectangleLinesEx(p->box, 1, LIME);
  DrawRectangleRec((Rectangle){p->box.x + 3, p->box.y + p->box.height,
                               p->box.width - 6, PROBE},
                   GREEN);
  DrawRectangleRec(
      (Rectangle){p->box.x - PROBE, p->box.y + 6, PROBE, p->box.height - 12},
      SKYBLUE);
  DrawRectangleRec((Rectangle){p->box.x + p->box.width, p->box.y + 6, PROBE,
                               p->box.height - 12},
                   ORANGE);
}

// (Re)load the level from the LDtk file; fall back to the built-in level.
static void LoadLevel(void) {
  Level_Unload(&level);
  levelLoaded = Level_Load(&level, LEVEL_FILE, NULL);
  solids = level.solids;
  solidCount = level.solidCount;

  if (levelLoaded) {
    solids = level.solids;
    solidCount = level.solidCount;
    killY = level.height + 512.0f; // a bit below the bottom of the level
    spawnPos = level.hasStart ? (Vector2){level.start.x - PLAYER_W / 2,
                                          level.start.y - STAND_H}
                              // feet on the PlayerStart's bottom edge
                              : (Vector2){120, GROUND_ROW * TILE - STAND_H};
  } else {
    TraceLog(LOG_ERROR, "LEVEL: could not load '%s' - the world is empty",
             LEVEL_FILE);
    killY = 1600.0f;
    spawnPos = (Vector2){0, 0};
    return;
  }
}

static void DrawCheckerboardBackground(Camera2D camera, int screenW,
                                       int screenH) {
  const float cellSize = 32.0f;

  Vector2 topLeft = GetScreenToWorld2D((Vector2){0, 0}, camera);
  Vector2 bottomRight =
      GetScreenToWorld2D((Vector2){(float)screenW, (float)screenH}, camera);

  int startX = (int)floorf(topLeft.x / cellSize) - 1;
  int endX = (int)ceilf(bottomRight.x / cellSize) + 1;
  int startY = (int)floorf(topLeft.y / cellSize) - 1;
  int endY = (int)ceilf(bottomRight.y / cellSize) + 1;

  // Dark base behind the checkerboard.
  ClearBackground((Color){0, 0, 0, 255});

  for (int y = startY; y < endY; y++) {
    for (int x = startX; x < endX; x++) {
      Color square =
          ((x + y) % 2 == 0) ? (Color){0, 0, 0, 255} : (Color){0, 0, 0, 255};

      DrawRectangle((int)(x * cellSize), (int)(y * cellSize), (int)cellSize,
                    (int)cellSize, square);

      // Bright outlines make bending easier to see.
      DrawRectangleLines((int)(x * cellSize), (int)(y * cellSize),
                         (int)cellSize, (int)cellSize, (Color){0, 0, 0, 255});
    }
  }

  // Colored world-origin markers help diagnose coordinate offsets.
  DrawLine(-160, 0, 160, 0, RED);
  DrawLine(0, -160, 0, 160, GREEN);
  DrawCircle(0, 0, 5.0f, YELLOW);
}

// Manual borderless window: undecorated + sized/positioned to the monitor.
// (Avoids ToggleBorderlessWindowed(), which can flicker / end up in true
// exclusive fullscreen on some raylib versions and platforms.)
static bool borderless = false;
static void SetBorderless(bool on) {
  borderless = on;
  if (on) {
    int m = GetCurrentMonitor();
    Vector2 mp = GetMonitorPosition(m);
    SetWindowState(FLAG_WINDOW_UNDECORATED);
    SetWindowPosition((int)mp.x, (int)mp.y);
    // 1px shorter than the monitor so Windows does not promote it to true
    // fullscreen (which hides the taskbar and can blink / change mode)
    SetWindowSize(GetMonitorWidth(m), GetMonitorHeight(m) - 1);
  } else {
    int m = GetCurrentMonitor();
    Vector2 mp = GetMonitorPosition(m);
    ClearWindowState(FLAG_WINDOW_UNDECORATED);
    SetWindowSize(1280, 720);
    SetWindowPosition((int)mp.x + (GetMonitorWidth(m) - 1280) / 2,
                      (int)mp.y + (GetMonitorHeight(m) - 720) / 2);
  }
}

// =====================================================================
//  RENDER LAYERS  (back to front)
//
//    1. TILES LAYER ........ level tiles + background   \
//    2. FX LAYER ........... ghosts, speed rings,        } PIXEL-PERFECT LAYER
//                            particles                   } rendered at 1 art px
//    3. PLAYER LAYER ....... player sprite + gun        /  = 1 texel, then
//                                                          upscaled by an
//                                                          integer (point
//                                                          filter)
//    4. NON-PIXEL FX LAYER . dash ribbon (full res) + ripple shader
//    5. UI LAYER ........... HUD text (and the debug overlay), never distorted
//
//  Everything in the pixel-perfect layer is drawn with a camera zoomed by
//  1/ART_PIXEL into a small render texture, so rotated sprites (the gun) are
//  rasterised on the art-pixel grid like Unity's pixel-perfect camera.
// =====================================================================
// Draws the tiles, fx and player layers. Used for BOTH modes: into the low-res
// target (pixel-first) or straight into the full-res scene (upscale-first).
// Must be called inside BeginMode2D(cam); w/h = size of what we are drawing
// into.
static void DrawPlayerLayer(const Player *pl) {
  ArtPlayer ap = MakeArtPlayer(pl);
  Art_DrawPlayer(&ap);
  Art_DrawGun(
      (Vector2){pl->box.x + pl->box.width / 2, pl->box.y + pl->box.height / 2},
      pl->aim);
}
static void DrawPixelLayerContents(Camera2D cam, int w, int h, Rectangle view,
                                   const Player *pl, bool withPlayer) {
  // --- tiles layer ---
  DrawCheckerboardBackground(cam, w, h);
  if (levelLoaded && level.tileCount > 0)
    Level_Draw(&level, view);

  // --- fx layer ---
  Fx_DrawPixelLayer(); // ghosts, speed rings, particles

  // --- player layer ---
  if (withPlayer) {
    DrawPlayerLayer(pl);
  }
}

static void DrawUILayer(const Player *p, int winH, bool upscaleFirst) {
  // DrawText("Move/Aim: WASD  |  Fire: U/P  |  Jump: J/Z "
  //          "(hold = higher, again = double, hold in air = glide)",
  //          10, 8, 16, RAYWHITE);
  //
  // DrawText(
  //     "Dash: K/X  |  Wall grab: hold L/Ctrl (W/S climb)  |  Shift: "
  //     "crouch / fast-fall / "
  //     "slide  |  R: respawn  |  F1: debug  |  F2: pixel mode  |  F5: reload",
  //     10, 28, 16, RAYWHITE);
  //
  // DrawText(TextFormat("jumps:%d  dashes:%d  stamina:%.0f", p->jumpsLeft,
  //                     p->dashesLeft, p->stamina),
  //          10, 52, 16, YELLOW);
  //
  // DrawText(TextFormat("speed:%.2f", fabsf(p->vel.x)), 10, 72, 16, YELLOW);
  //
  // DrawText(upscaleFirst
  //              ? "pixel mode: UPSCALE FIRST (full-res draw)"
  //              : "pixel mode: PIXEL FIRST (low-res draw, upscale last)",
  //          10, 92, 16, SKYBLUE);
  //
  // DrawFPS(10, winH - 24);
}

// =====================================================================
//  MAIN
// =====================================================================
int main(void) {
  // Flags only work BEFORE InitWindow. Start undecorated so there is no
  // window-style change (= no blinking) after the window is created.
  SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_UNDECORATED);
  InitWindow(1280, 720, "KnockBack Velocity");
  SetBorderless(true);
  int SCREEN_W = GetScreenWidth();
  int SCREEN_H = GetScreenHeight();

  SearchAndSetResourceDir("resources"); // so "player/idle.png" etc. are found
  Art_Load();
  Fx_Load();
  // Render the whole game scene to a texture before applying refraction.
  RenderTexture2D sceneTarget =
      LoadRenderTexture(GetScreenWidth(), GetScreenHeight());

  SetTextureFilter(sceneTarget.texture, TEXTURE_FILTER_POINT);

  // Low-res target for the pixel-perfect layer (sized every frame to the
  // window)
  RenderTexture2D pixelTarget = {0};
  bool upscaleFirst = UPSCALE_FIRST_DEFAULT;

  // Load the refraction shader from resources/shaders/ripple.fs.
  Shader rippleShader = LoadShader(NULL, "shaders/ripple.fs");
  TraceLog(LOG_INFO, "Ripple shader ID: %u", rippleShader.id);

  if (rippleShader.id == 0) {
    TraceLog(LOG_ERROR, "Failed to load shaders/ripple.fs");
  }

  int locResolution = GetShaderLocation(rippleShader, "resolution");

  int locCenter[FX_RIPPLE_CAPACITY];
  int locRadius[FX_RIPPLE_CAPACITY];
  int locStrength[FX_RIPPLE_CAPACITY];

  for (int i = 0; i < FX_RIPPLE_CAPACITY; i++) {
    char name[64];

    snprintf(name, sizeof(name), "rippleCenter%d", i);
    locCenter[i] = GetShaderLocation(rippleShader, name);

    snprintf(name, sizeof(name), "rippleRadius%d", i);
    locRadius[i] = GetShaderLocation(rippleShader, name);

    snprintf(name, sizeof(name), "rippleStrength%d", i);
    locStrength[i] = GetShaderLocation(rippleShader, name);
  }
  LoadLevel(); // must come before Respawn() so spawnPos is set

  // warn about any platform that is not a whole number of tiles (it would crop
  // tiles)
  for (int i = 0; i < SOLID_COUNT; i++) {
    Rectangle r = solids[i];
    if (fmodf(fabsf(r.x), TILE) > 0.01f || fmodf(fabsf(r.y), TILE) > 0.01f ||
        fmodf(fabsf(r.width), TILE) > 0.01f ||
        fmodf(fabsf(r.height), TILE) > 0.01f)
      TraceLog(LOG_WARNING,
               "LEVEL: solid %d is not aligned to the %d px tile grid", i,
               ART_TILE_PX);
  }

  Player player;
  Respawn(&player);

  Camera2D cam = {0};
  cam.offset = (Vector2){SCREEN_W / 2.0f, SCREEN_H / 2.0f};
  cam.zoom = 1.0f;
  cam.target = (Vector2){player.box.x, player.box.y};

  bool debug = false;
  float acc = 0.0f;
  Vector2 prev = {player.box.x,
                  player.box.y}; // position after the previous step
  while (!WindowShouldClose()) {
    float frameDt = fminf(GetFrameTime(), 0.05f);

    if (IsKeyPressed(KEY_F11))
      SetBorderless(!borderless);
    if (IsKeyPressed(BIND_RESPAWN))
      Respawn(&player);
    if (IsKeyPressed(BIND_PIXEL_MODE))
      upscaleFirst = !upscaleFirst;
    if (IsKeyPressed(BIND_DEBUG))
      debug = !debug;
    if (IsKeyPressed(BIND_RELOAD)) {
      LoadLevel();
      Respawn(&player);
    }

    ReadInput(&player);

    // fixed-timestep physics meaning predictable movement at any framerate
    acc += frameDt;
    while (acc >= PHYSICS_DT) {
      prev = (Vector2){player.box.x, player.box.y}; // <-- new
      if (player.hitstop > 0.0f)
        player.hitstop -= PHYSICS_DT; // dash freeze-frame
      else
        UpdatePlayer(&player, PHYSICS_DT);
      acc -= PHYSICS_DT;
    }
    // build the position to DRAW (physics keeps using player.box)
    float alpha = acc / PHYSICS_DT;
    if (fabsf(player.box.x - prev.x) > 64 || fabsf(player.box.y - prev.y) > 64)
      alpha = 1.0f;       // teleport (respawn): don't slide
    Player view = player; // cheap copy for drawing only
    view.box.x = prev.x + (player.box.x - prev.x) * alpha;
    view.box.y = prev.y + (player.box.y - prev.y) * alpha;

    // camera: follow the interpolated player on the art-pixel grid (no easing)
    cam.target.x =
        floorf((view.box.x + view.box.width / 2) / ART_PIXEL) * ART_PIXEL;
    cam.target.y =
        floorf((view.box.y + view.box.height) / ART_PIXEL) * ART_PIXEL - 40;

    Art_UpdatePlayerAnim(ChooseAnim(&player), frameDt);

    // follow the REAL window size (high-DPI / resized windows are bigger than
    // SCREEN_W x SCREEN_H)
    int winW = GetScreenWidth(), winH = GetScreenHeight();

    // whole-number screen pixels per art pixel, so pixels stay crisp:
    // 720p -> 4, 1080p -> 6, 1440p -> 8
    // Borderless mode deliberately makes the window one pixel shorter than
    // the monitor. Allow that 1px height difference before choosing the
    // integer upscale, otherwise 1280x719 falls from 4x to 3x and
    // 1920x1079 falls from 6x to 5x (different amounts of world visible).
    int artScale = (int)floorf(fminf(winW / (float)(REF_W / ART_PIXEL),
                                     (winH + (borderless ? 1.0f : 0.0f)) /
                                         (float)(REF_H / ART_PIXEL)));
    if (artScale < 1)
      artScale = 1;
    float zoom = (float)artScale / ART_PIXEL; // 1.0, 1.5, 2.0 ...
    cam.zoom = zoom;                          // fullCam is copied from cam
    Art_SetZoom(zoom);                        // see step 3
    cam.offset = (Vector2){floorf(winW / 2.0f), floorf(winH / 2.0f)};

    // Full-resolution camera: whole world pixels (+ dash shake). Used by the
    // non-pixel layer, the ripple and the debug overlay.
    Camera2D fullCam = cam;
    fullCam.target = (Vector2){roundf(cam.target.x), roundf(cam.target.y)};
    if (shakeTime > 0.0f) { // dash camera shake (whole pixels, fades out)
      shakeTime -= frameDt;
      int amp =
          (int)(SHAKE_AMPLITUDE * fmaxf(shakeTime, 0.0f) / SHAKE_DURATION +
                0.5f);
      if (amp > 0) {
        fullCam.target.x += (float)GetRandomValue(-amp, amp);
        fullCam.target.y += (float)GetRandomValue(-amp, amp);
      }
    }

    // Pixel-perfect camera: same view, but snapped to the art-pixel grid and
    // zoomed out by ART_PIXEL so 1 art pixel = 1 texel in the low-res target.
    // The leftover (< 1 art pixel) is re-applied when the target is upscaled,
    // so scrolling stays smooth while every sprite stays on the grid.
    int lowW = ((int)ceilf(winW / zoom / ART_PIXEL) + 3) & ~1;
    int lowH = ((int)ceilf(winH / zoom / ART_PIXEL) + 3) & ~1;
    Vector2 snapped = {floorf(fullCam.target.x / ART_PIXEL + 0.5f) * ART_PIXEL,
                       floorf(fullCam.target.y / ART_PIXEL + 0.5f) * ART_PIXEL};
    Camera2D pixelCam = {0};
    pixelCam.target = snapped;
    pixelCam.offset = (Vector2){(float)(lowW / 2), (float)(lowH / 2)};
    pixelCam.zoom = 1.0f / (float)ART_PIXEL;
    // world rectangle covered by the low-res target (used for culling)
    Rectangle pixelView = {snapped.x - (float)(lowW / 2 * ART_PIXEL),
                           snapped.y - (float)(lowH / 2 * ART_PIXEL),
                           (float)(lowW * ART_PIXEL),
                           (float)(lowH * ART_PIXEL)};

    BeginDrawing();

    // Handle window resizing / high-DPI size changes.
    winW = GetScreenWidth();
    winH = GetScreenHeight();

    if (sceneTarget.texture.width != winW ||
        sceneTarget.texture.height != winH) {
      UnloadRenderTexture(sceneTarget);

      sceneTarget = LoadRenderTexture(winW, winH);
      SetTextureFilter(sceneTarget.texture, TEXTURE_FILTER_POINT);
    }
    if (!upscaleFirst && (pixelTarget.texture.width != lowW ||
                          pixelTarget.texture.height != lowH)) {
      if (pixelTarget.id != 0)
        UnloadRenderTexture(pixelTarget);
      pixelTarget = LoadRenderTexture(lowW, lowH);
      SetTextureFilter(pixelTarget.texture, TEXTURE_FILTER_POINT);
    }

    // ------------------------------------------------------------
    // PASS 1 (pixel-first only): PIXEL-PERFECT LAYER into the low-res target
    //         tiles -> fx -> player (+ gun)
    // ------------------------------------------------------------
    if (!upscaleFirst) {
      BeginTextureMode(pixelTarget);
      ClearBackground(BLACK);
      BeginMode2D(pixelCam);
      DrawPixelLayerContents(pixelCam, lowW, lowH, pixelView, &view, false);
      EndMode2D();
      EndTextureMode();
    }

    // ------------------------------------------------------------
    // PASS 2: compose the scene at full resolution
    //         pixel layer + NON-PIXEL FX LAYER (dash ribbon)
    // ------------------------------------------------------------
    BeginTextureMode(sceneTarget);
    ClearBackground(BLACK);

    if (upscaleFirst) {
      // UPSCALE FIRST: draw the pixel layer directly at full resolution
      // (everything is still placed on the art-pixel grid)
      Rectangle fullView = {fullCam.target.x - fullCam.offset.x / zoom,
                            fullCam.target.y - fullCam.offset.y / zoom,
                            winW / zoom, winH / zoom};
      BeginMode2D(fullCam);
      DrawPixelLayerContents(fullCam, winW, winH, fullView, &view, true);
      EndMode2D();
    } else {
      // PIXEL FIRST: blow the low-res target up by an exact integer. Where its
      // top-left corner lands on screen (whole pixels: the camera target and
      // offset are integers):
      Vector2 o =
          GetWorldToScreen2D((Vector2){pixelView.x, pixelView.y}, fullCam);
      DrawTexturePro(pixelTarget.texture,
                     (Rectangle){0, 0, (float)lowW, -(float)lowH},
                     (Rectangle){o.x, o.y, (float)(lowW * artScale),
                                 (float)(lowH * artScale)},
                     (Vector2){0, 0}, 0.0f, WHITE);
    }

    BeginMode2D(fullCam);
    if (!upscaleFirst)
      DrawPlayerLayer(&view); // full-res, so squash & stretch is smooth
    Fx_DrawSmoothLayer();     // dash ribbon
    EndMode2D();
    EndTextureMode();

    // ------------------------------------------------------------
    // PASS 3: ripple (non-pixel layer): send the active ripples to the shader
    // ------------------------------------------------------------
    Vector2 centers[FX_RIPPLE_CAPACITY] = {0};
    float radii[FX_RIPPLE_CAPACITY] = {0};
    float strengths[FX_RIPPLE_CAPACITY] = {0};

    int rippleCount = Fx_GetRippleData(centers, radii, strengths);

    Vector2 centerUV[FX_RIPPLE_CAPACITY] = {0};
    float screenRadii[FX_RIPPLE_CAPACITY] = {0};

    for (int i = 0; i < rippleCount; i++) {
      // Convert the fixed world position to screen coordinates.
      Vector2 screenPos = GetWorldToScreen2D(centers[i], fullCam);

      // The negative source height in DrawTexturePro flips the texture
      // vertically, so flip the center's Y coordinate to match the shader.
      centerUV[i] = (Vector2){screenPos.x / (float)winW,
                              1.0f - screenPos.y / (float)winH};

      // Convert the world-space radius into screen pixels.
      screenRadii[i] = radii[i] * fullCam.zoom;
    }

    Vector2 resolution = {(float)winW, (float)winH};

    SetShaderValue(rippleShader, locResolution, &resolution,
                   SHADER_UNIFORM_VEC2);

    for (int i = 0; i < FX_RIPPLE_CAPACITY; i++) {
      SetShaderValue(rippleShader, locCenter[i], &centerUV[i],
                     SHADER_UNIFORM_VEC2);

      SetShaderValue(rippleShader, locRadius[i], &screenRadii[i],
                     SHADER_UNIFORM_FLOAT);

      SetShaderValue(rippleShader, locStrength[i], &strengths[i],
                     SHADER_UNIFORM_FLOAT);
    }

    // draw the composed scene through the refraction shader
    BeginShaderMode(rippleShader);

    DrawTexturePro(sceneTarget.texture,
                   (Rectangle){0, 0, (float)winW, -(float)winH},
                   (Rectangle){0, 0, (float)winW, (float)winH}, (Vector2){0, 0},
                   0.0f, WHITE);

    EndShaderMode();

    // ------------------------------------------------------------
    // PASS 4: debug overlay (full-res, exact positions, not distorted)
    // ------------------------------------------------------------
    if (debug) {
      BeginMode2D(fullCam);
      // DrawDebug(&player);
      // Vector2 pc = {player.box.x + player.box.width / 2,
      //               player.box.y + player.box.height / 2};
      // DrawCircleV((Vector2){pc.x + player.aim.x * 56, pc.y + player.aim.y *
      // 56},
      //             3.0f, (Color){255, 220, 120, 200}); // aim marker
      EndMode2D();
    }

    // ------------------------------------------------------------
    // PASS 5: UI LAYER (normal screen space, no refraction)
    // ------------------------------------------------------------
    DrawUILayer(&player, winH, upscaleFirst);

    EndDrawing();
  }

  Level_Unload(&level);
  UnloadShader(rippleShader);
  UnloadRenderTexture(sceneTarget);
  if (pixelTarget.id != 0)
    UnloadRenderTexture(pixelTarget);
  Fx_Unload();
  Art_Unload();
  CloseWindow();
  return 0;
}
