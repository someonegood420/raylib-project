/*
fx.c - see fx.h.

Optional sprites (resources/fx/): speedring.png and ripple.png. If a file is
missing a drawn placeholder is used, so everything works with no art.
*/

#include "art.h"
#include "fx.h"
#include <math.h>
#include <string.h>

// =====================================================================
//  CONFIG  (edit these)
//  1 Unity unit = 1 tile = ART_TILE_PX pixels.
// =====================================================================
#define FX_U(v) ((v) * (float)ART_TILE_PX)

// ---- Ghost trail  (GhostTrail.cs) ----
// ShowGhost() walks through the ghost pool: ghost i appears i * GHOST_INTERVAL
// seconds after the dash starts, copies your position / facing / sprite frame
// at that moment, starts at GHOST_TRAIL_COLOR and fades to GHOST_FADE_COLOR
// over GHOST_FADE_TIME. (A fade colour with alpha 0 makes it disappear.)
#define GHOST_MAX 4 // how many ghosts exist (= children of ghostsParent)
#define GHOST_INTERVAL 0.08f                           // ghostInterval
#define GHOST_FADE_TIME 0.5f                           // fadeTime
#define GHOST_TRAIL_COLOR ((Color){255, 93, 154, 255}) // trailColor
#define GHOST_FADE_COLOR ((Color){255, 93, 154, 0})    // fadeColor

// ---- Dash ribbon (TrailRenderer "dashline") ----
#define TRAIL_TIME 0.25f // seconds a point of the ribbon lives
#define TRAIL_WIDTH 4.0f // px at the head, tapering to 0
#define TRAIL_COLOR ((Color){255, 255, 255, 255})

// ---- Speed rings (ShockwaveRing.cs + PlayerMovement.SpawnSpeedRing) ----
#define SPEEDRING_SCALE 0.4f // texture size multiplier
#define SPEEDRING_THRESHOLD                                                    \
  FX_U(34) // Data.speedRingThreshold (|vx| or |vy| above this)
#define SPEEDRING_COOLDOWN 0.15f // shockwaveCooldown
#define SPEEDRING_EXPAND 3.0f    // expandSpeed
#define SPEEDRING_FADE 2.0f      // fadeSpeed
#define SPEEDRING_RADIUS 0.5f    // starting radius of the placeholder ring (px)
#define SPEEDRING_COLOR ((Color){255, 255, 255, 255})
#define SPEEDRING_SPRITE_ROT 0.0f // extra rotation for your sprite (degrees)

// ---- Ripple (SpawnRipple) ----
#define RIPPLE_LIFE 0.4f
#define RIPPLE_R0 FX_U(0.6f)
#define RIPPLE_R1 FX_U(3.6f)

// ---- Particles ----
#define MAX_PARTICLES 512
#define DASH_PARTICLE_RATE 90.0f // per second while the dash end-phase plays

#define MAX_RIPPLES 4
#define MAX_SPEEDRINGS 16
#define MAX_TRAIL_PTS 160

// =====================================================================
//  STATE
// =====================================================================
typedef struct {
  Vector2 pos, vel;
  float life, maxLife, size0, size1, gravity;
  Color c0, c1;
  bool active;
} Particle;

typedef struct {
  Rectangle box;
  ArtFrame frame;
  bool facingRight;
  float t;
  bool active;
} GhostSlot;
typedef struct {
  Vector2 p;
  float age;
  bool strandStart;
} TrailPt;
typedef struct {
  Vector2 pos;
  float t;
  bool active;
} Ripple;
typedef struct {
  Vector2 pos;
  float angle;
  float t;
  bool active;
} SpeedRing;

static Particle particles[MAX_PARTICLES];
static GhostSlot ghostSlots[GHOST_MAX];
static TrailPt trail[MAX_TRAIL_PTS];
static int trailCount = 0;
static bool trailWasEmitting = false;
static Ripple ripples[MAX_RIPPLES];
static SpeedRing rings[MAX_SPEEDRINGS];

static bool ghostSeq = false; // a ShowGhost() sequence is running
static int ghostIdx = 0;
static float ghostTimer = 0.0f;

static float fxTime = 0.0f;
static float lastRingTime = -999.0f;
static float dashEmitAcc = 0.0f;

static Texture2D ringTex;
static bool ringTexOk = false;

// =====================================================================
//  HELPERS
// =====================================================================
static float RandF(float a, float b) {
  return a + (b - a) * (float)GetRandomValue(0, 10000) / 10000.0f;
}

static Color LerpColor(Color a, Color b, float t) {
  if (t < 0.0f)
    t = 0.0f;
  if (t > 1.0f)
    t = 1.0f;
  return (Color){(unsigned char)(a.r + (b.r - a.r) * t),
                 (unsigned char)(a.g + (b.g - a.g) * t),
                 (unsigned char)(a.b + (b.b - a.b) * t),
                 (unsigned char)(a.a + (b.a - a.a) * t)};
}

static Vector2 Center(Rectangle b) {
  return (Vector2){b.x + b.width / 2.0f, b.y + b.height / 2.0f};
}

static Texture2D LoadFxTexture(const char *path, bool *ok) {
  *ok = false;
  if (!FileExists(path))
    return (Texture2D){0};
  Texture2D t = LoadTexture(path);
  if (t.id == 0)
    return t;
  SetTextureFilter(t, TEXTURE_FILTER_POINT);
  *ok = true;
  return t;
}

static void Emit(Vector2 pos, Vector2 vel, float life, float s0, float s1,
                 Color c0, Color c1, float gravity) {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    if (particles[i].active)
      continue;
    particles[i] =
        (Particle){pos, vel, life, life, s0, s1, gravity, c0, c1, true};
    return;
  }
}

// =====================================================================
//  LIFECYCLE
// =====================================================================
void Fx_Load(void) {
  ringTex = LoadFxTexture("fx/speedring.png", &ringTexOk);
  if (!ringTexOk)
    TraceLog(LOG_INFO,
             "FX: fx/speedring.png not found - drawing a placeholder ring");
}

void Fx_Unload(void) {
  if (ringTexOk)
    UnloadTexture(ringTex);
  ringTexOk = false;
}

void Fx_Reset(void) {
  memset(particles, 0, sizeof particles);
  memset(ghostSlots, 0, sizeof ghostSlots);
  memset(ripples, 0, sizeof ripples);
  memset(rings, 0, sizeof rings);
  trailCount = 0;
  trailWasEmitting = false;
  ghostSeq = false;
  lastRingTime = -999.0f;
  dashEmitAcc = 0.0f;
}

// =====================================================================
//  EVENTS
// =====================================================================
void Fx_Event(FxEvent e, Rectangle box, Vector2 dir, bool facingRight) {
  Vector2 c = Center(box);

  switch (e) {
  case FX_JUMP: // jumpparticle: dust puff at the feet
    for (int i = 0; i < 10; i++) {
      Vector2 pos = {c.x + RandF(-box.width / 2, box.width / 2),
                     box.y + box.height - 2};
      Vector2 vel = {RandF(-FX_U(2.5f), FX_U(2.5f)),
                     RandF(-FX_U(2.0f), -FX_U(0.3f))};
      Emit(pos, vel, RandF(0.3f, 0.5f), 7.0f, 2.0f, (Color){215, 215, 225, 230},
           (Color){215, 215, 225, 0}, FX_U(2.0f));
    }
    break;

  case FX_WALL_JUMP: // wjumpparticle: puff from the wall side, thrown away from
                     // the wall
  {
    float away = dir.x >= 0.0f ? 1.0f : -1.0f;
    float x =
        away > 0 ? box.x : box.x + box.width; // the side touching the wall
    for (int i = 0; i < 10; i++) {
      Vector2 pos = {x, box.y + RandF(box.height * 0.3f, box.height)};
      Vector2 vel = {away * RandF(FX_U(0.5f), FX_U(3.0f)),
                     RandF(-FX_U(1.0f), FX_U(1.0f))};
      Emit(pos, vel, RandF(0.25f, 0.45f), 7.0f, 2.0f,
           (Color){215, 215, 225, 230}, (Color){215, 215, 225, 0}, FX_U(1.5f));
    }
    break;
  }

  case FX_DASH_START: // ghostTrail.ShowGhost() + SpawnRipple()
  {
    ghostSeq = true;
    ghostIdx = 0;
    ghostTimer = 0.0f;

    // Capture the player's center at dash start.
    Vector2 c = Center(box);

    // Spawn a ripple at that fixed world position.
    for (int i = 0; i < MAX_RIPPLES; i++) {
      if (ripples[i].active)
        continue;

      ripples[i] = (Ripple){c, 0.0f, true};
      break;
    }
    break;
  }

  case FX_EXPLOSION: {
    // Explosions create a ripple and radial particles, not player ghost copies.
    for (int i = 0; i < MAX_RIPPLES; ++i) {
      if (!ripples[i].active) {
        ripples[i] = (Ripple){c, 0.0f, true};
        break;
      }
    }
    for (int i = 0; i < 28; ++i) {
      float angle = (6.2831853f * i) / 28.0f;
      float speed = RandF(FX_U(5.0f), FX_U(13.0f));
      Vector2 vel = {cosf(angle) * speed, sinf(angle) * speed};
      Emit(c, vel, RandF(0.18f, 0.4f), 8.0f, 1.0f, (Color){255, 248, 205, 255},
           (Color){255, 120, 40, 0}, FX_U(1.0f));
    }
    break;
  }
  }
}

// =====================================================================
//  TICK  (physics time)
// =====================================================================
void Fx_Tick(float dt, const FxPlayerState *p) {
  fxTime += dt;
  Vector2 c = Center(p->box);

  // ---- ghost trail: spawn ghost #ghostIdx every GHOST_INTERVAL ----
  if (ghostSeq) {
    ghostTimer -= dt;
    while (ghostSeq && ghostTimer <= 0.0f) {
      GhostSlot *g = &ghostSlots[ghostIdx];
      g->box = p->box;                 // copy position
      g->facingRight = p->facingRight; // copy flip
      g->frame = Art_CurrentFrame();   // copy sprite
      g->t = 0.0f;                     // colour starts at trailColor
      g->active = true;
      ghostIdx++;
      ghostTimer += GHOST_INTERVAL;
      if (ghostIdx >= GHOST_MAX)
        ghostSeq = false;
    }
  }
  for (int i = 0; i < GHOST_MAX; i++)
    if (ghostSlots[i].active && ghostSlots[i].t < GHOST_FADE_TIME)
      ghostSlots[i].t += dt;

  // ---- dash ribbon ----
  for (int i = 0; i < trailCount; i++)
    trail[i].age += dt;
  int drop = 0;
  while (drop < trailCount && trail[drop].age > TRAIL_TIME)
    drop++;
  if (drop > 0) {
    memmove(trail, trail + drop, (size_t)(trailCount - drop) * sizeof(TrailPt));
    trailCount -= drop;
  }
  if (p->dashAttacking) {
    bool far = trailCount == 0;
    if (!far) {
      float dx = c.x - trail[trailCount - 1].p.x,
            dy = c.y - trail[trailCount - 1].p.y;
      far = dx * dx + dy * dy > 4.0f;
    }
    if (far && trailCount < MAX_TRAIL_PTS)
      trail[trailCount++] = (TrailPt){c, 0.0f, !trailWasEmitting};
  }
  trailWasEmitting = p->dashAttacking;

  // ---- speed rings ----
  float ax = fabsf(p->vel.x), ay = fabsf(p->vel.y);
  if ((ax > SPEEDRING_THRESHOLD || ay > SPEEDRING_THRESHOLD) &&
      fxTime > lastRingTime + SPEEDRING_COOLDOWN) {
    for (int i = 0; i < MAX_SPEEDRINGS; i++) {
      if (rings[i].active)
        continue;
      rings[i] = (SpeedRing){c, atan2f(p->vel.y, p->vel.x), 0.0f,
                             true}; // rotated toward movement
      lastRingTime = fxTime;
      break;
    }
  }
  for (int i = 0; i < MAX_SPEEDRINGS; i++) {
    if (!rings[i].active)
      continue;
    rings[i].t += dt;
    if (1.0f - SPEEDRING_FADE * rings[i].t <= 0.0f)
      rings[i].active = false; // alpha hit 0 -> Destroy
  }

  // ---- ripples ----
  for (int i = 0; i < MAX_RIPPLES; i++) {
    if (!ripples[i].active)
      continue;
    ripples[i].t += dt;
    if (ripples[i].t >= RIPPLE_LIFE)
      ripples[i].active = false;
  }

  // ---- particles ----
  for (int i = 0; i < MAX_PARTICLES; i++) {
    Particle *q = &particles[i];
    if (!q->active)
      continue;
    q->life -= dt;
    if (q->life <= 0.0f) {
      q->active = false;
      continue;
    }
    q->vel.y += q->gravity * dt;
    q->pos.x += q->vel.x * dt;
    q->pos.y += q->vel.y * dt;
  }
}

// =====================================================================
//  DRAW
// =====================================================================
static void DrawEllipseRotated(Vector2 c, float a, float b, float angle,
                               float thick, Color col) {
  const int N = 28;
  float ca = cosf(angle), sa = sinf(angle);
  Vector2 prev = {0, 0};
  for (int i = 0; i <= N; i++) {
    float th = (float)i / N * 6.2831853f;
    float lx = a * cosf(th),
          ly = b * sinf(th); // a = along movement, b = across
    Vector2 pt = {c.x + lx * ca - ly * sa, c.y + lx * sa + ly * ca};
    if (i > 0)
      DrawLineEx(prev, pt, thick, col);
    prev = pt;
  }
}

int Fx_GetRippleData(Vector2 centers[FX_RIPPLE_CAPACITY],
                     float radii[FX_RIPPLE_CAPACITY],
                     float strengths[FX_RIPPLE_CAPACITY]) {
  int count = 0;

  for (int i = 0; i < MAX_RIPPLES; i++) {
    if (!ripples[i].active)
      continue;

    float k = ripples[i].t / RIPPLE_LIFE;
    k = fmaxf(0.0f, fminf(k, 1.0f));

    // Same ease-out expansion as the existing visible ripple.
    float e = 1.0f - (1.0f - k) * (1.0f - k);
    float r = RIPPLE_R0 + (RIPPLE_R1 - RIPPLE_R0) * e;

    centers[count] = ripples[i].pos;
    radii[count] = r;
    strengths[count] = 1.0f - k;
    count++;
  }

  return count;
}

// Snap a world coordinate down to the art-pixel grid, so things drawn into the
// low-res pixel target always land on whole pixels.
static float Snap(float v) {
  return floorf(v / (float)ART_PIXEL) * (float)ART_PIXEL;
}

// ---- PIXEL-PERFECT LAYER: ghosts, speed rings, particles ----
void Fx_DrawPixelLayer(void) {
  // ghosts: colour lerps trailColor -> fadeColor over fadeTime
  for (int i = 0; i < GHOST_MAX; i++) {
    if (!ghostSlots[i].active)
      continue;
    Color col = LerpColor(GHOST_TRAIL_COLOR, GHOST_FADE_COLOR,
                          ghostSlots[i].t / GHOST_FADE_TIME);
    if (col.a == 0)
      continue;
    Art_DrawGhost(ghostSlots[i].box, ghostSlots[i].frame,
                  ghostSlots[i].facingRight, col);
  }

  // speed rings: expand linearly, fade, keep the angle they spawned with
  for (int i = 0; i < MAX_SPEEDRINGS; i++) {
    if (!rings[i].active)
      continue;
    float s = 1.0f + SPEEDRING_EXPAND * rings[i].t;
    float alpha = 1.0f - SPEEDRING_FADE * rings[i].t;
    if (alpha <= 0.0f)
      continue;
    Color col = SPEEDRING_COLOR;
    col.a = (unsigned char)(col.a * alpha);
    if (ringTexOk) {
      float w = ringTex.width * ART_TILE_SCALE * SPEEDRING_SCALE * s;
      float h = ringTex.height * ART_TILE_SCALE * SPEEDRING_SCALE * s;
      Rectangle src = {0, 0, (float)ringTex.width, (float)ringTex.height};
      Rectangle dst = {rings[i].pos.x, rings[i].pos.y, w, h};
      DrawTexturePro(ringTex, src, dst, (Vector2){w / 2, h / 2},
                     rings[i].angle * 57.2957795f + SPEEDRING_SPRITE_ROT, col);
    } else
      DrawEllipseRotated(rings[i].pos, SPEEDRING_RADIUS * 0.35f * s,
                         SPEEDRING_RADIUS * s, rings[i].angle, (float)ART_PIXEL,
                         col);
  }

  // particles (whole art pixels, on the pixel grid)
  for (int i = 0; i < MAX_PARTICLES; i++) {
    const Particle *q = &particles[i];
    if (!q->active)
      continue;
    float k = 1.0f - q->life / q->maxLife; // 0 -> 1 over its life
    float sz = q->size0 + (q->size1 - q->size0) * k;
    if (sz < 1.0f)
      continue;
    sz = fmaxf((float)ART_PIXEL, Snap(sz + (float)ART_PIXEL * 0.5f));
    Color col = LerpColor(q->c0, q->c1, k);
    DrawRectangleRec(
        (Rectangle){Snap(q->pos.x - sz / 2), Snap(q->pos.y - sz / 2), sz, sz},
        col);
  }
  // dash ribbon (tapers + fades with age)
  for (int i = 1; i < trailCount; i++) {
    if (trail[i].strandStart)
      continue; // don't join two different dashes
    float k = 1.0f - trail[i].age / TRAIL_TIME;
    if (k <= 0.0f)
      continue;
    Color col = TRAIL_COLOR;
    col.a = (unsigned char)(col.a * k);
    DrawLineEx(trail[i - 1].p, trail[i].p, TRAIL_WIDTH * k, col);
  }
}

// ---- NON-PIXEL LAYER: smooth, full-resolution effects ----
// (the ripple lives here conceptually too, but it is a screen-space shader,
//  so main.c applies it to the composed image.)
void Fx_DrawSmoothLayer(void) {}
