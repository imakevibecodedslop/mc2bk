#ifndef __MC_WORLD_H__
#define __MC_WORLD_H__

#include <ultra64.h>

// the invisible minecraft grid laid over every bk level.
// 1 block = MC_BLOCK bk units, block (0,0,0) spans world 0..80 on each axis.
// blocks live in 16x16x16 sections (minecraft chunks are 16x16 columns of
// these). cleared when the map changes, nothing is saved yet.

#define SECTION_SIZE 16
#define SECTION_VOLUME (SECTION_SIZE * SECTION_SIZE * SECTION_SIZE)

#include "mcdata.h" // block ids and properties, generated from minecraft's data

typedef struct Section {
    s32 cx, cy, cz;      // section coords (block coords / 16)
    BlockId blocks[SECTION_VOLUME]; // index: (y * 16 + z) * 16 + x
    u16 block_count;
    u16 special_count;
    u16 index;           // its place in the section list (world_section_at)   // tnt, fire, fluids, redstone / model blocks: what the tick scans look for
    u8 dirty;            // mesh needs rebuilding
    u16 quad_count;
    Vtx *vtx;            // mesh, owned (recomp_alloc)
    Gfx *dl;
} Section;

// result of a raycast through the grid
typedef struct {
    s32 hit;
    s32 cell[3];     // block that was hit
    s32 normal[3];   // face that was hit (-1/0/1 per axis)
    f32 point[3];    // hit point, bk units
    f32 dist;        // distance from start, bk units
} VoxelHit;

void world_clear(void);
BlockId world_get(s32 x, s32 y, s32 z);      // the block id
u16 world_get_cell(s32 x, s32 y, s32 z);     // id + block state (CELL_*)
// world_set takes a plain id or a MAKE_CELL(id, state)
void world_set(s32 x, s32 y, s32 z, BlockId block);

// walks the grid from start to end (bk units). ignores the block the ray starts in.
s32 world_raycast(f32 start[3], f32 end[3], VoxelHit *out);
extern u32 world_change_count; // any cell changed since you last looked
extern int world_pick_mode; // set while aiming: noCollision blocks (dust, torches) are hit too
// the cell's shape box in 1/16 block (model blocks: their state's box, else the full cube)
extern u32 gMcFrame; // drawn frames (main.c)
void world_cell_box(u16 cell, u8 lo[3], u8 hi[3]);
int world_cell_boxes(u16 cell, int collision, McBox *out, int max); // every box (outline or collision), 1/16 block

// floor() to int for bk units -> block coords
s32 world_to_block(f32 v);

// section iteration for the renderer
s32 world_section_count(void);
int world_section_index_at(s32 x, s32 y, s32 z); // -1 if none
Section *world_section_at(s32 i);

// hooks the grid into bk's collision so the game (and steve) bump into blocks
void world_register_collision(void);

const char *block_name(BlockId block);

// full opaque cube: hides the faces of blocks next to it
int block_is_opaque(BlockId block);
int block_is_special(BlockId block); // counted in Section.special_count


// one textured block cube through a pose (hotbar icons, held block). see world_draw.c
#include "pose.h"
void world_emit_block_cube(Gfx **gfx, BlockId block, const Pose *p, Vtx *buf, int view_ortho, const u32 *tex32);
u16 world_block_particle_tex(BlockId block);
void world_emit_block_face(Gfx **gfx, BlockId block, const s32 cell[3], u8 face, const f32 cam[3], Vtx *buf);
int world_block_face_clip_vtx(BlockId block, const s32 cell[3], u8 f, const f32 cam[3], Vtx *buf, const f32 *top_y);
void world_emit_block_face_clip(Gfx **gfx, BlockId block, const s32 cell[3], u8 f, const f32 cam[3], Vtx *buf, const f32 *top_y);
extern int world_item_both_faces; // set around z-buffered draws of flat items
void world_emit_item_sprite(Gfx **gfx, const u32 *sprite, const Pose *p, Vtx *buf, int view_ortho);
void world_set_texture_state(Gfx **gfx);
void world_restore_texture_state(Gfx **gfx);

#endif
