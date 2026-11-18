#ifndef FX_H
#define FX_H
#include "raylib.h"
void Fx_Load(void);
void Fx_Unload(void);
void Fx_Reset(void);
void Fx_Tick(float dt, Vector2 position, Vector2 velocity);
void Fx_Draw(void);
void Fx_Burst(Vector2 position);
#endif
