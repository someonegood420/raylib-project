/*
 * level.c - ASCII level reader and automatic ground artwork.
 *
 * Read the top-level functions first: Level_Load, Level_Draw,
 * Level_CollectCrystals, Level_Unload. The helpers above them handle
 * text files, grid collisions, and choosing ground PNG variants.
 *
 * No JSON or jsmn in the running game. The separate ldtk2txt program
 * exports maps containing: # solid, . empty, P spawn, * frost crystal.
 */
#include "level.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_GRID 2048
#define MAX_VARIANTS 8
#define ART_SCALE 4

/* These names are editable: put your ground PNGs in resources/tiles/.
 * Numbered variants: groundtop1.png, groundtop2.png, ...
 * Single sprites: groundtowertop.png, groundtowermid.png, ...
 */
typedef enum {
    GROUND_TOP, GROUND_MID, GROUND_BOTTOM,
    GROUND_LEFT, GROUND_RIGHT,
    GROUND_TOP_LEFT, GROUND_TOP_RIGHT,
    GROUND_BOTTOM_LEFT, GROUND_BOTTOM_RIGHT,
    GROUND_TOWER_TOP, GROUND_TOWER_MID, GROUND_TOWER_BOTTOM,
    GROUND_PLATFORM, GROUND_SINGLE,
    GROUND_COUNT
} GroundType;

static const char *groundNames[GROUND_COUNT] = {
    "groundtop", "groundmid", "groundbottom",
    "groundleft", "groundright",
    "groundtopleft", "groundtopright",
    "groundbottomleft", "groundbottomright",
    "groundtowertop", "groundtowermid", "groundtowerbottom",
    "groundplatform", "groundsingle"
};

typedef struct { Texture2D options[MAX_VARIANTS]; int count; } GroundArt;
static GroundArt groundArt[GROUND_COUNT];
static Texture2D crystalArt;
static bool artLoaded;

static void DirectoryOf(const char *path, char *out, size_t size) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    size_t length = slash ? (size_t)(slash - path + 1) : 0;
    if (length >= size) length = size - 1;
    memcpy(out, path, length);
    out[length] = 0;
}

static void MakePath(char *out, size_t size, const char *dir, const char *name) {
    snprintf(out, size, "%s%s", dir, name);
}

static void LoadGroundArt(void) {
    if (artLoaded) return;
    artLoaded = true;
    for (int t = 0; t < GROUND_COUNT; t++) {
        char path[160];
        for (int i = 1; i <= MAX_VARIANTS; i++) {
            snprintf(path, sizeof path, "tiles/%s%d.png", groundNames[t], i);
            if (!FileExists(path)) continue;
            Texture2D tex = LoadTexture(path);
            if (tex.id) {
                SetTextureFilter(tex, TEXTURE_FILTER_POINT);
                groundArt[t].options[groundArt[t].count++] = tex;
            }
        }
        if (groundArt[t].count) continue;
        snprintf(path, sizeof path, "tiles/%s.png", groundNames[t]);
        if (FileExists(path)) {
            Texture2D tex = LoadTexture(path);
            if (tex.id) {
                SetTextureFilter(tex, TEXTURE_FILTER_POINT);
                groundArt[t].options[groundArt[t].count++] = tex;
            }
        }
    }
    if (FileExists("tiles/crystal.png")) {
        crystalArt = LoadTexture("tiles/crystal.png");
        if (crystalArt.id) SetTextureFilter(crystalArt, TEXTURE_FILTER_POINT);
    }
}

static void UnloadGroundArt(void) {
    if (!artLoaded) return;
    for (int t = 0; t < GROUND_COUNT; t++) {
        for (int i = 0; i < groundArt[t].count; i++)
            UnloadTexture(groundArt[t].options[i]);
        groundArt[t].count = 0;
    }
    if (crystalArt.id) UnloadTexture(crystalArt);
    crystalArt = (Texture2D){0};
    artLoaded = false;
}

static bool IsWall(const Level *level, int x, int y) {
    if (x < 0 || x >= level->columns || y < 0 || y >= level->rows) return false;
    return level->cells[y * level->columns + x] == '#';
}

/* Narrow 1-cell-wide columns get their own tower artwork.
 * Other blocks use their exposed edges and corners.
 * Add extra rules here as you add terrain sprites.
 */
static GroundType ChooseGround(const Level *level, int x, int y) {
    bool up = IsWall(level, x, y - 1);
    bool down = IsWall(level, x, y + 1);
    bool left = IsWall(level, x - 1, y);
    bool right = IsWall(level, x + 1, y);

    if (!left && !right) {
        if (!up && down) return GROUND_TOWER_TOP;
        if (up && down) return GROUND_TOWER_MID;
        if (up && !down) return GROUND_TOWER_BOTTOM;
        return GROUND_SINGLE;
    }
    if (!up && !down) return GROUND_PLATFORM;
    if (!up && !left) return GROUND_TOP_LEFT;
    if (!up && !right) return GROUND_TOP_RIGHT;
    if (!down && !left) return GROUND_BOTTOM_LEFT;
    if (!down && !right) return GROUND_BOTTOM_RIGHT;
    if (!up) return GROUND_TOP;
    if (!down) return GROUND_BOTTOM;
    if (!left) return GROUND_LEFT;
    if (!right) return GROUND_RIGHT;
    return GROUND_MID;
}

/* Coordinate-based variation: looks random but never flickers or changes
 * when a level is restarted. No rand()/GetRandomValue() needed. */
static int Variation(int x, int y, int count) {
    unsigned int h = (unsigned int)x * 73856093u;
    h ^= (unsigned int)y * 19349663u;
    h ^= h >> 13;
    return (int)(h % (unsigned int)count);
}

static void DrawSprite(Texture2D tex, Rectangle rectangle) {
    DrawTexturePro(tex, (Rectangle){0, 0, (float)tex.width, (float)tex.height},
                   rectangle, (Vector2){0, 0}, 0, WHITE);
}

static void DrawGround(const Level *level, int x, int y) {
    GroundType type = ChooseGround(level, x, y);
    GroundArt art = groundArt[type];
    if (!art.count) art = groundArt[GROUND_MID];
    Rectangle r = {(float)(x * LEVEL_TILE_SIZE), (float)(y * LEVEL_TILE_SIZE),
                   LEVEL_TILE_SIZE, LEVEL_TILE_SIZE};
    if (art.count) DrawSprite(art.options[Variation(x, y, art.count)], r);
    else DrawRectangleRec(r, (Color){82, 101, 118, 255});
}

static void DrawCrystals(const Level *level, Rectangle view) {
    for (int i = 0; i < level->crystalCount; i++) {
        if (level->crystalTaken[i]) continue;
        Rectangle r = level->crystals[i];
        if (!CheckCollisionRecs(r, view)) continue;
        if (crystalArt.id) DrawSprite(crystalArt, r);
        else {
            DrawRectangleRec((Rectangle){r.x + 8, r.y + 4, 16, 24}, SKYBLUE);
            DrawRectangleLinesEx((Rectangle){r.x + 8, r.y + 4, 16, 24}, 2, WHITE);
        }
    }
}

/* levels.txt contains: LEVEL 01_Intro.txt, LEVEL 02_Cave.txt, ... */
static int ReadIndex(const char *path, char names[][64], char files[][256], int capacity) {
    char dir[512], index[768], line[512];
    DirectoryOf(path, dir, sizeof dir);
    MakePath(index, sizeof index, dir, "levels.txt");
    FILE *f = fopen(index, "rb");
    if (!f) { TraceLog(LOG_WARNING, "LEVEL: cannot open %s", index); return 0; }
    int count = 0;
    while (count < capacity && fgets(line, sizeof line, f)) {
        char filename[256], full[768], name[64] = "";
        if (sscanf(line, "LEVEL %255s", filename) != 1) continue;
        MakePath(full, sizeof full, dir, filename);
        FILE *lf = fopen(full, "rb");
        if (!lf) continue;
        char header[512];
        while (fgets(header, sizeof header, lf)) {
            if (sscanf(header, "NAME %63s", name) == 1) break;
        }
        fclose(lf);
        if (!name[0]) continue;
        snprintf(names[count], 64, "%s", name);
        snprintf(files[count], 256, "%s", filename);
        count++;
    }
    fclose(f);
    return count;
}

int Level_List(const char *path, char names[][64], int capacity) {
    if (capacity <= 0) return 0;
    char (*files)[256] = calloc((size_t)capacity, sizeof *files);
    if (!files) return 0;
    int count = ReadIndex(path, names, files, capacity);
    free(files);
    return count;
}

/* Convert # runs to collision rectangles; one rectangle per horizontal run.
 * This remains compatible with main.c's Solid() collision checks. */
static bool BuildSolids(Level *level) {
    int total = 0;
    for (int y = 0; y < level->rows; y++)
        for (int x = 0; x < level->columns; x++)
            if (IsWall(level, x, y) && !IsWall(level, x - 1, y)) total++;
    level->solids = calloc((size_t)(total ? total : 1), sizeof(Rectangle));
    if (!level->solids) return false;
    for (int y = 0; y < level->rows; y++) {
        int x = 0;
        while (x < level->columns) {
            if (!IsWall(level, x, y)) { x++; continue; }
            int start = x;
            while (x < level->columns && IsWall(level, x, y)) x++;
            level->solids[level->solidCount++] = (Rectangle){
                (float)(start * LEVEL_TILE_SIZE), (float)(y * LEVEL_TILE_SIZE),
                (float)((x - start) * LEVEL_TILE_SIZE), LEVEL_TILE_SIZE};
        }
    }
    return true;
}

/* P is the TOP-left 8x16 LDtk entity, not a 1-cell-tall player.
 * Match the original Respawn(): start is its bottom-center in world pixels.
 */
static bool FindObjects(Level *level) {
    int spawnCount = 0;
    for (int y = 0; y < level->rows; y++) {
        for (int x = 0; x < level->columns; x++) {
            char c = level->cells[y * level->columns + x];
            if (c == 'P') {
                spawnCount++;
                level->start = (Vector2){x * LEVEL_TILE_SIZE + 16,
                                         y * LEVEL_TILE_SIZE + 2 * LEVEL_TILE_SIZE};
                level->hasStart = true;
                if (y + 1 >= level->rows ||
                    level->cells[(y + 1) * level->columns + x] != '.') {
                    TraceLog(LOG_WARNING, "LEVEL: P needs one empty cell below at (%d,%d)", x, y);
                    return false;
                }
            }
            if (c == '*') {
                if (level->crystalCount >= LEVEL_MAX_CRYSTALS) {
                    TraceLog(LOG_WARNING, "LEVEL: more than three frost crystals");
                    return false;
                }
                level->crystals[level->crystalCount++] = (Rectangle){
                    (float)(x * LEVEL_TILE_SIZE), (float)(y * LEVEL_TILE_SIZE),
                    LEVEL_TILE_SIZE, LEVEL_TILE_SIZE};
            }
        }
    }
    if (spawnCount != 1) TraceLog(LOG_WARNING, "LEVEL: expected one P, found %d", spawnCount);
    return spawnCount == 1;
}

static bool ReadMap(FILE *file, Level *level) {
    char line[MAX_GRID + 16];
    for (int y = 0; y < level->rows; y++) {
        if (!fgets(line, sizeof line, file)) return false;
        size_t length = strcspn(line, "\r\n");
        if (length != (size_t)level->columns) {
            TraceLog(LOG_WARNING, "LEVEL: map row %d has %d cells, expected %d", y,
                     (int)length, level->columns);
            return false;
        }
        for (int x = 0; x < level->columns; x++) {
            char c = line[x];
            if (c != '#' && c != '.' && c != 'P' && c != '*') return false;
            level->cells[y * level->columns + x] = c;
        }
    }
    if (!fgets(line, sizeof line, file)) return false;
    return strncmp(line, "END_MAP", 7) == 0;
}

static bool ReadLevelFile(Level *level, const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) { TraceLog(LOG_WARNING, "LEVEL: cannot open %s", filename); return false; }
    char line[512];
    int cellSize = 0, width = 0, height = 0;
    bool foundMap = false;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "CELL_SIZE %d", &cellSize) == 1) continue;
        if (sscanf(line, "SIZE %d %d", &width, &height) == 2) continue;
        if (strncmp(line, "MAP", 3) == 0) { foundMap = true; break; }
    }
    bool ok = foundMap && cellSize == LEVEL_TILE_SIZE &&
              width > 0 && height > 0 && width <= MAX_GRID && height <= MAX_GRID;
    if (ok) {
        level->columns = width;
        level->rows = height;
        level->width = (float)(width * LEVEL_TILE_SIZE);
        level->height = (float)(height * LEVEL_TILE_SIZE);
        level->cells = calloc((size_t)width * height, 1);
        ok = level->cells && ReadMap(file, level) && FindObjects(level) && BuildSolids(level);
    }
    fclose(file);
    if (!ok) TraceLog(LOG_WARNING, "LEVEL: invalid ASCII map or spawn: %s", filename);
    return ok;
}

bool Level_Load(Level *level, const char *path, const char *levelId) {
    memset(level, 0, sizeof *level);
    char names[64][64], files[64][256], directory[512], full[768];
    int count = ReadIndex(path, names, files, 64);
    if (!count) return false;
    int chosen = 0;
    if (levelId && levelId[0]) {
        chosen = -1;
        for (int i = 0; i < count; i++) {
            if (strcmp(names[i], levelId) == 0) { chosen = i; break; }
        }
        if (chosen < 0) { TraceLog(LOG_WARNING, "LEVEL: unknown level %s", levelId); return false; }
    }
    DirectoryOf(path, directory, sizeof directory);
    MakePath(full, sizeof full, directory, files[chosen]);
    if (!ReadLevelFile(level, full)) {
        Level_Unload(level);
        return false;
    }
    LoadGroundArt();
    TraceLog(LOG_INFO, "LEVEL: loaded %s (%dx%d, %d crystals)", names[chosen],
             level->columns, level->rows, level->crystalCount);
    return true;
}

void Level_Unload(Level *level) {
    free(level->cells);
    free(level->solids);
    memset(level, 0, sizeof *level);
    // Keep ground textures cached between level changes to avoid reloading.
    // Call Level_UnloadArt() once when the game closes, if desired.
}

void Level_UnloadArt(void) { UnloadGroundArt(); }

void Level_Draw(const Level *level, Rectangle view) {
    if (!level->cells) return;
    int firstX = (int)(view.x / LEVEL_TILE_SIZE) - 1;
    int firstY = (int)(view.y / LEVEL_TILE_SIZE) - 1;
    int lastX = (int)((view.x + view.width) / LEVEL_TILE_SIZE) + 1;
    int lastY = (int)((view.y + view.height) / LEVEL_TILE_SIZE) + 1;
    if (firstX < 0) firstX = 0;
    if (firstY < 0) firstY = 0;
    if (lastX >= level->columns) lastX = level->columns - 1;
    if (lastY >= level->rows) lastY = level->rows - 1;
    for (int y = firstY; y <= lastY; y++)
        for (int x = firstX; x <= lastX; x++)
            if (IsWall(level, x, y)) DrawGround(level, x, y);
    DrawCrystals(level, view);
}

int Level_CollectCrystals(Level *level, Rectangle player) {
    int collected = 0;
    for (int i = 0; i < level->crystalCount; i++) {
        if (!level->crystalTaken[i] && CheckCollisionRecs(player, level->crystals[i])) {
            level->crystalTaken[i] = true;
            collected++;
        }
    }
    return collected;
}
