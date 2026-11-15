# Raylib bomb abilities patch

This folder contains replacements for the seven source/header files from your `(2)` uploads. Copy the files over your existing same-named source files (back up your originals first). `jsmn.h`, `resource_dir.h`, Raylib, and your resources folder stay as they are.

## Controls / physics update

- **U / P** throws a physical bomb. The bomb explodes on terrain contact or fuse expiry.
- **Hold K** for 0.38 seconds; the player flashes white when charged.
- **Release K**: a **one-time upward impulse**, or **upward-forward** with A/D or left/right held. Velocity is not overwritten each tick. Momentum decays naturally via gravity and existing air control, unlike the previous timed dash.
- **While fully charged, press Shift in midair** to dive down. The dive applies a one-time downward impulse; on floor impact the explosion launches you **up**, or **up-forward** when holding A/D at impact. It consumes the same ability charge.
- **Touch the ground** to restore one charge, after a brief recharge lock to prevent the dive rebound from immediately replenishing it.
- **Glide is completely removed**: no remaining glide physics, input state or animation.
- **F5** reloads the level, **R** respawns.

### Tuning in main.c

`BOMB_BURST_UP_IMPULSE`, `BOMB_BURST_SIDE_IMPULSE`, `BOMB_DIVE_SPEED`, `BOMB_DIVE_REBOUND_UP`, `BOMB_DIVE_REBOUND_SIDE`, `BOMB_CHARGE_TIME`, `BOMB_RECHARGE_DELAY`.

Note: an impulse means instantaneous change in velocity for a mass-one character (equivalent to Unity `AddForce(..., ForceMode2D.Impulse)`). There is no longer a dash attack/end speed phase or ledge auto-hop. The existing movement acceleration and friction still influence airborne momentum.

## Sprites (relative to resources/)

- `player/groundthrow1.png` through `player/groundthrow5.png`: five separate single-frame PNGs, displayed at 12 frames/s.
- Optional future `player/airthrow1.png` through `player/airthrow5.png`: if missing, automatically uses the corresponding ground throw frame. Air and ground now have separate animation slots so you can replace the art later.
- `player/dive1.png` through `player/dive3.png`: three separate dive frames; the final frame stays displayed until impact.
- `bomb/bomb.png`: bomb projectile sprite. Point sampling and existing ART_PIXEL=4 scale are retained.
- `shaders/ghost.fs` must exist for the white silhouette charging flash. The code uses the same ghost shader as ghost trails; without it the fallback has no true silhouette effect.

## Destructible LDtk tiles

- Put destructible tiles on a separate LDtk Tile/Auto-layer whose name includes `Destructible` (case-sensitive exact spelling `Destructible` or lowercase `destructible`), **or** use a tileset filename containing `destructivetileset`.
- Put the destructive area in the existing IntGrid `Collision` layer too, so it starts solid.
- An explosion marks nearby destructible *tile instances* destroyed and cuts their tile-sized rectangles out of the merged IntGrid collision solids. Ordinary ground tile visuals are untouched.
- When a collision cell includes both a ground tile and a destructive tile, the simple subtraction implementation cannot distinguish ownership. Avoid overlapping solid ground and destructible areas for now. If later you need layered colliders, track collision occupancy by tile and rebuild the merged rectangles.
- No new menu, audio, or multi-level navigation was added in this patch.

## Tuning

In `main.c`: `BOMB_CHARGE_TIME`, `BOMB_DIVE_SPEED`, `BOMB_THROW_SPEED`, `BOMB_FUSE_TIME`, `BOMB_BLAST_RADIUS` and the impulse constants above.

## Validation

`gcc -std=c11 -fsyntax-only` succeeded for all four `.c` files using a minimal Raylib API declaration stub in this environment. This checks C syntax and type usage against the stub, **not** actual linking, gameplay or assets. Compile and run using your installed Raylib quickstart project for integration testing.
