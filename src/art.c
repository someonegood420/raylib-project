/*
 * art.c - player animations loaded from individually sliced PNG frames.
 *
 * All animations use the same layout relative to resources/:
 *   player/idle/1.png
 *   player/run/1.png ... player/run/12.png
 *   player/throw_ground/1.png ... player/throw_ground/5.png
 *   player/throw_air/1.png ... player/throw_air/5.png (optional)
 *   player/bomb_dive/1.png ... player/bomb_dive/3.png
 *
 * Air-throw frames fall back to their corresponding ground-throw frames.
 * Keep every frame of an animation on an identically-sized transparent canvas.
 */

#include "art.h"
#include <math.h>
#include <stdio.h>

// =====================================================================
//  CONFIG  (edit these)
// =====================================================================
#define SPRITE_SCALE                                                           \
  4 // integer upscale of player sprites (16x18 art -> 64x72 on screen)
#define SPRITE_OFFSET_X                                                        \
  0 // nudge the sprite relative to the hitbox (screen px, +x = forward)
#define SPRITE_OFFSET_Y 0 // +y = down

// The pixel-perfect layer needs sprites on the same grid as the tiles.
_Static_assert(SPRITE_SCALE == ART_PIXEL,
               "SPRITE_SCALE must equal ART_TILE_SCALE for pixel-perfect");

#define TINT_SPRITE_WHEN_NO_DASH                                               \
  false // optional blue tint after consuming burst charge

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
    [ANIM_THROW_GROUND] = {"player/throw_ground", 5, 12, false},
    [ANIM_THROW_AIR] = {"player/throw_air", 5, 12, false},
    [ANIM_BOMB_DIVE] = {"player/bomb_dive", 3, 12, false},
};

typedef struct {
  Texture2D frames[MAX_ANIM_FRAMES];
  bool owned[MAX_ANIM_FRAMES]; // false for a borrowed ground-throw frame
} Animation;

static Animation animations[ANIM_COUNT];
static Texture2D bombTex;
static bool bombOk = false;

// snap a world coordinate to the art-pixel grid
static float SnapPx(float v) {
  return floorf(v / (float)ART_PIXEL) * (float)ART_PIXEL;
}

// The camera zoom (set by main.c each frame). Squash & stretch is rounded to
// whole SCREEN pixels, which at zoom 1.5 are 2/3 of a world pixel.
static float viewZoom = 1.0f;
void Art_SetZoom(float z) { viewZoom = z > 0.0f ? z : 1.0f; }
static float RoundScreen(float v) { return roundf(v * viewZoom) / viewZoom; }

static AnimId curAnim = ANIM_IDLE;
static float animTime = 0.0f;

static Shader ghostShader = {0};

static Texture2D LoadPixelTexture(const char *path, bool *ok) {
  *ok = false;
  if (!FileExists(path)) {
    TraceLog(LOG_WARNING, "ART: '%s' not found - using placeholder", path);
    return (Texture2D){0};
  }
  Texture2D t = LoadTexture(path);
  if (t.id == 0)
    return t;
  SetTextureFilter(t, TEXTURE_FILTER_POINT); // crisp pixels, no blur
  *ok = true;
  return t;
}

// Load numbered PNGs from the folder specified in animDefs.
// Missing frames remain empty and are drawn as placeholders.
static void LoadAnimation(AnimId id) {
  const AnimDef *def = &animDefs[id];
  if (def->frameCount < 1 || def->frameCount > MAX_ANIM_FRAMES) {
    TraceLog(LOG_WARNING, "ART: invalid frame count for animation %d", id);
    return;
  }

  for (int i = 0; i < def->frameCount; i++) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%d.png", def->folder, i + 1);
    bool loaded = false;
    animations[id].frames[i] = LoadPixelTexture(path, &loaded);
    animations[id].owned[i] = loaded;
  }
}

static bool HasFrame(AnimId id, int frame) {
  return id >= 0 && id < ANIM_COUNT && frame >= 0 &&
         frame < animDefs[id].frameCount &&
         animations[id].frames[frame].id != 0;
}

void Art_Load(void) {
  TraceLog(LOG_INFO, "ART: working directory = %s", GetWorkingDirectory());
  ghostShader = LoadShader(NULL, "shaders/ghost.fs");

  // All animation types use the same loader and folder convention.
  for (int id = 0; id < ANIM_COUNT; id++)
    LoadAnimation((AnimId)id);

  // Air-throw art is optional: borrow the corresponding ground frame.
  for (int i = 0; i < 5; i++) {
    if (!HasFrame(ANIM_THROW_AIR, i) && HasFrame(ANIM_THROW_GROUND, i)) {
      animations[ANIM_THROW_AIR].frames[i] =
          animations[ANIM_THROW_GROUND].frames[i];
      animations[ANIM_THROW_AIR].owned[i] = false;
    }
  }
  bombTex = LoadPixelTexture("bomb/bombbluesmall.png", &bombOk);
}

void Art_Unload(void) {
  if (ghostShader.id != 0)
    UnloadShader(ghostShader);
  for (int a = 0; a < ANIM_COUNT; a++) {
    for (int f = 0; f < animDefs[a].frameCount; f++) {
      if (animations[a].owned[f])
        UnloadTexture(animations[a].frames[f]);
    }
  }
  if (bombOk)
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
static void DrawSpriteFrame(int animId, int frame, Rectangle box, float sx,
                            float sy, bool facingRight, Color tint) {
  if (animId < 0 || animId >= ANIM_COUNT || !HasFrame((AnimId)animId, frame))
    return;
  Texture2D t = animations[animId].frames[frame];
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

static void DrawSprite(Rectangle box, float sx, float sy, bool facingRight,
                       Color tint) {
  DrawSpriteFrame((int)curAnim, CurrentFrame(), box, sx, sy, facingRight, tint);
}

// =====================================================================
//  PLAYER
// =====================================================================

void Art_DrawPlayer(const ArtPlayer *a) {
  if (HasFrame(curAnim, CurrentFrame())) {
    Color tint = WHITE;
    if (a->noDash && TINT_SPRITE_WHEN_NO_DASH)
      tint = (Color){150, 190, 255, 255};
    if (a->lowStamina)
      tint = (Color){255, 110, 110, 255};
    if (a->chargedFlash && ghostShader.id != 0) {
      BeginShaderMode(ghostShader);
      DrawSprite(a->box, a->sx, a->sy, a->facingRight, WHITE);
      EndShaderMode();
    } else {
      if (a->chargedFlash)
        tint = WHITE; // shader missing: benign fallback
      DrawSprite(a->box, a->sx, a->sy, a->facingRight, tint);
    }
  }

  if (a->stamina01 < 1.0f) // stamina bar
  {
    // whole art pixels: 10 px wide, 1 px tall
    float bw = 10.0f * ART_PIXEL;
    float bh = (float)ART_PIXEL;
    float bx = SnapPx(a->box.x + a->box.width / 2 - bw / 2);
    bool hasSprite = HasFrame(curAnim, CurrentFrame());
    float by = SnapPx(a->box.y - 4.0f * ART_PIXEL - (hasSprite ? 12 : 0));
    DrawRectangleRec((Rectangle){bx, by, bw, bh}, (Color){40, 40, 50, 255});
    DrawRectangleRec(
        (Rectangle){bx, by, SnapPx(bw * a->stamina01 + ART_PIXEL * 0.5f), bh},
        GREEN);
  }
}

ArtFrame Art_CurrentFrame(void) {
  ArtFrame f = {-1, 0};
  if (HasFrame(curAnim, CurrentFrame())) {
    f.anim = (int)curAnim;
    f.frame = CurrentFrame();
  }
  return f;
}

// Ghost = a frozen copy of a sprite frame (like GhostTrail.cs copying
// playerSR.sprite).
void Art_DrawGhost(Rectangle box, ArtFrame frame, bool facingRight,
                   Color tint) {
  if (frame.anim >= 0 && frame.anim < ANIM_COUNT &&
      HasFrame((AnimId)frame.anim, frame.frame)) {
    if (ghostShader.id != 0) {
      BeginShaderMode(ghostShader);

      DrawSpriteFrame(frame.anim, frame.frame, box, 1.0f, 1.0f, facingRight,
                      tint);

      EndShaderMode();
    } else {
      // Fallback if the shader failed to load.
      DrawSpriteFrame(frame.anim, frame.frame, box, 1.0f, 1.0f, facingRight,
                      tint);
    }
  } else {
    DrawRectangleRec(box, tint);
  }
}

// =====================================================================
//  THROWN BOMB SPRITE
// =====================================================================
// Bomb is drawn in the full-resolution world pass, like the player, so
// fractional animation/rotation is not quantized by the low-res tile target.
void Art_DrawBomb(Vector2 center, float angle) {
  if (bombOk) {
    float w = bombTex.width * ART_PIXEL, h = bombTex.height * ART_PIXEL;
    DrawTexturePro(
        bombTex, (Rectangle){0, 0, (float)bombTex.width, (float)bombTex.height},
        (Rectangle){center.x, center.y, w, h}, (Vector2){w * 0.5f, h * 0.5f},
        angle, WHITE);
  } else {
    DrawCircleV(center, 12.0f, (Color){40, 40, 45, 255});
    DrawCircleLines((int)center.x, (int)center.y, 12, WHITE);
  }
}
