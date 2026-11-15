/*
level.h - loads a level made in LDtk (https://ldtk.io) into the game.

What it reads from the .ldtk file
  * IntGrid layer named "Collision"  -> merged into AABB solids (any value != 0
is solid)
  * Tile / Auto-layer layers         -> drawn straight from their tileset PNG
  * Entity layer, entity "PlayerStart" -> where the player spawns
  * Every other entity               -> kept in level.entities[] (spikes,
doors... later)

Everything is scaled by ART_TILE_SCALE, so with an 8 px LDtk grid one cell = 32
px in game.
*/
#ifndef LEVEL_H
#define LEVEL_H

#include "raylib.h"
#include <stdbool.h>

// Names used inside the LDtk project (change them here if you name things
// differently)
#define LDTK_COLLISION_LAYER "Collision"
#define LDTK_PLAYER_START "PlayerStart"
// Regular Tiles layer named exactly this: manually painted lethal, breakable tiles.
#define LDTK_DESTRUCTIBLE_LAYER "Destructible"

#define LEVEL_MAX_TILESETS 8
#define LEVEL_MAX_ENTITIES 512

typedef struct {
  int px, py;         // position in LDtk pixels (layer offset already added)
  int sx, sy;         // source position inside the tileset PNG
  int size;           // tile size (the layer's grid size)
  unsigned char flip; // bit0 = flip X, bit1 = flip Y
  unsigned char tex;  // index into Level.tex
  bool destructible, destroyed; // destructivetileset only
} LevelTile;

typedef struct {
  char id[48]; // LDtk entity identifier, e.g. "Spike"
  Rectangle r; // world rectangle (already scaled)
} LevelEntity;

typedef struct {
  // collision
  Rectangle *solids;
  int solidCount;

  // One collision rectangle per placed tile in the Destructible Tiles layer.
  Rectangle *hazards;
  int hazardCount, hazardCap;

  // visuals
  LevelTile *tiles;
  int tileCount, tileCap;
  Texture2D tex[LEVEL_MAX_TILESETS];
  char texPath[LEVEL_MAX_TILESETS][256];
  int texCount;

  // entities
  LevelEntity entities[LEVEL_MAX_ENTITIES];
  int entityCount;
  bool hasStart;
  Vector2 start; // bottom-centre of the PlayerStart entity, world px

  float width, height; // level size, world px
} Level;

// Load one level from an .ldtk file. levelId = the level's name in LDtk, or
// NULL for the first. Returns false (and logs why) on failure; the Level is
// left empty and safe to Unload.
bool Level_Load(Level *lv, const char *ldtkPath, const char *levelId);
// List level identifiers for the selector (up to capacity names).
int Level_List(const char *path, char names[][64], int capacity);
void Level_Unload(Level *lv);

// Draw all tile layers that are inside `view` (call between BeginMode2D /
// EndMode2D).
void Level_Draw(const Level *lv, Rectangle view);
// Checks only active destructible cells (independent from permanent solids).
bool Level_TouchesDestructible(const Level *lv, Rectangle box);
// Remove destructible tile hitboxes and manually painted graphics.
// Permanent Collision cells are never changed.
int Level_Explode(Level *lv, Vector2 center, float radius);

#endif
