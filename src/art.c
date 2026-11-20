#include "art.h"
#include <math.h>
#include <stdio.h>
#define MAX_FRAMES 16
typedef struct {
  const char *folder;
  int count;
  float fps;
  bool loop;
} AnimDef;
static const AnimDef defs[ANIM_COUNT] = {{"player/idle", 1, 6, true},
                                         {"player/run", 12, 12, true},
                                         {"player/jump", 6, 8, false},
                                         {"player/fall", 1, 8, true}};
static Texture2D frames[ANIM_COUNT][MAX_FRAMES];
static AnimId current = ANIM_IDLE;
static float elapsed;
static Shader whiteShader;

// Sprites sit on the art-pixel grid, so the world and player never disagree.
static float SnapToArtPixel(float v) {
  return floorf(v / ART_PIXEL) * ART_PIXEL;
}
// Round a world coordinate to a whole screen pixel at the given camera zoom.
static float RoundToScreen(float v, float zoom) {
  return roundf(v * zoom) / zoom;
}
static Texture2D Art_LoadPointTexture(const char *path) {
  Texture2D t = LoadTexture(path);
  if (t.id)
    SetTextureFilter(t, TEXTURE_FILTER_POINT);
  return t;
}
void Art_Load(void) {
  whiteShader = LoadShader(NULL, "shaders/white.fs");
  for (int a = 0; a < ANIM_COUNT; ++a)
    for (int i = 0; i < defs[a].count; ++i) {
      char path[128];
      snprintf(path, sizeof path, "%s/%d.png", defs[a].folder, i + 1);
      if (FileExists(path))
        frames[a][i] = Art_LoadPointTexture(path);
      else
        TraceLog(LOG_WARNING, "Missing player sprite: %s", path);
    }
}
void Art_Unload(void) {
  if (whiteShader.id)
    UnloadShader(whiteShader);
  for (int a = 0; a < ANIM_COUNT; ++a)
    for (int i = 0; i < defs[a].count; ++i)
      if (frames[a][i].id)
        UnloadTexture(frames[a][i]);
}
void Art_UpdatePlayerAnim(AnimId id, float dt) {
  if (id != current) {
    current = id;
    elapsed = 0;
  } else
    elapsed += dt;
}
void Art_DrawPlayer(const ArtPlayer *p) {
  const AnimDef *d = &defs[current];
  int n = (int)(elapsed * d->fps);
  n = d->loop ? n % d->count : (n < d->count ? n : d->count - 1);

  Texture2D t = frames[current][n];

  bool flash = p->chargedFlash && ((int)(GetTime() * 12.0) % 2 == 0);

  if (!t.id) {
    Color color = p->hot ? RED : flash ? WHITE : SKYBLUE;
    DrawRectangleRec(p->box, color);
    return;
  }

  float w = t.width * ART_PIXEL;
  float h = t.height * ART_PIXEL;
  float sx = p->sx > 0 ? p->sx : 1.0f;
  float sy = p->sy > 0 ? p->sy : 1.0f;
  float zoom = p->zoom > 0 ? p->zoom : 1.0f;

  // Center and feet snap to the art grid; stretched edges snap to screen px.
  float cx = SnapToArtPixel(p->box.x + p->box.width / 2 - w / 2) + w / 2;
  float feet = SnapToArtPixel(p->box.y + p->box.height - h) + h;
  float x0 = RoundToScreen(cx - w * sx / 2, zoom);
  float x1 = RoundToScreen(cx + w * sx / 2, zoom);
  float y0 = RoundToScreen(feet - h * sy, zoom);
  Rectangle dst = {x0, y0, x1 - x0, feet - y0};

  // Negative source width mirrors the sprite.
  Rectangle src = {0, 0, p->facingRight ? (float)t.width : -(float)t.width,
                   (float)t.height};
  Color tint = p->hot ? (Color){255, 130, 110, 255} : WHITE;

  if (flash && whiteShader.id)
    BeginShaderMode(whiteShader);
  DrawTexturePro(t, src, dst, (Vector2){0, 0}, 0, tint);
  if (flash && whiteShader.id)
    EndShaderMode();
}
