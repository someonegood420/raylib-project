/*
Raylib platformer: AABB collision + Celeste-style movement.
Ported from a Unity PlayerMovement script (run accel, coyote time, jump buffer,
variable jump height, hang time, double jump, wall slide / grab / jump,
stamina, 8-way dash with hitstop, super/hyper dash jumps, crouch + crouch
slide).

Controls
  Move ........ A/D or Left/Right
  Up / Down ... W/S or Up/Down (climb on wall, hyper dash jump)
  Bombdive .... Shift midair while K is fully charged
  Throw bomb ... U/P; WASD/arrow keys aim, K hold/release burst
  Jump ........ J or Z   (hold = higher, press again in air = double jump,
                              double jump available in air)
  Bombburst .... Hold K until white flashing, release + direction
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
#define RUN_MAX_SPEED U(20) // runMaxSpeed
#define RUN_ACCEL 10.0f
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
#define CROUCH_WALK_SPEED 80.0f // (added) slow crouch-walk so you can't get stuck under a ceiling

// ---- Jump ----
#define JUMP_HEIGHT U(3.5f)     // jumpHeight
#define JUMP_TIME_TO_APEX 0.45f // jumpTimeToApex
#define GRAVITY (2.0f * JUMP_HEIGHT / (JUMP_TIME_TO_APEX * JUMP_TIME_TO_APEX))
#define JUMP_FORCE (GRAVITY * JUMP_TIME_TO_APEX) // = 15.56 units/s
#define JUMP_SPEED_BOOST U(0.5f)                 // jumpSpeedBoost
#define AIR_JUMPS 1                              // jumpCount (jumps AFTER the ground jump)
#define JUMP_CUT_GRAV_MULT 4.0f                  // jumpCutGravityMult
#define HANG_GRAV_MULT 0.5f                      // jumpHangGravityMult
#define HANG_THRESHOLD U(0.2f)                   // jumpHangTimeThreshold
#define HANG_ACCEL_MULT 0.43f                    // jumpHangAccelerationMult
#define HANG_SPEED_MULT 0.97f                    // jumpHangMaxSpeedMult
#define COYOTE_TIME 0.10f                        // coyoteTime
#define JUMP_BUFFER 0.15f                        // jumpInputBufferTime
// The Unity script only lets you jump again once you START FALLING (CanJump has
// !IsJumping). true = you may double-jump while still rising; false = original.
#define ALLOW_JUMP_WHILE_RISING true

// ---- Wall jump ----
#define WALL_JUMP_FORCE_X U(17)   // wallJumpForce.x
#define WALL_JUMP_FORCE_Y U(15)   // wallJumpForce.y
#define CLIMB_JUMP_FORCE U(10)    // climbJumpForce
#define WALL_JUMP_GRACE 0.1f      // wallJumpGraceTime
#define WALL_JUMP_RUN_LERP 0.5f   // wallJumpRunLerp
#define WALL_JUMP_TIME 0.2f       // wallJumpTime
#define DO_TURN_ON_WALL_JUMP true // doTurnOnWallJump (face away from the wall)

// ---- Wall slide / climb ----
#define WALL_SLIDE_SPEED U(13)            // slideSpeed (-13)
#define WALL_SLIDE_ACCEL 10.0f            // slideAccel
#define WALL_SLIDE_UP_DECEL 1.1f          // slideUpDeccel
#define WALL_SLIDE_MIN_UP_SPEED U(0)      // slideMinUpSpeed (upward drift below this is cut to 0)
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

// Explosion-powered abilities (velocity impulses; Raylib has no Rigidbody2D).
#define BOMB_CHARGE_TIME 0.38f
#define BOMB_BURST_SPEED_PENALTY false // false = burst speed is always conserved (old behaviour)
#define BOMB_BURST_CARRY_WINDOW 0.25f  // time after a burst in which a double jump keeps the speed

#define BOMB_DIVE_SPEED U(35) // one-time downward plunge impulse
#define BOMB_BURST_RISE_GRAVITY 1
#define BOMB_BURST_VERTICAL_IMPULSE U(27)      // added upward velocity on K release
#define BOMB_BURST_DIAGONAL_SIDE_IMPULSE U(30) // side velocity

#define BOMB_BURST_DIAGONAL_UP_IMPULSE U(17)

#define BOMB_DIVE_REBOUND_UP U(17)   // upward impulse at ground impact
#define BOMB_DIVE_REBOUND_SIDE U(15) // optional upward-forward rebound
#define BOMB_BURST_FX_TIME 0.16f
#define BOMB_THROW_SPEED U(24)
#define BOMB_FUSE_TIME 1.1f
#define BOMB_BLAST_RADIUS U(2.0f)
#define BOMB_MAX 16
#define BOMB_THROW_DURATION (5.0f / 12.0f)
// 0-based frame of the throw animation where the bomb leaves the hand
// (3 = 4.png). Raise it to release later.
#define BOMB_THROW_RELEASE_FRAME 3

// ---- Juice / collision ----

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

// throwable bomb aim (8 directions)
#define BIND_AIM_LEFT KEY_A
#define BIND_AIM_RIGHT KEY_D
#define BIND_AIM_UP KEY_W
#define BIND_AIM_DOWN KEY_S

#define BIND_JUMP_1 KEY_J
#define BIND_JUMP_2 KEY_Z
#define BIND_BURST_1 KEY_K
// X no longer activates bombburst; charge and release K only.
#define BIND_GRAB_1 KEY_L
#define BIND_GRAB_2 KEY_LEFT_CONTROL

#define BIND_RESPAWN KEY_R
#define BIND_DEBUG KEY_F1
// Pixel-perfect layer mode (F2 toggles it while playing):
//   false = draw at art resolution, upscale last  (gun rotates on the art-pixel
//           grid, like Unity's pixel-perfect camera)   <- default
//   true  = upscale first: draw at full resolution, positions still snapped to
//           the art-pixel grid (rotated sprites keep smooth full-res edges)
#define REF_W 1280 // the view you designed for = 320 x 180 art pixels
#define REF_H 720
#define BIND_RELOAD KEY_F5 // reload the LDtk level without restarting the game
#define BIND_FIRE KEY_U    // throw bomb
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
// The ACTIVE level: the LDtk file if it loads

#define LEVEL_FILE "levels/world.ldtk" // inside the resources folder
static Level level;
static Rectangle *solids = NULL;
static int solidCount = 0;
#define SOLID_COUNT solidCount
static Vector2 spawnPos;       // top-left of the player when (re)spawning
static float killY = 1600.0f;  // fall below this and you respawn
static float shakeTime = 0.0f; // camera shake timer (explosions)

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

// Bomb projectiles live in world coordinates (independent of the camera).
typedef struct {
  Vector2 pos, vel;
  float fuse, angle;
  bool active;
} Bomb;
static Bomb bombs[BOMB_MAX];
static void ExplodeAt(Vector2 pos) {
  Level_Explode(&level, pos, BOMB_BLAST_RADIUS);
  // The collision array may be reallocated when merged IntGrid solids split.
  solids = level.solids;
  solidCount = level.solidCount;
  Fx_Event(FX_EXPLOSION, (Rectangle){pos.x - 8, pos.y - 8, 16, 16}, (Vector2){0, -1});
  shakeTime = SHAKE_DURATION;
}
static void ThrowBomb(Vector2 pos, Vector2 dir) {
  for (int i = 0; i < BOMB_MAX; ++i)
    if (!bombs[i].active) {
      bombs[i] = (Bomb){pos,
                        {dir.x * BOMB_THROW_SPEED, dir.y * BOMB_THROW_SPEED - U(5)},
                        BOMB_FUSE_TIME,
                        0,
                        true};
      break;
    }
}
static void TickBombs(float dt) {
  for (int i = 0; i < BOMB_MAX; ++i) {
    Bomb *b = &bombs[i];
    if (!b->active)
      continue;
    b->fuse -= dt;
    b->vel.y += GRAVITY * dt;
    Vector2 move = {b->vel.x * dt, b->vel.y * dt};
    // Sub-step along the projectile path to avoid tunneling through tiles.
    int steps = (int)ceilf(fmaxf(fabsf(move.x), fabsf(move.y)) / MAX_STEP);
    if (steps < 1)
      steps = 1;
    bool hit = false;
    for (int k = 0; k < steps; ++k) {
      b->pos.x += move.x / steps;
      b->pos.y += move.y / steps;
      if (FirstSolidHit((Rectangle){b->pos.x - 7, b->pos.y - 7, 14, 14}) >= 0) {
        hit = true;
        break;
      }
    }
    b->angle += 360.0f * dt;
    if (hit || b->fuse <= 0) {
      Vector2 pos = b->pos;
      b->active = false;
      ExplodeAt(pos);
    }
  }
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
  float lastPressedJump;
  float wallJumpStart;
  float burstFxTimer;
  float burstWindow; // > 0: burst speed is still protected
  bool burstDecay;   // true: burst speed is no longer protected (until you land)
  float hitstop;
  float convertTimer;
  int convertDir;

  // throwable bomb aim
  Vector2 aim; // unit vector from the player toward the cursor
  bool throwQueued;
  bool burstHeld, burstReleaseQueued, diveQueued, burstCharging, burstReady;
  float chargeTime, throwAnimTime, diveAnimTime;
  bool throwOnGround, bombDiving;
  bool throwPending;
  float throwRelease; // seconds until the bomb leaves the hand

  // crouch squash waiting for the crouch animation (DelayedCrouchSqueeze)
  bool csPending, csEntering;
  float csDelay;

  // state
  bool facingRight, grounded;
  bool jumping, jumpCut, jumpFalling;
  bool wallJumping, wallGrabbing, sliding;
  // Burst and rebound do not lock velocity; physics resumes immediately.
  bool bombBurstActive;
  bool crouching;
  bool crouchHeld; // crouch key (separate from the down input)
  int jumpsLeft;
  float stamina;

  // juice: squash & stretch tween
  float sx, sy;
  float squashX, squashY, squashTime, squashT;

  // wall-jump turn lock: after a wall jump you face away from the wall and
  // can't turn back toward it until you are actually moving that way
  bool turnLock;
  int turnLockDir;
} Player;

static float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float Signf(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }

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
  p->stamina = MAX_STAMINA;
  p->lastPressedJump = -1.0f;
  p->sx = p->sy = 1.0f;
  Fx_Reset();
  memset(bombs, 0, sizeof(bombs));
}

// ---------------------------------------------------------------------
// Move along ONE axis, resolving against every solid.
// Returns true if something blocked us. Sub-steps keep fast movement
// (dash) from tunnelling through thin platforms.
// ---------------------------------------------------------------------
static bool MoveAxis(Player *p, float amount, bool horizontal) {
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
  p->burstWindow = 0.0f;  // jumped in time: no decay, speed is kept
  if (onGround)           // bunnyhop boost
    p->vel.x += p->in.x * JUMP_SPEED_BOOST;
  Fx_Event(FX_JUMP, p->box, (Vector2){0, 0});
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
    p->vel.y = (p->vel.y > 0) ? -WALL_JUMP_FORCE_Y : p->vel.y - WALL_JUMP_FORCE_Y;
    if (DO_TURN_ON_WALL_JUMP) {
      p->facingRight = dir > 0;
      p->turnLock = true;
      p->turnLockDir = -dir; // the direction back toward the wall
    }
    Fx_Event(FX_WALL_JUMP, p->box, (Vector2){(float)dir, 0});
  }
}

// Equivalent to Rigidbody2D.AddForce(direction * impulse, ForceMode2D.Impulse)
// for a mass-one body: change velocity ONCE, never overwrite it each tick.
static void StartBombBurst(Player *p) {
  p->bombBurstActive = true;
  p->bombDiving = false; // a charged burst may interrupt a dive
  p->burstWindow = BOMB_BURST_SPEED_PENALTY ? BOMB_BURST_CARRY_WINDOW : 0.0f;
  p->burstDecay = false;
  float side = Signf(p->in.x); // up or up-forward, not an omnidirectional dash
  if (side != 0.0f)
    p->facingRight = side > 0.0f;
  p->jumping = p->jumpFalling = p->jumpCut = p->wallJumping = false;
  p->wallGrabbing = p->sliding = false;
  if (p->vel.x < 0 || p->vel.x > 0)
    p->vel.x = 0.0f;
  if (p->vel.y < 0 || p->vel.y > 0)
    p->vel.y = 0.0f;

  if (side == 0.0f) {
    // Straight upward
    p->vel.y = fminf(p->vel.y, 0.0f) - BOMB_BURST_VERTICAL_IMPULSE;
  } else {
    // Upward-forward
    p->vel.x += side * BOMB_BURST_DIAGONAL_SIDE_IMPULSE;
    p->vel.y = fminf(p->vel.y, 0.0f) - BOMB_BURST_DIAGONAL_UP_IMPULSE;
  }
  p->lastOnGround = 0.0f;
  p->burstFxTimer = BOMB_BURST_FX_TIME;
  Vector2 fxDir = {side, -1};
  Fx_Event(FX_DASH_START, p->box, fxDir);
  ExplodeAt((Vector2){p->box.x + p->box.width * 0.5f, p->box.y + p->box.height});
  Squash(p, JUMP_SQUASH_X, JUMP_SQUASH_Y, JUMP_SQUASH_TIME);
}

static void ReboundFromBombDive(Player *p) {
  // Impact causes the explosion; this impulse launches upward or up-forward.
  Vector2 impact = {p->box.x + p->box.width * 0.5f, p->box.y + p->box.height};
  ExplodeAt(impact);
  float side = Signf(p->in.x);
  if (side != 0.0f)
    p->facingRight = side > 0.0f;
  p->vel.x += side * BOMB_DIVE_REBOUND_SIDE;
  p->vel.y = -BOMB_DIVE_REBOUND_UP;
  p->bombDiving = false;
  p->jumping = p->jumpFalling = p->jumpCut = false;
  p->lastOnGround = 0.0f;
  p->burstFxTimer = BOMB_BURST_FX_TIME;
  Fx_Event(FX_DASH_START, p->box, (Vector2){side, -1});
  Squash(p, JUMP_SQUASH_X, JUMP_SQUASH_Y, JUMP_SQUASH_TIME);
}

static void Run(Player *p, float lerpAmount, float dt) {
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
  if ((p->jumping || p->wallJumping || p->jumpFalling) && fabsf(p->vel.y) < HANG_THRESHOLD) {
    rate *= HANG_ACCEL_MULT;
    target *= HANG_SPEED_MULT;
  }

  // conserve momentum: don't brake in the air if already faster than max in our
  // direction
  if (CONSERVE_MOMENTUM && !p->burstDecay && fabsf(p->vel.x) > fabsf(target) &&
      Signf(p->vel.x) == Signf(target) && hasTarget && p->lastOnGround <= 0)
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

  if (p->throwAnimTime > 0)
    p->throwAnimTime -= dt;
  if (p->bombDiving)
    p->diveAnimTime += dt;
  if (p->burstFxTimer > 0)
    p->burstFxTimer = fmaxf(0.0f, p->burstFxTimer - dt);
  if (p->burstWindow > 0.0f && (p->burstWindow -= dt) <= 0.0f)
    p->burstDecay = true; // window ran out without a jump

  // Throw is not knockback: it creates a physical, timed bomb projectile.
  if (p->throwQueued) {
    p->throwQueued = false;
    if (!p->throwPending) { // ignore presses while a throw is already winding
                            // up
      p->throwOnGround = p->grounded || p->lastOnGround > 0;
      AnimId a = p->throwOnGround ? ANIM_THROW_GROUND : ANIM_THROW_AIR;
      p->throwAnimTime = BOMB_THROW_DURATION;
      p->throwRelease = Art_FrameTime(a, BOMB_THROW_RELEASE_FRAME);
      p->throwPending = true;
      Art_RestartAnim(); // so a throw started during the last one plays from
                         // frame 1
    }
  }
  if (p->throwPending && (p->throwRelease -= dt) <= 0.0f) {
    p->throwPending = false;
    Vector2 dir = p->aim; // aim at the moment of release
    ThrowBomb((Vector2){p->box.x + p->box.width * 0.5f + dir.x * 24,
                        p->box.y + p->box.height * 0.5f + dir.y * 16},
              dir);
  }

  // ---------- facing ----------
  CheckDirectionToFace(p);

  // ---------- collision probes (thin AABBs hugging the player's box)
  // ----------
  Rectangle gp = {p->box.x + 3, p->box.y + p->box.height, p->box.width - 6, PROBE};
  Rectangle lp = {p->box.x - PROBE, p->box.y + 6, PROBE, p->box.height - 12};
  Rectangle rp = {p->box.x + p->box.width, p->box.y + 6, PROBE, p->box.height - 12};
  p->grounded = FirstSolidHit(gp) >= 0;

  bool checks = !p->jumping && !p->bombDiving;
  if (checks) {
    if (p->grounded) {
      if (p->lastOnGround < -0.1f)
        Squash(p, LAND_SQUASH_X, LAND_SQUASH_Y, LAND_SQUASH_TIME); // landing
      p->lastOnGround = COYOTE_TIME;
      p->stamina = MAX_STAMINA;
      p->jumpsLeft = AIR_JUMPS + 1;
      if (!p->bombBurstActive) { // not still rising from the burst
        p->burstDecay = false;
        p->burstWindow = 0.0f;
      }
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

  // ---------- charged bombburst / bombdive ----------
  // Charge anywhere, including midair and after prior bursts.
  // Releasing K only requires a completed charge.
  if (p->burstHeld) {
    p->burstCharging = true;
    p->chargeTime = fminf(BOMB_CHARGE_TIME, p->chargeTime + dt);
    p->burstReady = p->chargeTime >= BOMB_CHARGE_TIME;
  }
  if (p->diveQueued) {
    p->diveQueued = false;
    if (p->burstReady && !p->grounded && !p->bombDiving) {
      p->bombDiving = true;
      p->diveAnimTime = 0;
      p->burstReady = p->burstCharging = false;
      p->chargeTime = 0;
      p->wallGrabbing = p->sliding = false;
      p->jumping = p->wallJumping = false;
      p->jumpCut = p->jumpFalling = false;
      p->vel.x *= 0.5f;      // initial plunge retains some prior momentum
      p->burstWindow = 0.0f; // <-- new
      p->burstDecay = false; // <-- new
      p->vel.y = fmaxf(p->vel.y, 0.0f) + BOMB_DIVE_SPEED;
      shakeTime = SHAKE_DURATION;
    }
  }
  if (p->burstReleaseQueued) {
    p->burstReleaseQueued = false;
    if (p->burstReady)
      StartBombBurst(p);
    p->burstCharging = p->burstReady = false;
    p->chargeTime = 0;
  }
  if (!p->burstHeld && !p->burstCharging && !p->burstReady)
    p->chargeTime = 0;

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
  if (p->lastOnGround <= 0 && !p->jumping && !p->wallJumping && p->jumpsLeft == AIR_JUMPS + 1)
    p->jumpsLeft = AIR_JUMPS;

  // climb-jump -> wall-jump conversion (push away from the wall shortly after
  // climb-jumping)
  if (p->convertTimer > 0.0f) {
    p->convertTimer -= dt;
    if (p->in.x != 0.0f && (int)Signf(p->in.x) == p->convertDir) {
      p->stamina = Clampf(p->stamina + CLIMB_JUMP_COST - WALL_JUMP_COST, 0.0f, MAX_STAMINA);
      float fx = WALL_JUMP_FORCE_X * p->convertDir;
      p->vel.x = (Signf(p->vel.x) != Signf(fx)) ? fx : p->vel.x + fx;
      if (DO_TURN_ON_WALL_JUMP) {
        p->facingRight = p->convertDir > 0;
        p->turnLock = true;
        p->turnLockDir = -p->convertDir;
      }
      Fx_Event(FX_WALL_JUMP, p->box, (Vector2){(float)p->convertDir, 0});
      p->convertTimer = 0.0f;
    }
  }

  // ---------- wall jump / jump ----------
  if (!p->bombDiving && p->lastPressedJump > 0) {
    int dir = (p->lastOnWallR > 0) ? -1 : 1; // direction AWAY from the wall
    bool inputToward = p->in.x != 0.0f && (int)Signf(p->in.x) == -dir;
    bool climbJump = p->wallGrabbing && p->in.y > 0.0f && inputToward;
    float cost = climbJump ? CLIMB_JUMP_COST : WALL_JUMP_COST;

    if (p->lastOnWall > 0 && p->lastOnGround <= 0) // works even with no stamina
      DoWallJump(p, dir, climbJump, cost);
    else if (p->jumpsLeft > 0 && (ALLOW_JUMP_WHILE_RISING || !p->jumping))
      DoJump(p);
  }

  // ---------- wall grab ----------
  bool canGrab = p->lastOnWall > 0 && !p->bombDiving && !p->jumping && !p->wallJumping &&
                 p->lastOnGround <= 0 && p->stamina > 0.0f;
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
    Rectangle stand = {p->box.x, p->box.y + p->box.height - STAND_H, p->box.width, STAND_H};
    canStand = FirstSolidHit(stand) < 0;
  }
  bool wantCrouch =
      p->lastOnGround > 0 && !p->bombDiving && !p->wallGrabbing && (p->crouchHeld || !canStand);
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
  bool canSlide =
      p->lastOnWall > 0 && !p->jumping && !p->wallJumping && !p->bombDiving && p->lastOnGround <= 0;
  p->sliding = canSlide && !p->grabHeld &&
               ((p->lastOnWallL > 0 && p->in.x < 0) || (p->lastOnWallR > 0 && p->in.x > 0));

  // ---------- velocity ----------
  float maxFall = MAX_FALL_SPEED;
  if (p->bombDiving) {
    // One downward impulse was applied when dive began. Gravity is active.
    p->vel.y += GRAVITY * FALL_GRAV_MULT * dt;
  } else if (p->wallGrabbing) {
    p->vel.x = 0.0f;
    p->vel.y = p->in.y > 0 ? -CLIMB_UP_SPEED : (p->in.y < 0 ? CLIMB_DOWN_SPEED : 0.0f);
  } else {
    Run(p, p->wallJumping ? WALL_JUMP_RUN_LERP : 1.0f, dt);

    // pick the gravity multiplier (same priority order as the Unity script)
    float g = 1.0f;
    if (p->bombBurstActive && p->vel.y < 0.0f) {
      g = BOMB_BURST_RISE_GRAVITY;
    } else if (p->bombBurstActive && p->vel.y >= 0.0f) {
      p->bombBurstActive = false;
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
        p->vel.y += (WALL_SLIDE_SPEED - p->vel.y) * fminf(1.0f, WALL_SLIDE_ACCEL * dt);
      else // moving up the wall: bleed the upward speed off
        p->vel.y -= p->vel.y * fminf(1.0f, WALL_SLIDE_UP_DECEL * dt);
      // slideMinUpSpeed: cut a tiny upward drift so you don't float up the wall
      if (p->vel.y < 0.0f && -p->vel.y <= WALL_SLIDE_MIN_UP_SPEED && !p->wallGrabbing)
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
  bool hitX = MoveAxis(p, p->vel.x * dt, true);
  bool hitY = MoveAxis(p, p->vel.y * dt, false);
  if (hitX)
    p->vel.x = 0.0f;
  if (hitY)
    p->vel.y = 0.0f;
  if (p->bombDiving && hitY) {
    ReboundFromBombDive(p);
  }

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
  FxPlayerState fxs = {p->box, p->vel, p->facingRight, p->burstFxTimer > 0.0f};
  Fx_Tick(dt, &fxs);
  TickBombs(dt);

  // fell out of the world
  if (p->box.y > killY)
    Respawn(p);

  p->time += dt;
}

// =====================================================================
//  INPUT
// =====================================================================
#define KEYS(a, b) (IsKeyDown(a) || IsKeyDown(b))
#define PRESSED(a, b) (IsKeyPressed(a) || IsKeyPressed(b))
#define RELEASED(a, b) (IsKeyReleased(a) || IsKeyReleased(b))

static void ReadInput(Player *p) {
  p->in = (Vector2){(float)(KEYS(BIND_RIGHT_1, BIND_RIGHT_2) - KEYS(BIND_LEFT_1, BIND_LEFT_2)),
                    (float)(KEYS(BIND_UP_1, BIND_UP_2) - KEYS(BIND_DOWN_1, BIND_DOWN_2))};
  p->crouchHeld = IsKeyDown(BIND_CROUCH);
  p->jumpHeld = KEYS(BIND_JUMP_1, BIND_JUMP_2);
  p->grabHeld = KEYS(BIND_GRAB_1, BIND_GRAB_2);

  if (PRESSED(BIND_JUMP_1, BIND_JUMP_2)) {
    p->lastPressedJump = JUMP_BUFFER;
  }
  // releasing jump while still rising -> "jump cut" (higher gravity)
  if (RELEASED(BIND_JUMP_1, BIND_JUMP_2) && !p->jumpHeld && (p->jumping || p->wallJumping) &&
      p->vel.y < 0)
    p->jumpCut = true;

  bool held = IsKeyDown(BIND_BURST_1);
  if (!held && p->burstHeld)
    p->burstReleaseQueued = true;
  p->burstHeld = held;
  // Shift while charged and airborne triggers impact dive + explosion rebound.
  if (IsKeyPressed(BIND_CROUCH))
    p->diveQueued = true;

  // bomb aim uses WASD (8 directions); retain last direction when released
  Vector2 a = {(float)(IsKeyDown(BIND_AIM_RIGHT) - IsKeyDown(BIND_AIM_LEFT)),
               (float)(IsKeyDown(BIND_AIM_DOWN) - IsKeyDown(BIND_AIM_UP))};
  if (a.x != 0.0f || a.y != 0.0f) {
    float len = sqrtf(a.x * a.x + a.y * a.y);
    p->aim = (Vector2){a.x / len, a.y / len};
  } else if (p->aim.x == 0.0f && p->aim.y == 0.0f) {
    p->aim = (Vector2){p->facingRight ? 1.0f : -1.0f, 0.0f};
  }
  if (PRESSED(BIND_FIRE, BIND_FIRE_2))
    p->throwQueued = true;
}

// =====================================================================
//  DRAW
// =====================================================================
// Pick which animation to show. Order = priority (first match wins).
static AnimId ChooseAnim(const Player *p) {
  // Each enum identifies a complete animation; art.c advances its PNG frames.
  if (p->bombDiving)
    return ANIM_BOMB_DIVE;
  if (p->throwAnimTime > 0)
    return p->throwOnGround ? ANIM_THROW_GROUND : ANIM_THROW_AIR;
  if (p->wallGrabbing)
    return p->in.y != 0.0f ? ANIM_WALL_CLIMB : ANIM_WALL_GRAB;
  if (p->sliding)
    return ANIM_WALL_SLIDE;
  if (p->crouching)
    return ANIM_CROUCH;
  if (!p->grounded)
    return p->vel.y < 0.0f ? ANIM_JUMP : ANIM_FALL;
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
  a.chargedFlash = p->burstReady && sinf(p->time * 34.0f) > -0.1f;
  a.lowStamina = p->stamina <= 0.0f || (p->stamina < LOW_STAMINA && sinf(p->time * 40.0f) > 0.0f);
  a.stamina01 = p->stamina / MAX_STAMINA;
  return a;
}

static void DrawDebug(const Player *p) {
  for (int i = 0; i < SOLID_COUNT; i++)
    DrawRectangleLinesEx(solids[i], 1, (Color){255, 255, 255, 90});
  DrawRectangleLinesEx(p->box, 1, LIME);
  DrawRectangleRec((Rectangle){p->box.x + 3, p->box.y + p->box.height, p->box.width - 6, PROBE},
                   GREEN);
  DrawRectangleRec((Rectangle){p->box.x - PROBE, p->box.y + 6, PROBE, p->box.height - 12}, SKYBLUE);
  DrawRectangleRec((Rectangle){p->box.x + p->box.width, p->box.y + 6, PROBE, p->box.height - 12},
                   ORANGE);
}

// (Re)load the level from the LDtk file; fall back to the built-in level.
static void LoadLevel(void) {
  Level_Unload(&level);
  bool levelLoaded = Level_Load(&level, LEVEL_FILE, NULL);
  solids = level.solids;
  solidCount = level.solidCount;

  if (levelLoaded) {
    killY = level.height + 512.0f; // a bit below the bottom of the level
    spawnPos = level.hasStart ? (Vector2){level.start.x - PLAYER_W / 2, level.start.y - STAND_H}
                              // feet on PlayerStart
                              : (Vector2){0, 0};
  } else {
    TraceLog(LOG_ERROR, "LEVEL: could not load '%s' - the world is empty", LEVEL_FILE);
    killY = 1600.0f;
    spawnPos = (Vector2){0, 0};
    return;
  }
}

static void DrawCheckerboardBackground(Camera2D camera, int screenW, int screenH) {
  // const float cellSize = 32.0f;
  //
  // Vector2 topLeft = GetScreenToWorld2D((Vector2){0, 0}, camera);
  // Vector2 bottomRight =
  //     GetScreenToWorld2D((Vector2){(float)screenW, (float)screenH}, camera);
  //
  // int startX = (int)floorf(topLeft.x / cellSize) - 1;
  // int endX = (int)ceilf(bottomRight.x / cellSize) + 1;
  // int startY = (int)floorf(topLeft.y / cellSize) - 1;
  // int endY = (int)ceilf(bottomRight.y / cellSize) + 1;
  //
  // // Dark base behind the checkerboard.
  // ClearBackground((Color){0, 0, 0, 255});
  //
  // for (int y = startY; y < endY; y++) {
  //   for (int x = startX; x < endX; x++) {
  //     Color square =
  //         ((x + y) % 2 == 0) ? (Color){0, 0, 0, 255} : (Color){0, 0, 0, 255};
  //
  //     DrawRectangle((int)(x * cellSize), (int)(y * cellSize), (int)cellSize,
  //                   (int)cellSize, square);
  //
  //     // Bright outlines make bending easier to see.
  //     DrawRectangleLines((int)(x * cellSize), (int)(y * cellSize),
  //                        (int)cellSize, (int)cellSize, (Color){0, 0, 0,
  //                        255});
  //   }
  // }
  //
  // // Colored world-origin markers help diagnose coordinate offsets.
  // DrawLine(-160, 0, 160, 0, RED);
  // DrawLine(0, -160, 0, 160, GREEN);
  // DrawCircle(0, 0, 5.0f, YELLOW);
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
  for (int i = 0; i < BOMB_MAX; ++i)
    if (bombs[i].active)
      Art_DrawBomb(bombs[i].pos, bombs[i].angle);
}
static void DrawPixelLayerContents(Camera2D cam, int w, int h, Rectangle view) {
  // --- tiles layer ---
  DrawCheckerboardBackground(cam, w, h);
  Level_Draw(&level, view);

  // --- fx layer ---
  Fx_DrawPixelLayer(); // ghosts, speed rings, particles
}

static void DrawUILayer(const Player *p, int winH) {
  // DrawText("Move/Aim: WASD  |  Fire: U/P  |  Jump: J/Z "
  //          "(hold = higher, again = double jump)",
  //          10, 8, 16, RAYWHITE);
  //
  // DrawText(
  //     "Dash: K/X  |  Wall grab: hold L/Ctrl (W/S climb)  |  Shift: "
  //     "crouch / fast-fall / "
  //     "slide  |  R: respawn  |  F1: debug  |  F2: pixel mode  |  F5: reload",
  //     10, 28, 16, RAYWHITE);
  //
  // DrawText(TextFormat("jumps:%d  stamina:%.0f", p->jumpsLeft, p->stamina),
  // 10,
  //          52, 16, YELLOW);
  //
  // DrawText(TextFormat("speed:%.2f", fabsf(p->vel.x)), 10, 72, 16, YELLOW);
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
  RenderTexture2D sceneTarget = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());

  SetTextureFilter(sceneTarget.texture, TEXTURE_FILTER_POINT);

  // Low-res target for the pixel-perfect layer (sized every frame to the
  // window)
  RenderTexture2D pixelTarget = {0};

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
        fmodf(fabsf(r.width), TILE) > 0.01f || fmodf(fabsf(r.height), TILE) > 0.01f)
      TraceLog(LOG_WARNING, "LEVEL: solid %d is not aligned to the %d px tile grid", i,
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
  Vector2 prev = {player.box.x, player.box.y}; // position after the previous step
  while (!WindowShouldClose()) {
    float frameDt = fminf(GetFrameTime(), 0.05f);

    if (IsKeyPressed(KEY_F11))
      SetBorderless(!borderless);
    if (IsKeyPressed(BIND_RESPAWN))
      Respawn(&player);
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
        player.hitstop -= PHYSICS_DT; // hitstop freeze-frame
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
    cam.target.x = floorf((view.box.x + view.box.width / 2) / ART_PIXEL) * ART_PIXEL;
    cam.target.y = floorf((view.box.y + view.box.height) / ART_PIXEL) * ART_PIXEL - 40;

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
    int artScale =
        (int)floorf(fminf(winW / (float)(REF_W / ART_PIXEL),
                          (winH + (borderless ? 1.0f : 0.0f)) / (float)(REF_H / ART_PIXEL)));
    if (artScale < 1)
      artScale = 1;
    float zoom = (float)artScale / ART_PIXEL; // 1.0, 1.5, 2.0 ...
    cam.zoom = zoom;                          // fullCam is copied from cam
    Art_SetZoom(zoom);                        // see step 3
    cam.offset = (Vector2){floorf(winW / 2.0f), floorf(winH / 2.0f)};

    // Full-resolution camera: whole world pixels (+ explosion shake). Used by
    // the non-pixel layer, the ripple and the debug overlay.
    Camera2D fullCam = cam;
    fullCam.target = (Vector2){roundf(cam.target.x), roundf(cam.target.y)};
    if (shakeTime > 0.0f) { // dash camera shake (whole pixels, fades out)
      shakeTime -= frameDt;
      int amp = (int)(SHAKE_AMPLITUDE * fmaxf(shakeTime, 0.0f) / SHAKE_DURATION + 0.5f);
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
                           snapped.y - (float)(lowH / 2 * ART_PIXEL), (float)(lowW * ART_PIXEL),
                           (float)(lowH * ART_PIXEL)};

    BeginDrawing();

    // Handle window resizing / high-DPI size changes.
    winW = GetScreenWidth();
    winH = GetScreenHeight();

    if (sceneTarget.texture.width != winW || sceneTarget.texture.height != winH) {
      UnloadRenderTexture(sceneTarget);

      sceneTarget = LoadRenderTexture(winW, winH);
      SetTextureFilter(sceneTarget.texture, TEXTURE_FILTER_POINT);
    }
    if (pixelTarget.texture.width != lowW || pixelTarget.texture.height != lowH) {
      if (pixelTarget.id != 0)
        UnloadRenderTexture(pixelTarget);
      pixelTarget = LoadRenderTexture(lowW, lowH);
      SetTextureFilter(pixelTarget.texture, TEXTURE_FILTER_POINT);
    }

    // ------------------------------------------------------------
    // PASS 1 (pixel-first only): PIXEL-PERFECT LAYER into the low-res target
    //         tiles -> fx -> player (+ gun)
    // ------------------------------------------------------------
    BeginTextureMode(pixelTarget);
    ClearBackground(BLACK);
    BeginMode2D(pixelCam);
    DrawPixelLayerContents(pixelCam, lowW, lowH, pixelView);
    EndMode2D();
    EndTextureMode();

    // ------------------------------------------------------------
    // PASS 2: compose the scene at full resolution
    //         pixel layer + NON-PIXEL FX LAYER (dash ribbon)
    // ------------------------------------------------------------
    BeginTextureMode(sceneTarget);
    ClearBackground(BLACK);

    // PIXEL FIRST: blow the low-res target up by an exact integer. Where its
    // top-left corner lands on screen (whole pixels: the camera target and
    // offset are integers):
    Vector2 o = GetWorldToScreen2D((Vector2){pixelView.x, pixelView.y}, fullCam);
    DrawTexturePro(pixelTarget.texture, (Rectangle){0, 0, (float)lowW, -(float)lowH},
                   (Rectangle){o.x, o.y, (float)(lowW * artScale), (float)(lowH * artScale)},
                   (Vector2){0, 0}, 0.0f, WHITE);

    BeginMode2D(fullCam);
    DrawPlayerLayer(&view); // full-res, so squash & stretch is smooth
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
      centerUV[i] = (Vector2){screenPos.x / (float)winW, 1.0f - screenPos.y / (float)winH};

      // Convert the world-space radius into screen pixels.
      screenRadii[i] = radii[i] * fullCam.zoom;
    }

    Vector2 resolution = {(float)winW, (float)winH};

    SetShaderValue(rippleShader, locResolution, &resolution, SHADER_UNIFORM_VEC2);

    for (int i = 0; i < FX_RIPPLE_CAPACITY; i++) {
      SetShaderValue(rippleShader, locCenter[i], &centerUV[i], SHADER_UNIFORM_VEC2);

      SetShaderValue(rippleShader, locRadius[i], &screenRadii[i], SHADER_UNIFORM_FLOAT);

      SetShaderValue(rippleShader, locStrength[i], &strengths[i], SHADER_UNIFORM_FLOAT);
    }

    // draw the composed scene through the refraction shader
    BeginShaderMode(rippleShader);

    DrawTexturePro(sceneTarget.texture, (Rectangle){0, 0, (float)winW, -(float)winH},
                   (Rectangle){0, 0, (float)winW, (float)winH}, (Vector2){0, 0}, 0.0f, WHITE);

    EndShaderMode();

    // ------------------------------------------------------------
    // PASS 4: debug overlay (full-res, exact positions, not distorted)
    // ------------------------------------------------------------
    if (debug) {
      BeginMode2D(fullCam);
      DrawDebug(&player);
      Vector2 pc = {player.box.x + player.box.width / 2, player.box.y + player.box.height / 2};
      DrawCircleV((Vector2){pc.x + player.aim.x * 56, pc.y + player.aim.y * 56}, 3.0f,
                  (Color){255, 220, 120, 200}); // aim marker
      EndMode2D();
    }

    // ------------------------------------------------------------
    // PASS 5: UI LAYER (normal screen space, no refraction)
    // ------------------------------------------------------------
    DrawUILayer(&player, winH);

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
