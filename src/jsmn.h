/*
 * jsmn.h - minimal JSON tokenizer. Header-only, MIT license.
 * API-compatible with Serge Zaitsev's jsmn (https://github.com/zserge/jsmn):
 * jsmn_init(), jsmn_parse(), jsmntok_t, JSMN_* types and errors. Non-strict
 * mode, parent links always on (O(1) bracket matching), all functions static (include it from one .c file).
 *
 * Usage: call jsmn_parse(&p, js, len, NULL, 0) to COUNT the tokens, allocate
 * that many, jsmn_init() again, then jsmn_parse() into the array.
 * Token layout: an object's `size` is its number of keys, an array's is its
 * number of items, and a key token has size 1 (its value follows it).
 *
 * Copyright (c) 2010 Serge Zaitsev - MIT License
 */
#ifndef JSMN_H
#define JSMN_H

#include <stddef.h>

typedef enum {
  JSMN_UNDEFINED = 0,
  JSMN_OBJECT = 1,
  JSMN_ARRAY = 2,
  JSMN_STRING = 4,
  JSMN_PRIMITIVE = 8 // number, true, false, null
} jsmntype_t;

enum jsmnerr {
  JSMN_ERROR_NOMEM = -1, // not enough tokens
  JSMN_ERROR_INVAL = -2, // invalid character
  JSMN_ERROR_PART = -3   // incomplete JSON
};

typedef struct {
  jsmntype_t type;
  int start, end; // byte range in the text (strings: without the quotes)
  int size;       // number of children
  int parent;     // index of the parent token (-1 for the root)
} jsmntok_t;

typedef struct {
  unsigned int pos;     // offset in the text
  unsigned int toknext; // next token to allocate
  int toksuper;         // parent of the token being filled
} jsmn_parser;

static void jsmn_init(jsmn_parser *p) {
  p->pos = 0;
  p->toknext = 0;
  p->toksuper = -1;
}

static jsmntok_t *jsmn_alloc_token(jsmn_parser *p, jsmntok_t *toks, size_t n) {
  if (p->toknext >= n)
    return NULL;
  jsmntok_t *t = &toks[p->toknext++];
  t->start = t->end = -1;
  t->size = 0;
  t->parent = -1;
  return t;
}

static void jsmn_fill_token(jsmntok_t *t, jsmntype_t type, int start, int end) {
  t->type = type;
  t->start = start;
  t->end = end;
  t->size = 0;
}

static int jsmn_parse_primitive(jsmn_parser *p, const char *js, size_t len,
                                jsmntok_t *toks, size_t n) {
  int start = (int)p->pos;
  for (; p->pos < len && js[p->pos] != '\0'; p->pos++) {
    char c = js[p->pos];
    if (c == '\t' || c == '\r' || c == '\n' || c == ' ' || c == ',' ||
        c == ']' || c == '}' || c == ':')
      break;
    if (c < 32 || c >= 127) {
      p->pos = (unsigned)start;
      return JSMN_ERROR_INVAL;
    }
  }
  if (toks) {
    jsmntok_t *t = jsmn_alloc_token(p, toks, n);
    if (!t) {
      p->pos = (unsigned)start;
      return JSMN_ERROR_NOMEM;
    }
    jsmn_fill_token(t, JSMN_PRIMITIVE, start, (int)p->pos);
    t->parent = p->toksuper;
  }
  p->pos--; // the main loop's pos++ will step onto the terminator
  return 0;
}

static int jsmn_parse_string(jsmn_parser *p, const char *js, size_t len,
                             jsmntok_t *toks, size_t n) {
  int start = (int)p->pos;
  for (p->pos++; p->pos < len && js[p->pos] != '\0'; p->pos++) {
    char c = js[p->pos];
    if (c == '"') {
      if (toks) {
        jsmntok_t *t = jsmn_alloc_token(p, toks, n);
        if (!t) {
          p->pos = (unsigned)start;
          return JSMN_ERROR_NOMEM;
        }
        jsmn_fill_token(t, JSMN_STRING, start + 1, (int)p->pos);
        t->parent = p->toksuper;
      }
      return 0;
    }
    if (c == '\\' && p->pos + 1 < len) {
      p->pos++;
      switch (js[p->pos]) {
      case '"': case '/': case '\\': case 'b': case 'f': case 'r': case 'n':
      case 't':
        break;
      case 'u': // \uXXXX
        for (int i = 0; i < 4; i++) {
          p->pos++;
          char h = p->pos < len ? js[p->pos] : '\0';
          if (!((h >= '0' && h <= '9') || (h >= 'A' && h <= 'F') ||
                (h >= 'a' && h <= 'f'))) {
            p->pos = (unsigned)start;
            return JSMN_ERROR_INVAL;
          }
        }
        break;
      default:
        p->pos = (unsigned)start;
        return JSMN_ERROR_INVAL;
      }
    }
  }
  p->pos = (unsigned)start;
  return JSMN_ERROR_PART;
}

// Returns the number of tokens (also with toks == NULL: count only), or < 0.
static int jsmn_parse(jsmn_parser *p, const char *js, size_t len,
                      jsmntok_t *toks, unsigned int n) {
  int r, count = (int)p->toknext;
  jsmntok_t *t;

  for (; p->pos < len && js[p->pos] != '\0'; p->pos++) {
    char c = js[p->pos];
    switch (c) {
    case '{':
    case '[':
      count++;
      if (!toks)
        break;
      if (!(t = jsmn_alloc_token(p, toks, n)))
        return JSMN_ERROR_NOMEM;
      if (p->toksuper != -1)
        toks[p->toksuper].size++;
      t->parent = p->toksuper;
      t->type = (c == '{') ? JSMN_OBJECT : JSMN_ARRAY;
      t->start = (int)p->pos;
      p->toksuper = (int)p->toknext - 1;
      break;

    case '}':
    case ']': {
      if (!toks)
        break;
      jsmntype_t type = (c == '}') ? JSMN_OBJECT : JSMN_ARRAY;
      // walk up from the newest token to the innermost open container
      if (p->toknext < 1)
        return JSMN_ERROR_INVAL;
      t = &toks[p->toknext - 1];
      for (;;) {
        if (t->start != -1 && t->end == -1) {
          if (t->type != type)
            return JSMN_ERROR_INVAL;
          t->end = (int)p->pos + 1;
          p->toksuper = t->parent;
          break;
        }
        if (t->parent == -1)
          return JSMN_ERROR_INVAL; // unmatched closing bracket
        t = &toks[t->parent];
      }
      break;
    }

    case '"':
      if ((r = jsmn_parse_string(p, js, len, toks, n)) < 0)
        return r;
      count++;
      if (p->toksuper != -1 && toks)
        toks[p->toksuper].size++;
      break;

    case '\t': case '\r': case '\n': case ' ':
      break;

    case ':':
      p->toksuper = (int)p->toknext - 1; // the key
      break;

    case ',':
      // after a key's value, the parent is the container again
      if (toks && p->toksuper != -1 && toks[p->toksuper].type != JSMN_ARRAY &&
          toks[p->toksuper].type != JSMN_OBJECT)
        p->toksuper = toks[p->toksuper].parent;
      break;

    default: // number, true, false, null
      if ((r = jsmn_parse_primitive(p, js, len, toks, n)) < 0)
        return r;
      count++;
      if (p->toksuper != -1 && toks)
        toks[p->toksuper].size++;
      break;
    }
  }

  if (toks)
    for (int i = (int)p->toknext - 1; i >= 0; i--)
      if (toks[i].start != -1 && toks[i].end == -1)
        return JSMN_ERROR_PART; // unmatched { or [
  return count;
}

#endif
