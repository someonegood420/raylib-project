/*
 * level.h - Overheat's simple ASCII level reader.
 *
 * The LDtk converter exports files containing a MAP of # . P * characters.
 * level.c uses that map for collision, spawn, crystals and auto-tiling.
 * The rest of the game only needs the Level_* functions below.
 */
#ifndef LEVEL_H
#define LEVEL_H

#include "raylib.h"
#include <stdbool.h>

#define LEVEL_MAX_CRYSTALS 3
#define LEVEL_TILE_SIZE 32

typedef struct {
    Rectangle *solids;
    int solidCount;
    Rectangle crystals[LEVEL_MAX_CRYSTALS];
    bool crystalTaken[LEVEL_MAX_CRYSTALS];
    int crystalCount;
    bool hasStart;
    Vector2 start;          // Bottom-center of LDtk's 8x16 PlayerStart
    float width, height;    // World pixels, not ASCII characters

    // Internal map for automatically choosing ground sprites.
    char *cells;
    int columns, rows;
} Level;

/* Pass the existing "levels/world.ldtk" path OR "levels/levels.txt".
 * Both locate the exported ASCII levels in that same directory.
 * levelId is the NAME in the text file, or NULL for the first level. */
int Level_List(const char *path, char names[][64], int capacity);
bool Level_Load(Level *level, const char *path, const char *levelId);
void Level_Unload(Level *level);
void Level_UnloadArt(void); // Call at shutdown after Level_Unload
void Level_Draw(const Level *level, Rectangle view);
int Level_CollectCrystals(Level *level, Rectangle player);
#endif
