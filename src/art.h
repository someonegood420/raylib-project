/*
art.h - everything visual: player animations, tileset drawing, placeholders.
Gameplay code (main.c) never touches textures; it only says WHICH animation
to play and WHERE to draw. If a PNG is missing, a coloured placeholder is
drawn instead, so the game always runs.
*/
#ifndef ART_H
#define ART_H

#include "raylib.h"
#include <stdbool.h>

// Tile size, shared with the level code (main.c) so platforms always sit on the
// tile grid.
#define ART_TILE_SIZE 8  // one tile in the PNG (px)
#define ART_TILE_SCALE 4 // integer upscale
#define ART_TILE_PX                                                            \
  (ART_TILE_SIZE * ART_TILE_SCALE) // one tile on screen (32 px)

// The pixel-perfect layer is rendered at 1 art pixel = 1 texel, then upscaled
// by this integer. Everything in that layer must sit on this grid (world px).
#define ART_PIXEL ART_TILE_SCALE

// One entry per animation. Add new ones here AND in the table in art.c.
typedef enum {
  ANIM_IDLE,
  ANIM_RUN,
  ANIM_JUMP, // rising
  ANIM_FALL, // falling
  ANIM_DASH,
  ANIM_WALL_SLIDE,
  ANIM_WALL_GRAB,  // holding the wall, not moving
  ANIM_WALL_CLIMB, // holding the wall, moving up/down
  ANIM_CROUCH,
  ANIM_GLIDE,
  ANIM_COUNT
} AnimId;

// What the renderer needs to know about the player (no gameplay types leak in).
typedef struct {
  Rectangle box; // hitbox (sprite is drawn bottom-centred on it)
  float sx, sy;  // squash & stretch scale (1 = normal)
  bool facingRight;
  bool noDash;     // dash used up
  bool lowStamina; // flash red
  bool gliding;    // (placeholder only) draw a parachute
  float stamina01; // 0..1, bar is hidden at 1
} ArtPlayer;

void Art_Load(void);   // call once after InitWindow + SearchAndSetResourceDir
void Art_Unload(void); // call before CloseWindow

void Art_UpdatePlayerAnim(AnimId id, float dt); // call once per frame
void Art_SetZoom(float zoom); // camera zoom; main.c calls it once per frame
void Art_DrawPlayer(const ArtPlayer *a);
// A snapshot of which sprite frame is showing (so a ghost can freeze it).
typedef struct {
  int anim;
  int frame;
} ArtFrame; // anim = -1 -> no sprite loaded (placeholder)
ArtFrame Art_CurrentFrame(void);
void Art_DrawGhost(Rectangle box, ArtFrame frame, bool facingRight,
                   Color tint); // dash afterimage

// The gun: rotates around `pivot` (world px) to point along `aim` (unit
// vector). Drawn into the pixel-perfect layer, so the rotation is quantised to
// the low-res pixel grid, like Unity's pixel-perfect camera.
// Uses resources/player/gun.png if present, otherwise a placeholder bar.
void Art_DrawGun(Vector2 pivot, Vector2 aim);

#endif
