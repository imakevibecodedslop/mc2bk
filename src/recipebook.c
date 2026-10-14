// the crafting recipe book (RecipeBookComponent + CraftingRecipeBookComponent,
// RecipeBookPage, RecipeButton, RecipeBookTabButton, GhostSlots, and the
// server's ServerPlaceRecipe for clicking a recipe), for the inventory and
// the crafting table. recipes unlock like their recipes/ advancements: once
// one of their items has been in the inventory (inventory_changed). one
// recipe per button (no recipe groups / the right click overlay), no search
// box yet.

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "steve.h"
#include "survival.h"
#include "items.h"
#include "gui.h"
#include "mouse.h"
#include "recipebook.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

#define BOOK_W 147
#define BOOK_H 166
#define PER_PAGE 20

static u8 known[(MC_RECIPE_COUNT + 7) / 8];
static int book_open, book_filter; // RecipeBookSettings (crafting)
static int tab, page;
static int ghost = -1;      // GhostSlots: the recipe shown in the grid
// the search box (EditBox, 50 chars) and its query (lower case)
static char search[51];
static int search_len, search_focus;
// RecipeToast: results of the recipes unlocked since the toast last asked
#define NEW_MAX 64
static ItemId new_unlocks[NEW_MAX];
static int new_count;

// CraftingRecipeBookComponent.TABS: search, equipment, building, misc, redstone
// (CraftingBookCategory: misc 0, building 1, redstone 2, equipment 3)
static const s8 tab_category[5] = { -1, 3, 1, 0, 2 };

static int is_known(int r) {
    return (known[r >> 3] >> (r & 7)) & 1;
}

static void set_known(int r) {
    known[r >> 3] |= (u8)(1 << (r & 7));
}

static int ing_has(const McIngredient *ing, ItemId id) {
    int i;
    for (i = 0; i < ing->count; i++) {
        if (mc_ingredient_ids[ing->offset + i] == id) return TRUE;
    }
    return FALSE;
}

// --- unlocking (RecipeUnlockedTrigger via the recipes/ advancements) ---

ItemId recipebook_take_new_unlock(void) {
    ItemId id;
    int i;
    if (new_count == 0) return 0;
    id = new_unlocks[0];
    for (i = 1; i < new_count; i++) new_unlocks[i - 1] = new_unlocks[i];
    new_count--;
    return id;
}

void recipebook_tick(void) {
    int r, i;
    for (r = 0; r < MC_RECIPE_COUNT; r++) {
        const McIngredient *u = &mc_recipe_book[r].unlock;
        if (is_known(r)) continue;
        if (u->count == 0) {
            set_known(r); // nothing to unlock it: known from the start
            continue;
        }
        for (i = 0; i < INV_TOTAL; i++) {
            const ItemStack *st = &gSurv.inv[i];
            if (st->count > 0 && ing_has(u, st->id)) {
                set_known(r);
                if (new_count < NEW_MAX) new_unlocks[new_count++] = mc_recipes[r].result;
                break;
            }
        }
    }
}

void recipebook_reset(void) {
    int i;
    for (i = 0; i < (int)sizeof(known); i++) known[i] = 0;
    book_open = book_filter = FALSE;
    tab = page = 0;
    ghost = -1;
    search_len = 0;
    search[0] = 0;
    search_focus = FALSE;
    new_count = 0;
}

// --- saving: the known recipes by name (ids move between exports) ---

int recipebook_save_count(void) {
    int r, n = 0;
    for (r = 0; r < MC_RECIPE_COUNT; r++) if (is_known(r)) n++;
    return n;
}

const char *recipebook_save_name(int k) {
    int r;
    for (r = 0; r < MC_RECIPE_COUNT; r++) {
        if (is_known(r) && k-- == 0) return mc_recipe_names[r];
    }
    return "";
}

void recipebook_load_name(const char *name) {
    int r, k;
    for (r = 0; r < MC_RECIPE_COUNT; r++) {
        const char *s = mc_recipe_names[r];
        for (k = 0; s[k] && s[k] == name[k]; k++) { }
        if (s[k] == 0 && name[k] == 0) {
            set_known(r);
            return;
        }
    }
}

int recipebook_settings(void) {
    return (book_open ? 1 : 0) | (book_filter ? 2 : 0);
}

void recipebook_set_settings(int f) {
    book_open = (f & 1) != 0;
    book_filter = (f & 2) != 0;
}

// --- what can be crafted (StackedItemContents) ---

typedef struct {
    ItemId id;
    int count;
} Avail;

static Avail avail[INV_SIZE + 9];
static int avail_n;

static void gather(const ItemStack *grid, int grid_n) {
    int i, k;
    avail_n = 0;
    for (i = 0; i < INV_SIZE + grid_n; i++) {
        const ItemStack *st = (i < INV_SIZE) ? &gSurv.inv[i] : &grid[i - INV_SIZE];
        if (st->count == 0) continue;
        for (k = 0; k < avail_n && avail[k].id != st->id; k++) { }
        if (k == avail_n) {
            avail[avail_n].id = st->id;
            avail[avail_n].count = 0;
            avail_n++;
        }
        avail[k].count += st->count;
    }
}

// the recipe's non-empty cells, each needing `times` of one item
static const McIngredient *cells[9];
static int cell_n;

static void recipe_cells(int r) {
    const McRecipe *rc = &mc_recipes[r];
    int n = rc->w ? rc->w * rc->h : rc->h, i;
    cell_n = 0;
    for (i = 0; i < n && i < 9; i++) {
        if (rc->cells[i].count > 0) cells[cell_n++] = &rc->cells[i];
    }
}

static int match(int c, int times) {
    int k;
    if (c == cell_n) return TRUE;
    for (k = 0; k < avail_n; k++) {
        if (avail[k].count >= times && ing_has(cells[c], avail[k].id)) {
            avail[k].count -= times;
            if (match(c + 1, times)) {
                avail[k].count += times;
                return TRUE;
            }
            avail[k].count += times;
        }
    }
    return FALSE;
}

static int fits_grid(int r, int grid_w) {
    const McRecipe *rc = &mc_recipes[r];
    if (rc->w) return rc->w <= grid_w && rc->h <= grid_w;
    return rc->h <= grid_w * grid_w;
}

static int craftable(int r) {
    recipe_cells(r);
    return cell_n > 0 && match(0, 1);
}

// --- the page's list ---

static int list[MC_RECIPE_COUNT];
static int list_n;


static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// the recipe search tree, close enough: the result's name contains the query
static int search_matches(int r) {
    const char *name = item_name(mc_recipes[r].result);
    int i, k;
    if (search_len == 0) return TRUE;
    for (i = 0; name[i]; i++) {
        for (k = 0; k < search_len && name[i + k] && lower(name[i + k]) == search[k]; k++) { }
        if (k == search_len) return TRUE;
    }
    return FALSE;
}

int recipebook_search_focused(void) { return book_open && search_focus; }
void recipebook_focus_search(void) { search_focus = TRUE; }

void recipebook_type(char c) {
    if (search_len < 50) {
        search[search_len++] = lower(c);
        search[search_len] = 0;
        page = 0; // updateCollections(resetPage: the search changed)
    }
}

void recipebook_backspace(void) {
    if (search_len > 0) {
        search[--search_len] = 0;
        page = 0;
    }
}

static void build_list(const ItemStack *grid, int grid_w) {
    int r;
    gather(grid, grid_w * grid_w);
    list_n = 0;
    for (r = 0; r < MC_RECIPE_COUNT; r++) {
        if (!is_known(r) || !fits_grid(r, grid_w)) continue;
        if (tab > 0 && mc_recipe_book[r].category != tab_category[tab]) continue;
        if (!search_matches(r)) continue;
        if (book_filter && !craftable(r)) continue;
        list[list_n++] = r;
    }
}

// RecipeBookTabButton.updateVisibility: a category tab only with recipes in it
static int tab_shown(int t, int grid_w) {
    int r;
    if (t == 0) return TRUE;
    for (r = 0; r < MC_RECIPE_COUNT; r++) {
        if (is_known(r) && fits_grid(r, grid_w) && mc_recipe_book[r].category == tab_category[t]) return TRUE;
    }
    return FALSE;
}

// --- layout ---

int recipebook_visible(void) {
    return book_open;
}

void recipebook_toggle(void) {
    book_open = !book_open;
    ghost = -1;
}

int recipebook_narrow(s32 w) {
    return w < 379; // RecipeBookComponent.widthTooNarrow (InventoryScreen: width < 379)
}

// updateScreenPosition: where the container goes with the book open
s32 recipebook_left_pos(s32 w, s32 image_w) {
    if (book_open && !recipebook_narrow(w)) return 177 + (w - image_w - 200) / 2;
    return (w - image_w) / 2;
}

static s32 xo_of(s32 w) {
    return (w - BOOK_W) / 2 - (recipebook_narrow(w) ? 0 : 86);
}

static s32 yo_of(s32 h) {
    return (h - BOOK_H) / 2;
}

static int inside(s32 mx, s32 my, s32 x, s32 y, s32 w, s32 h) {
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

// hasClickedOutside's recipe book part
int recipebook_contains(s32 w, s32 h, s32 mx, s32 my) {
    s32 xo = xo_of(w), yo = yo_of(h);
    if (!book_open) return FALSE;
    return inside(mx, my, xo - 30, yo, BOOK_W + 30, BOOK_H);
}

// --- placing (ServerPlaceRecipe.placeRecipe) ---

// the grid back into the inventory (clearGrid); FALSE if it doesn't fit
static int clear_grid(ItemStack *grid, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (grid[i].count == 0) continue;
        if (surv_inv_add_stack(&grid[i]) > 0) return FALSE;
        grid[i].id = 0;
        grid[i].count = 0;
        grid[i].damage = 0;
    }
    return TRUE;
}

// take `times` of an item matching the ingredient out of the inventory
static ItemId take(const McIngredient *ing, int times, ItemId prefer) {
    int i, pass;
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < INV_SIZE; i++) {
            ItemStack *st = &gSurv.inv[i];
            int have, j;
            ItemId id = st->id;
            if (st->count == 0 || !ing_has(ing, id)) continue;
            if (pass == 0 && prefer != 0 && id != prefer) continue;
            have = 0;
            for (j = 0; j < INV_SIZE; j++) if (gSurv.inv[j].id == id) have += gSurv.inv[j].count;
            if (have < times) continue;
            // remove `times` of it, from the last stacks first
            for (j = INV_SIZE - 1; j >= 0 && times > 0; j--) {
                ItemStack *s2 = &gSurv.inv[j];
                int n;
                if (s2->count == 0 || s2->id != id) continue;
                n = (s2->count < times) ? s2->count : times;
                s2->count -= n;
                times -= n;
                if (s2->count == 0) {
                    s2->id = 0;
                    s2->damage = 0;
                }
            }
            return id;
        }
    }
    return 0;
}

static void place(int r, ItemStack *grid, int grid_w, int max) {
    const McRecipe *rc = &mc_recipes[r];
    int n = grid_w * grid_w, times = 1, x, y, k;

    if (!clear_grid(grid, n)) return;
    gather(grid, 0);
    recipe_cells(r);
    if (!match(0, 1)) return;
    if (max) {
        // as many as the ingredients' stacks allow (useMaxItems)
        int lim = 64;
        for (k = 0; k < cell_n; k++) {
            int a, best = 0;
            for (a = 0; a < avail_n; a++) {
                if (ing_has(cells[k], avail[a].id) && item_max_stack(avail[a].id) > best) best = item_max_stack(avail[a].id);
            }
            if (best < lim) lim = best;
        }
        for (times = lim; times > 1 && !match(0, times); times--) { }
    }
    if (rc->w) {
        // PlaceRecipeHelper: shaped from the grid's top left
        for (y = 0; y < rc->h; y++) {
            for (x = 0; x < rc->w; x++) {
                const McIngredient *ing = &rc->cells[y * rc->w + x];
                ItemStack *dst = &grid[y * grid_w + x];
                if (ing->count == 0) continue;
                dst->id = take(ing, times, 0);
                dst->count = dst->id ? (u8)times : 0;
                dst->damage = 0;
            }
        }
    } else {
        for (k = 0; k < rc->h && k < n; k++) {
            ItemStack *dst = &grid[k];
            dst->id = take(&rc->cells[k], times, 0);
            dst->count = dst->id ? (u8)times : 0;
            dst->damage = 0;
        }
    }
}

// --- drawing ---

#define SPR(name, x, y) gui_blit(gfx, mc_spr_##name, MC_SPR_##name##_W, MC_SPR_##name##_H, 0, 0, \
        MC_SPR_##name##_W, MC_SPR_##name##_H, x, y, MC_SPR_##name##_W, MC_SPR_##name##_H)

static void fake_item(Gfx **gfx, ItemId id, s32 x, s32 y) {
    ItemStack st;
    st.id = id;
    st.count = 1;
    st.damage = 0;
    gui_item(gfx, &st, x, y, FALSE);
}

// the first item of an ingredient, cycling every 30 ticks (SlotSelectTime)
static ItemId shown_option(const McIngredient *ing) {
    if (ing->count == 0) return 0;
    return mc_ingredient_ids[ing->offset + (gSteve.tick_count / 30) % ing->count];
}

static int hovered_recipe = -1;

void recipebook_draw(Gfx **gfx, s32 w, s32 h, s32 mx, s32 my, const ItemStack *grid, int grid_w) {
#ifdef MC_HAVE_TEXTURES
    s32 xo = xo_of(w), yo = yo_of(h);
    int t, i, n = 0, pages;
    static const ItemId tab_icons[5][2] = {
        { ITEM_COMPASS, 0 }, { ITEM_IRON_AXE, ITEM_GOLDEN_SWORD }, { BLOCK_BRICKS, 0 },
        { ITEM_LAVA_BUCKET, ITEM_APPLE }, { ITEM_REDSTONE, 0 },
    };

    hovered_recipe = -1;
    if (!book_open) return;
    build_list(grid, grid_w);
    pages = (list_n + PER_PAGE - 1) / PER_PAGE;
    if (page >= pages) page = 0;
    gui_blit(gfx, mc_gui_recipe_book, BOOK_W, BOOK_H, 0, 0, BOOK_W, BOOK_H, xo, yo, BOOK_W, BOOK_H);
    // the search box: EditBox (bordered, text at +4, +3), the hint when it's
    // empty and not focused, "_" blinking at the end while it is
    if (search_focus) gui_blit_nine_slice(gfx, mc_spr_text_field_highlighted, 200, 20, 1, xo + 25, yo + 13, 81, 14);
    else gui_blit_nine_slice(gfx, mc_spr_text_field, 200, 20, 1, xo + 25, yo + 13, 81, 14);
    {
        s32 tx = xo + 25 + 4, ty = yo + 13 + 3, tw = 0;
        if (search_len > 0) tw = gui_text(gfx, search, tx, ty, 0xFFFFFF, TRUE);
        else if (!search_focus) gui_text(gfx, MC_STR_GUI_RECIPEBOOK_SEARCH_HINT, tx, ty, 0xAAAAAA, TRUE); // SEARCH_HINT_STYLE: gray
        if (search_focus && (gSteve.tick_count / 6) % 2 == 0) gui_text(gfx, "_", tx + tw, ty, 0xFFFFFF, TRUE);
    }
    // tabs (updateTabs): x - 30, 27 apart
    for (t = 0; t < 5; t++) {
        s32 tx = xo - 30, ty = yo + 3 + 27 * n;
        int sel = (t == tab), move = sel ? -2 : 0;
        if (!tab_shown(t, grid_w)) continue;
        n++;
        if (sel) SPR(rb_tab_selected, tx - 2, ty);
        else SPR(rb_tab, tx, ty);
        if (tab_icons[t][1]) {
            fake_item(gfx, tab_icons[t][0], tx + 3 + move, ty + 5);
            fake_item(gfx, tab_icons[t][1], tx + 14 + move, ty + 5);
        } else {
            fake_item(gfx, tab_icons[t][0], tx + 9 + move, ty + 5);
        }
    }
    // the craftable filter (CycleButton, 26x16)
    {
        int hov = inside(mx, my, xo + 110, yo + 12, 26, 16);
        if (book_filter) {
            if (hov) SPR(rb_filter_enabled_highlighted, xo + 110, yo + 12);
            else SPR(rb_filter_enabled, xo + 110, yo + 12);
        } else {
            if (hov) SPR(rb_filter_disabled_highlighted, xo + 110, yo + 12);
            else SPR(rb_filter_disabled, xo + 110, yo + 12);
        }
    }
    // the page: 5 x 4 RecipeButtons, 25 apart
    gather(grid, grid_w * grid_w);
    for (i = 0; i < PER_PAGE && page * PER_PAGE + i < list_n; i++) {
        int r = list[page * PER_PAGE + i];
        s32 bx = xo + 11 + 25 * (i % 5), by = yo + 31 + 25 * (i / 5);
        if (craftable(r)) SPR(rb_slot_craftable, bx, by);
        else SPR(rb_slot_uncraftable, bx, by);
        {
            ItemStack st;
            st.id = mc_recipes[r].result;
            st.count = mc_recipes[r].count;
            st.damage = 0;
            gui_item(gfx, &st, bx + 4, by + 4, st.count > 1);
        }
        if (inside(mx, my, bx, by, 25, 25)) hovered_recipe = r;
    }
    // page arrows + "1/2"
    if (pages > 1) {
        char txt[16];
        int k = 0, a = page + 1, b = pages;
        char tmp[8];
        int m;
        m = 0; do { tmp[m++] = (char)('0' + a % 10); a /= 10; } while (a > 0);
        while (m > 0) txt[k++] = tmp[--m];
        txt[k++] = '/';
        m = 0; do { tmp[m++] = (char)('0' + b % 10); b /= 10; } while (b > 0);
        while (m > 0) txt[k++] = tmp[--m];
        txt[k] = 0;
        gui_text(gfx, txt, xo + 73 - gui_text_width(txt) / 2, yo + 141, 0xFFFFFF, FALSE);
        if (page < pages - 1) {
            if (inside(mx, my, xo + 93, yo + 137, 12, 17)) SPR(rb_page_forward_highlighted, xo + 93, yo + 137);
            else SPR(rb_page_forward, xo + 93, yo + 137);
        }
        if (page > 0) {
            if (inside(mx, my, xo + 38, yo + 137, 12, 17)) SPR(rb_page_backward_highlighted, xo + 38, yo + 137);
            else SPR(rb_page_backward, xo + 38, yo + 137);
        }
    }
#endif
}

// GhostSlots.extractRenderState: the ghost recipe's items in the empty grid
// slots (red under, white over), the result in its big slot
void recipebook_draw_ghost(Gfx **gfx, s32 left, s32 top, const ItemStack *grid, int grid_w,
    s32 gx, s32 gy, s32 rx, s32 ry) {
    const McRecipe *rc;
    int x, y, k;

    if (ghost < 0) return;
    rc = &mc_recipes[ghost];
    for (y = 0; y < grid_w; y++) {
        for (x = 0; x < grid_w; x++) {
            const McIngredient *ing = NULL;
            int gi = y * grid_w + x;
            s32 sx = left + gx + x * 18, sy = top + gy + y * 18;
            if (rc->w) {
                if (x < rc->w && y < rc->h) ing = &rc->cells[y * rc->w + x];
            } else if (gi < rc->h) {
                ing = &rc->cells[gi];
            }
            if (ing == NULL || ing->count == 0 || grid[gi].count > 0) continue;
            gui_fill(gfx, sx, sy, 16, 16, 0x30FF0000);
            fake_item(gfx, shown_option(ing), sx, sy);
            gui_fill(gfx, sx, sy, 16, 16, 0x30FFFFFF);
        }
    }
    k = 0;
    for (x = 0; x < grid_w * grid_w; x++) if (grid[x].count > 0) k++;
    gui_fill(gfx, left + rx - 4, top + ry - 4, 24, 24, 0x30FF0000);
    fake_item(gfx, rc->result, left + rx, top + ry);
    gui_fill(gfx, left + rx, top + ry, 16, 16, 0x30FFFFFF);
    (void)k;
}

const char *recipebook_tooltip(void) {
    return hovered_recipe >= 0 ? item_name(mc_recipes[hovered_recipe].result) : NULL;
}

// RecipeBookComponent.mouseClicked. TRUE if the click was the book's
int recipebook_click(s32 w, s32 h, s32 mx, s32 my, int button, int shift, ItemStack *grid, int grid_w) {
    s32 xo = xo_of(w), yo = yo_of(h);
    int t, i, n = 0, pages;

    if (!book_open || button != 0) return book_open && recipebook_contains(w, h, mx, my);
    // the magnifier (xo + 8 up to the box) or the box focuses it, any other
    // click unfocuses it
    if (inside(mx, my, xo + 8, yo + 13, 25 - 8, 14) || inside(mx, my, xo + 25, yo + 13, 81, 14)) {
        search_focus = TRUE;
        return TRUE;
    }
    search_focus = FALSE;
    build_list(grid, grid_w);
    pages = (list_n + PER_PAGE - 1) / PER_PAGE;
    // recipe buttons
    for (i = 0; i < PER_PAGE && page * PER_PAGE + i < list_n; i++) {
        int r = list[page * PER_PAGE + i];
        if (inside(mx, my, xo + 11 + 25 * (i % 5), yo + 31 + 25 * (i / 5), 25, 25)) {
            gather(grid, grid_w * grid_w);
            if (craftable(r)) {
                place(r, grid, grid_w, shift);
                ghost = -1;
            } else {
                // can't make it: GhostSlots (the grid emptied into the inventory first)
                clear_grid(grid, grid_w * grid_w);
                ghost = r;
            }
            return TRUE;
        }
    }
    // page arrows
    if (pages > 1 && page < pages - 1 && inside(mx, my, xo + 93, yo + 137, 12, 17)) {
        page++;
        return TRUE;
    }
    if (pages > 1 && page > 0 && inside(mx, my, xo + 38, yo + 137, 12, 17)) {
        page--;
        return TRUE;
    }
    // the filter
    if (inside(mx, my, xo + 110, yo + 12, 26, 16)) {
        book_filter = !book_filter;
        page = 0;
        return TRUE;
    }
    // tabs
    for (t = 0; t < 5; t++) {
        s32 tx = xo - 30, ty = yo + 3 + 27 * n;
        if (!tab_shown(t, grid_w)) continue;
        n++;
        if (inside(mx, my, tx, ty, 35, 27)) {
            if (tab != t) {
                tab = t;
                page = 0;
            }
            return TRUE;
        }
    }
    return recipebook_contains(w, h, mx, my);
}

// the ghost goes when the grid gets something or the screen closes
void recipebook_clear_ghost(void) {
    ghost = -1;
}
