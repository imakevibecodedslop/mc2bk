// the shaped block families (tools/mc_families.py): their VoxelShapes, how
// they're placed (getStateForPlacement / canBeReplaced), used (doors, fence
// gates) and how they follow their neighbours (updateShape: stairs corners,
// fence and pane connections, door halves). ported from StairBlock,
// SlabBlock, DoorBlock, FenceBlock, FenceGateBlock, IronBarsBlock,
// CrossCollisionBlock, LadderBlock, LanternBlock, RotatedPillarBlock,
// SnowLayerBlock, CarpetBlock, VegetationBlock (26.3).
//
// cell states (CELL_STATE) = the model state index (mc_families.py order):
//   slab: type (bottom, top, double)
//   stairs: (facing * 2 + half) * 5 + shape (straight, inner_left, inner_right, outer_left, outer_right)
//   door: ((facing * 2 + half) * 2 + hinge) * 2 + open   (half: lower, upper; hinge: left, right)
//   fence gate: (facing * 2 + in_wall) * 2 + open
//   fence / pane: the sides mask (north 1, east 2, south 4, west 8)
//   ladder, wall torch: facing;  lantern: hanging;  chain: axis (y, x, z);  snow: layers - 1
// facing: 0 south, 1 north, 2 east, 3 west

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "world.h"
#include "terrain.h"
#include "sound.h"
#include "shapes.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

#ifdef MC_HAVE_MODELS

// facing index -> step, and the turns (Direction.getClockWise: north east south west)
static const s8 fstep[4][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 } }; // (x, z)
static const u8 fcw[4] = { 3, 2, 0, 1 };   // south->west, north->east, east->south, west->north
static const u8 fccw[4] = { 2, 3, 1, 0 };  // south->east, north->west, east->north, west->south
static const u8 fopp[4] = { 1, 0, 3, 2 };
// the multipart sides (north, east, south, west) as facings
static const u8 side_facing[4] = { 1, 2, 0, 3 };

int shapes_family(BlockId b) {
    return (b < MC_BLOCK_COUNT) ? mc_block_family[b] : FAM_NONE;
}

static u16 cell_at(s32 x, s32 y, s32 z) {
    return world_get_cell(x, y, z);
}

// --- shapes ---

// the mc_model_states row of a family cell
static const McModelState *state_row(u16 cell) {
    BlockId b = CELL_ID(cell);
    const McBlockModel *m = &mc_block_models[b];
    int fam = shapes_family(b), st = CELL_STATE(cell);
    if (fam == FAM_FENCE || fam == FAM_PANE) return &mc_model_states[m->state_row + (st & 15)];
    if (fam == FAM_TRAPDOOR) st &= 15;
    if (st >= m->count) st = 0;
    return &mc_model_states[m->first + st];
}

int shapes_boxes(u16 cell, int collision, McBox *out, int max) {
    BlockId b = CELL_ID(cell);
    const McModelState *r;
    int n, i, k;
    if (b == BLOCK_AIR || !(mc_block(b)->flags & MCB_TEX_SLOTS)) return -1; // (not a family block)
    r = state_row(cell);
    n = collision ? r->ncoll : r->nbox;
    if (n > max) n = max;
    for (i = 0; i < n; i++) {
        const McShapeBox *sb = &mc_shape_boxes[r->box + (collision ? r->nbox : 0) + i];
        for (k = 0; k < 3; k++) {
            out[i].lo[k] = sb->lo[k];
            out[i].hi[k] = sb->hi[k];
        }
    }
    return n;
}

// the model state a family cell draws
int shapes_model_state(u16 cell) {
    int fam = shapes_family(CELL_ID(cell)), st = CELL_STATE(cell);
    if (fam == FAM_TRAPDOOR || fam == FAM_FENCE || fam == FAM_PANE) return st & 15;
    return st;
}

// --- neighbours ---

// isFaceSturdy toward a fence / pane: full opaque blocks, and bk's own ground
static int sturdy(s32 x, s32 y, s32 z) {
    BlockId b = CELL_ID(cell_at(x, y, z));
    if (b == BLOCK_AIR) return !terrain_is_carved(x, y, z) && terrain_cell_in_ground(x, y, z);
    return (mc_block(b)->flags & MCB_OPAQUE) != 0;
}

static int name_has(BlockId b, const char *part);

// #wooden_fences (nether_brick_fence isn't one: they don't join)
static int is_wooden_fence(BlockId b) {
    return !name_has(b, "nether_brick");
}

// FenceBlock.connectsTo / IronBarsBlock.attachsTo, d = the side (facing index)
static int connects(BlockId self, int fam, s32 x, s32 y, s32 z, int f) {
    s32 nx = x + fstep[f][0], nz = z + fstep[f][1];
    u16 c = cell_at(nx, y, nz);
    BlockId b = CELL_ID(c);
    int nf = shapes_family(b);
    if (fam == FAM_FENCE) {
        if (nf == FAM_FENCE && is_wooden_fence(b) == is_wooden_fence(self)) return TRUE;
        // FenceGateBlock.connectsToDirection: the gate's axis is across this side
        if (nf == FAM_GATE) {
            int gf = CELL_STATE(c) / 4;
            int gate_x = (gf == 2 || gf == 3), side_x = (f == 2 || f == 3);
            return gate_x != side_x;
        }
        return sturdy(nx, y, nz);
    }
    if (nf == FAM_PANE) return TRUE;
    return sturdy(nx, y, nz);
}

static int cross_mask(BlockId b, int fam, s32 x, s32 y, s32 z) {
    int i, m = 0;
    for (i = 0; i < 4; i++) {
        if (connects(b, fam, x, y, z, side_facing[i])) m |= 1 << i;
    }
    return m;
}

static int stairs_props(u16 c, int *facing, int *half) {
    if (shapes_family(CELL_ID(c)) != FAM_STAIRS) return FALSE;
    *facing = (CELL_STATE(c) / 5) / 2;
    *half = (CELL_STATE(c) / 5) % 2;
    return TRUE;
}

// StairBlock.canTakeShape
static int can_take_shape(int facing, int half, s32 x, s32 y, s32 z, int f) {
    int of, oh;
    if (!stairs_props(cell_at(x + fstep[f][0], y, z + fstep[f][1]), &of, &oh)) return TRUE;
    return of != facing || oh != half;
}

// StairBlock.getStairsShape
static int stairs_shape(int facing, int half, s32 x, s32 y, s32 z) {
    int bf, bh;
    if (stairs_props(cell_at(x + fstep[facing][0], y, z + fstep[facing][1]), &bf, &bh) && bh == half) {
        int axis_b = bf >= 2, axis = facing >= 2;
        if (axis_b != axis && can_take_shape(facing, half, x, y, z, fopp[bf])) {
            return bf == fccw[facing] ? 3 : 4; // outer left / right
        }
    }
    if (stairs_props(cell_at(x + fstep[fopp[facing]][0], y, z + fstep[fopp[facing]][1]), &bf, &bh) && bh == half) {
        int axis_b = bf >= 2, axis = facing >= 2;
        if (axis_b != axis && can_take_shape(facing, half, x, y, z, bf)) {
            return bf == fccw[facing] ? 1 : 2; // inner left / right
        }
    }
    return 0;
}

static int in_update, suppress;

// updateShape for one cell
static void update_cell(s32 x, s32 y, s32 z) {
    u16 c = cell_at(x, y, z);
    BlockId b = CELL_ID(c);
    int fam = shapes_family(b), st = CELL_STATE(c), ns = st;

    switch (fam) {
        case FAM_STAIRS: {
            int facing = (st / 5) / 2, half = (st / 5) % 2;
            ns = (facing * 2 + half) * 5 + stairs_shape(facing, half, x, y, z);
            break;
        }
        case FAM_FENCE:
        case FAM_PANE:
            ns = cross_mask(b, fam, x, y, z);
            break;
        case FAM_DOOR: {
            // the other half has to be this door (else this half goes too)
            int upper = (st >> 2) & 1;
            u16 o = cell_at(x, y + (upper ? -1 : 1), z);
            if (CELL_ID(o) != b || ((CELL_STATE(o) >> 2) & 1) == upper) {
                world_set(x, y, z, BLOCK_AIR);
                return;
            }
            break;
        }
        default:
            return;
    }
    if (ns != st) world_set(x, y, z, MAKE_CELL(b, ns));
}

void shapes_suppress(int on) {
    suppress += on ? 1 : -1;
    if (suppress < 0) suppress = 0;
}

void shapes_on_set(s32 x, s32 y, s32 z) {
    static const s8 nb[7][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    int i;
    if (suppress || in_update > 8) return; // (a chain of corners: stop somewhere)
    in_update++;
    for (i = 0; i < 7; i++) {
        s32 px = x + nb[i][0], py = y + nb[i][1], pz = z + nb[i][2];
        BlockId b = world_get(px, py, pz);
        if (b != BLOCK_AIR && shapes_family(b) != FAM_NONE) update_cell(px, py, pz);
    }
    in_update--;
}

// --- placing ---

static int look_facing(const f32 look[3]) {
    if (look[0] * look[0] > look[2] * look[2]) return look[0] > 0.0f ? 2 : 3;
    return look[2] > 0.0f ? 0 : 1;
}

static int clicked_dir(const s32 n[3]) {
    if (n[1] > 0) return 0; // up (the new cell is above the clicked one)
    if (n[1] < 0) return 1; // down
    if (n[2] > 0) return 2; // south
    if (n[2] < 0) return 3; // north
    if (n[0] > 0) return 4; // east
    return 5;               // west
}

// DoorBlock.getHinge (hit = the click in the cell, 0..1)
static int door_hinge(BlockId door, s32 x, s32 y, s32 z, int dirf, const f32 hit[3]) {
    int l = fccw[dirf], r = fcw[dirf];
    s32 lx = x + fstep[l][0], lz = z + fstep[l][1], rx = x + fstep[r][0], rz = z + fstep[r][1];
    int bal = (sturdy(lx, y, lz) ? -1 : 0) + (sturdy(lx, y + 1, lz) ? -1 : 0) + (sturdy(rx, y, rz) ? 1 : 0) + (sturdy(rx, y + 1, rz) ? 1 : 0);
    u16 lc = cell_at(lx, y, lz), rc = cell_at(rx, y, rz);
    int door_l = shapes_family(CELL_ID(lc)) == FAM_DOOR && !((CELL_STATE(lc) >> 2) & 1);
    int door_r = shapes_family(CELL_ID(rc)) == FAM_DOOR && !((CELL_STATE(rc) >> 2) & 1);
    (void)door;
    if ((!door_l || door_r) && bal <= 0) {
        if ((!door_r || door_l) && bal >= 0) {
            int sx = fstep[dirf][0], sz = fstep[dirf][1];
            f32 cx = hit[0], cz = hit[2];
            return ((sx >= 0 || !(cz < 0.5f)) && (sx <= 0 || !(cz > 0.5f)) && (sz >= 0 || !(cx > 0.5f)) && (sz <= 0 || !(cx < 0.5f))) ? 0 : 1;
        }
        return 0;
    }
    return 1;
}

int shapes_place(BlockId *block, int *state, s32 pos[3], const s32 normal[3], const f32 hit[3], const f32 look[3]) {
    BlockId b = *block;
    int fam = shapes_family(b), cd = clicked_dir(normal), lf = look_facing(look);

    switch (fam) {
        case FAM_SLAB:
            // SlabBlock: bottom, or top when clicked from under / the upper half of a side
            *state = (cd != 1 && (cd == 0 || !(hit[1] > 0.5f))) ? 0 : 1;
            return 1;
        case FAM_STAIRS: {
            int half = (cd != 1 && (cd == 0 || !(hit[1] > 0.5f))) ? 0 : 1;
            *state = (lf * 2 + half) * 5 + stairs_shape(lf, half, pos[0], pos[1], pos[2]);
            return 1;
        }
        case FAM_DOOR: {
            BlockId above = world_get(pos[0], pos[1] + 1, pos[2]);
            if (above != BLOCK_AIR && !(mc_block(above)->flags & (MCB_FLUID | MCB_WASHED))) return -1;
            *state = ((lf * 2 + 0) * 2 + door_hinge(b, pos[0], pos[1], pos[2], lf, hit)) * 2 + 0;
            return 1;
        }
        case FAM_GATE:
            *state = (lf * 2 + 0) * 2 + 0;
            return 1;
        case FAM_FENCE:
        case FAM_PANE:
            *state = cross_mask(b, fam, pos[0], pos[1], pos[2]);
            return 1;
        case FAM_LADDER:
            // LadderBlock: on the clicked side (the block it hangs on is behind)
            if (cd >= 2) *state = (cd == 2) ? 0 : (cd == 3) ? 1 : (cd == 4) ? 2 : 3;
            else *state = fopp[lf];
            return 1;
        case FAM_LANTERN:
            *state = (cd == 1) ? 1 : 0; // hanging under a block
            return 1;
        case FAM_CHAIN:
            *state = (cd <= 1) ? 0 : (cd >= 4) ? 1 : 2; // RotatedPillarBlock: the clicked face's axis
            return 1;
        case FAM_SNOW:
        case FAM_CARPET:
        case FAM_PLATE:
            *state = 0;
            return 1;
        case FAM_PLANT: {
            // VegetationBlock.mayPlaceOn: dirt-like ground under it
            BlockId under = world_get(pos[0], pos[1] - 1, pos[2]);
            if (under == BLOCK_AIR) {
                if (!(!terrain_is_carved(pos[0], pos[1] - 1, pos[2]) && terrain_cell_in_ground(pos[0], pos[1] - 1, pos[2]))) return -1;
            } else if (!(mc_block(under)->flags & MCB_MINE_SHOVEL) && under != BLOCK_MOSS_BLOCK) {
                return -1;
            }
            *state = 0;
            return 1;
        }
    }
    return 0;
}

// setting a placed block: a door gets its upper half with it (DoorBlock.setPlacedBy)
void shapes_set_placed(s32 pos[3], u16 cell) {
    if (shapes_family(CELL_ID(cell)) == FAM_DOOR) {
        suppress++;
        world_set(pos[0], pos[1], pos[2], cell);
        world_set(pos[0], pos[1] + 1, pos[2], MAKE_CELL(CELL_ID(cell), CELL_STATE(cell) | 4));
        suppress--;
        shapes_on_set(pos[0], pos[1], pos[2]);
        shapes_on_set(pos[0], pos[1] + 1, pos[2]);
        return;
    }
    world_set(pos[0], pos[1], pos[2], cell);
}

// SlabBlock / SnowLayerBlock.canBeReplaced: the same slab gets doubled, snow
// gets a layer. cell = where the block would go (the clicked cell when that
// one can take it: replacing_clicked). returns TRUE with the new cell value
int shapes_merge(BlockId held, s32 cell[3], int replacing_clicked, const s32 normal[3], f32 hit_y, u16 *out) {
    u16 c = cell_at(cell[0], cell[1], cell[2]);
    BlockId b = CELL_ID(c);
    int fam = shapes_family(held), st = CELL_STATE(c), cd = clicked_dir(normal);
    if (b != held) return FALSE;
    if (fam == FAM_SLAB) {
        int upper = hit_y > 0.5f;
        if (st == 2) return FALSE;
        if (replacing_clicked) {
            if (st == 0 ? !(cd == 0 || (upper && cd >= 2)) : !(cd == 1 || (!upper && cd >= 2))) return FALSE;
        }
        *out = MAKE_CELL(b, 2);
        return TRUE;
    }
    if (fam == FAM_SNOW) {
        if (st >= 7) return FALSE;
        if (replacing_clicked && cd != 0) return FALSE;
        *out = MAKE_CELL(b, st + 1);
        return TRUE;
    }
    return FALSE;
}

// --- using ---

static int name_has(BlockId b, const char *part) {
    const char *n = mc_registry_names[b];
    int i, k;
    for (i = 0; n[i]; i++) {
        for (k = 0; part[k] && n[i + k] == part[k]; k++) { }
        if (part[k] == 0) return TRUE;
    }
    return FALSE;
}

int shapes_use(s32 pos[3], const f32 look[3]) {
    u16 c = cell_at(pos[0], pos[1], pos[2]);
    BlockId b = CELL_ID(c);
    int fam = shapes_family(b), st = CELL_STATE(c);

    if (fam == FAM_DOOR) {
        // DoorBlock.useWithoutItem: BlockSetType.canOpenByHand (not iron)
        int upper = (st >> 2) & 1, open = !(st & 1);
        s32 oy = pos[1] + (upper ? -1 : 1);
        if (name_has(b, "iron_door")) return FALSE;
        suppress++;
        world_set(pos[0], pos[1], pos[2], MAKE_CELL(b, (st & ~1) | open));
        world_set(pos[0], oy, pos[2], MAKE_CELL(b, ((st & ~1) | open) ^ 4)); // (updateShape: the other half copies it)
        suppress--;
        sound_play_cell(open ? SND(BLOCK_WOODEN_DOOR_OPEN) : SND(BLOCK_WOODEN_DOOR_CLOSE), pos[0], pos[1], pos[2], 1.0f,
            sound_random() * 0.1f + 0.9f);
        return TRUE;
    }
    if (fam == FAM_GATE) {
        // FenceGateBlock.useWithoutItem: opens away from the player
        int facing = st / 4, in_wall = (st / 2) & 1;
        if (st & 1) {
            st &= ~1;
            sound_play_cell(SND(BLOCK_FENCE_GATE_CLOSE), pos[0], pos[1], pos[2], 1.0f, sound_random() * 0.1f + 0.9f);
        } else {
            int pf = look_facing(look);
            if (facing == fopp[pf]) facing = pf;
            st = (facing * 2 + in_wall) * 2 + 1;
            sound_play_cell(SND(BLOCK_FENCE_GATE_OPEN), pos[0], pos[1], pos[2], 1.0f, sound_random() * 0.1f + 0.9f);
        }
        world_set(pos[0], pos[1], pos[2], MAKE_CELL(b, st));
        return TRUE;
    }
    return FALSE;
}

// how many times a broken cell's loot drops (snow: once per layer)
int shapes_drop_count(u16 cell) {
    int fam = shapes_family(CELL_ID(cell));
    if (fam == FAM_SNOW) return CELL_STATE(cell) + 1;
    return 1;
}

int shapes_climbable(s32 x, s32 y, s32 z) {
    return shapes_family(world_get(x, y, z)) == FAM_LADDER;
}

#else
int shapes_family(BlockId b) { (void)b; return 0; }
int shapes_boxes(u16 cell, int collision, McBox *out, int max) { (void)cell; (void)collision; (void)out; (void)max; return -1; }
int shapes_model_state(u16 cell) { return CELL_STATE(cell); }
void shapes_on_set(s32 x, s32 y, s32 z) { (void)x; (void)y; (void)z; }
void shapes_suppress(int on) { (void)on; }
int shapes_place(BlockId *block, int *state, s32 pos[3], const s32 normal[3], const f32 hit[3], const f32 look[3]) { return 0; }
void shapes_set_placed(s32 pos[3], u16 cell) { world_set(pos[0], pos[1], pos[2], cell); }
int shapes_merge(BlockId held, s32 cell[3], int replacing_clicked, const s32 normal[3], f32 hit_y, u16 *out) { return FALSE; }
int shapes_use(s32 pos[3], const f32 look[3]) { return FALSE; }
int shapes_drop_count(u16 cell) { return 1; }
int shapes_climbable(s32 x, s32 y, s32 z) { return FALSE; }
#endif
