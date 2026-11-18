#ifndef ART_H
#define ART_H
#include "raylib.h"
#include <stdbool.h>
#define ART_TILE_SIZE 8
#define ART_TILE_SCALE 4
#define ART_TILE_PX (ART_TILE_SIZE * ART_TILE_SCALE)
#define ART_PIXEL ART_TILE_SCALE
typedef enum { ANIM_IDLE, ANIM_RUN, ANIM_JUMP, ANIM_FALL, ANIM_COUNT } AnimId;
typedef struct {
  Rectangle box;
  bool facingRight;
  bool hot, chargedFlash;
  float sx, sy;
} ArtPlayer;
Texture2D Art_LoadPointTexture(const char *path);
void Art_Load(void);
void Art_Unload(void);
void Art_UpdatePlayerAnim(AnimId id, float dt);
void Art_SetZoom(float zoom);
void Art_DrawPlayer(const ArtPlayer *player);
#endif
