// advancements: progress (PlayerAdvancements / AdvancementProgress for the
// criteria the mod can tell apart: inventory_changed, consume_item,
// player_killed_entity, entity_killed_player), which ones the player gets to
// see (AdvancementVisibilityEvaluator), the toasts (ToastManager,
// AdvancementToast, RecipeToast) and the screen L opens (AdvancementsScreen,
// AdvancementTab, AdvancementWidget, AdvancementTabType). the data comes
// from the jar (tools/mc_advancements.py).

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "steve.h"
#include "survival.h"
#include "items.h"
#include "gui.h"
#include "mouse.h"
#include "screen.h"
#include "recipebook.h"
#include "advancements.h"
#include "sound.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

extern u64 osGetTime(void);

#ifdef MC_HAVE_TEXTURES

static u8 crit_done[(MC_ADV_CRIT_COUNT + 7) / 8];
static u8 adv_done[(MC_ADV_COUNT + 7) / 8];
static u8 visible[MC_ADV_COUNT]; // sent to the client (the evaluator)
static int vis_dirty = TRUE;
static u32 inv_hash; // inventory_changed: only when it did

// Mth.floor
static s32 mth_floor(f32 v) {
    s32 i = (s32)v;
    return v < (f32)i ? i - 1 : i;
}

static int bit(const u8 *b, int i) { return (b[i >> 3] >> (i & 7)) & 1; }
static void set_bit(u8 *b, int i) { b[i >> 3] |= (u8)(1 << (i & 7)); }

static int ing_has(const McIngredient *ing, ItemId id) {
    int i;
    for (i = 0; i < ing->count; i++) if (mc_ingredient_ids[ing->offset + i] == id) return TRUE;
    return FALSE;
}

// Util.getMillis from the 46.875 MHz counter, by 32 bit steps (no 64 bit
// division in a mod; called every frame, far under the 91 s wrap)
static u32 millis(void) {
    static u32 last, acc, ms;
    u32 now = (u32)osGetTime();
    acc += now - last;
    last = now;
    ms += acc / 46875u;
    acc %= 46875u;
    return ms;
}

static void toast_add_advancement(int a);

// --- AdvancementProgress ---

static int crit_index_done(int a, int k) {
    return bit(crit_done, mc_advs[a].crit_first + k);
}

// countCompletedRequirements: groups with a done criterion
static int groups_done(int a) {
    const McAdvancement *ad = &mc_advs[a];
    int g, k, n = 0;
    for (g = 0; g < ad->req_count; g++) {
        const McIngredient *grp = &mc_adv_req_groups[ad->req_first + g];
        for (k = 0; k < grp->count; k++) {
            if (crit_index_done(a, mc_adv_req_items[grp->offset + k])) { n++; break; }
        }
    }
    return n;
}

// isDone: every requirement group has a done criterion
static int requirements_met(int a) {
    return mc_advs[a].req_count > 0 && groups_done(a) == mc_advs[a].req_count;
}

static f32 get_percent(int a) {
    if (mc_advs[a].crit_count == 0 || mc_advs[a].req_count == 0) return 0.0f;
    return (f32)groups_done(a) / (f32)mc_advs[a].req_count;
}

// PlayerAdvancements.award
static void award(int a, int k) {
    int c = mc_advs[a].crit_first + k;
    if (bit(crit_done, c) || bit(adv_done, a)) return;
    set_bit(crit_done, c);
    vis_dirty = TRUE;
    if (requirements_met(a)) {
        set_bit(adv_done, a);
        recomp_printf("[mc] advancement: %s\n", mc_adv_names[a]);
        // display.shouldShowToast (the chat announcement has no chat here)
        if ((mc_advs[a].flags & 5) == 5) toast_add_advancement(a);
    }
}

static int pred_matches_inventory(const McIngredient *pred) {
    int i;
    for (i = 0; i < INV_TOTAL; i++) {
        const ItemStack *st = &gSurv.inv[i];
        if (st->count > 0 && ing_has(pred, st->id)) return TRUE;
    }
    return FALSE;
}

static void toast_add_recipe(ItemId item);

// InventoryChangeTrigger: every item predicate matched by something held
void advancements_tick(void) {
    int a, k, p;
    u32 h = 2166136261u;
    ItemId r;

    for (a = 0; a < INV_TOTAL; a++) h = (h ^ (gSurv.inv[a].id | (gSurv.inv[a].count << 16))) * 16777619u;
    if (h != inv_hash) {
        inv_hash = h;
        for (a = 0; a < MC_ADV_COUNT; a++) {
            const McAdvancement *ad = &mc_advs[a];
            if (bit(adv_done, a)) continue;
            for (k = 0; k < ad->crit_count; k++) {
                const McAdvCrit *c = &mc_adv_crits[ad->crit_first + k];
                int all = TRUE;
                if (c->trigger != 1 || crit_index_done(a, k)) continue;
                for (p = 0; p < c->pred_count; p++) {
                    if (!pred_matches_inventory(&mc_adv_preds[c->pred_first + p])) { all = FALSE; break; }
                }
                if (all) award(a, k);
            }
        }
    }
    // RecipeToast.addOrUpdate for each recipe the book just learned
    while ((r = recipebook_take_new_unlock()) != 0) toast_add_recipe(r);
}

static void fire(int trigger, ItemId item, int mob) {
    int a, k;
    for (a = 0; a < MC_ADV_COUNT; a++) {
        const McAdvancement *ad = &mc_advs[a];
        if (bit(adv_done, a)) continue;
        for (k = 0; k < ad->crit_count; k++) {
            const McAdvCrit *c = &mc_adv_crits[ad->crit_first + k];
            if (c->trigger != trigger || crit_index_done(a, k)) continue;
            // KilledTrigger: the entity predicate (pred_first = the mobs it takes)
            if ((trigger == 3 || trigger == 4) && !(c->pred_first & mob)) continue;
            if (trigger == 2 && c->pred_count > 0 && (item == 0 || !ing_has(&mc_adv_preds[c->pred_first], item))) continue;
            award(a, k);
        }
    }
}

void advancements_consumed(ItemId item) { fire(2, item, 0); }      // ConsumeItemTrigger
void advancements_killed_mob(int mob) { fire(3, 0, mob); }        // KilledTrigger (player_killed_entity)
void advancements_killed_by_mob(int mob) { fire(4, 0, mob); }     // KilledTrigger (entity_killed_player)

// --- AdvancementVisibilityEvaluator ---

enum { RULE_SHOW, RULE_HIDE, RULE_NO_CHANGE };
static u8 rule_stack[MC_ADV_COUNT + 4];

static int eval_rule(int a) {
    if (!(mc_advs[a].flags & 4)) return RULE_HIDE;
    if (bit(adv_done, a)) return RULE_SHOW;
    return (mc_advs[a].flags & 2) ? RULE_HIDE : RULE_NO_CHANGE;
}

static int eval_node(int a, int depth) {
    int self_or_desc = bit(adv_done, a), c, i, vis;

    rule_stack[depth] = (u8)eval_rule(a);
    for (c = 0; c < MC_ADV_COUNT; c++) {
        if (mc_advs[c].parent == a) self_or_desc |= eval_node(c, depth + 1);
    }
    vis = self_or_desc;
    if (!vis) {
        // evaluateVisiblityForUnfinishedNode: itself, its parent, grandparent
        for (i = 0; i <= 2; i++) {
            int r = rule_stack[depth - i];
            if (r == RULE_SHOW) { vis = TRUE; break; }
            if (r == RULE_HIDE) break;
        }
    }
    visible[a] = (u8)vis;
    return self_or_desc;
}

static void update_visibility(void) {
    int a;
    if (!vis_dirty) return;
    vis_dirty = FALSE;
    for (a = 0; a < MC_ADV_COUNT; a++) visible[a] = FALSE;
    for (a = 0; a < MC_ADV_COUNT; a++) {
        if (mc_advs[a].parent >= 0) continue;
        rule_stack[0] = rule_stack[1] = rule_stack[2] = RULE_NO_CHANGE;
        eval_node(a, 3);
    }
}

// --- saving: done criteria as "advancement", "criterion" ---

int advancements_save_count(void) {
    int c, n = 0;
    for (c = 0; c < MC_ADV_CRIT_COUNT; c++) if (bit(crit_done, c)) n++;
    return n;
}

void advancements_save_entry(int k, const char **adv, const char **crit) {
    int a, c;
    for (a = 0; a < MC_ADV_COUNT; a++) {
        for (c = 0; c < mc_advs[a].crit_count; c++) {
            if (bit(crit_done, mc_advs[a].crit_first + c) && k-- == 0) {
                *adv = mc_adv_names[a];
                *crit = mc_adv_crit_names[mc_advs[a].crit_first + c];
                return;
            }
        }
    }
    *adv = *crit = "";
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

void advancements_load_entry(const char *adv, const char *crit) {
    int a, c;
    for (a = 0; a < MC_ADV_COUNT; a++) {
        if (!streq(mc_adv_names[a], adv)) continue;
        for (c = 0; c < mc_advs[a].crit_count; c++) {
            // (criteria the mod can't track never get done: an older build
            // awarded every kill criterion for any kill, those are dropped)
            if (streq(mc_adv_crit_names[mc_advs[a].crit_first + c], crit)
                && mc_adv_crits[mc_advs[a].crit_first + c].trigger != 0) set_bit(crit_done, mc_advs[a].crit_first + c);
        }
        if (requirements_met(a)) set_bit(adv_done, a);
        vis_dirty = TRUE;
        return;
    }
}

// --- text: StringSplitter.splitLines (words, a word too long is cut) ---

#define MAX_LINES 8
typedef struct {
    const char *start[MAX_LINES];
    u8 len[MAX_LINES];
    int n;
} Lines;

static s32 str_width_n(const char *s, int n) {
    char buf[128];
    int i;
    if (n > 127) n = 127;
    for (i = 0; i < n; i++) buf[i] = s[i];
    buf[n] = 0;
    return gui_text_width(buf);
}

static void split_lines(const char *s, s32 width, Lines *out) {
    out->n = 0;
    while (*s && out->n < MAX_LINES) {
        const char *p = s, *cut = NULL;
        while (*p && *p != '\n') {
            if (*p == ' ') cut = p;
            if (str_width_n(s, (int)(p - s) + 1) > width && p > s) break;
            p++;
        }
        if (*p == 0 || *p == '\n') cut = p;
        else if (cut == NULL || cut == s) cut = p;
        out->start[out->n] = s;
        out->len[out->n] = (u8)(cut - s);
        out->n++;
        s = (*cut == ' ' || *cut == '\n') ? cut + 1 : cut;
    }
}

static s32 lines_max_width(const Lines *l) {
    s32 m = 0, w;
    int i;
    for (i = 0; i < l->n; i++) {
        w = str_width_n(l->start[i], l->len[i]);
        if (w > m) m = w;
    }
    return m;
}

static void draw_line(Gfx **gfx, const Lines *l, int i, f32 x, f32 y, u32 rgb, int shadow, u8 alpha) {
    char buf[128];
    int k, n = l->len[i] > 127 ? 127 : l->len[i];
    for (k = 0; k < n; k++) buf[k] = l->start[i][k];
    buf[n] = 0;
    if (alpha == 255) gui_text(gfx, buf, x, y, rgb, shadow);
    else gui_text_alpha(gfx, buf, x, y, rgb, shadow, alpha);
}

static void fake_item(Gfx **gfx, ItemId id, f32 x, f32 y) {
    ItemStack st;
    if (id == 0) return;
    st.id = id;
    st.count = 1;
    st.pad = 0;
    st.damage = 0;
    gui_item(gfx, &st, x, y, FALSE);
}

#define SPR(name, x, y) gui_blit(gfx, mc_spr_##name, MC_SPR_##name##_W, MC_SPR_##name##_H, 0, 0, \
        MC_SPR_##name##_W, MC_SPR_##name##_H, x, y, MC_SPR_##name##_W, MC_SPR_##name##_H)

// --- ToastManager (5 slots of 32) with AdvancementToast / RecipeToast ---

#define TOAST_MAX 16
#define TOAST_ITEMS 32
typedef struct {
    s16 adv;                 // -1: RecipeToast
    s8 slot;                 // -1: queued
    u8 hiding, finished;
    s32 anim_start, became_full, full_for;
    f32 portion;
    // RecipeToast
    ItemId items[TOAST_ITEMS];
    u8 n_items, changed;
    s32 last_changed;
} Toast;
static Toast toasts[TOAST_MAX];
static int toast_n;

static Toast *toast_new(int adv) {
    Toast *t;
    if (toast_n >= TOAST_MAX) return NULL;
    t = &toasts[toast_n++];
    t->adv = (s16)adv;
    t->slot = -1;
    t->hiding = TRUE;
    t->finished = FALSE;
    t->anim_start = -1;
    t->became_full = -1;
    t->full_for = 0;
    t->portion = 0.0f;
    t->n_items = 0;
    t->changed = FALSE;
    t->last_changed = 0;
    return t;
}

static void toast_add_advancement(int a) {
    toast_new(a);
}

static void toast_add_recipe(ItemId item) {
    Toast *t = NULL;
    int i;
    for (i = 0; i < toast_n; i++) if (toasts[i].adv < 0) t = &toasts[i]; // getToast(RecipeToast)
    if (t == NULL) t = toast_new(-1);
    if (t == NULL) return;
    if (t->n_items < TOAST_ITEMS) t->items[t->n_items++] = item;
    t->changed = TRUE;
}

// Toast.update: what it wants to be
static int toast_wants_hide(Toast *t) {
    if (t->adv >= 0) return t->full_for >= 5000;
    if (t->changed) {
        t->last_changed = t->full_for;
        t->changed = FALSE;
    }
    return t->n_items == 0 || t->full_for - t->last_changed >= 5000;
}

// one visibility sound per update (visibilityChangeSoundPlayed)
static int vis_sound_played;

static void toast_vis_sound(int hiding) {
    if (vis_sound_played) return;
    vis_sound_played = TRUE;
    // Toast.Visibility.playSound: forUI(UI_TOAST_IN / OUT, 1, 1)
    sound_play_ui(hiding ? SND(UI_TOAST_OUT) : SND(UI_TOAST_IN), 1.0f, 1.0f);
}

// ToastInstance.update
static void toast_update(Toast *t, s32 now) {
    f32 prog;
    int wants_hide;
    if (t->anim_start == -1) {
        t->anim_start = now;
        t->hiding = FALSE;
        toast_vis_sound(FALSE);
    }
    if (!t->hiding && now - t->anim_start <= 600) t->became_full = now;
    t->full_for = now - t->became_full;
    prog = (f32)(now - t->anim_start) / 600.0f;
    if (prog < 0.0f) prog = 0.0f;
    if (prog > 1.0f) prog = 1.0f;
    prog *= prog;
    t->portion = t->hiding ? 1.0f - prog : prog;
    wants_hide = toast_wants_hide(t);
    if (wants_hide != t->hiding) {
        t->anim_start = now - (s32)((1.0f - t->portion) * 600.0f);
        t->hiding = (u8)wants_hide;
        toast_vis_sound(wants_hide);
    }
    t->finished = t->hiding && now - t->anim_start > 600;
}

static void toast_draw(Gfx **gfx, Toast *t, s32 sw) {
    f32 x = sw - 160.0f * t->portion, y = t->slot * 32.0f; // xPos / yPos
    if (t->adv >= 0) {
        const McAdvancement *ad = &mc_advs[t->adv];
        static const char *const kind[3] = { MC_STR_ADVANCEMENTS_TOAST_TASK, MC_STR_ADVANCEMENTS_TOAST_GOAL, MC_STR_ADVANCEMENTS_TOAST_CHALLENGE };
        u32 col = ad->frame == 2 ? 0xFF88FF : 0xFFFF00; // -30465 / -256
        Lines l;
        SPR(toast_advancement, x, y);
        split_lines(ad->title, 125, &l);
        if (l.n == 1) {
            gui_text(gfx, kind[ad->frame], x + 30, y + 7, col, FALSE);
            draw_line(gfx, &l, 0, x + 30, y + 18, 0xFFFFFF, FALSE, 255);
        } else if (t->full_for < 1500) {
            f32 f = (1500 - t->full_for) / 300.0f;
            s32 alpha = (s32)((f > 1.0f ? 1.0f : f) * 255.0f);
            if (alpha > 3) gui_text_alpha(gfx, kind[ad->frame], x + 30, y + 11, col, FALSE, (u8)alpha);
        } else {
            f32 f = (t->full_for - 1500) / 300.0f;
            s32 alpha = (s32)((f > 1.0f ? 1.0f : f) * 252.0f), ly = 32 / 2 - l.n * 9 / 2, i;
            for (i = 0; i < l.n && alpha > 3; i++, ly += 9) draw_line(gfx, &l, i, x + 30, y + ly, 0xFFFFFF, FALSE, (u8)alpha);
        }
        fake_item(gfx, ad->icon, x + 8, y + 8);
    } else {
        // (the crafting station's 0.6 scale icon at 3, 3 isn't drawn: the gui
        // has no scaled items)
        int idx = 0;
        if (t->n_items > 0) {
            s32 per = 5000 / t->n_items;
            if (per < 1) per = 1;
            idx = (t->full_for / per) % t->n_items;
        }
        SPR(toast_recipe, x, y);
        gui_text(gfx, MC_STR_RECIPE_TOAST_TITLE, x + 30, y + 7, 0x500050, FALSE);       // -11534256
        gui_text(gfx, MC_STR_RECIPE_TOAST_DESCRIPTION, x + 30, y + 18, 0x000000, FALSE); // -16777216
        if (t->n_items > 0) fake_item(gfx, t->items[idx], x + 8, y + 8);
    }
}

void advancements_draw_toasts(Gfx **gfx, s32 w) {
    s32 now = (s32)millis();
    u8 used = 0;
    int i, j;

    // update: drop the finished ones, then queued ones take free slots
    vis_sound_played = FALSE;
    for (i = 0; i < toast_n; i++) {
        if (toasts[i].slot < 0) continue;
        toast_update(&toasts[i], now);
        if (toasts[i].finished) {
            for (j = i + 1; j < toast_n; j++) toasts[j - 1] = toasts[j];
            toast_n--;
            i--;
        }
    }
    for (i = 0; i < toast_n; i++) if (toasts[i].slot >= 0) used |= (u8)(1 << toasts[i].slot);
    for (i = 0; i < toast_n; i++) {
        if (toasts[i].slot >= 0) continue;
        for (j = 0; j < 5 && (used & (1 << j)); j++) { }
        if (j == 5) break;
        toasts[i].slot = (s8)j;
        used |= (u8)(1 << j);
        // AdvancementToast.getSoundEvent: a challenge's fanfare
        if (toasts[i].adv >= 0 && mc_advs[toasts[i].adv].frame == 2) sound_play_ui(SND(UI_TOAST_CHALLENGE_COMPLETE), 1.0f, 1.0f);
        toast_update(&toasts[i], now);
    }
    for (i = 0; i < toast_n; i++) {
        if (toasts[i].slot >= 0 && !toasts[i].finished) toast_draw(gfx, &toasts[i], w);
    }
}

void advancements_reset(void) {
    int i;
    for (i = 0; i < (int)sizeof(crit_done); i++) crit_done[i] = 0;
    for (i = 0; i < (int)sizeof(adv_done); i++) adv_done[i] = 0;
    toast_n = 0;
    inv_hash = 0;
    vis_dirty = TRUE;
}

// --- AdvancementsScreen ---

#define WIN_W 252
#define WIN_H 140
#define IN_W 234
#define IN_H 113

static int open;
static int sel_root = -1; // the selected tab's root advancement
static f32 scroll_x[MC_ADV_TAB_COUNT], scroll_y[MC_ADV_TAB_COUNT];
static u8 centered[MC_ADV_TAB_COUNT];
static f32 fade;
static int hovered = -1;
static u32 last_tick;
static int scrolling;
static f32 drag_x, drag_y;

int advancements_open(void) { return open; }

void advancements_set_open(int o) {
    open = o;
    scrolling = FALSE;
    hovered = -1;
    fade = 0.0f;
    last_tick = gSteve.tick_count;
}

// the tabs: roots the client knows (visible), with a display + background
static int tab_list(int *roots) {
    int t, n = 0;
    update_visibility();
    for (t = 0; t < MC_ADV_TAB_COUNT; t++) {
        if (visible[mc_adv_tab_roots[t]]) roots[n++] = t;
    }
    return n;
}

// a widget for each visible advancement with a display, in its root's tab
static int has_widget(int a, int t) {
    return mc_advs[a].tab == t && visible[a] && (mc_advs[a].flags & 4);
}

// shown / hoverable: not hidden, or done
static int widget_drawn(int a) {
    return !(mc_advs[a].flags & 2) || bit(adv_done, a);
}

static s32 wx(int a) { return mth_floor(mc_advs[a].x * 28.0f); }
static s32 wy(int a) { return mth_floor(mc_advs[a].y * 27.0f); }

static void tab_bounds(int t, s32 *mnx, s32 *mny, s32 *mxx, s32 *mxy) {
    int a;
    *mnx = *mny = 0x7FFFFFFF;
    *mxx = *mxy = -0x7FFFFFFF - 1;
    for (a = 0; a < MC_ADV_COUNT; a++) {
        if (!has_widget(a, t)) continue;
        if (wx(a) < *mnx) *mnx = wx(a);
        if (wx(a) + 28 > *mxx) *mxx = wx(a) + 28;
        if (wy(a) < *mny) *mny = wy(a);
        if (wy(a) + 27 > *mxy) *mxy = wy(a) + 27;
    }
}

// findFirstVisibleParent, then tab.getWidget
static int widget_parent(int a) {
    int p = mc_advs[a].parent;
    while (p >= 0 && !(mc_advs[p].flags & 4)) p = mc_advs[p].parent;
    return (p >= 0 && has_widget(p, mc_advs[a].tab)) ? p : -1;
}

// the scissor (enableScissor on the inside), by hand
static s32 cl, ct;

static void clip_fill(Gfx **gfx, s32 x0, s32 y0, s32 x1, s32 y1, u32 col) {
    if (x0 < cl) x0 = cl;
    if (y0 < ct) y0 = ct;
    if (x1 > cl + IN_W) x1 = cl + IN_W;
    if (y1 > ct + IN_H) y1 = ct + IN_H;
    if (x1 > x0 && y1 > y0) gui_fill(gfx, x0, y0, x1 - x0, y1 - y0, col);
}

// GuiGraphics.horizontalLine / verticalLine (end points included / excluded)
static void hline(Gfx **gfx, s32 x0, s32 x1, s32 y, u32 col) {
    if (x1 < x0) { s32 t = x0; x0 = x1; x1 = t; }
    clip_fill(gfx, x0, y, x1 + 1, y + 1, col);
}
static void vline(Gfx **gfx, s32 x, s32 y0, s32 y1, u32 col) {
    if (y1 < y0) { s32 t = y0; y0 = y1; y1 = t; }
    clip_fill(gfx, x, y0 + 1, x + 1, y1, col);
}

static void clip_blit(Gfx **gfx, const u32 *tex, s32 tw, s32 th, s32 x, s32 y) {
    s32 u0 = 0, v0 = 0, u1 = tw, v1 = th;
    if (x < cl) u0 = cl - x;
    if (y < ct) v0 = ct - y;
    if (x + tw > cl + IN_W) u1 = cl + IN_W - x;
    if (y + th > ct + IN_H) v1 = ct + IN_H - y;
    if (u1 <= u0 || v1 <= v0) return;
    gui_blit(gfx, tex, tw, th, u0, v0, u1 - u0, v1 - v0, x + u0, y + v0, u1 - u0, v1 - v0);
}

static const u32 *frame_tex(int frame, int obtained) {
    switch (frame * 2 + (obtained ? 1 : 0)) {
        case 0: return mc_spr_adv_task_frame_unobtained;
        case 1: return mc_spr_adv_task_frame_obtained;
        case 2: return mc_spr_adv_goal_frame_unobtained;
        case 3: return mc_spr_adv_goal_frame_obtained;
        case 4: return mc_spr_adv_challenge_frame_unobtained;
        default: return mc_spr_adv_challenge_frame_obtained;
    }
}

static int fmt_progress(char *out, int a, int b) {
    char tmp[8];
    int k = 0, m;
    m = 0; do { tmp[m++] = (char)('0' + a % 10); a /= 10; } while (a > 0);
    while (m > 0) out[k++] = tmp[--m];
    out[k++] = '/'; // advancements.progress "%s/%s"
    m = 0; do { tmp[m++] = (char)('0' + b % 10); b /= 10; } while (b > 0);
    while (m > 0) out[k++] = tmp[--m];
    out[k] = 0;
    return k;
}

// AdvancementWidget: title lines, description (findOptimalLines), width
typedef struct {
    Lines title, desc;
    s32 width;
} WidgetText;

static void widget_text(int a, WidgetText *wt) {
    static const s8 offsets[5] = { 0, 10, -10, 25, -25 };
    const McAdvancement *ad = &mc_advs[a];
    s32 title_w, prog_w = 0, longest, best_dist = 0x7FFFFFFF;
    int i;
    Lines best, test;

    split_lines(ad->title, 163, &wt->title);
    title_w = lines_max_width(&wt->title);
    if (title_w < 80) title_w = 80;
    if (ad->req_count > 1) {
        char p[16];
        fmt_progress(p, ad->req_count, ad->req_count);
        prog_w = gui_text_width(p) + 8;
    }
    longest = 29 + title_w + prog_w;
    best.n = 0;
    for (i = 0; i < 5; i++) {
        s32 dist;
        split_lines(ad->desc, longest - offsets[i], &test);
        dist = lines_max_width(&test) - longest;
        if (dist < 0) dist = -dist;
        if (dist <= 10) { best = test; break; }
        if (dist < best_dist) { best_dist = dist; best = test; }
    }
    wt->desc = best;
    if (lines_max_width(&best) > longest) longest = lines_max_width(&best);
    wt->width = longest + 3 + 5;
}

// AdvancementWidget.extractHover, (xo, yo) the scroll, local to the inside
static void widget_hover(Gfx **gfx, int a, s32 xo, s32 yo, s32 screen_xo, s32 screen_w) {
    const McAdvancement *ad = &mc_advs[a];
    WidgetText wt;
    s32 x = wx(a), y = wy(a), tbh, title_top, tbb, desc_th, desc_h, prog_w = 0, first_w, title_left, bg_h, i;
    int left_side, top_side, first_obt, second_obt, icon_obt;
    f32 amount = get_percent(a);
    char prog[16];
    u32 dcol = ad->frame == 2 ? 0xAA00AA : 0x55FF55; // ChatFormatting DARK_PURPLE / GREEN

    widget_text(a, &wt);
    tbh = 9 * wt.title.n + 9 + 8;
    title_top = yo + y + (26 - tbh) / 2;
    tbb = title_top + tbh;
    desc_th = wt.desc.n * 9;
    desc_h = 6 + desc_th;
    left_side = screen_xo + xo + x + wt.width + 26 >= screen_w;
    prog[0] = 0;
    if (ad->crit_count > 0 && ad->req_count > 1) {
        fmt_progress(prog, groups_done(a), ad->req_count);
        prog_w = gui_text_width(prog);
    }
    top_side = tbb + desc_h >= 113;
    first_w = mth_floor(amount * wt.width);
    if (amount >= 1.0f) {
        first_w = wt.width / 2;
        first_obt = second_obt = icon_obt = TRUE;
    } else if (first_w < 2) {
        first_w = wt.width / 2;
        first_obt = second_obt = icon_obt = FALSE;
    } else if (first_w > wt.width - 2) {
        first_w = wt.width / 2;
        first_obt = second_obt = TRUE;
        icon_obt = FALSE;
    } else {
        first_obt = TRUE;
        second_obt = icon_obt = FALSE;
    }
    title_left = left_side ? xo + x - wt.width + 26 + 6 : xo + x;
    bg_h = tbh + desc_h;
    // to screen space
    title_left += cl;
    title_top += ct;
    tbb += ct;
    if (wt.desc.n > 0) {
        gui_blit_nine_slice(gfx, mc_spr_adv_title_box, 200, 26, 10, title_left, top_side ? tbb - bg_h : title_top, wt.width, bg_h);
    }
    if (first_obt != second_obt && tbh == 26) {
        // two halves of the box (the sprite at 200 x 26 is its own texture)
        s32 second_w = wt.width - first_w;
        gui_blit(gfx, mc_spr_adv_box_obtained, 200, 26, 0, 0, first_w, 26, title_left, title_top, first_w, 26);
        gui_blit(gfx, mc_spr_adv_box_unobtained, 200, 26, 200 - second_w, 0, second_w, 26, title_left + first_w, title_top, second_w, 26);
    } else {
        gui_blit_nine_slice(gfx, first_obt ? mc_spr_adv_box_obtained : mc_spr_adv_box_unobtained, 200, 26, 10,
            title_left, title_top, wt.width, tbh);
    }
    gui_blit(gfx, frame_tex(ad->frame, icon_obt), 26, 26, 0, 0, 26, 26, cl + xo + x + 3, ct + yo + y, 26, 26);
    for (i = 0; i < wt.title.n; i++) {
        draw_line(gfx, &wt.title, i, left_side ? title_left + 5 : cl + xo + x + 32, title_top + 9 + i * 9, 0xFFFFFF, TRUE, 255);
    }
    if (prog[0]) {
        gui_text(gfx, prog, left_side ? cl + xo + x - prog_w : cl + xo + x + wt.width - prog_w - 5, title_top + 9, 0xFFFFFF, TRUE);
    }
    for (i = 0; i < wt.desc.n; i++) {
        draw_line(gfx, &wt.desc, i, title_left + 5, (top_side ? title_top - desc_th + 1 : tbb) + i * 9, dcol, TRUE, 255);
    }
    fake_item(gfx, ad->icon, cl + xo + x + 8, ct + yo + y + 5);
}

static int tab_mouse_over(s32 left, s32 top, int index, s32 mx, s32 my) {
    s32 x = left + 32 * index, y = top - 32 + 4; // ABOVE: (28 + 4) * index, -32 + 4
    return mx > x && mx < x + 28 && my > y && my < y + 32;
}

static int selected_tab(int *roots, int n) {
    int i;
    for (i = 0; i < n; i++) if (mc_adv_tab_roots[roots[i]] == sel_root) return roots[i];
    if (n > 0) {
        sel_root = mc_adv_tab_roots[roots[0]];
        return roots[0];
    }
    return -1;
}

// AdvancementTab.tick (20 a second)
static void tab_tick(int t, s32 rmx, s32 rmy) {
    int a, hit = FALSE;
    if (rmx > 0 && rmx < IN_W && rmy > 0 && rmy < IN_H) {
        s32 sx = mth_floor(scroll_x[t]), sy = mth_floor(scroll_y[t]);
        for (a = 0; a < MC_ADV_COUNT; a++) {
            s32 x0, y0;
            if (!has_widget(a, t) || !widget_drawn(a)) continue;
            x0 = sx + wx(a);
            y0 = sy + wy(a);
            if (rmx >= x0 && rmx <= x0 + 26 && rmy >= y0 && rmy <= y0 + 26) {
                hit = TRUE;
                hovered = a;
                break;
            }
        }
    }
    if (hit) {
        fade += 0.06f;
        if (fade > 0.3f) fade = 0.3f;
    } else {
        fade -= 0.12f;
        if (fade < 0.0f) fade = 0.0f;
        hovered = -1;
    }
}

static void draw_connections(Gfx **gfx, int t, s32 sx, s32 sy, int background) {
    u32 col = background ? 0xFF000000 : 0xFFFFFFFF;
    int a;
    for (a = 0; a < MC_ADV_COUNT; a++) {
        int p;
        s32 dep_x, split_x, dep_y, my_x, my_y;
        if (!has_widget(a, t) || (p = widget_parent(a)) < 0) continue;
        dep_x = cl + sx + wx(p) + 13;
        split_x = cl + sx + wx(p) + 26 + 4;
        dep_y = ct + sy + wy(p) + 13;
        my_x = cl + sx + wx(a) + 13;
        my_y = ct + sy + wy(a) + 13;
        if (background) {
            hline(gfx, split_x, dep_x, dep_y - 1, col);
            hline(gfx, split_x + 1, dep_x, dep_y, col);
            hline(gfx, split_x, dep_x, dep_y + 1, col);
            hline(gfx, my_x, split_x - 1, my_y - 1, col);
            hline(gfx, my_x, split_x - 1, my_y, col);
            hline(gfx, my_x, split_x - 1, my_y + 1, col);
            vline(gfx, split_x - 1, my_y, dep_y, col);
            vline(gfx, split_x + 1, my_y, dep_y, col);
        } else {
            hline(gfx, split_x, dep_x, dep_y, col);
            hline(gfx, my_x, split_x, my_y, col);
            vline(gfx, split_x, my_y, dep_y, col);
        }
    }
}

// the Done button (HeaderAndFooterLayout footer, 200 x 20)
static s32 done_x(s32 w) { return (w - 200) / 2; }
static s32 done_y(s32 h) { return h - 33 + (33 - 20) / 2; }

void advancements_draw(Gfx **gfx, s32 w, s32 h, s32 mx, s32 my) {
    s32 left = (w - WIN_W) / 2, top = (h - WIN_H) / 2;
    int roots[MC_ADV_TAB_COUNT], n, t, i, a;

    if (!open) return;
    n = tab_list(roots);
    t = selected_tab(roots, n);
    cl = left + 9;
    ct = top + 18;
    // tick: the hover and its fade
    while (last_tick != gSteve.tick_count) {
        last_tick++;
        if (t >= 0) tab_tick(t, mx - left - 9, my - top - 18);
    }

    // the header title and the Done button
    gui_text(gfx, MC_STR_GUI_ADVANCEMENTS, (w - gui_text_width(MC_STR_GUI_ADVANCEMENTS)) / 2, (33 - 9) / 2, 0xFFFFFF, TRUE);
    {
        s32 bx = done_x(w), by = done_y(h);
        if (mx >= bx && my >= by && mx < bx + 200 && my < by + 20) SPR(button_highlighted, bx, by);
        else SPR(button, bx, by);
        gui_text(gfx, MC_STR_GUI_DONE, bx + (200 - gui_text_width(MC_STR_GUI_DONE)) / 2, by + 6, 0xFFFFFF, TRUE);
    }

    // extractInside
    if (t < 0) {
        gui_fill(gfx, cl, ct, IN_W, IN_H, 0xFF000000);
        gui_text(gfx, MC_STR_ADVANCEMENTS_EMPTY, cl + 117 - gui_text_width(MC_STR_ADVANCEMENTS_EMPTY) / 2, ct + 56 - 9 / 2, 0xFFFFFF, TRUE);
        gui_text(gfx, MC_STR_ADVANCEMENTS_SAD_LABEL, cl + 117 - gui_text_width(MC_STR_ADVANCEMENTS_SAD_LABEL) / 2, ct + IN_H - 9, 0xFFFFFF, TRUE);
    } else {
        s32 mnx, mny, mxx, mxy, sx, sy, ox, oy, x, y;
        tab_bounds(t, &mnx, &mny, &mxx, &mxy);
        if (!centered[t]) {
            scroll_x[t] = 117 - (mxx + mnx) / 2;
            scroll_y[t] = 56 - (mxy + mny) / 2;
            centered[t] = TRUE;
        }
        sx = mth_floor(scroll_x[t]);
        sy = mth_floor(scroll_y[t]);
        ox = sx % 16;
        oy = sy % 16;
        for (x = -1; x <= 15; x++) {
            for (y = -1; y <= 8; y++) clip_blit(gfx, &mc_adv_bg[t * 256], 16, 16, cl + ox + 16 * x, ct + oy + 16 * y);
        }
        draw_connections(gfx, t, sx, sy, TRUE);
        draw_connections(gfx, t, sx, sy, FALSE);
        for (a = 0; a < MC_ADV_COUNT; a++) {
            s32 fx, fy;
            if (!has_widget(a, t) || !widget_drawn(a)) continue;
            fx = cl + sx + wx(a);
            fy = ct + sy + wy(a);
            clip_blit(gfx, frame_tex(mc_advs[a].frame, get_percent(a) >= 1.0f), 26, 26, fx + 3, fy);
            // (items can't be cut: only drawn when they're all inside)
            if (fx + 8 >= cl && fy + 5 >= ct && fx + 24 <= cl + IN_W && fy + 21 <= ct + IN_H) {
                fake_item(gfx, mc_advs[a].icon, fx + 8, fy + 5);
            }
        }
    }

    // extractWindow: the frame, the tabs (when there's more than one), title
    gui_blit(gfx, mc_gui_adv_window, WIN_W, WIN_H, 0, 0, WIN_W, WIN_H, left, top, WIN_W, WIN_H);
    if (n > 1) {
        for (i = 0; i < n; i++) {
            s32 tx = left + 32 * i, ty = top - 32 + 4;
            int sel = roots[i] == t;
            if (i == 0) { if (sel) SPR(adv_tab_above_left_selected, tx, ty); else SPR(adv_tab_above_left, tx, ty); }
            else if (i == 8 - 1) { if (sel) SPR(adv_tab_above_right_selected, tx, ty); else SPR(adv_tab_above_right, tx, ty); }
            else { if (sel) SPR(adv_tab_above_middle_selected, tx, ty); else SPR(adv_tab_above_middle, tx, ty); }
        }
        for (i = 0; i < n; i++) fake_item(gfx, mc_advs[mc_adv_tab_roots[roots[i]]].icon, left + 32 * i + 6, top - 32 + 4 + 9);
    }
    gui_text(gfx, t >= 0 ? mc_advs[mc_adv_tab_roots[t]].title : MC_STR_GUI_ADVANCEMENTS, left + 8, top + 6, 0x404040, FALSE);

    // extractTooltips: the fade over the inside, the hovered widget, then a
    // hovered tab's name
    if (t >= 0) {
        if (fade > 0.0f) gui_fill(gfx, cl, ct, IN_W, IN_H, (u32)mth_floor(fade * 255.0f) << 24);
        if (hovered >= 0 && has_widget(hovered, t)) {
            widget_hover(gfx, hovered, mth_floor(scroll_x[t]), mth_floor(scroll_y[t]), left, w);
        }
    }
    if (n > 1) {
        for (i = 0; i < n; i++) {
            if (tab_mouse_over(left, top, i, mx, my)) {
                const char *name = mc_advs[mc_adv_tab_roots[roots[i]]].title;
                u32 col = 0xFFFFFF;
                screen_draw_tooltip(gfx, &name, &col, 1);
            }
        }
    }
}

// AdvancementTab.scroll
static void tab_scroll(int t, f32 dx, f32 dy) {
    s32 mnx, mny, mxx, mxy;
    tab_bounds(t, &mnx, &mny, &mxx, &mxy);
    if (mxx - mnx > IN_W) {
        scroll_x[t] += dx;
        if (scroll_x[t] < -(mxx - IN_W)) scroll_x[t] = -(mxx - IN_W);
        if (scroll_x[t] > 0.0f) scroll_x[t] = 0.0f;
    }
    if (mxy - mny > IN_H) {
        scroll_y[t] += dy;
        if (scroll_y[t] < -(mxy - IN_H)) scroll_y[t] = -(mxy - IN_H);
        if (scroll_y[t] > 0.0f) scroll_y[t] = 0.0f;
    }
}

// mouseClicked (tabs, Done), mouseDragged, mouseReleased, mouseScrolled.
// returns TRUE when the screen should close (Done)
int advancements_input(s32 w, s32 h, f32 fmx, f32 fmy, int clicks, int held, int wheel) {
    s32 left = (w - WIN_W) / 2, top = (h - WIN_H) / 2, mx = (s32)fmx, my = (s32)fmy;
    int roots[MC_ADV_TAB_COUNT], n, i, t;

    if (!open) return FALSE;
    n = tab_list(roots);
    t = selected_tab(roots, n);
    if (clicks > 0) {
        for (i = 0; i < n && n > 1; i++) {
            if (tab_mouse_over(left, top, i, mx, my)) {
                sel_root = mc_adv_tab_roots[roots[i]];
                t = roots[i];
                hovered = -1;
                break;
            }
        }
        if (mx >= done_x(w) && my >= done_y(h) && mx < done_x(w) + 200 && my < done_y(h) + 20) {
            sound_play_ui(SND(UI_BUTTON_CLICK), 0.25f, 1.0f); // AbstractButton.playDownSound
            return TRUE;
        }
    }
    if (held) {
        if (fmx != drag_x || fmy != drag_y) {
            if (!scrolling) scrolling = TRUE;
            else if (t >= 0) tab_scroll(t, fmx - drag_x, fmy - drag_y);
        }
    } else {
        scrolling = FALSE;
    }
    drag_x = fmx;
    drag_y = fmy;
    if (wheel != 0 && t >= 0) tab_scroll(t, 0.0f, wheel * 16.0f);
    return FALSE;
}

#else
void advancements_tick(void) {}
void advancements_consumed(ItemId item) { (void)item; }
void advancements_killed_mob(int mob) { (void)mob; }
void advancements_killed_by_mob(int mob) { (void)mob; }
void advancements_reset(void) {}
int advancements_save_count(void) { return 0; }
void advancements_save_entry(int k, const char **adv, const char **crit) { (void)k; *adv = *crit = ""; }
void advancements_load_entry(const char *adv, const char *crit) { (void)adv; (void)crit; }
void advancements_draw_toasts(Gfx **gfx, s32 w) { (void)gfx; (void)w; }
int advancements_open(void) { return FALSE; }
void advancements_set_open(int o) { (void)o; }
void advancements_draw(Gfx **gfx, s32 w, s32 h, s32 mx, s32 my) { (void)gfx; (void)w; (void)h; (void)mx; (void)my; }
int advancements_input(s32 w, s32 h, f32 mx, f32 my, int clicks, int held, int wheel) { (void)w; (void)h; (void)mx; (void)my; (void)clicks; (void)held; (void)wheel; return FALSE; }
#endif
