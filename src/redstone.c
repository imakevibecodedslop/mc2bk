// redstone, ported from minecraft 26.3 (see redstone.h for the cell states):
//   - Level.getSignal / getDirectSignalTo / getBestNeighborSignal /
//     hasNeighborSignal, isRedstoneConductor (full opaque blocks, not the
//     redstone block)
//   - RedstoneWireBlock: getConnectionState (dot / cross), getSignal /
//     getDirectSignal, DefaultRedstoneWireEvaluator.calculateTargetStrength
//     (best non-wire neighbour signal, or the strongest wire next to it, also
//     one up / down, minus 1)
//   - LeverBlock / ButtonBlock (stone 20 ticks, wood 30): 15 to every side,
//     strong power into the block they're on
//   - RedstoneTorchBlock / RedstoneWallTorchBlock: off while the block they
//     hang on is powered (2 tick delay, burnout after 8 toggles in 60 ticks,
//     160 ticks off), strong power into the block above
//   - RedstoneLampBlock: lit while powered, goes out 4 ticks later
//   - TrapDoorBlock: opens while powered, wooden ones by hand too
// minecraft updates neighbours as things change; here every tick the wires
// are solved from scratch (the same strengths), then the torches, lamps and
// trapdoors look at their inputs.

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "redstone.h"
#include "survival.h"
#include "terrain.h"
#include "sound.h"
#include "shapes.h"

// face order: up, down, south, north, east, west
static const s8 dir[6][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 } };
enum { D_UP, D_DOWN, D_SOUTH, D_NORTH, D_EAST, D_WEST };
// the wire's sides in the model's order: north, east, south, west
static const u8 wire_side_dir[4] = { D_NORTH, D_EAST, D_SOUTH, D_WEST };

#define FACING_DIR(f) ((f) + 2) // facing 0..3 (south, north, east, west) -> face index

static u32 tick_now;
static int best_neighbor_signal(s32 x, s32 y, s32 z);
static int should_signal = TRUE; // RedstoneWireBlock.shouldSignal

static u16 cell_at(s32 x, s32 y, s32 z) {
    return world_get_cell(x, y, z);
}

static int is_wire(BlockId b) {
    return b == BLOCK_REDSTONE_WIRE;
}

// levers, every button, and the pressure plates (a plate is a floor button
// here: state bit 0 powered, attached to the block under it)
static int is_lever_or_button(BlockId b) {
    int f = shapes_family(b);
    return b == BLOCK_LEVER || f == FAM_BUTTON || f == FAM_PLATE;
}

static int is_trapdoor(BlockId b) {
    return shapes_family(b) == FAM_TRAPDOOR;
}

static int is_torch(BlockId b) {
    return shapes_family(b) == FAM_TORCH; // torch, soul_torch, copper_torch
}

static int is_wall_torch(BlockId b) {
    return shapes_family(b) == FAM_WALL_TORCH;
}

static int name_has(BlockId b, const char *part) {
    const char *n = mc_registry_names[b];
    int i, k;
    for (i = 0; n[i]; i++) {
        for (k = 0; part[k] && n[i + k] == part[k]; k++) { }
        if (part[k] == 0) return TRUE;
    }
    return FALSE;
}

// BlockSetType: stone and polished blackstone click and press like stone,
// the rest like wood (buttons stay down 20 / 30 ticks)
static int stone_type(BlockId b) {
    return name_has(b, "stone");
}

// BlockBehaviour.isRedstoneConductor: solid full blocks (the redstone block
// is a signal source, not a conductor)
static int is_conductor(BlockId b) {
    return b != BLOCK_AIR && b != BLOCK_REDSTONE_BLOCK && (mc_block(b)->flags & MCB_OPAQUE) != 0;
}

// isFaceSturdy(UP) for the wire's canSurviveOn: full blocks and trapdoors on top
static int sturdy_top(u16 cell) {
    BlockId b = CELL_ID(cell);
    if (is_trapdoor(b)) return (CELL_STATE(cell) & 2) && !(CELL_STATE(cell) & 1); // top half, closed
    return b != BLOCK_AIR && (mc_block(b)->flags & MCB_OPAQUE) != 0;
}

// FaceAttachedHorizontalDirectionalBlock.getConnectedDirection: the block it
// hangs on (as the direction from that block to the lever)... minecraft's
// getDirectSignal compares it with the receiver -> emitter direction
static int attached_dir(u16 cell) {
    int s = CELL_STATE(cell), face = s / 8, facing = (s / 2) % 4;
    if (face == 0) return D_UP;   // floor
    if (face == 2) return D_DOWN; // ceiling
    return FACING_DIR(facing);    // wall
}

// --- redstone wire connections (RedstoneWireBlock) ---

// shouldConnectTo(state, direction): wire always, signal sources when a side is asked
static int connects_to(u16 cell, int with_dir) {
    BlockId b = CELL_ID(cell);
    if (is_wire(b)) return TRUE;
    if (!with_dir) return FALSE;
    return b == BLOCK_REDSTONE_BLOCK || is_lever_or_button(b) || b == BLOCK_REDSTONE_TORCH || b == BLOCK_REDSTONE_WALL_TORCH;
}

// getConnectingSide: 1 none, 2 side, 4 up
static u8 connecting_side(s32 x, s32 y, s32 z, int d, int can_up) {
    s32 nx = x + dir[d][0], nz = z + dir[d][2];
    u16 rel = cell_at(nx, y, nz);

    if (can_up) {
        int placeable_above = is_trapdoor(CELL_ID(rel)) || sturdy_top(rel);
        if (placeable_above && connects_to(cell_at(nx, y + 1, nz), FALSE)) {
            // relativeState.isFaceSturdy(opposite): full blocks
            if (CELL_ID(rel) != BLOCK_AIR && (mc_block(CELL_ID(rel))->flags & MCB_OPAQUE)) return 4;
            return 2;
        }
    }
    if (!connects_to(rel, TRUE) && (is_conductor(CELL_ID(rel)) || !connects_to(cell_at(nx, y - 1, nz), FALSE))) {
        return 1;
    }
    return 2;
}

void redstone_wire_sides(s32 x, s32 y, s32 z, u16 cell, u8 out[4]) {
    int can_up = !is_conductor(CELL_ID(cell_at(x, y + 1, z)));
    int i, any_ns, any_ew, all_none = TRUE;

    for (i = 0; i < 4; i++) {
        out[i] = connecting_side(x, y, z, wire_side_dir[i], can_up);
        if (out[i] != 1) all_none = FALSE;
    }
    // getConnectionState: a dot stays a dot; otherwise a lone line along one
    // axis, or a cross with nothing around
    if ((CELL_STATE(cell) & 16) && all_none) return;
    any_ns = out[0] != 1 || out[2] != 1;
    any_ew = out[1] != 1 || out[3] != 1;
    if (out[3] == 1 && !any_ns) out[3] = 2;
    if (out[1] == 1 && !any_ns) out[1] = 2;
    if (out[0] == 1 && !any_ew) out[0] = 2;
    if (out[2] == 1 && !any_ew) out[2] = 2;
}

u32 redstone_wire_color(int power) {
    f32 p = power / 15.0f;
    f32 r = p * 0.6f + (p > 0.0f ? 0.4f : 0.3f);
    f32 g = p * p * 0.7f - 0.5f, b = p * p * 0.6f - 0.7f;
    if (g < 0.0f) g = 0.0f;
    if (b < 0.0f) b = 0.0f;
    return ((u32)(r * 255.0f) << 16) | ((u32)(g * 255.0f) << 8) | (u32)(b * 255.0f);
}

int redstone_powered(s32 pos[3]) {
    return best_neighbor_signal(pos[0], pos[1], pos[2]) > 0; // Level.hasNeighborSignal
}

int redstone_model_state(u16 cell) {
    BlockId b = CELL_ID(cell);
    int s = CELL_STATE(cell);
    if (b == BLOCK_FIRE) return 0; // its state is the age
    if (is_trapdoor(b)) return s & 15;
    if (is_wire(b)) return s & 15;
    return s;
}

// --- signals (Block.getSignal / getDirectSignal, d = receiver -> emitter) ---

static int signal_of(s32 x, s32 y, s32 z, u16 cell, int d) {
    BlockId b = CELL_ID(cell);
    int s = CELL_STATE(cell);

    if (b == BLOCK_REDSTONE_BLOCK) return 15;
    if (is_lever_or_button(b)) return (s & 1) ? 15 : 0;
    if (b == BLOCK_REDSTONE_TORCH) return ((s & 1) && d != D_UP) ? 15 : 0;
    if (b == BLOCK_REDSTONE_WALL_TORCH) return ((s & 1) && FACING_DIR(s >> 1) != d) ? 15 : 0;
    if (is_wire(b)) {
        u8 sides[4];
        int power = s & 15, k;
        if (!should_signal || d == D_DOWN || power == 0) return 0;
        if (d == D_UP) return power;
        redstone_wire_sides(x, y, z, cell, sides);
        for (k = 0; k < 4; k++) {
            if (wire_side_dir[k] == (d ^ 1)) return sides[k] != 1 ? power : 0;
        }
    }
    return 0;
}

static int direct_signal_of(s32 x, s32 y, s32 z, u16 cell, int d) {
    BlockId b = CELL_ID(cell);
    int s = CELL_STATE(cell);

    if (is_lever_or_button(b)) return ((s & 1) && attached_dir(cell) == d) ? 15 : 0;
    if (b == BLOCK_REDSTONE_TORCH || b == BLOCK_REDSTONE_WALL_TORCH) return d == D_DOWN ? signal_of(x, y, z, cell, d) : 0;
    if (is_wire(b)) return should_signal ? signal_of(x, y, z, cell, d) : 0;
    return 0;
}

// Level.getDirectSignalTo: the strong power going into a block
static int direct_signal_to(s32 x, s32 y, s32 z) {
    int d, best = 0;
    for (d = 0; d < 6; d++) {
        s32 nx = x + dir[d][0], ny = y + dir[d][1], nz = z + dir[d][2];
        int v = direct_signal_of(nx, ny, nz, cell_at(nx, ny, nz), d);
        if (v > best) best = v;
        if (best >= 15) break;
    }
    return best;
}

// Level.getSignal(pos, d): what pos gives; a conductor passes on its strong power
static int level_signal(s32 x, s32 y, s32 z, int d) {
    u16 cell = cell_at(x, y, z);
    int s = signal_of(x, y, z, cell, d);
    if (is_conductor(CELL_ID(cell))) {
        int t = direct_signal_to(x, y, z);
        if (t > s) s = t;
    }
    return s;
}

// Level.getBestNeighborSignal
static int best_neighbor_signal(s32 x, s32 y, s32 z) {
    int d, best = 0;
    for (d = 0; d < 6; d++) {
        int v = level_signal(x + dir[d][0], y + dir[d][1], z + dir[d][2], d);
        if (v > best) best = v;
        if (best >= 15) break;
    }
    return best;
}

// --- components in the loaded grid ---

#define MAX_PARTS 512
typedef struct {
    s32 pos[3];
    u16 cell;
    u8 target; // wires: solved strength
} Part;
static Part parts[MAX_PARTS];
static int part_count;

static int is_component(BlockId b) {
    int f = shapes_family(b);
    return is_wire(b) || is_lever_or_button(b) || is_trapdoor(b) || b == BLOCK_REDSTONE_TORCH
        || is_torch(b) || is_wall_torch(b)
        || f == FAM_PLANT || f == FAM_CARPET || f == FAM_DOOR || f == FAM_SNOW || f == FAM_LADDER || f == FAM_LANTERN
        || b == BLOCK_REDSTONE_WALL_TORCH || b == BLOCK_REDSTONE_LAMP || b == BLOCK_REDSTONE_BLOCK;
}

static void gather(void) {
    s32 i, n = world_section_count();
    part_count = 0;
    for (i = 0; i < n; i++) {
        Section *s = world_section_at(i);
        s32 k;
        if (s == NULL || s->special_count == 0) continue;
        for (k = 0; k < SECTION_VOLUME && part_count < MAX_PARTS; k++) {
            u16 c = s->blocks[k];
            if (c == BLOCK_AIR || !is_component(CELL_ID(c))) continue;
            parts[part_count].pos[0] = s->cx * SECTION_SIZE + (k % SECTION_SIZE);
            parts[part_count].pos[1] = s->cy * SECTION_SIZE + (k / (SECTION_SIZE * SECTION_SIZE));
            parts[part_count].pos[2] = s->cz * SECTION_SIZE + ((k / SECTION_SIZE) % SECTION_SIZE);
            parts[part_count].cell = c;
            part_count++;
        }
    }
}

static void set_state(s32 pos[3], u16 cell, int state) {
    world_set(pos[0], pos[1], pos[2], MAKE_CELL(CELL_ID(cell), state));
}

// --- scheduled ticks (LevelTicks) ---

#define MAX_TICKS 64
typedef struct {
    int used;
    s32 pos[3];
    BlockId block;
    u32 when;
} Sched;
static Sched sched[MAX_TICKS];

static int scheduled(const s32 pos[3], BlockId b) {
    int i;
    for (i = 0; i < MAX_TICKS; i++) {
        if (sched[i].used && sched[i].block == b && sched[i].pos[0] == pos[0] && sched[i].pos[1] == pos[1]
            && sched[i].pos[2] == pos[2]) return TRUE;
    }
    return FALSE;
}

static void schedule(const s32 pos[3], BlockId b, int delay) {
    int i;
    if (scheduled(pos, b)) return; // scheduleTick doesn't add a second one
    for (i = 0; i < MAX_TICKS; i++) {
        if (!sched[i].used) {
            sched[i].used = TRUE;
            sched[i].pos[0] = pos[0];
            sched[i].pos[1] = pos[1];
            sched[i].pos[2] = pos[2];
            sched[i].block = b;
            sched[i].when = tick_now + delay;
            return;
        }
    }
}

// RedstoneTorchBlock.RECENT_TOGGLES
#define MAX_TOGGLES 32
static struct { s32 pos[3]; u32 when; int used; } toggles[MAX_TOGGLES];

static int toggled_too_often(const s32 pos[3], int add) {
    int i, count = 0;
    if (add) {
        int slot = 0;
        for (i = 0; i < MAX_TOGGLES; i++) {
            if (!toggles[i].used) { slot = i; break; }
            if (toggles[i].when < toggles[slot].when) slot = i;
        }
        toggles[slot].used = TRUE;
        toggles[slot].pos[0] = pos[0];
        toggles[slot].pos[1] = pos[1];
        toggles[slot].pos[2] = pos[2];
        toggles[slot].when = tick_now;
    }
    for (i = 0; i < MAX_TOGGLES; i++) {
        if (toggles[i].used && toggles[i].pos[0] == pos[0] && toggles[i].pos[1] == pos[1] && toggles[i].pos[2] == pos[2]) {
            if (++count >= 8) return TRUE;
        }
    }
    return FALSE;
}

// the torch's input: the block it stands / hangs on
static int torch_powered(s32 pos[3], u16 cell) {
    if (CELL_ID(cell) == BLOCK_REDSTONE_TORCH) {
        return level_signal(pos[0], pos[1] - 1, pos[2], D_DOWN) > 0;
    } else {
        int back = FACING_DIR(CELL_STATE(cell) >> 1) ^ 1; // facing.getOpposite()
        return level_signal(pos[0] + dir[back][0], pos[1] + dir[back][1], pos[2] + dir[back][2], back) > 0;
    }
}

static void run_tick(Sched *t) {
    u16 cell = cell_at(t->pos[0], t->pos[1], t->pos[2]);
    BlockId b = CELL_ID(cell);
    int s = CELL_STATE(cell);

    if (b != t->block) return;
    if (shapes_family(b) == FAM_PLATE) {
        // BasePressurePlateBlock.tick: still pressed -> again in 20 ticks
        if (redstone_plate_pressed(t->pos)) {
            schedule(t->pos, b, 20);
        } else if (s & 1) {
            set_state(t->pos, cell, s & ~1);
            sound_play_cell(stone_type(b) ? SND(BLOCK_STONE_PRESSURE_PLATE_CLICK_OFF) : SND(BLOCK_WOODEN_PRESSURE_PLATE_CLICK_OFF),
                t->pos[0], t->pos[1], t->pos[2], 1.0f, 1.0f);
        }
    } else if (is_lever_or_button(b) && b != BLOCK_LEVER) {
        // ButtonBlock.tick -> checkPressed (no arrows): released
        if (s & 1) {
            set_state(t->pos, cell, s & ~1);
            // ButtonBlock.playSound: BlockSetType's click off
            sound_play_cell(stone_type(b) ? SND(BLOCK_STONE_BUTTON_CLICK_OFF) : SND(BLOCK_WOODEN_BUTTON_CLICK_OFF),
                t->pos[0], t->pos[1], t->pos[2], 1.0f, 1.0f);
        }
    } else if (b == BLOCK_REDSTONE_TORCH || b == BLOCK_REDSTONE_WALL_TORCH) {
        int powered = torch_powered(t->pos, cell);
        int i;
        // RedstoneTorchBlock.tick: forget toggles older than 60 ticks
        for (i = 0; i < MAX_TOGGLES; i++) {
            if (toggles[i].used && tick_now - toggles[i].when > 60) toggles[i].used = FALSE;
        }
        if (s & 1) {
            if (powered) {
                set_state(t->pos, cell, s & ~1);
                if (toggled_too_often(t->pos, TRUE)) schedule(t->pos, b, 160);
            }
        } else if (!powered && !toggled_too_often(t->pos, FALSE)) {
            set_state(t->pos, cell, s | 1);
        }
    } else if (b == BLOCK_REDSTONE_LAMP) {
        // RedstoneLampBlock.tick: off if still unpowered
        if ((s & 1) && best_neighbor_signal(t->pos[0], t->pos[1], t->pos[2]) == 0) set_state(t->pos, cell, 0);
    }
}

// DefaultRedstoneWireEvaluator: every wire's strength, solved together
static void solve_wires(void) {
    int i, pass, changed;

    // the signal from everything but wire (shouldSignal = false)
    should_signal = FALSE;
    for (i = 0; i < part_count; i++) {
        Part *p = &parts[i];
        if (is_wire(CELL_ID(p->cell))) p->target = (u8)best_neighbor_signal(p->pos[0], p->pos[1], p->pos[2]);
    }
    should_signal = TRUE;
    // getIncomingWireSignal: the strongest wire around - 1, until it settles
    for (pass = 0; pass < 64; pass++) {
        changed = FALSE;
        for (i = 0; i < part_count; i++) {
            Part *p = &parts[i];
            s32 x = p->pos[0], y = p->pos[1], z = p->pos[2];
            int above_conductor, d, best = 0, j;
            if (!is_wire(CELL_ID(p->cell))) continue;
            above_conductor = is_conductor(CELL_ID(cell_at(x, y + 1, z)));
            for (d = 2; d < 6; d++) {
                s32 nx = x + dir[d][0], nz = z + dir[d][2];
                BlockId nb = CELL_ID(cell_at(nx, y, nz));
                for (j = 0; j < part_count; j++) {
                    Part *q = &parts[j];
                    s32 qy = q->pos[1];
                    if (!is_wire(CELL_ID(q->cell)) || q->pos[0] != nx || q->pos[2] != nz) continue;
                    if (qy == y || (qy == y + 1 && is_conductor(nb) && !above_conductor) || (qy == y - 1 && !is_conductor(nb))) {
                        if (q->target > best) best = q->target;
                    }
                }
            }
            if (best - 1 > p->target) {
                p->target = (u8)(best - 1);
                changed = TRUE;
            }
        }
        if (!changed) break;
    }
    for (i = 0; i < part_count; i++) {
        Part *p = &parts[i];
        if (is_wire(CELL_ID(p->cell)) && (CELL_STATE(p->cell) & 15) != p->target) {
            int st = (CELL_STATE(p->cell) & 16) | p->target;
            set_state(p->pos, p->cell, st);
            p->cell = MAKE_CELL(CELL_ID(p->cell), st);
        }
    }
}

// a sturdy face to hang on: full blocks, or bk's ground
static int support_at(s32 x, s32 y, s32 z) {
    BlockId b = CELL_ID(cell_at(x, y, z));
    if (b != BLOCK_AIR) return (mc_block(b)->flags & MCB_OPAQUE) != 0;
    return !terrain_is_carved(x, y, z) && terrain_cell_in_ground(x, y, z);
}

// canSurvive: what each block needs, -1 = nothing (trapdoors, lamps...)
static int needs_support(u16 cell, s32 pos[3], s32 out[3]) {
    BlockId b = CELL_ID(cell);
    int s = CELL_STATE(cell), d = -1, k;
    if (is_wire(b) || b == BLOCK_REDSTONE_TORCH || is_torch(b)) {
        d = D_DOWN; // RedstoneWireBlock / TorchBlock: the block below
    } else if (b == BLOCK_REDSTONE_WALL_TORCH) {
        d = FACING_DIR(s >> 1) ^ 1; // WallTorchBlock: the wall behind
    } else if (is_wall_torch(b)) {
        d = FACING_DIR(s) ^ 1;
    } else if (is_lever_or_button(b)) {
        d = attached_dir(cell) ^ 1; // FaceAttachedHorizontalDirectionalBlock.canSurvive
    } else {
        switch (shapes_family(b)) {
            case FAM_PLANT:   // VegetationBlock.canSurvive
            case FAM_CARPET:  // CarpetBlock: not over air
            case FAM_SNOW:    // SnowLayerBlock
                d = D_DOWN;
                break;
            case FAM_DOOR:    // DoorBlock: the lower half on a sturdy top (the upper follows it)
                if (!((s >> 2) & 1)) d = D_DOWN;
                break;
            case FAM_LADDER:  // LadderBlock: the wall behind it
                d = FACING_DIR(s & 3) ^ 1;
                break;
            case FAM_LANTERN: // LanternBlock: hanging from above, or standing
                d = (s & 1) ? D_UP : D_DOWN;
                break;
        }
    }
    if (d < 0) return FALSE;
    for (k = 0; k < 3; k++) out[k] = pos[k] + dir[d][k];
    return TRUE;
}

// steve on a plate (BasePressurePlateBlock.getSignalStrength: an entity in
// its touch box, the bottom quarter of the cell)
int redstone_plate_pressed(const s32 pos[3]) {
    f32 *p = gSteve.pos;
    f32 fx = p[0] / MC_BLOCK - pos[0], fz = p[2] / MC_BLOCK - pos[2], fy = p[1] / MC_BLOCK - pos[1];
    f32 hw = 0.3f; // (steve's half width)
    if (GM_SPECTATING()) return FALSE;
    return fx + hw > 0.125f && fx - hw < 0.875f && fz + hw > 0.125f && fz - hw < 0.875f && fy > -0.01f && fy < 0.25f;
}

static void plates_check(void) {
    s32 c[3];
    u16 cell;
    c[0] = world_to_block(gSteve.pos[0]);
    c[1] = world_to_block(gSteve.pos[1] + 1.0f);
    c[2] = world_to_block(gSteve.pos[2]);
    if (world_get(c[0], c[1], c[2]) == BLOCK_AIR) return;
    cell = cell_at(c[0], c[1], c[2]);
    if (shapes_family(CELL_ID(cell)) == FAM_PLATE && !(CELL_STATE(cell) & 1) && redstone_plate_pressed(c)) {
        // checkPressed: powered, the click, a recheck in 20 ticks
        set_state(c, cell, CELL_STATE(cell) | 1);
        sound_play_cell(stone_type(CELL_ID(cell)) ? SND(BLOCK_STONE_PRESSURE_PLATE_CLICK_ON) : SND(BLOCK_WOODEN_PRESSURE_PLATE_CLICK_ON),
            c[0], c[1], c[2], 1.0f, 1.0f);
        schedule(c, CELL_ID(cell), 20);
    }
}

void redstone_tick(void) {
    int i;

    tick_now++;
    plates_check();
    for (i = 0; i < MAX_TICKS; i++) {
        if (sched[i].used && (s32)(tick_now - sched[i].when) >= 0) {
            sched[i].used = FALSE;
            run_tick(&sched[i]);
        }
    }
    {
        // rescan the grid only when something in it changed
        static u32 seen = 0xFFFFFFFF;
        int k;
        if (seen != world_change_count) {
            gather();
            seen = world_change_count;
        } else {
            for (k = 0; k < part_count; k++) parts[k].cell = cell_at(parts[k].pos[0], parts[k].pos[1], parts[k].pos[2]);
        }
    }
    if (part_count == 0) return;
    // updateShape: blocks that lost what they hang on pop off with their drops
    for (i = 0; i < part_count; i++) {
        Part *p = &parts[i];
        s32 sup[3];
        if (needs_support(p->cell, p->pos, sup) && !support_at(sup[0], sup[1], sup[2])) {
            surv_explode_block(p->pos, CELL_ID(p->cell), 1.0f);
            p->cell = BLOCK_AIR;
        }
    }
    solve_wires();

    // neighborChanged for the rest
    for (i = 0; i < part_count; i++) {
        Part *p = &parts[i];
        u16 cell = cell_at(p->pos[0], p->pos[1], p->pos[2]);
        BlockId b = CELL_ID(cell);
        int s = CELL_STATE(cell);

        if (b == BLOCK_REDSTONE_TORCH || b == BLOCK_REDSTONE_WALL_TORCH) {
            if ((s & 1) == torch_powered(p->pos, cell)) schedule(p->pos, b, 2);
        } else if (b == BLOCK_REDSTONE_LAMP) {
            int powered = best_neighbor_signal(p->pos[0], p->pos[1], p->pos[2]) > 0;
            if ((s & 1) && !powered) schedule(p->pos, b, 4);
            else if (!(s & 1) && powered) set_state(p->pos, cell, 1);
        } else if (is_trapdoor(b)) {
            int powered = best_neighbor_signal(p->pos[0], p->pos[1], p->pos[2]) > 0;
            if (powered != ((s & 16) != 0)) {
                int ns = (s & ~(16 | 1)) | (powered ? 16 | 1 : 0);
                set_state(p->pos, cell, ns);
            }
        }
    }
}

void redstone_clear(void) {
    int i;
    for (i = 0; i < MAX_TICKS; i++) sched[i].used = FALSE;
    for (i = 0; i < MAX_TOGGLES; i++) toggles[i].used = FALSE;
}

// --- items and placement ---

BlockId redstone_item_block(u16 item) {
    int i;
    for (i = 0; i < MC_ITEM_PLACE_COUNT; i++) {
        if (mc_item_places[i][0] == item) return mc_item_places[i][1];
    }
    return (item < MC_BLOCK_COUNT) ? item : BLOCK_AIR;
}

static BlockId wall_variant(BlockId b) {
    int i;
    for (i = 0; i < MC_ITEM_PLACE_COUNT; i++) {
        if (mc_item_places[i][1] == b) return mc_item_places[i][2];
    }
    return BLOCK_AIR;
}

// Direction -> facing index (south, north, east, west), -1 for up / down
static int facing_of(int d) {
    return (d >= 2) ? d - 2 : -1;
}

static int normal_dir(const s32 n[3]) {
    int d;
    for (d = 0; d < 6; d++) {
        if (dir[d][0] == n[0] && dir[d][1] == n[1] && dir[d][2] == n[2]) return d;
    }
    return D_UP;
}

// context.getHorizontalDirection: the way steve looks
static int look_facing(const f32 look[3]) {
    if (look[0] * look[0] > look[2] * look[2]) return look[0] > 0.0f ? 2 : 3;
    return look[2] > 0.0f ? 0 : 1;
}

int redstone_place(BlockId *block, int *state, const s32 normal[3], f32 hit_y, const f32 look[3], s32 pos[3]) {
    BlockId b = *block;
    int clicked = normal_dir(normal); // the clicked face (pointing into the new cell)

    if (is_lever_or_button(b)) {
        // FaceAttachedHorizontalDirectionalBlock.getStateForPlacement (on the clicked face)
        if (clicked == D_UP) *state = 0 * 8 + look_facing(look) * 2;
        else if (clicked == D_DOWN) *state = 2 * 8 + look_facing(look) * 2;
        else *state = 1 * 8 + facing_of(clicked) * 2;
        return TRUE;
    }
    if (b == BLOCK_REDSTONE_TORCH || is_torch(b)) {
        // StandingAndWallBlockItem: standing on a top face, the wall torch on a side
        if (clicked == D_UP) {
            *state = (b == BLOCK_REDSTONE_TORCH) ? 1 : 0;
        } else if (clicked == D_DOWN) {
            return FALSE;
        } else {
            *block = wall_variant(b);
            *state = (b == BLOCK_REDSTONE_TORCH) ? facing_of(clicked) * 2 + 1 : facing_of(clicked);
        }
        return TRUE;
    }
    if (is_trapdoor(b)) {
        // TrapDoorBlock.getStateForPlacement
        int facing, half;
        if (clicked >= 2) {
            facing = facing_of(clicked);
            half = hit_y > 0.5f ? 1 : 0;
        } else {
            facing = look_facing(look) ^ 1; // getHorizontalDirection().getOpposite()
            half = clicked == D_UP ? 0 : 1;
        }
        *state = facing * 4 + half * 2;
        if (best_neighbor_signal(pos[0], pos[1], pos[2]) > 0) *state |= 16 | 1;
        return TRUE;
    }
    if (b == BLOCK_REDSTONE_LAMP) {
        *state = best_neighbor_signal(pos[0], pos[1], pos[2]) > 0 ? 1 : 0;
        return TRUE;
    }
    if (is_wire(b)) {
        *state = 0; // crossState with power 0 (solved next tick)
        return TRUE;
    }
    *state = 0;
    return TRUE;
}

int redstone_use(s32 pos[3]) {
    u16 cell = cell_at(pos[0], pos[1], pos[2]);
    BlockId b = CELL_ID(cell);
    int s = CELL_STATE(cell);

    if (b == BLOCK_LEVER) {
        set_state(pos, cell, s ^ 1); // LeverBlock.pull
        sound_play_cell(SND(BLOCK_LEVER_CLICK), pos[0], pos[1], pos[2], 0.3f, (s & 1) ? 0.5f : 0.6f); // LeverBlock.playSound
        return TRUE;
    }
    if (shapes_family(b) == FAM_BUTTON) {
        if (s & 1) return TRUE; // CONSUME: already pressed
        set_state(pos, cell, s | 1); // ButtonBlock.press
        sound_play_cell(stone_type(b) ? SND(BLOCK_STONE_BUTTON_CLICK_ON) : SND(BLOCK_WOODEN_BUTTON_CLICK_ON),
            pos[0], pos[1], pos[2], 1.0f, 1.0f);
        schedule(pos, b, stone_type(b) ? 20 : 30);
        return TRUE;
    }
    if (is_trapdoor(b) && !name_has(b, "iron")) {
        set_state(pos, cell, s ^ 1); // TrapDoorBlock.toggle (iron: BlockSetType.IRON can't open by hand)
        sound_play_cell((s & 1) ? SND(BLOCK_WOODEN_TRAPDOOR_CLOSE) : SND(BLOCK_WOODEN_TRAPDOOR_OPEN), pos[0], pos[1], pos[2], 1.0f,
            sound_random() * 0.1f + 0.9f); // TrapDoorBlock.playSound
        return TRUE;
    }
    if (is_wire(b)) {
        // RedstoneWireBlock.useWithoutItem: a lone cross <-> dot
        u8 sides[4];
        int k, cross = TRUE, dot = TRUE;
        redstone_wire_sides(pos[0], pos[1], pos[2], cell, sides);
        for (k = 0; k < 4; k++) {
            if (sides[k] != 2) cross = FALSE;
            if (sides[k] != 1) dot = FALSE;
        }
        if (cross || dot) {
            set_state(pos, cell, s ^ 16);
            return TRUE;
        }
    }
    return FALSE;
}
