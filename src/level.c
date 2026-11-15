/*
level.c - LDtk loader. See level.h for what it reads.
Uses jsmn (jsmn.h, MIT license) to tokenize the .ldtk file.
*/

#include "art.h"
#include "jsmn.h"
#include "level.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SCALE ((float)ART_TILE_SCALE)

// ---------------------------------------------------------------------
//  tiny JSON layer on top of jsmn. A "node" is just a token index (-1 = none).
//  Every helper accepts -1 and then returns a default, so lookups can be
//  chained without checking each step.
// ---------------------------------------------------------------------
typedef struct {
  char *text;
  jsmntok_t *t;
} Json;

static void JsonFree(Json *j) {
  UnloadFileText(j->text);
  free(j->t);
  memset(j, 0, sizeof *j);
}

static bool JsonLoad(Json *j, const char *path) {
  memset(j, 0, sizeof *j);
  j->text = LoadFileText(path);
  if (!j->text)
    return false;
  size_t len = strlen(j->text);
  jsmn_parser p;
  jsmn_init(&p);
  int n = jsmn_parse(&p, j->text, len, NULL, 0); // pass 1: count tokens
  if (n > 0 && (j->t = malloc((size_t)n * sizeof *j->t))) {
    jsmn_init(&p);
    if (jsmn_parse(&p, j->text, len, j->t, (unsigned)n) == n) // pass 2: fill
      return true;
  }
  JsonFree(j);
  return false;
}

// index of the token after node i and all of its children
static int Next(const Json *j, int i) {
  int n = i + 1;
  for (int k = j->t[i].size; k > 0; k--)
    n = Next(j, n);
  return n;
}

static int Count(const Json *j, int i) {
  return (i >= 0 && (j->t[i].type == JSMN_ARRAY || j->t[i].type == JSMN_OBJECT))
             ? j->t[i].size
             : 0;
}

// for each child `it` of array `arr`
#define EACH(j, arr, it)                                                       \
  for (int it = (arr) + 1, it##_n = Count(j, arr); it##_n-- > 0;               \
       it = Next(j, it))

// value node of key k in object o, or -1
static int Get(const Json *j, int o, const char *k) {
  if (o < 0 || j->t[o].type != JSMN_OBJECT)
    return -1;
  size_t kl = strlen(k);
  EACH(j, o, i) {
    const jsmntok_t *t = &j->t[i];
    if ((size_t)(t->end - t->start) == kl && !memcmp(j->text + t->start, k, kl))
      return i + 1;
  }
  return -1;
}

static double NumAt(const Json *j, int i, double def) {
  if (i < 0 || j->t[i].type != JSMN_PRIMITIVE)
    return def;
  char c = j->text[j->t[i].start];
  return (c == '-' || (c >= '0' && c <= '9')) ? strtod(j->text + j->t[i].start, NULL)
                                              : def;
}

static double Num(const Json *j, int o, const char *k, double def) {
  return NumAt(j, Get(j, o, k), def);
}

// idx-th item of a number array (0 if missing)
static double ArrNum(const Json *j, int arr, int idx) {
  if (idx >= Count(j, arr))
    return 0.0;
  int i = arr + 1;
  while (idx-- > 0)
    i = Next(j, i);
  return NumAt(j, i, 0.0);
}

// copy string value of key k into buf ("" if missing / not a string)
static const char *Str(const Json *j, int o, const char *k, char *buf,
                       size_t n) {
  int v = Get(j, o, k);
  buf[0] = '\0';
  if (v >= 0 && j->t[v].type == JSMN_STRING) {
    size_t len = (size_t)(j->t[v].end - j->t[v].start);
    if (len >= n)
      len = n - 1;
    memcpy(buf, j->text + j->t[v].start, len);
    buf[len] = '\0';
  }
  return buf;
}

// is the string value of key k exactly v?
static bool Is(const Json *j, int o, const char *k, const char *v) {
  int s = Get(j, o, k);
  size_t n = strlen(v);
  return s >= 0 && j->t[s].type == JSMN_STRING &&
         (size_t)(j->t[s].end - j->t[s].start) == n &&
         !memcmp(j->text + j->t[s].start, v, n);
}

// ---------------------------------------------------------------------
//  textures (one per tileset PNG, shared between layers)
// ---------------------------------------------------------------------
static int GetTexture(Level *lv, const char *dir, const char *rel) {
  char full[256];
  snprintf(full, sizeof full, "%s%s", dir, rel);

  for (int i = 0; i < lv->texCount; i++)
    if (strcmp(lv->texPath[i], full) == 0)
      return i;

  if (lv->texCount >= LEVEL_MAX_TILESETS) {
    TraceLog(LOG_WARNING, "LEVEL: too many tilesets (max %d)",
             LEVEL_MAX_TILESETS);
    return -1;
  }
  if (!FileExists(full)) {
    TraceLog(
        LOG_WARNING,
        "LEVEL: tileset '%s' not found (path is relative to the .ldtk file)",
        full);
    return -1;
  }
  Texture2D t = LoadTexture(full);
  if (t.id == 0)
    return -1;
  SetTextureFilter(t, TEXTURE_FILTER_POINT);

  int idx = lv->texCount++;
  lv->tex[idx] = t;
  snprintf(lv->texPath[idx], sizeof lv->texPath[idx], "%s", full);
  return idx;
}

// ---------------------------------------------------------------------
//  tiles
// ---------------------------------------------------------------------
static void AddTile(Level *lv, LevelTile t) {
  if (lv->tileCount == lv->tileCap) {
    int cap = lv->tileCap ? lv->tileCap * 2 : 1024;
    LevelTile *n = realloc(lv->tiles, (size_t)cap * sizeof(LevelTile));
    if (!n)
      return;
    lv->tiles = n;
    lv->tileCap = cap;
  }
  lv->tiles[lv->tileCount++] = t;
}

static void AddTiles(Level *lv, const Json *j, int arr, int tex, int size,
                      int offX, int offY, bool destructible) {
  EACH(j, arr, t) {
    int px = Get(j, t, "px"), src = Get(j, t, "src");
    LevelTile lt;
    lt.px = (int)ArrNum(j, px, 0) + offX;
    lt.py = (int)ArrNum(j, px, 1) + offY;
    lt.sx = (int)ArrNum(j, src, 0);
    lt.sy = (int)ArrNum(j, src, 1);
    lt.size = size;
    lt.flip = (unsigned char)((int)Num(j, t, "f", 0) & 3);
    lt.tex = (unsigned char)tex;
    lt.destructible = destructible;
    lt.destroyed = false;
    AddTile(lv, lt);
  }
}

// ---------------------------------------------------------------------
//  collision: IntGrid -> as few rectangles as possible
//  Greedy merge: take the first free solid cell, grow right as far as
//  possible, then grow down while the whole row below is solid + free.
// ---------------------------------------------------------------------
static void BuildSolids(Level *lv, const Json *j, int csv, int cw, int ch,
                        int gs, int offX, int offY) {
  int n = cw * ch;
  int cap = 64;
  lv->solids = malloc((size_t)cap * sizeof(Rectangle));
  lv->solidCount = 0;
  if (!lv->solids || n <= 0)
    return;

  unsigned char *solid = calloc((size_t)n, 1);
  unsigned char *used = calloc((size_t)n, 1);
  if (!solid || !used) {
    free(solid);
    free(used);
    return;
  }

  // any value except "0" is solid (primitives have no children, so the
  // tokens of the array are simply consecutive)
  int i = 0;
  EACH(j, csv, it) {
    if (i >= n)
      break;
    const jsmntok_t *t = &j->t[it];
    solid[i++] = (unsigned char)!(t->end - t->start == 1 &&
                                  j->text[t->start] == '0');
  }

  for (int y = 0; y < ch; y++) {
    for (int x = 0; x < cw; x++) {
      int idx = y * cw + x;
      if (!solid[idx] || used[idx])
        continue;

      int w = 1;
      while (x + w < cw && solid[idx + w] && !used[idx + w])
        w++;

      int h = 1;
      for (;;) {
        if (y + h >= ch)
          break;
        bool ok = true;
        for (int k = 0; k < w; k++) {
          int jj = (y + h) * cw + x + k;
          if (!solid[jj] || used[jj]) {
            ok = false;
            break;
          }
        }
        if (!ok)
          break;
        h++;
      }

      for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
          used[(y + yy) * cw + x + xx] = 1;

      if (lv->solidCount == cap) {
        cap *= 2;
        Rectangle *ns = realloc(lv->solids, (size_t)cap * sizeof(Rectangle));
        if (!ns)
          goto done;
        lv->solids = ns;
      }
      lv->solids[lv->solidCount++] = (Rectangle){
          (float)((x * gs + offX) * ART_TILE_SCALE),
          (float)((y * gs + offY) * ART_TILE_SCALE),
          (float)(w * gs * ART_TILE_SCALE), (float)(h * gs * ART_TILE_SCALE)};
    }
  }
done:
  free(solid);
  free(used);
}

// ---------------------------------------------------------------------
//  entities
// ---------------------------------------------------------------------
static void AddEntities(Level *lv, const Json *j, int list, int offX,
                        int offY) {
  EACH(j, list, e) {
    char id[48];
    Str(j, e, "__identifier", id, sizeof id);
    int px = Get(j, e, "px");
    double w = Num(j, e, "width", 0), h = Num(j, e, "height", 0);
    // "px" is the entity's PIVOT point; convert to its top-left corner so any
    // pivot setting works
    int pv = Get(j, e, "__pivot");
    double x = ArrNum(j, px, 0) + offX - ArrNum(j, pv, 0) * w;
    double y = ArrNum(j, px, 1) + offY - ArrNum(j, pv, 1) * h;

    if (strcmp(id, LDTK_PLAYER_START) == 0) {
      lv->hasStart = true;
      lv->start =
          (Vector2){(float)((x + w / 2.0) * SCALE), (float)((y + h) * SCALE)};
    }
    if (lv->entityCount < LEVEL_MAX_ENTITIES) {
      LevelEntity *le = &lv->entities[lv->entityCount++];
      snprintf(le->id, sizeof le->id, "%s", id);
      le->r = (Rectangle){(float)(x * SCALE), (float)(y * SCALE),
                          (float)(w * SCALE), (float)(h * SCALE)};
    }
  }
}

// ---------------------------------------------------------------------
//  public API
// ---------------------------------------------------------------------
// grid size + total pixel offset of a layer
static void LayerGrid(const Json *j, int layer, int *gs, int *ox, int *oy) {
  *gs = (int)Num(j, layer, "__gridSize", 8);
  *ox = (int)Num(j, layer, "__pxTotalOffsetX", Num(j, layer, "pxOffsetX", 0));
  *oy = (int)Num(j, layer, "__pxTotalOffsetY", Num(j, layer, "pxOffsetY", 0));
}

static bool Fill(Level *lv, const Json *root, Json *ext, const char *path,
                 const char *levelId) {
  // folder of the .ldtk file (tileset paths inside it are relative to this)
  char dir[256];
  {
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    const char *s = (b && (!a || b > a)) ? b : a;
    size_t n = s ? (size_t)(s - path) + 1 : 0;
    if (n >= sizeof dir)
      n = sizeof dir - 1;
    memcpy(dir, path, n);
    dir[n] = '\0';
  }

  // pick the level
  int level = -1;
  EACH(root, Get(root, 0, "levels"), l) {
    if (!levelId || Is(root, l, "identifier", levelId)) {
      level = l;
      break;
    }
  }
  if (level < 0) {
    TraceLog(LOG_WARNING, "LEVEL: level '%s' not found in '%s'",
             levelId ? levelId : "(first)", path);
    return false;
  }

  // layer data (null in the main file when "Save levels to separate files" is
  // on)
  const Json *j = root;
  int layers = Get(root, level, "layerInstances");
  if (layers < 0 || root->t[layers].type != JSMN_ARRAY) {
    char rel[256], full[512];
    Str(root, level, "externalRelPath", rel, sizeof rel);
    snprintf(full, sizeof full, "%s%s", dir, rel);
    if (rel[0] && FileExists(full) && JsonLoad(ext, full)) {
      j = ext;
      layers = Get(ext, 0, "layerInstances");
    } else
      layers = -1;
  }
  if (layers < 0 || j->t[layers].type != JSMN_ARRAY) {
    TraceLog(
        LOG_WARNING,
        "LEVEL: no layer data found (is the .ldtkl file next to the .ldtk?)");
    return false;
  }

  lv->width = (float)(Num(root, level, "pxWid", 0) * SCALE);
  lv->height = (float)(Num(root, level, "pxHei", 0) * SCALE);

  int count = Count(j, layers);
  int *list = malloc((size_t)(count > 0 ? count : 1) * sizeof(int));
  if (!list)
    return false;
  int n = 0;
  EACH(j, layers, ly) list[n++] = ly;

  // 1) collision layer: the one named LDTK_COLLISION_LAYER, else the first
  // IntGrid layer
  int coll = -1;
  for (int i = 0; i < n; i++) {
    if (!Is(j, list[i], "__type", "IntGrid"))
      continue;
    if (Is(j, list[i], "__identifier", LDTK_COLLISION_LAYER)) {
      coll = list[i];
      break;
    }
    if (coll < 0)
      coll = list[i];
  }
  if (coll >= 0) {
    int gs, ox, oy;
    LayerGrid(j, coll, &gs, &ox, &oy);
    BuildSolids(lv, j, Get(j, coll, "intGridCsv"),
                (int)Num(j, coll, "__cWid", 0), (int)Num(j, coll, "__cHei", 0),
                gs, ox, oy);
  } else
    TraceLog(LOG_WARNING,
             "LEVEL: no IntGrid layer named '%s' - nothing is solid!",
             LDTK_COLLISION_LAYER);

  // 2) tiles + entities. LDtk lists layers top-most first, so draw them in
  // reverse.
  for (int i = n - 1; i >= 0; i--) {
    int layer = list[i], gs, ox, oy;
    LayerGrid(j, layer, &gs, &ox, &oy);

    char rel[256];
    if (Str(j, layer, "__tilesetRelPath", rel, sizeof rel)[0]) {
      int tex = GetTexture(lv, dir, rel);
      if (tex >= 0) {
        // Identify by LDtk layer name OR tileset filename.
        // Naming the layer "Destructible" is recommended.
        char layerId[128];
        Str(j, layer, "__identifier", layerId, sizeof layerId);
        bool destructible = strstr(layerId, "Destructible") != NULL ||
                            strstr(layerId, "destructible") != NULL ||
                            strstr(rel, "destructivetileset") != NULL;
        AddTiles(lv, j, Get(j, layer, "autoLayerTiles"), tex, gs, ox, oy, destructible);
        AddTiles(lv, j, Get(j, layer, "gridTiles"), tex, gs, ox, oy, destructible);
      }
    }
    if (Is(j, layer, "__type", "Entities"))
      AddEntities(lv, j, Get(j, layer, "entityInstances"), ox, oy);
  }

  char id[64];
  TraceLog(LOG_INFO, "LEVEL: loaded '%s' - %d solids, %d tiles, %d entities%s",
           Str(root, level, "identifier", id, sizeof id), lv->solidCount,
           lv->tileCount, lv->entityCount,
           lv->hasStart ? "" : " (no PlayerStart entity)");

  free(list);
  return true;
}

bool Level_Load(Level *lv, const char *path, const char *levelId) {
  memset(lv, 0, sizeof *lv);

  Json root, ext;
  memset(&ext, 0, sizeof ext);
  if (!FileExists(path)) {
    TraceLog(LOG_WARNING, "LEVEL: '%s' not found", path);
    return false;
  }
  if (!JsonLoad(&root, path)) {
    TraceLog(LOG_WARNING, "LEVEL: '%s' is not valid JSON", path);
    return false;
  }
  bool ok = Fill(lv, &root, &ext, path, levelId);
  JsonFree(&ext);
  JsonFree(&root);
  return ok;
}

void Level_Unload(Level *lv) {
  free(lv->solids);
  free(lv->tiles);
  for (int i = 0; i < lv->texCount; i++)
    UnloadTexture(lv->tex[i]);
  memset(lv, 0, sizeof *lv);
}

void Level_Draw(const Level *lv, Rectangle view) {
  for (int i = 0; i < lv->tileCount; i++) {
    const LevelTile *t = &lv->tiles[i];
    if (t->destroyed) continue;
    float s = t->size * SCALE;
    Rectangle dst = {t->px * SCALE, t->py * SCALE, s, s};
    if (dst.x + s < view.x || dst.x > view.x + view.width ||
        dst.y + s < view.y || dst.y > view.y + view.height)
      continue;

    Rectangle src = {(float)t->sx, (float)t->sy,
                     (t->flip & 1) ? -(float)t->size : (float)t->size,
                     (t->flip & 2) ? -(float)t->size : (float)t->size};
    DrawTexturePro(lv->tex[t->tex], src, dst, (Vector2){0, 0}, 0.0f, WHITE);
  }
}

// Remove a tile-sized rectangle from all merged IntGrid collision rectangles.
// Splitting into non-overlapping pieces preserves the rest of the solid.
static void CutCollision(Level *lv, Rectangle cut) {
  int oldCount = lv->solidCount;
  Rectangle *old = lv->solids;
  Rectangle *next = malloc((size_t)(oldCount * 4 + 4) * sizeof(Rectangle));
  if (!next) return;
  int count = 0;
  for (int i=0; i<oldCount; ++i) {
    Rectangle s=old[i];
    float x0=fmaxf(s.x,cut.x), x1=fminf(s.x+s.width,cut.x+cut.width);
    float y0=fmaxf(s.y,cut.y), y1=fminf(s.y+s.height,cut.y+cut.height);
    if (x0>=x1 || y0>=y1) { next[count++]=s; continue; }
    if (y0>s.y) next[count++]=(Rectangle){s.x,s.y,s.width,y0-s.y};
    if (y1<s.y+s.height) next[count++]=(Rectangle){s.x,y1,s.width,s.y+s.height-y1};
    if (x0>s.x) next[count++]=(Rectangle){s.x,y0,x0-s.x,y1-y0};
    if (x1<s.x+s.width) next[count++]=(Rectangle){x1,y0,s.x+s.width-x1,y1-y0};
  }
  free(old);
  lv->solids=next;
  lv->solidCount=count;
}

int Level_Explode(Level *lv, Vector2 center, float radius) {
  int destroyed=0;
  for (int i=0; i<lv->tileCount; ++i) {
    LevelTile *t=&lv->tiles[i];
    if (!t->destructible || t->destroyed) continue;
    float s=t->size*SCALE;
    Rectangle r={t->px*SCALE,t->py*SCALE,s,s};
    float cx=fmaxf(r.x,fminf(center.x,r.x+r.width));
    float cy=fmaxf(r.y,fminf(center.y,r.y+r.height));
    float dx=cx-center.x,dy=cy-center.y;
    if (dx*dx+dy*dy>radius*radius) continue;
    t->destroyed=true;
    destroyed++;
    CutCollision(lv,r);
  }
  return destroyed;
}
