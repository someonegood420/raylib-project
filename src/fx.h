/*
fx.h - visual effects ported from the Unity project:
  * ghost trail   (GhostTrail.cs)      - afterimages when you dash
  * dash trail    (dashline)           - a ribbon behind you during the dash
  * particles     (jump / wall jump / dash end / gun)
  * ripple        (SpawnRipple)        - expanding ring at dash start
  * speed rings   (ShockwaveRing.cs)   - rings when you go faster than a
threshold

Gameplay code reports what happened; this file decides how it looks.
All timers run in PHYSICS time, so everything freezes during hitstop like
Unity's timeScale = 0.
*/
#ifndef FX_H
#define FX_H

#include "raylib.h"
#include <stdbool.h>
#define FX_RIPPLE_CAPACITY 4

// Returns active ripples in world coordinates.
// The radius is in world pixels.
int Fx_GetRippleData(Vector2 centers[FX_RIPPLE_CAPACITY],
                     float radii[FX_RIPPLE_CAPACITY],
                     float strengths[FX_RIPPLE_CAPACITY]);
typedef enum {
  FX_JUMP,       // jump puff at the feet
  FX_WALL_JUMP,  // puff on the wall side; dir.x = direction you jump
  FX_DASH_START, // burst ghost trail + ripple
  FX_EXPLOSION  // bomb impact blast: particles + ripple
} FxEvent;

typedef struct {
  Rectangle box;
  Vector2 vel;
  bool facingRight;
  bool dashAttacking; // dash ribbon emits
  bool dashEnding;    // dash particles emit
} FxPlayerState;

void Fx_Load(void); // after InitWindow + SearchAndSetResourceDir
void Fx_Unload(void);
void Fx_Reset(void); // on respawn

void Fx_Event(FxEvent e, Rectangle box, Vector2 dir, bool facingRight);
void Fx_Tick(float dt, const FxPlayerState *p); // once per PHYSICS step

// Two draw calls, one per render layer (see main.c):
void Fx_DrawPixelLayer(void);  // ghosts, speed rings, particles
                               // -> drawn INTO the low-res pixel-perfect target
void Fx_DrawSmoothLayer(void); // dash ribbon
                               // -> drawn at full resolution, above the pixels
// (ripples are a post-process shader: see Fx_GetRippleData)

#endif
