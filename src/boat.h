#ifndef __MC_BOAT_H__
#define __MC_BOAT_H__

#include <ultra64.h>
#include "steve.h"

// boats (BoatItem, AbstractBoat / Boat, BoatModel, AbstractBoatRenderer).
// they float on bk's own water and on grid water. not saved: they go away
// with the map.
int boat_item_use(const f32 eye[3], const f32 dir[3], f32 reach); // BoatItem.use
int boat_interact(const f32 eye[3], const f32 dir[3], f32 reach); // right click a boat: ride it
int boat_attack(const f32 eye[3], const f32 dir[3], f32 reach, f32 damage); // hit a boat
int boat_riding(void);                                           // steve is in a boat
f32 boat_take_turn(f32 pt);                                      // the view's share of the boat's turn up to this partial tick (added to steve's yaw)
void boat_rider_tick(Steve *s, const SteveInput *in);            // the ridden boat + steve on it
void boats_tick(void);                                           // the other boats
void boats_clear(void);
void boats_draw(Gfx **gfx, Mtx **mtx, f32 cam[3], f32 pt);
int boat_is_item(u16 item);

#endif
