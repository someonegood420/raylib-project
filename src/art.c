/*
art.c - pixel-art sprites, animations and tilesets.

FOLDER LAYOUT (relative to the "resources" folder):
    resources/player/idle.png        <- horizontal strips, one file per
animation resources/player/run.png resources/tiles/ground.png       <- 3x3
tileset (see below)

Every file is optional. Missing file = coloured placeholder.
*/

#include "art.h"
#include <math.h>

// =====================================================================
//  CONFIG  (edit these)
// =====================================================================
#define SPRITE_SCALE                                                           \
  4 // integer upscale of player sprites (16x18 art -> 64x72 on screen)
#define SPRITE_OFFSET_X                                                        \
  0 // nudge the sprite relative to the hitbox (screen px, +x = forward)
#define SPRITE_OFFSET_Y 0 // +y = down

#define TILE_SCALE                                                             \
  ART_TILE_SCALE // integer upscale of tiles                     (set in art.h)
#define HSTRIP_SWAP_ENDS                                                       \
  false // true if your horizontal strip's left/right end caps came out mirrored

//[NOTE] DEAD CODE START
// ---- gun ----
#define GUN_FILE "player/gun.png" // single frame, drawn pointing RIGHT
#define GUN_PIVOT_X 2             // art px from the sprite's left edge
#define GUN_PIVOT_Y 2 // art px from the sprite's top edge (put it on the grip)
#define GUN_OFFSET_Y -2 // art px: where the pivot sits relative to box centre
#define GUN_PLACEHOLDER_W 8 // art px
#define GUN_PLACEHOLDER_H 3 // art px
//[NOTE] DEAD CODE END

// The pixel-perfect layer needs sprites on the same grid as the tiles.
_Static_assert(SPRITE_SCALE == ART_PIXEL,
               "SPRITE_SCALE must equal ART_TILE_SCALE for pixel-perfect");

#define TINT_SPRITE_WHEN_NO_DASH                                               \
  false // true = sprites turn blue after dashing (placeholder always does)

// ---------------------------------------------------------------------
//  ANIMATION TABLE
//  Each animation is ONE PNG: all frames side by side in a single row,
//  every frame the same size. frame width = image width / frames.
//  Keep the character's FEET on the bottom edge of the frame.
// ---------------------------------------------------------------------
typedef struct {
  const char *file; // path inside resources/
  int frames;       // number of frames in the strip
  float fps;        // playback speed
  bool loop;        // false = hold the last frame
} AnimDef;

static const AnimDef animDefs[ANIM_COUNT] = {
    [ANIM_IDLE] = {"player/idle.png", 1, 6, true},
    [ANIM_RUN] = {"player/run.png", 12, 12, true},
    [ANIM_JUMP] = {"player/jump.png", 6, 8, false},
    [ANIM_FALL] = {"player/fall.png", 1, 8, true},
    [ANIM_DASH] = {"player/dash.png", 4, 12, false},
    [ANIM_WALL_SLIDE] = {"player/wall_slide.png", 1, 8, true},
    [ANIM_WALL_GRAB] = {"player/wall_grab.png", 1, 1, true},
    [ANIM_WALL_CLIMB] = {"player/wall_climb.png", 6, 10, true},
    [ANIM_CROUCH] = {"player/crouch.png", 1, 1, true},
    [ANIM_GLIDE] = {"player/glide.png", 2, 6, true},
};

// =====================================================================
//  STATE
// =====================================================================
static Texture2D animTex[ANIM_COUNT];
static bool animOk[ANIM_COUNT];

//[NOTE] DEAD CODE START
static Texture2D gunTex;
static bool gunOk = false;
//[NOTE] DEAD CODE END
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

void Art_Load(void) {
  TraceLog(LOG_INFO, "ART: working directory = %s", GetWorkingDirectory());
  ghostShader = LoadShader(NULL, "shaders/ghost.fs");
  for (int i = 0; i < ANIM_COUNT; i++) {
    animTex[i] = LoadPixelTexture(animDefs[i].file, &animOk[i]);
    if (animOk[i])
      TraceLog(LOG_INFO, "ART: loaded %s (%dx%d)", animDefs[i].file,
               animTex[i].width, animTex[i].height);
  }
  //[NOTE] DEAD CODE START
  gunTex = LoadPixelTexture(GUN_FILE, &gunOk);
  //[NOTE] DEAD CODE END
}

void Art_Unload(void) {
  if (ghostShader.id != 0)
    UnloadShader(ghostShader);

  for (int i = 0; i < ANIM_COUNT; i++)
    if (animOk[i])
      UnloadTexture(animTex[i]);
  // if (tilesetOk)
  //   UnloadTexture(tileset);

  //[NOTE] DEAD CODE START
  if (gunOk)
    UnloadTexture(gunTex);
  //[NOTE] DEAD CODE END
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
  const AnimDef *d = &animDefs[curAnim];
  int f = (int)(animTime * d->fps);
  if (d->loop)
    return f % d->frames;
  return f >= d->frames ? d->frames - 1 : f;
}

// Draw one frame of an animation bottom-centred on `box`, flipped when facing
// left.
static void DrawSpriteFrame(int anim, int frame, Rectangle box, float sx,
                            float sy, bool facingRight, Color tint) {
  const AnimDef *d = &animDefs[anim];
  Texture2D t = animTex[anim];

  float fw = (float)t.width / d->frames;
  float fh = (float)t.height;
  Rectangle src = {frame * fw, 0.0f, facingRight ? fw : -fw, fh};

  float baseW = fw * SPRITE_SCALE, baseH = fh * SPRITE_SCALE; // unsquashed size
  float dw = baseW * sx, dh = baseH * sy;
  float ox = facingRight ? SPRITE_OFFSET_X : -SPRITE_OFFSET_X;

  // anchor = the UNSQUASHED sprite, snapped to the art grid, so the feet and
  // centre line never leave the grid (identical to before when sx = sy = 1)
  float ax = SnapPx(box.x + box.width / 2.0f - baseW / 2.0f + ox);
  float ay = SnapPx(box.y + box.height - baseH + SPRITE_OFFSET_Y);

  // the squash grows from the bottom-centre in whole SCREEN pixels, not art px
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
static void DrawPlaceholder(const ArtPlayer *a) {
  float w = a->box.width * a->sx;
  float h = a->box.height * a->sy;
  Rectangle r = {a->box.x + a->box.width / 2 - w / 2,
                 a->box.y + a->box.height - h, w, h};

  Color c = (Color){240, 240, 255, 255};
  if (a->noDash)
    c = (Color){110, 170, 255, 255};
  if (a->lowStamina)
    c = RED;

  DrawRectangleRec(r, c);
  float ex = a->facingRight ? r.x + r.width * 0.55f : r.x + r.width * 0.10f;
  DrawRectangleRec((Rectangle){ex, r.y + r.height * 0.18f, r.width * 0.35f,
                               r.height * 0.16f},
                   (Color){30, 30, 40, 255});
  if (a->gliding)
    DrawRectangleRec((Rectangle){r.x - 8, r.y - 10, r.width + 16, 5}, YELLOW);
}

void Art_DrawPlayer(const ArtPlayer *a) {
  if (animOk[curAnim]) {
    Color tint = WHITE;
    if (a->noDash && TINT_SPRITE_WHEN_NO_DASH)
      tint = (Color){150, 190, 255, 255};
    if (a->lowStamina)
      tint = (Color){255, 110, 110, 255};
    DrawSprite(a->box, a->sx, a->sy, a->facingRight, tint);
  } else
    DrawPlaceholder(a);

  if (a->stamina01 < 1.0f) // stamina bar
  {
    // whole art pixels: 10 px wide, 1 px tall
    float bw = 10.0f * ART_PIXEL;
    float bh = (float)ART_PIXEL;
    float bx = SnapPx(a->box.x + a->box.width / 2 - bw / 2);
    float by = SnapPx(a->box.y - 4.0f * ART_PIXEL - (animOk[curAnim] ? 12 : 0));
    DrawRectangleRec((Rectangle){bx, by, bw, bh}, (Color){40, 40, 50, 255});
    DrawRectangleRec(
        (Rectangle){bx, by, SnapPx(bw * a->stamina01 + ART_PIXEL * 0.5f), bh},
        GREEN);
  }
}

ArtFrame Art_CurrentFrame(void) {
  ArtFrame f = {-1, 0};
  if (animOk[curAnim]) {
    f.anim = (int)curAnim;
    f.frame = CurrentFrame();
  }
  return f;
}

// Ghost = a frozen copy of a sprite frame (like GhostTrail.cs copying
// playerSR.sprite).
void Art_DrawGhost(Rectangle box, ArtFrame frame, bool facingRight,
                   Color tint) {
  if (frame.anim >= 0 && frame.anim < ANIM_COUNT && animOk[frame.anim]) {
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
//  GUN
//  Rotated in the LOW-RES pixel target, so the rotation steps are as coarse as
//  the art pixels (the Unity pixel-perfect-camera look).
// =====================================================================
//[NOTE] DEAD CODE START
void Art_DrawGun(Vector2 pivot, Vector2 aim) {
  float angle = atan2f(aim.y, aim.x) * 57.2957795f; // degrees
  bool flipY = aim.x < 0.0f; // aiming left: mirror so it isn't upside down

  // pivot position on the pixel grid
  Vector2 p = {SnapPx(pivot.x), SnapPx(pivot.y + GUN_OFFSET_Y * ART_PIXEL)};

  if (gunOk) {
    float w = (float)gunTex.width, h = (float)gunTex.height;
    float py = flipY ? (h - GUN_PIVOT_Y) : GUN_PIVOT_Y;
    Rectangle src = {0, 0, w, flipY ? -h : h};
    Rectangle dst = {p.x, p.y, w * ART_PIXEL, h * ART_PIXEL};
    DrawTexturePro(gunTex, src, dst,
                   (Vector2){GUN_PIVOT_X * ART_PIXEL, py * ART_PIXEL}, angle,
                   WHITE);
  } else {
    float w = GUN_PLACEHOLDER_W * ART_PIXEL, h = GUN_PLACEHOLDER_H * ART_PIXEL;
    DrawRectanglePro((Rectangle){p.x, p.y, w, h},
                     (Vector2){GUN_PIVOT_X * ART_PIXEL, h / 2.0f}, angle,
                     (Color){200, 200, 215, 255});
  }
}
//[NOTE] DEAD CODE END

// =====================================================================
//  TILES
//
//  ground.png atlas (each cell = one 8x8 tile, 7x7 cells):
//
//    col:   0      1      2      3      4      5      6
//  row 0  [TL ] [top1 ][top2 ][top3 ][top4 ] [TR ] [vTop ]
//  row 1  [L1 ] [ce   ][ce   ][ce   ][ce   ] [R1 ] [vRnd1]
//  row 2  [L2 ] [ce   ][ctr  ][ctr  ][ce   ] [R2 ] [vRnd2]
//  row 3  [L3 ] [ce   ][ctr  ][ctr  ][ce   ] [R3 ] [vRnd3]
//  row 4  [L4 ] [ce   ][ce   ][ce   ][ce   ] [R4 ] [vBot ]
//  row 5  [BL ] [bot1 ][bot2 ][bot3 ][bot4 ] [BR ] [single2]
//  row 6  [hL ] [hRnd1][hRnd2][hRnd3][hR   ] [single1][single3]
//
//  ce = "centre-edge" (the inner ring), ctr = centre fill.
//
//  Platforms are filled like a nested 9-slice: an outer ring (corners + random
//  edge variants), an inner ring of centre-edge tiles, and centre fill.
//  Platforms 1 tile tall use the horizontal strip (row 6); 1 tile wide use the
//  vertical strip (col 6); 1x1 picks one of the three single tiles.
//  Random picks come from a hash of the tile's world position, so they never
//  change from frame to frame.
// =====================================================================
typedef enum {
  ROLE_FIRST,
  ROLE_SECOND,
  ROLE_MID,
  ROLE_SECOND_LAST,
  ROLE_LAST
} Role;

static Role AxisRole(int i, int n) {
  if (i == 0)
    return ROLE_FIRST;
  if (i == n - 1)
    return ROLE_LAST;
  if (n >= 4 && i == 1)
    return ROLE_SECOND;
  if (n >= 4 && i == n - 2)
    return ROLE_SECOND_LAST;
  return ROLE_MID;
}

static unsigned TileHash(int gx, int gy) {
  unsigned h = (unsigned)gx * 73856093u ^ (unsigned)gy * 19349663u;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return h;
}

// inner ring / centre column (or row) for a role
static int InnerIndex(Role r, unsigned h) {
  if (r == ROLE_SECOND)
    return 1;
  if (r == ROLE_SECOND_LAST)
    return 4;
  return 2 + (int)(h & 1u); // centre: tile 2 or 3
}

// Which atlas cell (tx, ty) for tile (cx, cy) of a cols x rows platform?
// (gx, gy) = world tile coordinate, only used for the random choice.
static void PickTile(int cx, int cy, int cols, int rows, int gx, int gy,
                     int *tx, int *ty) {
  unsigned h = TileHash(gx, gy);

  if (cols == 1 && rows == 1) // isolated single tile
  {
    static const int sx[3] = {5, 6, 6};
    static const int sy[3] = {6, 5, 6};
    *tx = sx[h % 3u];
    *ty = sy[h % 3u];
    return;
  }
  if (rows == 1) // horizontal strip: nothing below
  {
    *ty = 6;
    bool first = (cx == 0), last = (cx == cols - 1);
    if (HSTRIP_SWAP_ENDS) {
      bool t = first;
      first = last;
      last = t;
    }
    if (first)
      *tx = 0;
    else if (last)
      *tx = 4;
    else
      *tx = 1 + (int)(h % 3u);
    return;
  }
  if (cols == 1) // vertical strip: nothing beside
  {
    *tx = 6;
    if (cy == 0)
      *ty = 0;
    else if (cy == rows - 1)
      *ty = 4;
    else
      *ty = 1 + (int)(h % 3u);
    return;
  }

  Role rx = AxisRole(cx, cols), ry = AxisRole(cy, rows);
  bool outerX = (rx == ROLE_FIRST || rx == ROLE_LAST);
  bool outerY = (ry == ROLE_FIRST || ry == ROLE_LAST);

  if (outerX && outerY) {
    *tx = (rx == ROLE_FIRST) ? 0 : 5;
    *ty = (ry == ROLE_FIRST) ? 0 : 5;
  } else if (outerY) {
    *ty = (ry == ROLE_FIRST) ? 0 : 5;
    *tx = 1 + (int)(h % 4u);
  } // top/bottom-randN
  else if (outerX) {
    *tx = (rx == ROLE_FIRST) ? 0 : 5;
    *ty = 1 + (int)(h % 4u);
  } // left/right-randN
  else {
    *tx = InnerIndex(rx, h);
    *ty = InnerIndex(ry, h >> 8);
  } // centre-edge / centre
}
