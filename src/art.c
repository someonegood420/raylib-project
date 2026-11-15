/*
 * art.c - player animations loaded from individually sliced PNG frames.
 *
 * All animations use the same layout relative to resources/:
 *   player/idle/1.png
 *   player/run/1.png ... player/run/12.png
 *   player/throw_ground/1.png ... player/throw_ground/5.png
 *   player/throw_air/1.png ... player/throw_air/5.png
 *   player/bomb_dive/1.png ... player/bomb_dive/3.png
 *
 * Keep every frame of an animation on an identically-sized transparent canvas.
 */

#include "art.h"
#include <math.h>
#include <stdio.h>

// =====================================================================
//  CONFIG  (edit these)
// =====================================================================
#define SPRITE_SCALE 4    // integer upscale of player sprites (16x18 art -> 64x72 on screen)
#define SPRITE_OFFSET_X 0 // nudge the sprite relative to the hitbox (screen px, +x = forward)
#define SPRITE_OFFSET_Y 0 // +y = down

// The pixel-perfect layer needs sprites on the same grid as the tiles.
_Static_assert(SPRITE_SCALE == ART_PIXEL,
               "SPRITE_SCALE must equal ART_TILE_SCALE for pixel-perfect");

// =====================================================================
//  ANIMATION DEFINITIONS
//  One animation = one folder; frames are 1.png, 2.png, ...
// =====================================================================
#define MAX_ANIM_FRAMES 16

typedef struct {
  const char *folder;
  int frameCount;
  float fps;
  bool loop;
} AnimDef;

static const AnimDef animDefs[ANIM_COUNT] = {
    [ANIM_IDLE] = {"player/idle", 1, 6, true},
    [ANIM_RUN] = {"player/run", 12, 12, true},
    [ANIM_JUMP] = {"player/jump", 6, 8, false},
    [ANIM_FALL] = {"player/fall", 1, 8, true},
    [ANIM_WALL_SLIDE] = {"player/wall_slide", 1, 8, true},
    [ANIM_WALL_GRAB] = {"player/wall_grab", 1, 1, true},
    [ANIM_WALL_CLIMB] = {"player/wall_climb", 6, 10, true},
    [ANIM_CROUCH] = {"player/crouch", 1, 1, true},
    [ANIM_THROW_GROUND] = {"player/throw_ground", 6, 20, false},
    [ANIM_THROW_AIR] = {"player/throw_ground", 6, 50, false},
    [ANIM_BOMB_DIVE] = {"player/bomb_dive", 3, 12, false},
};

static Texture2D animFrames[ANIM_COUNT][MAX_ANIM_FRAMES];
static Texture2D bombTex;

// snap a world coordinate to the art-pixel grid
static float SnapPx(float v) { return floorf(v / (float)ART_PIXEL) * (float)ART_PIXEL; }

// The camera zoom (set by main.c each frame). Squash & stretch is rounded to
// whole SCREEN pixels, which at zoom 1.5 are 2/3 of a world pixel.
static float viewZoom = 1.0f;
void Art_SetZoom(float z) { viewZoom = z > 0.0f ? z : 1.0f; }
static float RoundScreen(float v) { return roundf(v * viewZoom) / viewZoom; }

static AnimId curAnim = ANIM_IDLE;
static float animTime = 0.0f;
// Seconds from the start of an animation until frame `frame` (0-based) shows.
float Art_FrameTime(AnimId id, int frame) { return frame / animDefs[id].fps; }
// Restart the current animation from frame 1 (for a repeated throw).
void Art_RestartAnim(void) { animTime = 0.0f; }
static Shader ghostShader = {0};

Texture2D Art_LoadPointTexture(const char *path) {
  Texture2D t = LoadTexture(path);
  if (t.id != 0)
    SetTextureFilter(t, TEXTURE_FILTER_POINT); // crisp pixels, no blur
  return t;
}

// Load numbered PNGs from the folder specified in animDefs.
// Missing frames remain empty and are drawn as placeholders.
static void LoadAnimation(AnimId id) {
  const AnimDef *def = &animDefs[id];
  for (int i = 0; i < def->frameCount; i++) {
    char path[256];
    snprintf(path, sizeof path, "%s/%d.png", def->folder, i + 1);
    animFrames[id][i] = Art_LoadPointTexture(path);
    if (animFrames[id][i].id == 0)
      TraceLog(LOG_FATAL, "ART: missing frame %s", path); // exits
  }
}

void Art_Load(void) {
  TraceLog(LOG_INFO, "ART: working directory = %s", GetWorkingDirectory());
  ghostShader = LoadShader(NULL, "shaders/ghost.fs");

  // All animation types use the same loader and folder convention.
  for (int id = 0; id < ANIM_COUNT; id++)
    LoadAnimation((AnimId)id);

  bombTex = Art_LoadPointTexture("bomb/bombbluesmall.png");
}

void Art_Unload(void) {
  if (ghostShader.id != 0)
    UnloadShader(ghostShader);
  for (int a = 0; a < ANIM_COUNT; a++)
    for (int f = 0; f < animDefs[a].frameCount; f++)
      UnloadTexture(animFrames[a][f]); // no-op for frames that never loaded
  UnloadTexture(bombTex);
}

// =====================================================================
//  ANIMATION
// =====================================================================
void Art_UpdatePlayerAnim(AnimId id, float dt) {
  if (id != curAnim) {
    curAnim = id;
    animTime = 0.0f;
  } else
    animTime += dt;
}

static int CurrentFrame(void) {
  const AnimDef *a = &animDefs[curAnim];
  if (a->frameCount < 1)
    return 0;
  int frame = (int)(animTime * a->fps);
  if (a->loop)
    return frame % a->frameCount;
  return frame >= a->frameCount ? a->frameCount - 1 : frame;
}

// Draw one pre-sliced PNG frame, using the existing full-resolution geometry.
// Squash/stretch continues to be rounded to physical screen pixels.
static void DrawSpriteFrame(int animId, int frame, Rectangle box, float sx, float sy,
                            bool facingRight, Color tint) {
  Texture2D t = animFrames[animId][frame];
  float fw = (float)t.width, fh = (float)t.height;
  Rectangle src = {0, 0, facingRight ? fw : -fw, fh};

  float baseW = fw * SPRITE_SCALE, baseH = fh * SPRITE_SCALE;
  float dw = baseW * sx, dh = baseH * sy;
  float ox = facingRight ? SPRITE_OFFSET_X : -SPRITE_OFFSET_X;
  float ax = SnapPx(box.x + box.width / 2.0f - baseW / 2.0f + ox);
  float ay = SnapPx(box.y + box.height - baseH + SPRITE_OFFSET_Y);
  float cx = ax + baseW / 2.0f, feet = ay + baseH;
  float x0 = RoundScreen(cx - dw / 2.0f), x1 = RoundScreen(cx + dw / 2.0f);
  float y0 = RoundScreen(feet - dh);
  Rectangle dst = {x0, y0, x1 - x0, feet - y0};
  DrawTexturePro(t, src, dst, (Vector2){0, 0}, 0.0f, tint);
}

static void DrawSprite(Rectangle box, float sx, float sy, bool facingRight, Color tint) {
  DrawSpriteFrame((int)curAnim, CurrentFrame(), box, sx, sy, facingRight, tint);
}

// =====================================================================
//  PLAYER
// =====================================================================

void Art_DrawPlayer(const ArtPlayer *a) {
  Color tint = a->lowStamina ? (Color){255, 110, 110, 255} : WHITE;
  bool flash = a->chargedFlash && ghostShader.id != 0;
  if (flash) {
    tint = WHITE;
    BeginShaderMode(ghostShader);
  }
  DrawSprite(a->box, a->sx, a->sy, a->facingRight, tint);
  if (flash)
    EndShaderMode();

  if (a->stamina01 < 1.0f) {
    float bw = 10.0f * ART_PIXEL, bh = (float)ART_PIXEL;
    float bx = SnapPx(a->box.x + a->box.width / 2 - bw / 2);
    float by = SnapPx(a->box.y - 4.0f * ART_PIXEL - 12); // hasSprite is always true now
    DrawRectangleRec((Rectangle){bx, by, bw, bh}, (Color){40, 40, 50, 255});
    DrawRectangleRec((Rectangle){bx, by, SnapPx(bw * a->stamina01 + ART_PIXEL * 0.5f), bh}, GREEN);
  }
}

ArtFrame Art_CurrentFrame(void) { return (ArtFrame){(int)curAnim, CurrentFrame()}; }

// Ghost = a frozen copy of a sprite frame (like GhostTrail.cs copying
// playerSR.sprite).
void Art_DrawGhost(Rectangle box, ArtFrame frame, bool facingRight, Color tint) {
  bool shade = ghostShader.id != 0;
  if (shade)
    BeginShaderMode(ghostShader);
  DrawSpriteFrame(frame.anim, frame.frame, box, 1.0f, 1.0f, facingRight, tint);
  if (shade)
    EndShaderMode();
}

// =====================================================================
//  THROWN BOMB SPRITE
// =====================================================================
// Bomb is drawn in the full-resolution world pass, like the player, so
// fractional animation/rotation is not quantized by the low-res tile target.
void Art_DrawBomb(Vector2 center, float angle) {
  if (bombTex.id != 0) {
    float w = bombTex.width * ART_PIXEL, h = bombTex.height * ART_PIXEL;
    DrawTexturePro(bombTex, (Rectangle){0, 0, (float)bombTex.width, (float)bombTex.height},
                   (Rectangle){center.x, center.y, w, h}, (Vector2){w * 0.5f, h * 0.5f}, angle,
                   WHITE);
  }
}
