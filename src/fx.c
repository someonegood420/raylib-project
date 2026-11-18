#include "fx.h"
#include <math.h>
#define MAX_RINGS 16
typedef struct {
  Vector2 pos;
  float angle, age;
  bool active;
} Ring;
static Ring rings[MAX_RINGS];
static Texture2D ringTexture;
static float cooldown;
static Vector2 burstAt;
static float burstAge = 10;
void Fx_Load(void) {
  if (FileExists("fx/speedring.png")) {
    ringTexture = LoadTexture("fx/speedring.png");
    if (ringTexture.id)
      SetTextureFilter(ringTexture, TEXTURE_FILTER_POINT);
  }
}
void Fx_Unload(void) {
  if (ringTexture.id)
    UnloadTexture(ringTexture);
}
void Fx_Reset(void) {
  for (int i = 0; i < MAX_RINGS; i++)
    rings[i].active = false;
  cooldown = 0;
  burstAge = 10;
}
void Fx_Burst(Vector2 position) {
  burstAt = position;
  burstAge = 0;
}
void Fx_Tick(float dt, Vector2 pos, Vector2 vel) {
  if (cooldown > 0)
    cooldown -= dt;
  burstAge += dt;
  for (int i = 0; i < MAX_RINGS; i++) {
    if (rings[i].active && (rings[i].age += dt) > 0.4f)
      rings[i].active = false;
    if (!rings[i].active && cooldown <= 0 &&
        (fabsf(vel.x) > 950 || fabsf(vel.y) > 950)) {
      rings[i] = (Ring){pos, atan2f(vel.y, vel.x) * RAD2DEG, 0, true};
      cooldown = 0.12f;
    }
  }
}
void Fx_Draw(void) {
  for (int i = 0; i < MAX_RINGS; i++)
    if (rings[i].active) {
      Ring *r = &rings[i];
      float fade = 1 - r->age / 0.4f;
      if (ringTexture.id) {
        float scale = 1.0f + (r->age * 2.0f); // Grows as it ages
        float w = ringTexture.width * 1.5f * scale;
        float h = ringTexture.height * 1.5f * scale;
        DrawTexturePro(ringTexture,
                       (Rectangle){0, 0, (float)ringTexture.width,
                                   (float)ringTexture.height},
                       (Rectangle){r->pos.x, r->pos.y, w, h},
                       (Vector2){w / 2, h / 2}, r->angle,
                       (Color){200, 240, 255, (unsigned char)(255 * fade)});
      } else
        DrawCircleLines((int)r->pos.x, (int)r->pos.y, (float)(18 + 30 * r->age),
                        SKYBLUE);
    }
  // Placeholder for an optional sprite explosion animation behind a BombBurst.
  if (burstAge < 0.2f)
    DrawCircleLines((int)burstAt.x, (int)burstAt.y, 12 + 90 * burstAge, ORANGE);
}
