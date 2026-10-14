// the f3 debug screen, ported from DebugScreenOverlay + the debug entries
// (26.3). minecraft's default profile, plus the targeted block and fluid
// (DebugEntryLookingAt), in minecraft's order: entries sorted by id, priority
// lines alternating left / right, plain lines and then groups split in half
// between the two sides. each line gets the translucent gray box behind it.
// steve mode only: bk's own mode has no f3 screen.
//
// entries that only make sense for java minecraft (memory, java / cpu specs,
// texture filtering, packets) are left out.

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "survival.h"
#include "fluids.h"
#include "gui.h"
#include "mouse.h"
#include "options.h"
#include "f3.h"

extern u32 gFps; // main.c: game frames in the last second

#define MAX_LINES 32
#define LINE_LEN 96

typedef struct {
    char text[MAX_LINES][LINE_LEN];
    u8 underline[MAX_LINES]; // ChatFormatting.UNDERLINE on the line
    int n;
} Lines;

static Lines left, right;

// --- a tiny String.format ---

typedef struct {
    char *p, *end;
} Buf;

static void put_s(Buf *b, const char *s) {
    while (*s && b->p < b->end) *b->p++ = *s++;
    *b->p = '\0';
}

static void put_i(Buf *b, s32 v, int min_digits) {
    char tmp[12];
    int len = 0;
    u32 u = (v < 0) ? (u32)-v : (u32)v;

    do {
        tmp[len++] = '0' + (u % 10);
        u /= 10;
    } while (u != 0 || len < min_digits);
    if (v < 0 && b->p < b->end) *b->p++ = '-';
    while (len > 0 && b->p < b->end) *b->p++ = tmp[--len];
    *b->p = '\0';
}

// %.<decimals>f, rounded half away from zero
static void put_f(Buf *b, f32 v, int decimals) {
    s32 scale = 1, whole;
    u32 frac, total;
    int i, neg = v < 0.0f;

    for (i = 0; i < decimals; i++) scale *= 10;
    if (neg) v = -v;
    total = (u32)(v * scale + 0.5f);
    whole = (s32)(total / (u32)scale);
    frac = total % (u32)scale;
    if (neg && total != 0 && b->p < b->end) *b->p++ = '-';
    put_i(b, whole, 1);
    if (decimals > 0) {
        put_s(b, ".");
        put_i(b, (s32)frac, decimals);
    }
}

static Buf line_buf(Lines *l, int underline) {
    Buf b;

    if (l->n >= MAX_LINES) l->n = MAX_LINES - 1; // overflow: keep overwriting the last
    l->underline[l->n] = (u8)underline;
    b.p = l->text[l->n];
    b.end = b.p + LINE_LEN - 1;
    *b.p = '\0';
    l->n++;
    return b;
}

static void add_empty(Lines *l) {
    line_buf(l, FALSE);
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

// --- groups (DebugScreenDisplayer.addToGroup) ---

#define MAX_GROUPS 4
static Lines groups[MAX_GROUPS];
static int group_count;

static Lines *new_group(void) {
    Lines *g = &groups[group_count++];
    g->n = 0;
    return g;
}

// Mth.wrapDegrees
static f32 wrap_degrees(f32 d) {
    while (d >= 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

static s32 floor_i(f32 v) {
    s32 i = (s32)v;
    return ((f32)i > v) ? i - 1 : i;
}

static const char *const dir_names[6] = { "up", "down", "south", "north", "east", "west" };

// the properties of the mod's model blocks, mc_models.MODEL_BLOCKS: a model
// block's state index runs over these with the last property fastest
typedef struct {
    const char *name;
    int count;
    const char *prop[3];
    const char *const *values[3];
    int nvalues[3];
} ModelProps;

static const char *const v_face[] = { "floor", "wall", "ceiling" };
static const char *const v_facing[] = { "south", "north", "east", "west" };
static const char *const v_bool[] = { "false", "true" };
static const char *const v_half[] = { "bottom", "top" };
static const char *const v_power[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15" };

static const ModelProps model_props[] = {
    { "lever", 3, { "face", "facing", "powered" }, { v_face, v_facing, v_bool }, { 3, 4, 2 } },
    { "stone_button", 3, { "face", "facing", "powered" }, { v_face, v_facing, v_bool }, { 3, 4, 2 } },
    { "oak_button", 3, { "face", "facing", "powered" }, { v_face, v_facing, v_bool }, { 3, 4, 2 } },
    { "redstone_torch", 1, { "lit" }, { v_bool }, { 2 } },
    { "redstone_wall_torch", 2, { "facing", "lit" }, { v_facing, v_bool }, { 4, 2 } },
    { "redstone_lamp", 1, { "lit" }, { v_bool }, { 2 } },
    { "oak_trapdoor", 3, { "facing", "half", "open" }, { v_facing, v_half, v_bool }, { 4, 2, 2 } },
    { "iron_trapdoor", 3, { "facing", "half", "open" }, { v_facing, v_half, v_bool }, { 4, 2, 2 } },
    { "redstone_wire", 1, { "power" }, { v_power }, { 16 } },
    { "wall_torch", 1, { "facing" }, { v_facing }, { 4 } },
};

// DebugEntryLookingAtState.getPropertyValueString: "name: value", booleans
// green / red. the color is the line's first byte (1 green, 2 red, 0 plain)
static void add_prop(Lines *g, const char *name, const char *value) {
    Buf b = line_buf(g, FALSE);
    char color = streq(value, "true") ? 1 : (streq(value, "false") ? 2 : 0);

    if (color) {
        *b.p++ = color;
        *b.p = '\0';
    }
    put_s(&b, name);
    put_s(&b, ": ");
    put_s(&b, value);
}

static void add_state_props(Lines *g, u16 cell) {
    BlockId id = CELL_ID(cell);
    int st = CELL_STATE(cell);
    const char *reg = mc_registry_names[id];
    u32 i;

    switch (mc_block_place[id]) {
        case PLACE_HFACING:
        case PLACE_FACING:
        case PLACE_FACING_TOWARD:
            add_prop(g, "facing", dir_names[st < 6 ? st : 0]);
            return;
        case PLACE_AXIS:
            add_prop(g, "axis", st == 2 ? "x" : (st == 1 ? "z" : "y"));
            return;
        case PLACE_FLUID:
            add_prop(g, "level", v_power[st & 15]);
            return;
        case PLACE_MODEL:
            for (i = 0; i < sizeof(model_props) / sizeof(model_props[0]); i++) {
                const ModelProps *mp = &model_props[i];
                int k, stride = 1;

                if (!streq(mp->name, reg)) continue;
                for (k = mp->count - 1; k >= 0; k--) {
                    stride *= mp->nvalues[k];
                }
                for (k = 0; k < mp->count; k++) {
                    stride /= mp->nvalues[k];
                    add_prop(g, mp->prop[k], mp->values[k][(st / stride) % mp->nvalues[k]]);
                }
                return;
            }
            return;
    }
}

// DebugEntryLookingAtState.extractInfo
static void looking_at(Lines *g, const char *prefix, const s32 cell[3], const char *reg) {
    Buf b = line_buf(g, TRUE);

    put_s(&b, prefix);
    put_s(&b, ": ");
    put_i(&b, cell[0], 1);
    put_s(&b, ", ");
    put_i(&b, cell[1], 1);
    put_s(&b, ", ");
    put_i(&b, cell[2], 1);
    b = line_buf(g, FALSE);
    put_s(&b, "minecraft:");
    put_s(&b, reg);
}

static void build(void) {
    Lines regular;
    Buf b;
    int i, mid;
    f32 x = gSteve.pos[0] / MC_BLOCK, y = gSteve.pos[1] / MC_BLOCK, z = gSteve.pos[2] / MC_BLOCK;
    s32 bx = floor_i(x), by = floor_i(y), bz = floor_i(z);

    left.n = right.n = regular.n = 0;
    group_count = 0;

    // sorted by id: 3d_crosshair, fps, game_version, looking_at_block_state,
    // looking_at_fluid_state, player_position, player_section_position,
    // system_specs, tps

    // DebugEntryFps (priority: left)
    b = line_buf(&left, FALSE);
    put_i(&b, (s32)gFps, 1);
    put_s(&b, " fps");

    // DebugEntryVersion (priority: right, the left side has more)
    b = line_buf(&right, FALSE);
    put_s(&b, "Minecraft 26.3 (Banjo: Recompiled/steve)");

    // DebugEntryLookingAt.BlockStateInfo
    {
        Lines *g = new_group();
        if (gF3Target.block_hit) {
            looking_at(g, "Targeted Block", gF3Target.cell, mc_registry_names[CELL_ID(gF3Target.cell_value)]);
            add_state_props(g, gF3Target.cell_value);
        }
    }
    // DebugEntryLookingAt.FluidStateInfo: a FluidState (water / flowing_water)
    // with falling, and level on flowing fluids
    {
        Lines *g = new_group();
        if (gF3Target.fluid_hit) {
            u16 c = gF3Target.fcell_value;
            int lvl = CELL_STATE(c), lava = fluid_type_at(gF3Target.fcell[0], gF3Target.fcell[1], gF3Target.fcell[2]) == FLUID_LAVA;
            const char *reg = (lvl == 0) ? (lava ? "lava" : "water") : (lava ? "flowing_lava" : "flowing_water");

            looking_at(g, "Targeted Fluid", gF3Target.fcell, reg);
            add_prop(g, "falling", lvl >= 8 ? "true" : "false");
            if (lvl != 0) add_prop(g, "level", v_power[lvl >= 8 ? 8 : 8 - lvl]);
        }
    }
    // DebugEntryPosition + DebugEntrySectionPosition
    {
        Lines *g = new_group();
        f32 yrot = wrap_degrees(-gSteve.yaw), xrot = wrap_degrees(-gSteve.pitch);
        // Direction.fromYRot: 2d data value 0 south, 1 west, 2 north, 3 east
        static const char *const dir2d[4] = { "south", "west", "north", "east" };
        static const char *const towards[4] = { "Towards positive Z", "Towards negative X", "Towards negative Z", "Towards positive X" };
        int d = floor_i(yrot / 90.0f + 0.5f) & 3;
        s32 cx = bx >> 4, cy = by >> 4, cz = bz >> 4;

        b = line_buf(g, FALSE);
        put_s(&b, "XYZ: ");
        put_f(&b, x, 3);
        put_s(&b, " / ");
        put_f(&b, y, 5);
        put_s(&b, " / ");
        put_f(&b, z, 3);
        b = line_buf(g, FALSE);
        put_s(&b, "Block: ");
        put_i(&b, bx, 1);
        put_s(&b, " ");
        put_i(&b, by, 1);
        put_s(&b, " ");
        put_i(&b, bz, 1);
        b = line_buf(g, FALSE);
        put_s(&b, "Chunk: ");
        put_i(&b, cx, 1);
        put_s(&b, " ");
        put_i(&b, cy, 1);
        put_s(&b, " ");
        put_i(&b, cz, 1);
        put_s(&b, " [");
        put_i(&b, cx & 31, 1);
        put_s(&b, " ");
        put_i(&b, cz & 31, 1);
        put_s(&b, " in r.");
        put_i(&b, cx >> 5, 1);
        put_s(&b, ".");
        put_i(&b, cz >> 5, 1);
        put_s(&b, ".mca]");
        b = line_buf(g, FALSE);
        put_s(&b, "Facing: ");
        put_s(&b, dir2d[d]);
        put_s(&b, " (");
        put_s(&b, towards[d]);
        put_s(&b, ") (");
        put_f(&b, yrot, 1);
        put_s(&b, " / ");
        put_f(&b, xrot, 1);
        put_s(&b, ")");
        // the dimension: bk's map
        b = line_buf(g, FALSE);
        put_s(&b, "banjo:map_");
        put_i(&b, map_get(), 1);
        put_s(&b, " FC: 0");
        b = line_buf(g, FALSE);
        put_s(&b, "Section-relative: ");
        put_i(&b, bx & 15, 2);
        put_s(&b, " ");
        put_i(&b, by & 15, 2);
        put_s(&b, " ");
        put_i(&b, bz & 15, 2);
    }
    // DebugEntrySystemSpecs: only the display line applies here
    {
        Lines *g = new_group();
        b = line_buf(g, FALSE);
        put_s(&b, "Display: ");
        put_i(&b, gMouse.window_w, 1);
        put_s(&b, "x");
        put_i(&b, gMouse.window_h, 1);
        put_s(&b, " (RT64)");
    }
    // DebugEntryTps: the integrated server's smoothed tick time
    b = line_buf(&regular, FALSE);
    put_s(&b, "Integrated server @ ");
    put_f(&b, gSmoothedTickMs, 1);
    put_s(&b, "/50.0 ms, 0 tx, 0 rx");

    // DebugScreenOverlay.extractRenderState: the layout
    if (left.n > 0) add_empty(&left);
    if (right.n > 0) add_empty(&right);
    if (regular.n > 0) {
        mid = (regular.n + 1) / 2;
        for (i = 0; i < regular.n; i++) {
            Lines *dst = (i < mid) ? &left : &right;
            Buf o = line_buf(dst, regular.underline[i]);
            put_s(&o, regular.text[i]);
        }
        add_empty(&left);
        if (mid < regular.n) add_empty(&right);
    }
    mid = (group_count + 1) / 2;
    for (i = 0; i < group_count; i++) {
        Lines *dst = (i < mid) ? &left : &right;
        int k;

        if (groups[i].n == 0) continue;
        for (k = 0; k < groups[i].n; k++) {
            Buf o = line_buf(dst, groups[i].underline[k]);
            put_s(&o, groups[i].text[k]);
        }
        add_empty(dst);
    }
}

// DebugScreenOverlay.extractLines: the boxes first, then the text
static void draw_lines(Gfx **gfx, const Lines *l, int align_left, s32 screen_w) {
    const s32 height = 9;
    int i;

    for (i = 0; i < l->n; i++) {
        const char *s = l->text[i];
        s32 width, x, top;

        if (*s == 1 || *s == 2) s++;
        if (*s == '\0') continue;
        width = gui_text_width(s);
        x = align_left ? 2 : screen_w - 2 - width;
        top = 2 + height * i;
        gui_fill(gfx, x - 1, top - 1, width + 2, height, 0x90505050);
    }
    for (i = 0; i < l->n; i++) {
        const char *s = l->text[i];
        u32 rgb = 0xE0E0E0;
        s32 width, x, top;

        // ChatFormatting.GREEN / RED
        if (*s == 1) rgb = 0x55FF55, s++;
        else if (*s == 2) rgb = 0xFF5555, s++;
        if (*s == '\0') continue;
        width = gui_text_width(s);
        x = align_left ? 2 : screen_w - 2 - width;
        top = 2 + height * i;
        gui_text(gfx, s, x, top, rgb, FALSE);
        if (l->underline[i]) {
            // Font: the underline effect, one pixel under the glyphs
            gui_fill(gfx, x - 1, top + height - 1, width + 1, 1, 0xFF000000 | rgb);
        }
    }
}

void f3_draw(Gfx **gfx, s32 w, s32 h) {
    (void)h;
    if (!gShowDebug || !gSteve.enabled) {
        return;
    }
    build();
    draw_lines(gfx, &left, TRUE, w);
    draw_lines(gfx, &right, FALSE, w);
}

// DebugCrosshairRenderer: the 3d crosshair (on in the default profile, it
// replaces the crosshair sprite and attack indicator, Hud.extractCrosshair).
// three axes one unit in front of the camera, rotated with the camera,
// scaled 0.01 * gui scale: black 4 px outlines, then red x, green y, blue z
// at 2 px. projected here and drawn in the gui pass, far axes first
int f3_crosshair_3d(void) {
    return gShowDebug && gSteve.enabled;
}

void f3_draw_crosshair(Gfx **gfx, s32 w, s32 h) {
    static const u32 colors[3] = { 0xFFFF0000, 0xFF00FF00, 0xFF7F7FFF };
    f32 yr = -gSteve.yaw, xr = -gSteve.pitch;
    f32 sy = ml_sin_deg(yr), cy = ml_cos_deg(yr), sx = ml_sin_deg(xr), cx = ml_cos_deg(xr);
    f32 fov = option_fov() * steve_lerp(gSteve.o_fov_mod, gSteve.fov_mod, steve_partial_tick());
    f32 k = (f32)h * 0.5f * ml_cos_deg(fov * 0.5f) / ml_sin_deg(fov * 0.5f); // gui px per unit at distance 1
    f32 sc = 0.01f * gGui.scale, ox = w * 0.5f, oy = h * 0.5f;
    f32 px[3], py[3], depth[3];
    int order[3] = { 0, 1, 2 };
    int a, i, j;

    for (a = 0; a < 3; a++) {
        // scale(-s, s, -s), rotateY(yRot), rotateX(xRot), translate(0, 0, -1)
        f32 p0 = (a == 0) ? -sc : 0.0f, p1 = (a == 1) ? sc : 0.0f, p2 = (a == 2) ? -sc : 0.0f;
        f32 x1 = cy * p0 + sy * p2, z1 = -sy * p0 + cy * p2;
        f32 y2 = cx * p1 - sx * z1, z2 = sx * p1 + cx * z1;
        f32 dist = 1.0f - z2;

        px[a] = ox + x1 / dist * k;
        py[a] = oy - y2 / dist * k;
        depth[a] = dist;
    }
    for (i = 0; i < 2; i++) {
        for (j = i + 1; j < 3; j++) {
            if (depth[order[j]] > depth[order[i]]) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
    for (i = 0; i < 3; i++) {
        a = order[i];
        gui_line(gfx, ox, oy, px[a], py[a], 4.0f / gGui.scale, 0xFF000000);
        gui_line(gfx, ox, oy, px[a], py[a], 2.0f / gGui.scale, colors[a]);
    }
}
