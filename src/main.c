// Overheat: precision platformer in C + raylib.
// J/Z jump, K hold/release to burst, A/D move, Esc pause, F1 debug, R restart.
#include "art.h"
#include "fx.h"
#include "level.h"
#include "raylib.h"
#include "resource_dir.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define LEVEL_FILE "levels/world.ldtk"
#define PHYSICS_DT (1.0f / 240.0f)
#define PLAYER_W 40.0f
#define PLAYER_H 64.0f
// Movement tuning retained from the original BombRush controller.
#define FALL_GRAV_MULT 1.5f
#define MAX_FALL_SPEED 800.0f
#define FAST_FALL_MULT 5.0f
#define MAX_FAST_FALL 960.0f
#define RUN_MAX_SPEED 640.0f
#define RUN_ACCEL 10.0f
#define RUN_DECEL 10.0f
#define ACCEL_IN_AIR 1.0f
#define DECEL_IN_AIR 0.201f
#define SNAP_MIN 3.2f
#define SNAP_MAX 3.2f
#define CONSERVE_MOMENTUM true
#define JUMP_HEIGHT 112.0f
#define JUMP_TIME_TO_APEX 0.45f
#define GRAVITY (2.0f * JUMP_HEIGHT / (JUMP_TIME_TO_APEX * JUMP_TIME_TO_APEX))
#define JUMP_FORCE (GRAVITY * JUMP_TIME_TO_APEX)
#define JUMP_SPEED_BOOST 16.0f
#define AIR_JUMPS 1
#define JUMP_CUT_GRAV_MULT 4.0f
#define HANG_GRAV_MULT 0.5f
#define HANG_THRESHOLD 6.4f
#define HANG_ACCEL_MULT 0.43f
#define HANG_SPEED_MULT 0.97f
#define COYOTE_TIME 0.10f
#define JUMP_BUFFER 0.15f
#define ALLOW_JUMP_WHILE_RISING true
#define WALL_JUMP_FORCE_X 544.0f
#define WALL_JUMP_FORCE_Y 480.0f
// CLIMB_JUMP_FORCE omitted: wall climbing has been removed.
#define WALL_JUMP_GRACE 0.1f
#define WALL_JUMP_RUN_LERP 0.5f
#define WALL_JUMP_TIME 0.2f
#define DO_TURN_ON_WALL_JUMP true
#define BOMB_BURST_RISE_GRAVITY 1.0f
#define BOMB_BURST_VERTICAL_IMPULSE 864.0f
#define BOMB_BURST_DIAGONAL_SIDE_IMPULSE 960.0f
#define BOMB_BURST_DIAGONAL_UP_IMPULSE 544.0f
#define BOMB_BURST_MAX_SPEED 1440.0f
#define BOMB_BURST_MIN_CARRY_SPEED 800.0f
#define BURST_CHARGE_TIME 0.38f
#define HEAT_DOUBLE 26.0f
#define HEAT_BURST 42.0f
#define COOL_PER_SEC 8.0f
#define COOL_CRYSTAL 36.0f
#define WATER_SPEED 170.0f
#define WATER_GROWTH 3.0f
#define STEP_SIZE 4.0f
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct {
  Rectangle box;
  Vector2 vel;
  bool facingRight, grounded, jumpHeld, jumpCut;
  bool charging, overheated, burstRising, wallJumping, jumpFalling;
  float charge, heat, coyote, jumpBuffer, wallGrace, wallJumpTime;
  int lastWall;
  int airJumps, collected;
} Player;
typedef struct {
  float x, seconds;
} WaterBoss;
typedef enum { MENU_MAIN, MENU_LEVELS, MENU_PLAY, MENU_PAUSE } Menu;
typedef enum {
  SFX_JUMP,
  SFX_WALL,
  SFX_BURST,
  SFX_CRYSTAL,
  SFX_DEATH,
  SFX_SELECT,
  SFX_PAUSE,
  SFX_COUNT
} SoundId;
static Sound sounds[SFX_COUNT];
static const char *soundPaths[SFX_COUNT] = {
    "audio/jump.wav",    "audio/walljump.wav", "audio/burst.wav",
    "audio/crystal.wav", "audio/death.wav",    "audio/select.wav",
    "audio/pause.wav"};
static Level level;
static char selectedLevel[64] = "";
static char levelNames[64][64];
static int levelCount, levelPage;
static Player player;
static WaterBoss water;
static Menu menu = MENU_MAIN;
static bool debug, quitRequested, jumpQueued;
static float accumulator;
static RenderTexture2D pixelTarget, sceneTarget;
static Rectangle previousBox;
// Read once per physics step and shared by the movement functions.
typedef struct {
  int moveDir, burstDir;
  bool jumpHeld, burstHeld, fastFall;
} Input;
static Input input;

// Manual borderless window: undecorated + sized/positioned to the monitor.
// Avoid ToggleBorderlessWindowed() on systems where it flickers or behaves
// like exclusive fullscreen. The 1px height reduction is a Windows workaround.
static bool borderless = true;
static void SetBorderless(bool on) {
  if (borderless == on)
    return;
  borderless = on;
  int m = GetCurrentMonitor();
  Vector2 mp = GetMonitorPosition(m);
  if (on) {
    SetWindowState(FLAG_WINDOW_UNDECORATED);
    SetWindowPosition((int)mp.x, (int)mp.y);
    SetWindowSize(GetMonitorWidth(m), GetMonitorHeight(m) - 1);
  } else {
    ClearWindowState(FLAG_WINDOW_UNDECORATED);
    SetWindowSize(1280, 720);
    SetWindowPosition((int)mp.x + (GetMonitorWidth(m) - 1280) / 2,
                      (int)mp.y + (GetMonitorHeight(m) - 720) / 2);
  }
}

// keeps a value between a range
static float Clamp(float x, float min, float max) {
  return fmaxf(min, fminf(max, x));
}
static bool Overlap(Rectangle a, Rectangle b) {
  return CheckCollisionRecs(a, b);
}
static void SoundPlay(SoundId id) {
  if (sounds[id].frameCount)
    PlaySound(sounds[id]);
}
static void SoundLoadAll(void) {
  InitAudioDevice();
  for (int i = 0; i < SFX_COUNT; i++)
    if (FileExists(soundPaths[i]))
      sounds[i] = LoadSound(soundPaths[i]);
}
static void SoundUnloadAll(void) {
  for (int i = 0; i < SFX_COUNT; i++)
    if (sounds[i].frameCount)
      UnloadSound(sounds[i]);
  CloseAudioDevice();
}
static bool Solid(Rectangle r) {
  for (int i = 0; i < level.solidCount; i++)
    if (Overlap(r, level.solids[i]))
      return true;
  return false;
}
static void Respawn(void) {
  memset(&player, 0, sizeof player);
  player.box = (Rectangle){level.hasStart ? level.start.x - PLAYER_W / 2 : 64,
                           level.hasStart ? level.start.y - PLAYER_H : 64,
                           PLAYER_W, PLAYER_H};
  player.facingRight = true;
  previousBox = player.box;
  player.airJumps = AIR_JUMPS;
  water = (WaterBoss){player.box.x - 360, 0};
  Fx_Reset();
  jumpQueued = false;
}
static void ReloadLevel(void) {
  Level_Unload(&level);
  if (!Level_Load(&level, LEVEL_FILE, selectedLevel[0] ? selectedLevel : NULL))
    TraceLog(LOG_WARNING, "Could not load %s", LEVEL_FILE);
  Respawn();
  accumulator = 0;
}
static void AddHeat(float amount) {
  player.heat = Clamp(player.heat + amount, 0, 100);
  if (player.heat >= 100)
    player.overheated = true;
}
// Move in 4-pixel increments so high-speed bursts do not tunnel through walls.
static int MoveAxis(float distance, bool horizontal) {
  int wallDir = 0;
  while (fabsf(distance) > 0.0001f) {
    float step = Clamp(distance, -STEP_SIZE, STEP_SIZE);
    Rectangle next = player.box;
    if (horizontal)
      next.x += step;
    else
      next.y += step;
    if (Solid(next)) {
      if (horizontal) {
        wallDir = step > 0 ? 1 : -1;
        player.vel.x = 0;
      } else {
        if (step > 0) {
          player.grounded = true;
          player.airJumps = AIR_JUMPS;
        }
        player.vel.y = 0;
      }
      break;
    }
    player.box = next;
    distance -= step;
  }
  return wallDir;
}
static int TouchingWall(void) {
  Rectangle probe = player.box;
  probe.x -= 2;
  if (Solid(probe))
    return -1;
  probe.x += 4;
  if (Solid(probe))
    return 1;
  return 0;
}
static void Jump(bool fromGround) {
  player.vel.y = -JUMP_FORCE;
  if (fromGround) {
    player.vel.x += input.moveDir * JUMP_SPEED_BOOST;
  }
  player.jumpCut = false;
  player.jumpFalling = false;
  player.burstRising = false;
  player.wallJumping = false;
  player.grounded = false;
  player.coyote = 0;
  player.jumpBuffer = 0;
  SoundPlay(SFX_JUMP);
}
static void Burst(void) {
  if (player.overheated)
    return;
  int dir = (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))    ? -1
            : (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) ? 1
                                                         : 0;
  // One-shot impulses. Carry existing horizontal speed only when already
  // moving in the burst direction at or above BOMB_BURST_MIN_CARRY_SPEED.
  player.vel.y =
      dir ? -BOMB_BURST_DIAGONAL_UP_IMPULSE : -BOMB_BURST_VERTICAL_IMPULSE;
  if (dir) {
    if (player.vel.x * dir <= 0.0f ||
        fabsf(player.vel.x) < BOMB_BURST_MIN_CARRY_SPEED)
      player.vel.x = 0.0f;
    player.vel.x = Clamp(player.vel.x + dir * BOMB_BURST_DIAGONAL_SIDE_IMPULSE,
                         -BOMB_BURST_MAX_SPEED, BOMB_BURST_MAX_SPEED);
    player.facingRight = dir > 0;
  }
  player.burstRising = true;
  player.wallJumping = false;
  player.grounded = false;
  player.jumpCut = false;
  player.jumpFalling = false;
  player.coyote = 0;
  AddHeat(HEAT_BURST);
  Fx_Burst((Vector2){player.box.x + PLAYER_W / 2 - (dir ? dir * 28 : 0),
                     player.box.y + PLAYER_H / 2});
  SoundPlay(SFX_BURST);
}
// Input is shared so the movement steps need few parameters.
static void ReadInput(void) {
  bool left = IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT);
  bool right = IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT);
  input.moveDir = (int)right - (int)left;
  input.burstDir = left    ? -1
                   : right ? 1
                           : 0; // Preserve left priority for burst.
  input.jumpHeld = IsKeyDown(KEY_J) || IsKeyDown(KEY_Z);
  input.burstHeld = IsKeyDown(KEY_K);
  input.fastFall = IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN);
  if (input.moveDir && !player.wallJumping)
    player.facingRight = input.moveDir > 0;
}

static void CountDownTimers(float dt) {
  player.heat = Clamp(player.heat - COOL_PER_SEC * dt, 0, 100);
  if (player.overheated && player.heat <= 40)
    player.overheated = false;
  if (player.grounded)
    player.coyote = COYOTE_TIME;
  else
    player.coyote = fmaxf(0, player.coyote - dt);
  int wall = TouchingWall();
  if (wall) {
    player.wallGrace = WALL_JUMP_GRACE;
    // Keep the side for a short time after leaving the wall.
    player.lastWall = wall;
  } else
    player.wallGrace = fmaxf(0, player.wallGrace - dt);
  if (jumpQueued) {
    player.jumpBuffer = JUMP_BUFFER;
    jumpQueued = false;
  } else
    player.jumpBuffer = fmaxf(0, player.jumpBuffer - dt);
}

static void HandleJumpButton(void) {
  if (player.jumpBuffer > 0) {
    if (player.coyote > 0) {
      Jump(true);
    } else if (player.wallGrace > 0 && !player.grounded) {
      int away = TouchingWall() ? -TouchingWall() : -player.lastWall;
      Jump(false);
      player.vel.x = away * WALL_JUMP_FORCE_X;
      player.vel.y = -WALL_JUMP_FORCE_Y;
      player.wallJumping = true;
      player.wallJumpTime = WALL_JUMP_TIME;
      if (DO_TURN_ON_WALL_JUMP)
        player.facingRight = away > 0;
      player.wallGrace = 0;
      SoundPlay(SFX_WALL);
    } else if (player.airJumps > 0 && !player.overheated &&
               (ALLOW_JUMP_WHILE_RISING || player.vel.y >= 0)) {
      player.airJumps--;
      Jump(false);
      AddHeat(HEAT_DOUBLE);
    }
  }
  // Jump cut uses stronger gravity, rather than instantly removing velocity.
  if (!input.jumpHeld && player.vel.y < 0 && !player.jumpCut &&
      !player.burstRising)
    player.jumpCut = true;
}

static void UpdateRunning(float dt) {
  if (player.wallJumpTime > 0)
    player.wallJumpTime = fmaxf(0, player.wallJumpTime - dt);
  if (player.wallJumpTime <= 0)
    player.wallJumping = false;
  if (player.wallJumping && player.wallJumpTime > 0) {
    float target = input.moveDir * RUN_MAX_SPEED;
    target = player.vel.x + (target - player.vel.x) * WALL_JUMP_RUN_LERP;
    player.vel.x +=
        (target - player.vel.x) * Clamp(RUN_ACCEL * ACCEL_IN_AIR * dt, 0, 1);
  } else {
    float target = input.moveDir * RUN_MAX_SPEED;
    float rate = player.grounded ? (input.moveDir ? RUN_ACCEL : RUN_DECEL)
                                 : (input.moveDir ? RUN_ACCEL * ACCEL_IN_AIR
                                                  : RUN_DECEL * DECEL_IN_AIR);
    if (!player.grounded && fabsf(player.vel.y) < HANG_THRESHOLD &&
        !player.burstRising) {
      rate *= HANG_ACCEL_MULT;
      target *= HANG_SPEED_MULT;
    }
    if (CONSERVE_MOMENTUM && !player.grounded && input.moveDir &&
        player.vel.x * input.moveDir > 0 && fabsf(player.vel.x) > fabsf(target))
      rate = 0;
    player.vel.x += (target - player.vel.x) * Clamp(rate * dt, 0, 1);
  }
  if (!input.moveDir && fabsf(player.vel.x) < SNAP_MIN)
    player.vel.x = 0;
  if (input.moveDir && player.vel.x * input.moveDir > 0 &&
      fabsf(fabsf(player.vel.x) - RUN_MAX_SPEED) < SNAP_MAX &&
      (fabsf(player.vel.x) < RUN_MAX_SPEED || player.grounded))
    player.vel.x = input.moveDir * RUN_MAX_SPEED;
}

static void UpdateBurstCharge(float dt) {
  if (input.burstHeld && !player.overheated) {
    player.charging = true;
    player.charge = fminf(BURST_CHARGE_TIME, player.charge + dt);
  }
  if (!input.burstHeld && player.charging) {
    if (player.charge >= BURST_CHARGE_TIME)
      Burst();
    player.charge = 0;
    player.charging = false;
  }
}

static void ApplyGravity(float dt) {
  float gravityMult = 1.0f;
  float maxFall = MAX_FALL_SPEED;
  if (player.burstRising && player.vel.y < 0)
    gravityMult = BOMB_BURST_RISE_GRAVITY;
  else {
    if (player.burstRising && player.vel.y >= 0)
      player.burstRising = false;
    if (input.fastFall && player.vel.y > 0) {
      gravityMult = FAST_FALL_MULT;
      maxFall = MAX_FAST_FALL;
    } else if (player.jumpCut && player.vel.y < 0)
      gravityMult = JUMP_CUT_GRAV_MULT;
    else if (!player.grounded && fabsf(player.vel.y) < HANG_THRESHOLD)
      gravityMult = HANG_GRAV_MULT;
    else if (player.vel.y > 0)
      gravityMult = FALL_GRAV_MULT;
  }
  player.vel.y = fminf(player.vel.y + GRAVITY * gravityMult * dt, maxFall);
  if (player.vel.y > 0)
    player.jumpFalling = true;
}

static void MovePlayer(float dt) {
  player.grounded = false;
  MoveAxis(player.vel.x * dt, true);
  MoveAxis(player.vel.y * dt, false);
  Rectangle feet = player.box;
  feet.y += 2;
  if (player.vel.y >= 0 && Solid(feet)) {
    player.grounded = true;
    player.airJumps = AIR_JUMPS;
    player.jumpCut = false;
    player.jumpFalling = false;
    player.burstRising = false;
    player.wallJumping = false;
  }
}

static void CollectCrystals(void) {
  int got = Level_CollectCrystals(&level, player.box);
  if (got) {
    player.collected += got;
    player.heat = Clamp(player.heat - COOL_CRYSTAL * got, 0, 100);
    SoundPlay(SFX_CRYSTAL);
  }
}

// Return true if the level restarted; don't update effects on the old player.
static bool UpdateWaterBoss(float dt) {
  water.seconds += dt;
  water.x += (WATER_SPEED + WATER_GROWTH * water.seconds) * dt;
  Rectangle boss = {water.x - 80, -10000, 80, 20000};
  if (Overlap(player.box, boss)) {
    SoundPlay(SFX_DEATH);
    ReloadLevel();
    return true;
  }
  if (player.box.y > level.height + 300 && level.height > 0) {
    ReloadLevel();
    return true;
  }
  return false;
}

static void Update(float dt) {
  ReadInput();
  CountDownTimers(dt);
  HandleJumpButton();
  UpdateRunning(dt);
  UpdateBurstCharge(dt);
  ApplyGravity(dt);
  MovePlayer(dt);
  CollectCrystals();
  if (UpdateWaterBoss(dt))
    return;
  Vector2 center = {player.box.x + PLAYER_W / 2, player.box.y + PLAYER_H / 2};
  Fx_Tick(dt, center, player.vel);
}
static AnimId Animation(void) {
  if (!player.grounded)
    return player.vel.y < 0 ? ANIM_JUMP : ANIM_FALL;
  return fabsf(player.vel.x) > 30 ? ANIM_RUN : ANIM_IDLE;
}
static int Button(const char *text, int y, int w) {
  int sw = GetScreenWidth(), x = (sw - w) / 2;
  Rectangle r = {(float)x, (float)y, (float)w, 42};
  bool hover = CheckCollisionPointRec(GetMousePosition(), r);
  DrawRectangleRec(r, hover ? (Color){60, 84, 112, 255}
                            : (Color){35, 44, 60, 255});
  DrawRectangleLinesEx(r, 2, hover ? SKYBLUE : GRAY);
  DrawText(text, x + (w - MeasureText(text, 22)) / 2, y + 9, 22, WHITE);
  if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
    SoundPlay(SFX_SELECT);
    return 1;
  }
  return 0;
}
static void Title(const char *title) {
  int size = 44;
  DrawText(title, (GetScreenWidth() - MeasureText(title, size)) / 2,
           GetScreenHeight() / 6, size, WHITE);
}
static void DrawMainMenu(void) {
  Title("OVERHEAT");
  int y = GetScreenHeight() / 2 - 55;
  if (Button("Play", y, 260)) {
    selectedLevel[0] = 0;
    ReloadLevel();
    menu = MENU_PLAY;
  }
  if (Button("Level Select", y + 58, 260)) {
    levelPage = 0;
    menu = MENU_LEVELS;
  }
  if (Button("Quit", y + 116, 260))
    quitRequested = true;
}
static void DrawLevelMenu(void) {
  Title("SELECT LEVEL");
  int first = levelPage * 6;
  int count = MIN(6, levelCount - first);
  int y = GetScreenHeight() / 3;
  if (count <= 0)
    DrawText("No LDtk levels found", 30, y, 24, WHITE);
  for (int i = 0; i < count; i++)
    if (Button(levelNames[first + i], y + i * 51, 270)) {
      snprintf(selectedLevel, sizeof selectedLevel, "%s",
               levelNames[first + i]);
      ReloadLevel();
      menu = MENU_PLAY;
    }
  int below = y + count * 51 + 10;
  if (levelPage > 0 && Button("Previous", below, 270))
    levelPage--;
  if (first + 6 < levelCount && Button("Next", below + 48, 270))
    levelPage++;
  if (Button("Back", GetScreenHeight() - 70, 270))
    menu = MENU_MAIN;
}
static void DrawPauseMenu(void) {
  DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                (Color){0, 0, 0, 185});
  Title("PAUSED");
  int y = GetScreenHeight() / 2 - 70;
  if (Button("Resume", y, 280))
    menu = MENU_PLAY;
  if (Button("Restart", y + 55, 280)) {
    ReloadLevel();
    menu = MENU_PLAY;
  }
  if (Button("Level Select", y + 110, 280)) {
    levelPage = 0;
    menu = MENU_LEVELS;
  }
  if (Button("Main Menu", y + 165, 280))
    menu = MENU_MAIN;
}
// Original hybrid renderer: pixel-art world + full-resolution player.
// Physics is interpolated only for drawing, never for collision checks.
// World layer, drawn in the low-resolution render texture.
static void DrawLevel(Rectangle view) { Level_Draw(&level, view); }
static void DrawWaterBoss(Rectangle view) {
  DrawRectangleRec(
      (Rectangle){water.x - 80, view.y - 200, 80, view.height + 400},
      (Color){35, 125, 205, 220});
}
static void DrawCrystals(void) {
  for (int i = 0; i < level.crystalCount; i++)
    if (!level.crystalTaken[i])
      DrawRectangleLinesEx(level.crystals[i], 2, (Color){145, 240, 255, 180});
}
// Player is drawn only in the full-resolution camera pass.
static void DrawPlayer(Rectangle drawn) {
  Art_DrawPlayer(&(ArtPlayer){
      .box = drawn,
      .facingRight = player.facingRight,
      .hot = player.overheated,
      .chargedFlash = player.charging && player.charge >= BURST_CHARGE_TIME &&
                      !player.overheated,
      .sx = 1.0f,
      .sy = 1.0f});
}
static void DrawHud(int winW, int winH) {
  DrawRectangle(20, 18, 220, 16, (Color){50, 50, 50, 255});

  DrawText(TextFormat("speed:%.2f", fabsf(player.vel.x)), 10, 72, 16, YELLOW);
  DrawRectangle(20, 18, (int)(220 * player.heat / 100), 16,
                player.overheated ? RED : ORANGE);
  DrawText(TextFormat("HEAT %.0f%%", player.heat), 20, 37, 19, WHITE);
  DrawText(TextFormat("FROST %d/3", player.collected), winW - 155, 20, 21,
           SKYBLUE);
  if (player.collected == 3)
    DrawText("100% CRYSTALS", winW - 195, 47, 18, GREEN);
  if (player.overheated)
    DrawText("OVERHEATED - COOL DOWN", 20, 62, 20, RED);
  else if (player.charging)
    DrawText(player.charge >= BURST_CHARGE_TIME ? "BURST READY" : "CHARGING...",
             winW - 225, winH - 25, 18, YELLOW);
  DrawText("J/Z jump  |  K charge burst  |  ESC pause", 20, winH - 25, 16,
           GRAY);
}

static void DrawWorld(void) {
  int winW = GetScreenWidth(), winH = GetScreenHeight();
  // Round to keep the intended scale when borderless is 1px short (e.g.
  // 1920x1079).
  int artScale = (int)roundf(fminf(winW / 320.0f, winH / 180.0f));
  if (artScale < 1)
    artScale = 1;
  float zoom = (float)artScale / ART_PIXEL;
  float alpha = Clamp(accumulator / PHYSICS_DT, 0, 1);
  Rectangle drawn = player.box;
  if (fabsf(drawn.x - previousBox.x) < 64 &&
      fabsf(drawn.y - previousBox.y) < 64) {
    drawn.x = previousBox.x + (drawn.x - previousBox.x) * alpha;
    drawn.y = previousBox.y + (drawn.y - previousBox.y) * alpha;
  }
  Camera2D fullCam = {0};
  fullCam.target =
      (Vector2){floorf((drawn.x + drawn.width / 2) / ART_PIXEL) * ART_PIXEL,
                floorf((drawn.y + drawn.height) / ART_PIXEL) * ART_PIXEL - 40};
  fullCam.offset = (Vector2){floorf(winW / 2.0f), floorf(winH / 2.0f)};
  fullCam.zoom = zoom;
  Art_SetZoom(zoom);

  int lowW = ((int)ceilf(winW / (float)artScale) + 3) & ~1;
  int lowH = ((int)ceilf(winH / (float)artScale) + 3) & ~1;
  Vector2 snapped = {floorf(fullCam.target.x / ART_PIXEL + 0.5f) * ART_PIXEL,
                     floorf(fullCam.target.y / ART_PIXEL + 0.5f) * ART_PIXEL};
  Camera2D pixelCam = {0};
  pixelCam.target = snapped;
  pixelCam.offset = (Vector2){lowW / 2.0f, lowH / 2.0f};
  pixelCam.zoom = 1.0f / ART_PIXEL;
  Rectangle view = {snapped.x - lowW * ART_PIXEL / 2.0f,
                    snapped.y - lowH * ART_PIXEL / 2.0f, lowW * ART_PIXEL,
                    lowH * ART_PIXEL};
  if (!pixelTarget.id || pixelTarget.texture.width != lowW ||
      pixelTarget.texture.height != lowH) {
    if (pixelTarget.id)
      UnloadRenderTexture(pixelTarget);
    pixelTarget = LoadRenderTexture(lowW, lowH);
    SetTextureFilter(pixelTarget.texture, TEXTURE_FILTER_POINT);
  }
  if (!sceneTarget.id || sceneTarget.texture.width != winW ||
      sceneTarget.texture.height != winH) {
    if (sceneTarget.id)
      UnloadRenderTexture(sceneTarget);
    sceneTarget = LoadRenderTexture(winW, winH);
    SetTextureFilter(sceneTarget.texture, TEXTURE_FILTER_POINT);
  }
  // Pass 1: tiles, boss, and effects on the native pixel grid.
  BeginTextureMode(pixelTarget);
  ClearBackground((Color){18, 24, 35, 255});
  BeginMode2D(pixelCam);
  DrawLevel(view);
  DrawWaterBoss(view);
  DrawCrystals();
  Fx_Draw();
  EndMode2D();
  EndTextureMode();

  // Pass 2: upscale world by whole pixels, then draw the player in full res.
  BeginTextureMode(sceneTarget);
  ClearBackground(BLACK);
  Vector2 topLeft = GetWorldToScreen2D((Vector2){view.x, view.y}, fullCam);
  DrawTexturePro(pixelTarget.texture,
                 (Rectangle){0, 0, (float)lowW, -(float)lowH},
                 (Rectangle){topLeft.x, topLeft.y, (float)(lowW * artScale),
                             (float)(lowH * artScale)},
                 (Vector2){0, 0}, 0, WHITE);
  BeginMode2D(fullCam);
  DrawPlayer(drawn);
  if (debug) {
    for (int i = 0; i < level.solidCount; i++)
      DrawRectangleLinesEx(level.solids[i], 2, GREEN);
    DrawRectangleLinesEx(drawn, 2, RED);
  }
  EndMode2D();
  EndTextureMode();
  DrawTexturePro(
      sceneTarget.texture, (Rectangle){0, 0, (float)winW, -(float)winH},
      (Rectangle){0, 0, (float)winW, (float)winH}, (Vector2){0, 0}, 0, WHITE);

  DrawHud(winW, winH);
}
int main(void) {
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
  InitWindow(1280, 720, "Overheat");
  SetExitKey(KEY_NULL);
  SearchAndSetResourceDir("resources");
  SoundLoadAll();
  Art_Load();
  Fx_Load();
  levelCount = Level_List(LEVEL_FILE, levelNames, 64);
  ReloadLevel();
  while (!WindowShouldClose() && !quitRequested) {
    float dt = fminf(GetFrameTime(), 0.05f);
    if (IsKeyPressed(KEY_F11))
      SetBorderless(!borderless);
    if (IsKeyPressed(KEY_ESCAPE)) {
      if (menu == MENU_PLAY) {
        menu = MENU_PAUSE;
        SoundPlay(SFX_PAUSE);
      } else if (menu == MENU_PAUSE) {
        menu = MENU_PLAY;
        SoundPlay(SFX_PAUSE);
      } else if (menu == MENU_LEVELS)
        menu = MENU_MAIN;
    }
    if (menu == MENU_PLAY) {
      if (IsKeyPressed(KEY_F1))
        debug = !debug;
      if (IsKeyPressed(KEY_R))
        ReloadLevel();
      if (IsKeyPressed(KEY_F5))
        ReloadLevel();
      if (IsKeyPressed(KEY_J) || IsKeyPressed(KEY_Z))
        jumpQueued = true;
      accumulator += dt;
      while (accumulator >= PHYSICS_DT) {
        previousBox = player.box;
        Update(PHYSICS_DT);
        accumulator -= PHYSICS_DT;
      }
      Art_UpdatePlayerAnim(Animation(), dt);
    }
    BeginDrawing();
    ClearBackground(BLACK);
    if (menu == MENU_MAIN)
      DrawMainMenu();
    else if (menu == MENU_LEVELS)
      DrawLevelMenu();
    else {
      DrawWorld();
      if (menu == MENU_PAUSE)
        DrawPauseMenu();
    }
    EndDrawing();
  }
  Level_Unload(&level);
  if (pixelTarget.id)
    UnloadRenderTexture(pixelTarget);
  if (sceneTarget.id)
    UnloadRenderTexture(sceneTarget);
  Fx_Unload();
  Art_Unload();
  SoundUnloadAll();
  CloseWindow();
  return 0;
}
