// minecraft's inventory (e) and crafting table screens, ported from:
//   - InventoryMenu / CraftingMenu slot layouts and quickMoveStack
//   - AbstractContainerMenu.doClick: PICKUP (left/right), QUICK_MOVE (shift),
//     SWAP (number keys over a slot), THROW (outside the window, q over a slot)
//   - ResultSlot.onTake (uses up one of each ingredient), shift-click crafts all
//   - AbstractContainerScreen: layout, hover highlight, carried item, tooltip
//   - the vanilla recipes for what we have: planks, sticks, crafting table,
//     wooden and stone sword/shovel/pickaxe/axe (shaped recipes also match
//     mirrored, like ShapedRecipePattern)
// mouse + keyboard only (needs the mouse companion mod for the cursor).

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "survival.h"
#include "items.h"
#include "gui.h"
#include "mouse.h"
#include "screen.h"
#include "furnace.h"
#include "chest.h"
#include "hotbars.h"
#include "recipebook.h"
#include "advancements.h"
#include "sound.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

enum { SCREEN_NONE, SCREEN_INVENTORY, SCREEN_CRAFTING, SCREEN_CREATIVE, SCREEN_FURNACE, SCREEN_GAMEMODE, SCREEN_CHEST, SCREEN_ADVANCEMENTS };

enum { SLOT_RESULT, SLOT_CRAFT, SLOT_INV, SLOT_NONE, SLOT_PICKER, SLOT_FURNACE, SLOT_DESTROY, SLOT_CHEST, SLOT_ARMOR, SLOT_OFFHAND };

typedef struct {
    u8 kind;
    u8 index; // craft grid index / inventory index
    s16 x, y; // position in the screen (imageWidth x imageHeight space)
} MenuSlot;

static int screen = SCREEN_NONE;
static MenuSlot slots[64];
static int slot_count;
static ItemStack craft[9];
static int craft_w; // 2 or 3
static ItemStack result;
static ItemStack carried;
static int just_closed;

// the window size (imageWidth / imageHeight): 176x166, creative 195x136
static s32 image_w = 176, image_h = 166;
#define IMAGE_W image_w
#define IMAGE_H image_h

// creative (CreativeModeInventoryScreen + ItemPickerMenu, 26.3). the tabs and
// their items are minecraft's own (CreativeModeTabs.java, see mcdata.h).
static ItemStack picker[45];     // CONTAINER: what the 5x9 grid shows
static u8 picker_locked[45];     // CREATIVE_SLOT_LOCK (saved hotbar placeholders)
static Furnace *cur_furnace;     // the open furnace (SCREEN_FURNACE)
static Chest *cur_chest;         // the open chest (SCREEN_CHEST)
static f32 scroll_offs;
static int scrolling;            // dragging the scroller
static int selected_tab = 0;     // static like CreativeModeInventoryScreen.selectedTab (getDefaultTab)
static u16 menu_items[1024];     // ItemPickerMenu.items
static u8 menu_items_locked[1024];
static int menu_item_count;
static char search_text[51];     // the EditBox (maxLength 50)
static int search_len;
static int has_clicked_outside;

static int hovered = -1;
static s32 mouse_x, mouse_y, left_pos, top_pos;

int screen_is_open(void) {
    return screen != SCREEN_NONE;
}

int screen_just_closed(void) {
    return just_closed;
}

void screen_clear_just_closed(void) {
    just_closed = FALSE;
}

// --- recipes ---

// the recipes are minecraft's own crafting_shaped / crafting_shapeless
// recipes (data/minecraft/recipe), generated into mc_recipes (mcdata.h).
// an ingredient is a set of item ids (tags are expanded).

static int ingredient_has(const McIngredient *ing, ItemId id) {
    int i;
    for (i = 0; i < ing->count; i++) {
        if (mc_ingredient_ids[ing->offset + i] == id) return TRUE;
    }
    return FALSE;
}

// ShapedRecipePattern.matches: trim the grid to its used area, then compare
// with the pattern and its mirror
static int match_shaped(const McRecipe *rc, s32 minx, s32 miny, s32 maxx, s32 maxy) {
    int mirror, x, y;

    if (rc->w != maxx - minx + 1 || rc->h != maxy - miny + 1) {
        return FALSE;
    }
    for (mirror = 0; mirror < 2; mirror++) {
        int ok = TRUE;
        for (y = 0; y < rc->h && ok; y++) {
            for (x = 0; x < rc->w && ok; x++) {
                ItemStack *st = &craft[(miny + y) * craft_w + minx + x];
                const McIngredient *want = &rc->cells[y * rc->w + (mirror ? rc->w - 1 - x : x)];
                if (want->count == 0) {
                    ok = st->count == 0;
                } else {
                    ok = st->count > 0 && ingredient_has(want, st->id);
                }
            }
        }
        if (ok) return TRUE;
    }
    return FALSE;
}

// ShapelessRecipe.matches: every item used by exactly one ingredient
static int shapeless_assign(const McRecipe *rc, ItemId *stacks, int n, int k, u16 used) {
    int i;
    if (k == n) return TRUE;
    for (i = 0; i < rc->h; i++) {
        if ((used & (1 << i)) == 0 && ingredient_has(&rc->cells[i], stacks[k])
            && shapeless_assign(rc, stacks, n, k + 1, used | (1 << i))) {
            return TRUE;
        }
    }
    return FALSE;
}

static int match_shapeless(const McRecipe *rc) {
    ItemId stacks[9];
    int n = 0, i;

    for (i = 0; i < craft_w * craft_w; i++) {
        if (craft[i].count > 0) stacks[n++] = craft[i].id;
    }
    return n == rc->h && shapeless_assign(rc, stacks, n, 0, 0);
}

static void update_result(void) {
    s32 minx = 3, miny = 3, maxx = -1, maxy = -1, x, y;
    u32 r;

    result.id = 0;
    result.count = 0;
    result.damage = 0;
    for (y = 0; y < craft_w; y++) {
        for (x = 0; x < craft_w; x++) {
            if (craft[y * craft_w + x].count > 0) {
                if (x < minx) minx = x;
                if (y < miny) miny = y;
                if (x > maxx) maxx = x;
                if (y > maxy) maxy = y;
            }
        }
    }
    if (maxx < 0) {
        return;
    }
    for (r = 0; r < MC_RECIPE_COUNT; r++) {
        const McRecipe *rc = &mc_recipes[r];
        int ok = (rc->w == 0) ? match_shapeless(rc) : match_shaped(rc, minx, miny, maxx, maxy);

        if (ok) {
            result.id = rc->result;
            result.count = rc->count;
            return;
        }
    }
}

// ResultSlot.onTake: one of every ingredient is used up
static void consume_ingredients(void) {
    int i;
    for (i = 0; i < craft_w * craft_w; i++) {
        if (craft[i].count > 0 && --craft[i].count == 0) {
            craft[i].id = 0;
            craft[i].damage = 0;
        }
    }
    update_result();
}

// --- slots ---

static void add_slot(u8 kind, u8 index, s16 x, s16 y) {
    MenuSlot *s = &slots[slot_count++];
    s->kind = kind;
    s->index = index;
    s->x = x;
    s->y = y;
}

// AbstractContainerMenu.addStandardInventorySlots(inventory, 8, 84)
static void add_inventory_slots_at(int top) {
    int x, y;
    for (y = 0; y < 3; y++) {
        for (x = 0; x < 9; x++) {
            add_slot(SLOT_INV, (u8)(x + (y + 1) * 9), 8 + x * 18, top + y * 18);
        }
    }
    for (x = 0; x < 9; x++) {
        add_slot(SLOT_INV, (u8)x, 8 + x * 18, top + 58);
    }
}

static void add_inventory_slots(void) {
    add_inventory_slots_at(84);
}

static void build_slots(void) {
    int x, y;

    slot_count = 0;
    if (screen == SCREEN_CREATIVE && mc_tabs[selected_tab].type == TAB_INVENTORY) {
        // selectTab, INVENTORY: the InventoryMenu's slots moved around.
        // 0-4 (crafting) are off screen, 5-8 armor at 54 + col * 54, 6 + row * 27,
        // 9-35 at (9, 54), 36-44 the hotbar at (9, 112), 45 offhand (35, 20),
        // then the destroy item slot at (173, 112)
        for (x = 0; x < 4; x++) add_slot(SLOT_ARMOR, (u8)x, 54 + (x / 2) * 54, 6 + (x % 2) * 27);
        for (y = 0; y < 3; y++) for (x = 0; x < 9; x++) add_slot(SLOT_INV, (u8)(9 + x + y * 9), 9 + x * 18, 54 + y * 18);
        for (x = 0; x < 9; x++) add_slot(SLOT_INV, (u8)x, 9 + x * 18, 112);
        add_slot(SLOT_OFFHAND, 0, 35, 20);
        add_slot(SLOT_DESTROY, 0, 173, 112);
        return;
    }
    if (screen == SCREEN_CREATIVE) {
        // ItemPickerMenu: 5 rows of 9 at (9, 18), addInventoryHotbarSlots(9, 112)
        for (y = 0; y < 5; y++) for (x = 0; x < 9; x++) add_slot(SLOT_PICKER, (u8)(x + y * 9), 9 + x * 18, 18 + y * 18);
        for (x = 0; x < 9; x++) add_slot(SLOT_INV, (u8)x, 9 + x * 18, 112);
        return;
    }
    if (screen == SCREEN_CHEST) {
        // ChestMenu (3 rows): the chest at (8, 18), the inventory at 103 + (rows - 4) * 18
        for (y = 0; y < 3; y++) for (x = 0; x < 9; x++) add_slot(SLOT_CHEST, (u8)(x + y * 9), 8 + x * 18, 18 + y * 18);
        add_inventory_slots_at(103 + (3 - 4) * 18);
        return;
    }
    if (screen == SCREEN_FURNACE) {
        // AbstractFurnaceMenu: input (56, 17), fuel (56, 53), result (116, 35)
        add_slot(SLOT_FURNACE, 0, 56, 17);
        add_slot(SLOT_FURNACE, 1, 56, 53);
        add_slot(SLOT_FURNACE, 2, 116, 35);
        add_inventory_slots();
        return;
    }
    if (screen == SCREEN_INVENTORY) {
        // InventoryMenu: result (154, 28), 2x2 grid at (98, 18), the 4
        // ArmorSlots (head, chest, legs, feet) at (8, 8 + i * 18), the
        // inventory, then the offhand (45) at (77, 62)
        add_slot(SLOT_RESULT, 0, 154, 28);
        for (y = 0; y < 2; y++) for (x = 0; x < 2; x++) add_slot(SLOT_CRAFT, (u8)(x + y * 2), 98 + x * 18, 18 + y * 18);
        for (x = 0; x < 4; x++) add_slot(SLOT_ARMOR, (u8)x, 8, 8 + x * 18);
        add_inventory_slots();
        add_slot(SLOT_OFFHAND, 0, 77, 62);
    } else {
        // CraftingMenu: result (124, 35), 3x3 grid at (30, 17)
        add_slot(SLOT_RESULT, 0, 124, 35);
        for (y = 0; y < 3; y++) for (x = 0; x < 3; x++) add_slot(SLOT_CRAFT, (u8)(x + y * 3), 30 + x * 18, 17 + y * 18);
        add_inventory_slots();
    }
}

static ItemStack *slot_stack(int i) {
    MenuSlot *s = &slots[i];
    switch (s->kind) {
        case SLOT_RESULT: return &result;
        case SLOT_CRAFT: return &craft[s->index];
        case SLOT_INV: return &gSurv.inv[s->index];
        case SLOT_PICKER: return &picker[s->index];
        case SLOT_FURNACE: return cur_furnace ? &cur_furnace->items[s->index] : NULL;
        case SLOT_CHEST: return cur_chest ? &cur_chest->items[s->index] : NULL;
        case SLOT_ARMOR: return &gSurv.inv[INV_ARMOR(s->index)];
        case SLOT_OFFHAND: return &gSurv.inv[INV_OFFHAND];
    }
    return NULL;
}

static void clear(ItemStack *st) {
    st->id = 0;
    st->count = 0;
    st->damage = 0;
}

static int same_item(const ItemStack *a, const ItemStack *b) {
    return a->id == b->id && a->damage == b->damage;
}

// ArmorSlot.mayPlace: only what's equippable in that slot (one at a time)
static int slot_may_place(int i, ItemId id) {
    if (slots[i].kind == SLOT_ARMOR) {
        return id >= ITEM_FIRST && id < ITEM_END && mc_item_armor[id - ITEM_FIRST].slot == slots[i].index;
    }
    return slots[i].kind != SLOT_RESULT;
}

// AbstractContainerMenu.moveItemStackTo: merge into matching stacks, then
// into empty slots. returns TRUE if anything moved.
static int move_stack_to(ItemStack *st, int start, int end, int reverse) {
    int moved = FALSE, i, max = item_max_stack(st->id);

    if (max > 1) {
        for (i = reverse ? end - 1 : start; st->count > 0 && (reverse ? i >= start : i < end); i += reverse ? -1 : 1) {
            ItemStack *d = slot_stack(i);
            if (d != NULL && slots[i].kind != SLOT_RESULT && d->count > 0 && same_item(d, st) && d->count < max) {
                int n = max - d->count;
                if (n > st->count) n = st->count;
                d->count += n;
                st->count -= n;
                moved = TRUE;
            }
        }
    }
    for (i = reverse ? end - 1 : start; st->count > 0 && (reverse ? i >= start : i < end); i += reverse ? -1 : 1) {
        ItemStack *d = slot_stack(i);
        if (d != NULL && slot_may_place(i, st->id) && d->count == 0) {
            *d = *st;
            clear(st);
            moved = TRUE;
            break;
        }
    }
    if (st->count == 0) clear(st);
    return moved;
}

// InventoryMenu.quickMoveStack / CraftingMenu.quickMoveStack
// AbstractFurnaceMenu.quickMoveStack: 0 input, 1 fuel, 2 result, 3-29 the
// inventory, 30-38 the hotbar
// ChestMenu.quickMoveStack: the chest's slots go to the inventory (from the
// end), the inventory's to the chest
static void quick_move_chest(int i) {
    ItemStack *st = slot_stack(i);
    ItemStack moving;
    if (st == NULL || st->count == 0) return;
    moving = *st;
    if (i < CHEST_SLOTS) move_stack_to(&moving, CHEST_SLOTS, CHEST_SLOTS + 36, TRUE);
    else move_stack_to(&moving, 0, CHEST_SLOTS, FALSE);
    *st = moving;
    if (st->count == 0) clear(st);
}

static void quick_move_furnace(int i) {
    ItemStack *st = slot_stack(i);
    ItemStack moving;
    ItemStack old_input = cur_furnace->items[0];

    if (st == NULL || st->count == 0) return;
    moving = *st;
    if (i == 2) {
        move_stack_to(&moving, 3, 39, TRUE);
        if (moving.count < st->count) furnace_award_xp(cur_furnace, cur_furnace->pos); // onTake
    } else if (i < 2) {
        move_stack_to(&moving, 3, 39, FALSE);
    } else if (furnace_can_smelt(moving.id)) {
        move_stack_to(&moving, 0, 1, FALSE);
    } else if (furnace_is_fuel(moving.id)) {
        move_stack_to(&moving, 1, 2, FALSE);
    } else if (i < 30) {
        move_stack_to(&moving, 30, 39, FALSE);
    } else {
        move_stack_to(&moving, 3, 30, FALSE);
    }
    *st = moving;
    if (st->count == 0) clear(st);
    if (cur_furnace->items[0].id != old_input.id || cur_furnace->items[0].count == 0) {
        ItemStack now = cur_furnace->items[0];
        cur_furnace->items[0] = old_input;
        furnace_set_input(cur_furnace, &now);
    }
}

static int quick_move(int i) {
    ItemStack *st = slot_stack(i);
    ItemStack moving;
    int inv = screen == SCREEN_INVENTORY;
    int main0 = inv ? 9 : 10, hot0 = inv ? 36 : 37, end = inv ? 45 : 46;

    if (st == NULL || st->count == 0) {
        return FALSE;
    }
    moving = *st;
    if (slots[i].kind == SLOT_RESULT) {
        // only craft if the whole result fits (no partial crafts)
        int room = 0, j, max = item_max_stack(moving.id);
        for (j = main0; j < end; j++) {
            ItemStack *d = slot_stack(j);
            if (d->count == 0) room += max;
            else if (same_item(d, &moving)) room += max - d->count;
        }
        if (room < moving.count) {
            return FALSE;
        }
        move_stack_to(&moving, main0, end, TRUE);
        consume_ingredients();
        return TRUE;
    }
    if (inv && (slots[i].kind == SLOT_ARMOR || slots[i].kind == SLOT_OFFHAND)) {
        move_stack_to(&moving, main0, end, FALSE); // armor / offhand -> the inventory
        *st = moving;
        return FALSE;
    }
    if (inv && slots[i].kind == SLOT_INV) {
        // getEquipmentSlotForItem: armor into its empty slot (8 - index = 5 + slot)
        ItemId id = moving.id;
        int a = (id >= ITEM_FIRST && id < ITEM_END) ? mc_item_armor[id - ITEM_FIRST].slot : 255;
        if (a < 4 && gSurv.inv[INV_ARMOR(a)].count == 0) {
            move_stack_to(&moving, 5 + a, 6 + a, FALSE);
            *st = moving;
            return FALSE;
        }
        if (id == ITEM_SHIELD && gSurv.inv[INV_OFFHAND].count == 0) {
            move_stack_to(&moving, 45, 46, FALSE); // the shield: the offhand
            *st = moving;
            return FALSE;
        }
    }
    if (slots[i].kind == SLOT_CRAFT) {
        move_stack_to(&moving, main0, end, FALSE);
    } else if (!inv && !move_stack_to(&moving, 1, 10, FALSE)) {
        if (i < hot0) move_stack_to(&moving, hot0, end, FALSE);
        else move_stack_to(&moving, main0, hot0, FALSE);
    } else if (inv) {
        if (i < hot0) move_stack_to(&moving, hot0, end, FALSE);
        else move_stack_to(&moving, main0, hot0, FALSE);
    }
    *st = moving;
    if (slots[i].kind == SLOT_CRAFT) update_result();
    return FALSE;
}

static void throw_stack(ItemStack *st, int count) {
    ItemStack keep;
    int sel = gSurv.selected;

    if (st->count == 0) return;
    if (count > st->count) count = st->count;
    // reuse the drop code: put the thrown part in a scratch hotbar slot
    keep = gSurv.inv[sel];
    gSurv.inv[sel] = *st;
    gSurv.inv[sel].count = (u8)count;
    surv_drop_selected(TRUE);
    gSurv.inv[sel] = keep;
    st->count -= count;
    if (st->count == 0) clear(st);
}

static void click_slot(int i, int button);

// furnace slots: FurnaceResultSlot can only be taken from, FurnaceFuelSlot
// only takes fuel, and a new input restarts the cooking (setItem)
static void click_furnace(int i, int button) {
    int idx = slots[i].index;
    ItemStack *st = slot_stack(i);
    ItemStack before = *st;

    if (idx == 2 && carried.count > 0 && !(st->count > 0 && same_item(&carried, st))) {
        return; // can't put things in the result slot
    }
    if (idx == 2 && carried.count > 0) {
        // taking more of the same result
        int n = item_max_stack(carried.id) - carried.count;
        if (n > st->count) n = st->count;
        carried.count += n;
        st->count -= n;
        if (st->count == 0) clear(st);
        if (n > 0) furnace_award_xp(cur_furnace, cur_furnace->pos);
        return;
    }
    if (idx == 1 && carried.count > 0 && !furnace_is_fuel(carried.id)) {
        return; // mayPlace: only fuel
    }
    click_slot(-2 - i, button); // plain PICKUP rules for the rest
    if (idx == 2 && before.count > 0 && (st->count < before.count)) {
        furnace_award_xp(cur_furnace, cur_furnace->pos); // FurnaceResultSlot.onTake
    }
    if (idx == 0) {
        ItemStack now = *st;
        *st = before;
        furnace_set_input(cur_furnace, &now);
    }
}

// AbstractContainerMenu.doClick, PICKUP
static void click_slot(int i, int button) {
    ItemStack *st;
    int max;

    if (i <= -2) {
        i = -2 - i; // (from click_furnace: plain pickup rules on a furnace slot)
    } else if (i >= 0 && slots[i].kind == SLOT_FURNACE) {
        click_furnace(i, button);
        return;
    }
    st = slot_stack(i);
    if (st == NULL) return;
    if (carried.count > 0 && !slot_may_place(i, carried.id) && slots[i].kind != SLOT_RESULT) {
        // can't go in here: only taking out (into an empty cursor) works
        return;
    }
    if (slots[i].kind == SLOT_RESULT) {
        // ResultSlot: take it all if the cursor can hold it
        if (result.count == 0) return;
        if (carried.count == 0) {
            carried = result;
            consume_ingredients();
        } else if (same_item(&carried, &result) && carried.count + result.count <= item_max_stack(carried.id)) {
            carried.count += result.count;
            consume_ingredients();
        }
        return;
    }
    if (st->count == 0) {
        if (carried.count > 0) {
            int n = (button == 0) ? carried.count : 1;
            max = item_max_stack(carried.id);
            if (n > max) n = max;
            *st = carried;
            st->count = (u8)n;
            carried.count -= n;
            if (carried.count == 0) clear(&carried);
        }
    } else if (carried.count == 0) {
        int n = (button == 0) ? st->count : (st->count + 1) / 2;
        carried = *st;
        carried.count = (u8)n;
        st->count -= n;
        if (st->count == 0) clear(st);
    } else if (same_item(st, &carried)) {
        max = item_max_stack(st->id);
        if (button == 0) {
            int n = max - st->count;
            if (n > carried.count) n = carried.count;
            st->count += n;
            carried.count -= n;
        } else if (st->count < max) {
            st->count++;
            carried.count--;
        }
        if (carried.count == 0) clear(&carried);
    } else {
        ItemStack t = *st;
        *st = carried;
        carried = t;
    }
    if (slots[i].kind == SLOT_CRAFT) {
        update_result();
        recipebook_clear_ghost(); // (GhostSlots go when the grid changes)
    }
}

// --- creative ---

// CreativeModeTab.shouldDisplay: category tabs only with items
static int tab_shown(int t) {
    return mc_tabs[t].type != TAB_CATEGORY || mc_tabs[t].count > 0;
}

// ItemPickerMenu.calculateRowCount: positiveCeilDiv(items, 9) - 5
static int creative_row_count(void) {
    return (menu_item_count + 8) / 9 - 5;
}

static int creative_can_scroll(void) {
    return (mc_tabs[selected_tab].flags & TABF_CAN_SCROLL) && menu_item_count > 45;
}

// ItemPickerMenu.scrollTo
static void creative_scroll_to(f32 offs) {
    int rows = creative_row_count();
    int row = (int)(offs * rows + 0.5f), x, y;

    if (row < 0) row = 0;
    for (y = 0; y < 5; y++) {
        for (x = 0; x < 9; x++) {
            int i = x + (y + row) * 9;
            ItemStack *st = &picker[x + y * 9];
            st->damage = 0;
            picker_locked[x + y * 9] = FALSE;
            if (i >= 0 && i < menu_item_count && menu_items[i] != 0) {
                st->id = menu_items[i];
                st->count = 1;
                picker_locked[x + y * 9] = menu_items_locked[i];
            } else {
                st->id = 0;
                st->count = 0;
            }
        }
    }
}

static void add_menu_item(u16 id, u8 locked) {
    if (menu_item_count < 1024) {
        menu_items[menu_item_count] = id;
        menu_items_locked[menu_item_count] = locked;
        menu_item_count++;
    }
}

// the creativeNameSearch tree, close enough: the term (lower case) inside
// the item's name or its registry path
static int search_matches(u16 id) {
    const char *names[2];
    int n, k, i;

    names[0] = item_name(id);
    names[1] = mc_registry_names[id];
    for (n = 0; n < 2; n++) {
        const char *h = names[n];
        for (i = 0; h[i] != 0; i++) {
            for (k = 0; k < search_len; k++) {
                char c = h[i + k];
                if (c == 0) break;
                if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (c != search_text[k]) break;
            }
            if (k == search_len) return TRUE;
        }
    }
    return FALSE;
}

// refreshSearchResults
static void refresh_search(void) {
    const McTab *t = &mc_tabs[selected_tab];
    int i;

    menu_item_count = 0;
    for (i = 0; i < t->count; i++) {
        u16 id = mc_tab_items[t->first + i];
        if (search_len == 0 || search_matches(id)) add_menu_item(id, FALSE);
    }
    scroll_offs = 0.0f;
    creative_scroll_to(0.0f);
}

// selectTab
static void creative_select_tab(int t) {
    int old = selected_tab, i, h;

    selected_tab = t;
    menu_item_count = 0;
    if (mc_tabs[t].type == TAB_HOTBAR) {
        // the 9 saved hotbars; an empty one shows a paper placeholder on its own
        // diagonal ("Save hotbar with C + 1"), locked
        for (h = 0; h < 9; h++) {
            if (hotbars_is_empty(h)) {
                for (i = 0; i < 9; i++) add_menu_item(i == h ? ITEM_PAPER : 0, i == h);
            } else {
                for (i = 0; i < 9; i++) add_menu_item(hotbars_get(h, i)->id, FALSE);
            }
        }
    } else if (mc_tabs[t].type == TAB_CATEGORY) {
        for (i = 0; i < mc_tabs[t].count; i++) add_menu_item(mc_tab_items[mc_tabs[t].first + i], FALSE);
    }
    build_slots();
    if (mc_tabs[t].type == TAB_SEARCH) {
        if (old != t) search_len = 0;
        search_text[search_len] = 0;
        refresh_search();
    } else {
        search_len = 0;
        search_text[0] = 0;
    }
    scroll_offs = 0.0f;
    creative_scroll_to(0.0f);
}

// CreativeModeInventoryScreen.slotClicked on a CONTAINER slot
static void click_picker(int i, int button, int quick) {
    ItemStack *clicked = slot_stack(i);

    if (picker_locked[slots[i].index]) {
        return; // !mayPickup
    }
    if (carried.count > 0 && clicked->count > 0 && same_item(&carried, clicked)) {
        if (button == 0) {
            if (quick) {
                carried.count = (u8)item_max_stack(carried.id);
            } else if (carried.count < item_max_stack(carried.id)) {
                carried.count++;
            }
        } else if (--carried.count == 0) {
            clear(&carried);
        }
    } else if (clicked->count > 0 && carried.count == 0) {
        carried = *clicked;
        carried.count = (u8)(quick ? item_max_stack(clicked->id) : clicked->count);
    } else if (button == 0) {
        clear(&carried);
    } else if (carried.count > 0 && --carried.count == 0) {
        clear(&carried);
    }
}

// AbstractContainerMenu.moveItemStackTo on the inventory itself
static void inv_move_to(ItemStack *st, int start, int end) {
    int i, max = item_max_stack(st->id);

    for (i = start; i < end && st->count > 0 && max > 1; i++) {
        ItemStack *d = &gSurv.inv[i];
        if (d->count > 0 && same_item(d, st) && d->count < max) {
            int n = max - d->count;
            if (n > st->count) n = st->count;
            d->count += n;
            st->count -= n;
        }
    }
    for (i = start; i < end && st->count > 0; i++) {
        if (gSurv.inv[i].count == 0) {
            gSurv.inv[i] = *st;
            st->count = 0;
        }
    }
    if (st->count == 0) clear(st);
}

// the creative screen's mouse handling for one click (slotClicked / mouseClicked)
static void creative_click(int b, int quick) {
    int type = mc_tabs[selected_tab].type;

    if (hovered < 0) {
        // outside the window (and not on a tab): THROW the carried stack
        if (type != TAB_INVENTORY && carried.count > 0 && has_clicked_outside) {
            throw_stack(&carried, b == 0 ? carried.count : 1);
        } else if (type == TAB_INVENTORY && carried.count > 0 && has_clicked_outside) {
            throw_stack(&carried, carried.count);
        }
        return;
    }
    if (slots[hovered].kind == SLOT_DESTROY) {
        if (quick) {
            // shift on the bin: the whole inventory goes
            int i;
            for (i = 0; i < INV_TOTAL; i++) clear(&gSurv.inv[i]);
        } else {
            clear(&carried);
        }
        return;
    }
    if (slots[hovered].kind == SLOT_PICKER) {
        click_picker(hovered, b, quick);
        return;
    }
    if (slots[hovered].kind == SLOT_INV) {
        int idx = slots[hovered].index;
        if (quick && type == TAB_INVENTORY) {
            // InventoryMenu.quickMoveStack: inventory <-> hotbar
            ItemStack moving = gSurv.inv[idx];
            clear(&gSurv.inv[idx]);
            if (idx < 9) inv_move_to(&moving, 9, 36);
            else inv_move_to(&moving, 0, 9);
            if (moving.count > 0) gSurv.inv[idx] = moving;
        } else if (quick) {
            // ItemPickerMenu.quickMoveStack: a hotbar slot is emptied
            clear(&gSurv.inv[idx]);
        } else {
            click_slot(hovered, b);
        }
    }
}

// --- open / close ---

static void open_screen(int which) {
    int i;

    screen = which;
    craft_w = (which == SCREEN_CRAFTING) ? 3 : 2;
    image_w = (which == SCREEN_CREATIVE) ? 195 : 176;
    image_h = (which == SCREEN_CREATIVE) ? 136 : (which == SCREEN_CHEST ? 114 + 3 * 18 : 166);
    if (which == SCREEN_CREATIVE) {
        // init: selectTab(selectedTab) again
        scrolling = FALSE;
        search_len = 0;
        search_text[0] = 0;
        creative_select_tab(selected_tab);
    }
    for (i = 0; i < 9; i++) clear(&craft[i]);
    clear(&result);
    clear(&carried);
    build_slots();
}

void screen_open_inventory(void) {
    // creative players get the creative inventory (LocalPlayer.openInventory)
    if (GM_SPECTATING()) return; // spectators have no inventory
    open_screen(gSurv.creative ? SCREEN_CREATIVE : SCREEN_INVENTORY);
}

void screen_open_furnace(s32 pos[3]) {
    cur_furnace = furnace_at(pos, TRUE);
    if (cur_furnace != NULL) {
        open_screen(SCREEN_FURNACE);
    }
}

// ChestBlock.useWithoutItem -> the ChestMenu (startOpen: one more viewer)
void screen_open_chest(s32 pos[3]) {
    cur_chest = chest_at(pos, TRUE);
    if (cur_chest != NULL) {
        // ChestLidController / ContainerOpenersCounter: the first viewer opens it
        if (cur_chest->viewers == 0) sound_play_cell(SND(BLOCK_CHEST_OPEN), pos[0], pos[1], pos[2], 0.5f, sound_random() * 0.1f + 0.9f);
        cur_chest->viewers++;
        open_screen(SCREEN_CHEST);
    }
}

void screen_open_crafting(void) {
    open_screen(SCREEN_CRAFTING);
}

// AbstractContainerMenu.removed + clearContainer: the grid and the cursor go
// back into the inventory, or get dropped if it's full
void screen_close(void) {
    int i;

    if (screen == SCREEN_NONE) return;
    for (i = 0; i < 9; i++) {
        if (craft[i].count > 0) {
            int left = surv_inv_add_stack(&craft[i]);
            if (left > 0) throw_stack(&craft[i], left);
        }
        clear(&craft[i]);
    }
    if (carried.count > 0) {
        int left = surv_inv_add_stack(&carried);
        if (left > 0) throw_stack(&carried, left);
        clear(&carried);
    }
    clear(&result);
    if (screen == SCREEN_CHEST && cur_chest != NULL) {
        cur_chest->viewers--; // stopOpen
        if (cur_chest->viewers == 0) sound_play_cell(SND(BLOCK_CHEST_CLOSE), cur_chest->pos[0], cur_chest->pos[1], cur_chest->pos[2], 0.5f, sound_random() * 0.1f + 0.9f);
        cur_chest = NULL;
    }
    if (screen == SCREEN_ADVANCEMENTS) advancements_set_open(FALSE);
    screen = SCREEN_NONE;
    just_closed = TRUE;
    recipebook_clear_ghost();
}

// --- input (once per game frame) ---

static void update_hover(void) {
    int i;

    gui_update_metrics();
    mouse_x = gMouse.cursor_x / gGui.scale;
    mouse_y = gMouse.cursor_y / gGui.scale;
    left_pos = (gGui.width - IMAGE_W) / 2;
    if (screen == SCREEN_INVENTORY || screen == SCREEN_CRAFTING) {
        left_pos = recipebook_left_pos(gGui.width, IMAGE_W); // updateScreenPosition
    }
    top_pos = (gGui.height - IMAGE_H) / 2;
    hovered = -1;
    for (i = 0; i < slot_count; i++) {
        s32 sx = left_pos + slots[i].x, sy = top_pos + slots[i].y;
        if (slots[i].kind != SLOT_NONE && mouse_x >= sx - 1 && mouse_x < sx + 17 && mouse_y >= sy - 1 && mouse_y < sy + 17) {
            hovered = i;
        }
    }
}

// getTabX / getTabY (relative to leftPos / topPos)
static s32 tab_x(int t) {
    const McTab *tab = &mc_tabs[t];
    if (tab->flags & TABF_ALIGNED_RIGHT) return IMAGE_W - 27 * (7 - tab->col) + 1;
    return 27 * tab->col;
}

static s32 tab_y(int t) {
    return mc_tabs[t].row == 0 ? -32 : IMAGE_H;
}

// checkTabClicked
static int tab_clicked(int t, s32 xm, s32 ym) {
    s32 x = tab_x(t), y = tab_y(t);
    return xm >= x && xm <= x + 26 && ym >= y && ym <= y + 32;
}

// insideScrollbar
static int inside_scrollbar(s32 xm, s32 ym) {
    s32 xscr = left_pos + 175, yscr = top_pos + 18;
    return xm >= xscr && ym >= yscr && xm < xscr + 14 && ym < yscr + 112 && (mc_tabs[selected_tab].flags & TABF_CAN_SCROLL);
}

// the search EditBox's keys (we poll them ourselves: the game gives no text events)
static u8 key_was[64];
static int key_edge(int sc) {
    int down = mouse_key_held(sc) != 0, edge = down && !key_was[sc & 63];
    key_was[sc & 63] = (u8)down;
    return edge;
}

static void search_type(char c) {
    if (search_len < 50) {
        search_text[search_len++] = c;
        search_text[search_len] = 0;
        refresh_search();
    }
}

// creative keyPressed + charTyped: TRUE if the key went into the search box
static int creative_keys(void) {
    int sc, typed = FALSE;

    if (mc_tabs[selected_tab].type != TAB_SEARCH) {
        // keyChat (t): straight to the search tab
        if (key_edge(23)) {
            int t;
            for (t = 0; t < MC_TAB_COUNT; t++) {
                if (mc_tabs[t].type == TAB_SEARCH) creative_select_tab(t);
            }
            return TRUE;
        }
        return FALSE;
    }
    for (sc = 4; sc <= 29; sc++) {           // a-z
        if (key_edge(sc)) { search_type((char)('a' + sc - 4)); typed = TRUE; }
    }
    for (sc = 30; sc <= 39; sc++) {          // 1-9, 0
        int over_item = hovered >= 0 && (slots[hovered].kind != SLOT_PICKER || slot_stack(hovered)->count > 0);
        if (key_edge(sc) && !over_item) { search_type(sc == 39 ? '0' : (char)('1' + sc - 30)); typed = TRUE; }
    }
    if (key_edge(44)) { search_type(' '); typed = TRUE; }
    if (key_edge(45)) { search_type(mouse_key_held(KEY_LSHIFT) ? '_' : '-'); typed = TRUE; }
    if (key_edge(42) && search_len > 0) {   // backspace
        search_text[--search_len] = 0;
        refresh_search();
        typed = TRUE;
    }
    return typed || TRUE; // the focused box captures every key (isInputCaptured)
}

// the recipe book's focused search box takes the keys (EditBox.charTyped /
// keyPressed: backspace)
static int book_typing;

static void book_keys(void) {
    int sc;
    for (sc = 4; sc <= 29; sc++) if (key_edge(sc)) recipebook_type((char)('a' + sc - 4));
    for (sc = 30; sc <= 39; sc++) if (key_edge(sc)) recipebook_type(sc == 39 ? '0' : (char)('1' + sc - 30));
    if (key_edge(44)) recipebook_type(' ');
    if (key_edge(45)) recipebook_type(mouse_key_held(KEY_LSHIFT) ? '_' : '-');
    if (key_edge(42)) recipebook_backspace();
}

static int prev_left_held;

// creative mouse: tabs select on release, the scroller drags, clicks go to slotClicked
static void creative_mouse(void) {
    s32 xm = mouse_x - left_pos, ym = mouse_y - top_pos;
    int b, t;

    for (b = 0; b < 2; b++) {
        int clicks = (b == 0) ? gMouse.left_clicks : gMouse.right_clicks;
        while (clicks-- > 0) {
            int on_tab = FALSE;
            if (b == 0) {
                for (t = 0; t < MC_TAB_COUNT; t++) {
                    if (tab_shown(t) && tab_clicked(t, xm, ym)) on_tab = TRUE;
                }
                if (on_tab) continue; // selected on release
                if (mc_tabs[selected_tab].type != TAB_INVENTORY && inside_scrollbar(mouse_x, mouse_y)) {
                    scrolling = creative_can_scroll();
                    continue;
                }
            }
            // hasClickedOutside: outside the window and not on the selected tab
            has_clicked_outside = (mouse_x < left_pos || mouse_y < top_pos || mouse_x >= left_pos + IMAGE_W
                || mouse_y >= top_pos + IMAGE_H) && !tab_clicked(selected_tab, xm, ym);
            creative_click(b, mouse_key_held(KEY_LSHIFT));
        }
    }
    // mouseReleased
    if (prev_left_held && !gMouse.left_held) {
        scrolling = FALSE;
        for (t = 0; t < MC_TAB_COUNT; t++) {
            if (tab_shown(t) && tab_clicked(t, xm, ym)) {
                creative_select_tab(t);
                break;
            }
        }
    }
    prev_left_held = gMouse.left_held;
    // mouseDragged
    if (scrolling) {
        s32 yscr = top_pos + 18, yscr2 = yscr + 112;
        scroll_offs = ((f32)mouse_y - yscr - 7.5f) / (f32)(yscr2 - yscr - 15);
        if (scroll_offs < 0.0f) scroll_offs = 0.0f;
        if (scroll_offs > 1.0f) scroll_offs = 1.0f;
        creative_scroll_to(scroll_offs);
    }
    // mouseScrolled: subtractInputFromScroll
    if (gMouse.wheel != 0 && creative_can_scroll()) {
        scroll_offs -= (f32)gMouse.wheel / (f32)creative_row_count();
        if (scroll_offs < 0.0f) scroll_offs = 0.0f;
        if (scroll_offs > 1.0f) scroll_offs = 1.0f;
        creative_scroll_to(scroll_offs);
    }
    // CLONE (middle click): a full stack of it on the cursor
    if (gMouse.middle_clicks > 0 && hovered >= 0 && carried.count == 0) {
        ItemStack *st = slot_stack(hovered);
        if (st != NULL && st->count > 0 && !(slots[hovered].kind == SLOT_PICKER && picker_locked[slots[hovered].index])) {
            carried = *st;
            carried.count = (u8)item_max_stack(st->id);
        }
    }
}

// --- GameModeSwitcherScreen (26.3) ---

// GameModeIcon order: CREATIVE, SURVIVAL, ADVENTURE, SPECTATOR
static const int gm_icon_mode[4] = { GM_CREATIVE, GM_SURVIVAL, GM_ADVENTURE, GM_SPECTATOR };
static int gm_hovered;       // currentlyHovered (icon index)
static int gm_first_set;     // setFirstMousePos
static s32 gm_first_x, gm_first_y;

static int gm_icon_of(int mode) {
    int i;
    for (i = 0; i < 4; i++) {
        if (gm_icon_mode[i] == mode) return i;
    }
    return 1;
}

void screen_open_gamemode_switcher(void) {
    // getDefaultSelected: the previous mode, else creative <-> survival
    int def = (gSurv.prev_gamemode >= 0 && gSurv.prev_gamemode != gSurv.gamemode) ? gSurv.prev_gamemode
        : (gSurv.gamemode == GM_CREATIVE ? GM_SURVIVAL : GM_CREATIVE);
    screen = SCREEN_GAMEMODE;
    slot_count = 0;
    gm_hovered = gm_icon_of(def);
    gm_first_set = FALSE;
    just_closed = FALSE;
}

// keyPressed (f4: next), keyReleased (f3: switch and close), the mouse
// hovering a slot once it moved
static void gamemode_frame(void) {
    s32 w, h, i;

    gui_update_metrics();
    w = gGui.width;
    h = gGui.height;
    mouse_x = gMouse.cursor_x / gGui.scale;
    mouse_y = gMouse.cursor_y / gGui.scale;
    if (mouse_key_pressed(KEY_F4)) {
        gm_first_set = FALSE;
        gm_hovered = (gm_hovered + 1) % 4; // getNext
    }
    if (!gm_first_set) {
        gm_first_x = mouse_x;
        gm_first_y = mouse_y;
        gm_first_set = TRUE;
    }
    if (mouse_x != gm_first_x || mouse_y != gm_first_y) {
        for (i = 0; i < 4; i++) {
            s32 sx = w / 2 - (4 * 31 - 5) / 2 + i * 31, sy = h / 2 - 31;
            if (mouse_x >= sx && mouse_y >= sy && mouse_x < sx + 26 && mouse_y < sy + 26) gm_hovered = i;
        }
    }
    if (!mouse_key_held(KEY_F3)) {
        // switchToHoveredGameMode, then setScreen(null)
        surv_set_gamemode(gm_icon_mode[gm_hovered]);
        screen = SCREEN_NONE;
        just_closed = TRUE;
    }
}

// AdvancementsScreen: L (keyAdvancements) or e closes it, Done too
static void advancements_frame(void) {
    gui_update_metrics();
    mouse_x = gMouse.cursor_x / gGui.scale;
    mouse_y = gMouse.cursor_y / gGui.scale;
    if (mouse_key_pressed(KEY_L) || mouse_key_pressed(KEY_E)) {
        screen_close();
        return;
    }
    if (advancements_input(gGui.width, gGui.height, (f32)gMouse.cursor_x / gGui.scale, (f32)gMouse.cursor_y / gGui.scale,
            gMouse.left_clicks, gMouse.left_held, gMouse.wheel)) {
        screen_close();
    }
}

void screen_frame(void) {
    int b, k;

    just_closed = FALSE;
    if (screen == SCREEN_GAMEMODE) {
        gamemode_frame();
        return;
    }
    if (screen == SCREEN_ADVANCEMENTS) {
        advancements_frame();
        return;
    }
    // keyAdvancements (handleKeybinds: only with no screen open)
    if (screen == SCREEN_NONE && mouse_key_pressed(KEY_L)) {
        screen = SCREEN_ADVANCEMENTS;
        advancements_set_open(TRUE);
        return;
    }
    if (screen == SCREEN_CREATIVE) {
        update_hover();
        if (creative_keys() && mc_tabs[selected_tab].type == TAB_SEARCH) {
            mouse_key_pressed(KEY_E); // (the box ate the e)
            creative_mouse();
            goto creative_swaps;
        }
    }
    // RecipeBookComponent.keyPressed: a focused search box captures the keys,
    // keyChat (t) focuses it
    book_typing = FALSE;
    if ((screen == SCREEN_INVENTORY || screen == SCREEN_CRAFTING) && recipebook_visible()) {
        if (recipebook_search_focused()) {
            book_keys();
            book_typing = TRUE;
        } else if (key_edge(23)) {
            recipebook_focus_search();
            book_typing = TRUE;
        }
    }
    // e opens the inventory and closes any screen (esc belongs to the recomp's
    // own menu, so it isn't used)
    if (!book_typing && mouse_key_pressed(KEY_E)) {
        if (screen == SCREEN_NONE) {
            screen_open_inventory();
        } else {
            screen_close();
        }
        return;
    }
    if (screen == SCREEN_NONE) {
        return;
    }
    update_hover();
    if (screen == SCREEN_CREATIVE) {
        creative_mouse();
        goto creative_swaps;
    }

    for (b = 0; b < 2; b++) {
        int clicks = (b == 0) ? gMouse.left_clicks : gMouse.right_clicks;
        while (clicks-- > 0) {
            int outside = mouse_x < left_pos || mouse_y < top_pos || mouse_x >= left_pos + IMAGE_W || mouse_y >= top_pos + IMAGE_H;

            if (screen == SCREEN_INVENTORY || screen == SCREEN_CRAFTING) {
                // the recipe book's ImageButton toggles it; the book's own clicks
                s32 bx = (screen == SCREEN_INVENTORY) ? left_pos + 104 : left_pos + 5;
                s32 by = (screen == SCREEN_INVENTORY) ? gGui.height / 2 - 22 : gGui.height / 2 - 49;
                if (b == 0 && mouse_x >= bx && mouse_x < bx + 20 && mouse_y >= by && mouse_y < by + 18) {
                    sound_play_ui(SND(UI_BUTTON_CLICK), 0.25f, 1.0f); // AbstractButton.playDownSound
                    recipebook_toggle();
                    update_hover();
                    continue;
                }
                if (hovered < 0 && recipebook_click(gGui.width, gGui.height, mouse_x, mouse_y, b, mouse_key_held(KEY_LSHIFT), craft, craft_w)) {
                    sound_play_ui(SND(UI_BUTTON_CLICK), 0.25f, 1.0f); // the book's widgets: playDownSound
                    update_result();
                    continue;
                }
                if (recipebook_contains(gGui.width, gGui.height, mouse_x, mouse_y)) outside = FALSE; // hasClickedOutside
            }
            if (hovered >= 0) {
                if (mouse_key_held(KEY_LSHIFT) && screen == SCREEN_CHEST) {
                    quick_move_chest(hovered);
                } else if (mouse_key_held(KEY_LSHIFT) && screen == SCREEN_FURNACE) {
                    quick_move_furnace(hovered);
                } else if (mouse_key_held(KEY_LSHIFT)) {
                    // QUICK_MOVE (repeats for the result slot: craft them all)
                    if (slots[hovered].kind == SLOT_RESULT) {
                        while (result.count > 0 && quick_move(hovered)) { }
                    } else {
                        quick_move(hovered);
                    }
                } else {
                    click_slot(hovered, b);
                }
            } else if (outside && carried.count > 0) {
                throw_stack(&carried, b == 0 ? carried.count : 1); // THROW
            }
        }
    }

creative_swaps:
    if (book_typing) return; // (the box had the keys: no hotbar swaps or throws)
    // SWAP: number keys over a slot swap it with that hotbar slot
    for (k = 0; k < 9 && hovered >= 0; k++) {
        if (slots[hovered].kind == SLOT_DESTROY || slots[hovered].kind == SLOT_NONE) {
            break;
        }
        if (mouse_key_pressed(KEY_1 + k) && slots[hovered].kind == SLOT_PICKER) {
            // creative SWAP: a full stack of it into that hotbar slot
            ItemStack *st = slot_stack(hovered);
            if (st->count > 0 && !picker_locked[slots[hovered].index]) {
                gSurv.inv[k] = *st;
                gSurv.inv[k].count = (u8)item_max_stack(st->id);
            }
        } else if (mouse_key_pressed(KEY_1 + k) && slots[hovered].kind != SLOT_RESULT) {
            ItemStack *st = slot_stack(hovered);
            ItemStack t = gSurv.inv[k];
            gSurv.inv[k] = *st;
            *st = t;
            if (slots[hovered].kind == SLOT_CRAFT) update_result();
        }
    }
    // creative THROW from the list: one (ctrl: a full stack), the list keeps it
    if (screen == SCREEN_CREATIVE && hovered >= 0 && slots[hovered].kind == SLOT_PICKER && mouse_key_pressed(KEY_Q)
        && !(mc_tabs[selected_tab].type == TAB_SEARCH)) {
        ItemStack st = *slot_stack(hovered);
        if (st.count > 0 && !picker_locked[slots[hovered].index]) {
            st.count = (u8)(mouse_key_held(KEY_LCTRL) ? item_max_stack(st.id) : 1);
            throw_stack(&st, st.count);
        }
    }
    // q over a slot throws one (ctrl: the stack)
    if (hovered >= 0 && mouse_key_pressed(KEY_Q) && carried.count == 0 && slots[hovered].kind != SLOT_RESULT
        && slots[hovered].kind != SLOT_PICKER && slots[hovered].kind != SLOT_DESTROY && slots[hovered].kind != SLOT_NONE) {
        ItemStack *st = slot_stack(hovered);
        throw_stack(st, mouse_key_held(KEY_LCTRL) ? st->count : 1);
        if (slots[hovered].kind == SLOT_CRAFT) update_result();
    }
}

// --- drawing ---

#ifdef MC_HAVE_TEXTURES
// UNSELECTED/SELECTED_TOP_TABS and _BOTTOM_TABS (26x32 sprites), [row * 7 + col]
static const u32 *const tab_sprites[14][2] = {
    { mc_spr_tab_top_unselected_1, mc_spr_tab_top_selected_1 },
    { mc_spr_tab_top_unselected_2, mc_spr_tab_top_selected_2 },
    { mc_spr_tab_top_unselected_3, mc_spr_tab_top_selected_3 },
    { mc_spr_tab_top_unselected_4, mc_spr_tab_top_selected_4 },
    { mc_spr_tab_top_unselected_5, mc_spr_tab_top_selected_5 },
    { mc_spr_tab_top_unselected_6, mc_spr_tab_top_selected_6 },
    { mc_spr_tab_top_unselected_7, mc_spr_tab_top_selected_7 },
    { mc_spr_tab_bottom_unselected_1, mc_spr_tab_bottom_selected_1 },
    { mc_spr_tab_bottom_unselected_2, mc_spr_tab_bottom_selected_2 },
    { mc_spr_tab_bottom_unselected_3, mc_spr_tab_bottom_selected_3 },
    { mc_spr_tab_bottom_unselected_4, mc_spr_tab_bottom_selected_4 },
    { mc_spr_tab_bottom_unselected_5, mc_spr_tab_bottom_selected_5 },
    { mc_spr_tab_bottom_unselected_6, mc_spr_tab_bottom_selected_6 },
    { mc_spr_tab_bottom_unselected_7, mc_spr_tab_bottom_selected_7 },
};

// extractTabButton
static void draw_tab(Gfx **gfx, int t) {
    const McTab *tab = &mc_tabs[t];
    int sel = (t == selected_tab), top = tab->row == 0, col = tab->col > 6 ? 6 : tab->col;
    s32 x = left_pos + tab_x(t);
    s32 y = top_pos - (top ? 28 : -(IMAGE_H - 4));
    ItemStack icon;

    gui_blit(gfx, tab_sprites[tab->row * 7 + col][sel], 26, 32, 0, 0, 26, 32, x, y, 26, 32);
    if (tab->type == TAB_INVENTORY) {
        gui_chest_icon(gfx, x + 13 - 8, y + 16 - 8 + (top ? 1 : -1)); // icon(Blocks.CHEST)
        return;
    }
    icon.id = tab->icon;
    icon.count = 1;
    icon.damage = 0;
    gui_item(gfx, &icon, x + 13 - 8, y + 16 - 8 + (top ? 1 : -1), FALSE);
}

// ItemAttributeModifiers.ATTRIBUTE_MODIFIER_FORMAT: "#.##"
static void fmt_amount(char *out, f32 v) {
    s32 c = (s32)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f)), ip, fp, k = 0, m;
    char tmp[12];
    if (c < 0) { out[k++] = '-'; c = -c; }
    ip = c / 100;
    fp = c % 100;
    m = 0;
    do { tmp[m++] = (char)('0' + ip % 10); ip /= 10; } while (ip > 0);
    while (m > 0) out[k++] = tmp[--m];
    if (fp != 0) {
        out[k++] = '.';
        out[k++] = (char)('0' + fp / 10);
        if (fp % 10) out[k++] = (char)('0' + fp % 10);
    }
    out[k] = 0;
}

// "+%s %s" / "%s %s" with the amount and the attribute's name
static void fmt_modifier(char *out, const char *fmt, const char *amount, const char *name, int lead_space) {
    int k = 0, arg = 0;
    if (lead_space) out[k++] = ' ';
    while (*fmt && k < 60) {
        if (fmt[0] == '%' && fmt[1] == 's') {
            const char *a = arg++ == 0 ? amount : name;
            while (*a && k < 60) out[k++] = *a++;
            fmt += 2;
        } else {
            out[k++] = *fmt++;
        }
    }
    out[k] = 0;
}

// ItemStack.addAttributeTooltips: armor ("When on Head:" + blue "+N"), and
// the main hand's attack damage / speed (dark green, the player's base in)
static int attribute_tooltip(ItemId id, const char **lines, u32 *colors, int n, int max) {
    static char buf[6][64];
    char num[16];
    int b = 0;

    if (id >= ITEM_FIRST && id < ITEM_END && mc_item_armor[id - ITEM_FIRST].slot <= 3 && n + 5 <= max) {
        static const char *const when[4] = { MC_STR_ITEM_MODIFIERS_HEAD, MC_STR_ITEM_MODIFIERS_CHEST,
            MC_STR_ITEM_MODIFIERS_LEGS, MC_STR_ITEM_MODIFIERS_FEET };
        const McArmor *a = &mc_item_armor[id - ITEM_FIRST];
        lines[n] = "";
        colors[n++] = 0xFFFFFF;
        lines[n] = when[a->slot];
        colors[n++] = 0xAAAAAA; // GRAY
        if (a->defense > 0) {
            fmt_amount(num, (f32)a->defense);
            fmt_modifier(buf[b], MC_STR_ATTRIBUTE_MODIFIER_PLUS_0, num, MC_STR_ATTRIBUTE_NAME_ARMOR, FALSE);
            lines[n] = buf[b++];
            colors[n++] = 0x5555FF; // BLUE
        }
        if (a->toughness > 0.0f) {
            fmt_amount(num, a->toughness);
            fmt_modifier(buf[b], MC_STR_ATTRIBUTE_MODIFIER_PLUS_0, num, MC_STR_ATTRIBUTE_NAME_ARMOR_TOUGHNESS, FALSE);
            lines[n] = buf[b++];
            colors[n++] = 0x5555FF;
        }
        if (a->kb_resist > 0.0f) {
            fmt_amount(num, a->kb_resist * 10.0f); // (knockback resistance shows x 10)
            fmt_modifier(buf[b], MC_STR_ATTRIBUTE_MODIFIER_PLUS_0, num, MC_STR_ATTRIBUTE_NAME_KNOCKBACK_RESISTANCE, FALSE);
            lines[n] = buf[b++];
            colors[n++] = 0x5555FF;
        }
    } else if (item_tool_kind(id) != TOOL_NONE && n + 4 <= max) {
        lines[n] = "";
        colors[n++] = 0xFFFFFF;
        lines[n] = MC_STR_ITEM_MODIFIERS_MAINHAND;
        colors[n++] = 0xAAAAAA;
        fmt_amount(num, item_attack_damage(id));
        fmt_modifier(buf[b], MC_STR_ATTRIBUTE_MODIFIER_EQUALS_0, num, MC_STR_ATTRIBUTE_NAME_ATTACK_DAMAGE, TRUE);
        lines[n] = buf[b++];
        colors[n++] = 0x00AA00; // DARK_GREEN (the base modifier)
        fmt_amount(num, 20.0f / item_attack_delay(id));
        fmt_modifier(buf[b], MC_STR_ATTRIBUTE_MODIFIER_EQUALS_0, num, MC_STR_ATTRIBUTE_NAME_ATTACK_SPEED, TRUE);
        lines[n] = buf[b++];
        colors[n++] = 0x00AA00;
    }
    return n;
}

// a tooltip of several lines (TooltipRenderUtil + ClientTextTooltip): 10 px
// per line, 2 more after the first
static void draw_tooltip(Gfx **gfx, const char **lines, const u32 *colors, int n) {
    s32 tw = 0, th = 8, tx = mouse_x + 12, ty = mouse_y - 12, i, y;

    for (i = 0; i < n; i++) {
        s32 w = gui_text_width(lines[i]);
        if (w > tw) tw = w;
    }
    if (n > 1) th = 8 + 2 + (n - 1) * 10;
    gui_blit_nine_slice(gfx, mc_spr_tooltip_background, 100, 100, 9, tx - 12, ty - 12, tw + 24, th + 24);
    gui_blit_nine_slice(gfx, mc_spr_tooltip_frame, 100, 100, 10, tx - 12, ty - 12, tw + 24, th + 24);
    for (i = 0, y = ty; i < n; i++) {
        gui_text(gfx, lines[i], tx, y, colors[i], TRUE);
        y += (i == 0) ? 12 : 10;
    }
}

void screen_draw_tooltip(Gfx **gfx, const char **lines, const u32 *colors, int n) {
    draw_tooltip(gfx, lines, colors, n);
}
#endif

void screen_draw(Gfx **gfx, s32 w, s32 h) {
#ifdef MC_HAVE_TEXTURES
    int i;

    if (screen == SCREEN_NONE) {
        return;
    }
    if (screen == SCREEN_GAMEMODE) {
        // extractBackground (no dimming): the panel, 125x75
        static const char *const names[4] = { MC_STR_GAMEMODE_CREATIVE, MC_STR_GAMEMODE_SURVIVAL,
            MC_STR_GAMEMODE_ADVENTURE, MC_STR_GAMEMODE_SPECTATOR };
        static const ItemId icons[4] = { BLOCK_GRASS_BLOCK, ITEM_IRON_SWORD, ITEM_BURIED_TREASURE_MAP, ITEM_ENDER_EYE };
        s32 xo = w / 2 - 62, yo = h / 2 - 31 - 27, all = 4 * 31 - 5;
        const char *key = MC_STR_DEBUG_GAMEMODES_PRESS_F4;
        char next[32];
        int k2 = 0, n2;
        const char *f = MC_STR_DEBUG_GAMEMODES_SELECT_NEXT;
        s32 kw, nw, tx;

        gui_blit(gfx, mc_gui_gamemode_switcher, 125, 75, 0, 0, 125, 75, xo, yo, 125, 75);
        // the hovered mode's name, white, centred 20 above the slots
        gui_text(gfx, names[gm_hovered], w / 2 - gui_text_width(names[gm_hovered]) / 2, h / 2 - 31 - 20, 0xFFFFFF, TRUE);
        // "[ F4 ] Next" (debug.gamemodes.select_next with the key in aqua)
        while (*f && k2 < 30) {
            if (f[0] == '%' && f[1] == 's') { f += 2; continue; }
            next[k2++] = *f++;
        }
        next[k2] = 0;
        kw = gui_text_width(key);
        nw = gui_text_width(next);
        tx = w / 2 - (kw + nw) / 2;
        gui_text(gfx, key, tx, h / 2 + 5, 0x55FFFF, TRUE);
        gui_text(gfx, next, tx + kw, h / 2 + 5, 0xFFFFFF, TRUE);
        for (n2 = 0; n2 < 4; n2++) {
            s32 sx = w / 2 - all / 2 + n2 * 31, sy = h / 2 - 31;
            ItemStack st;
            gui_blit(gfx, mc_spr_gm_slot, 26, 26, 0, 0, 26, 26, sx, sy, 26, 26);
            if (n2 == gm_hovered) {
                gui_blit(gfx, mc_spr_gm_selection, 26, 26, 0, 0, 26, 26, sx, sy, 26, 26);
            }
            st.id = icons[n2];
            st.count = 1;
            st.damage = 0;
            gui_item(gfx, &st, sx + 5, sy + 5, FALSE);
        }
        return;
    }
    update_hover();

    // Screen.renderTransparentBackground: dark gradient over the world
    gui_fill_screen(gfx, 0xC8101010);

    if (screen == SCREEN_ADVANCEMENTS) {
        advancements_draw(gfx, w, h, mouse_x, mouse_y);
        return;
    }

    if (screen == SCREEN_CREATIVE) {
        // extractBackground: the other tabs, the tab's background, then the
        // scroller, the selected tab on top
        static const u32 *const backgrounds[3] = { mc_gui_creative_items, mc_gui_creative_item_search, mc_gui_creative_inventory };
        int t;
        for (t = 0; t < MC_TAB_COUNT; t++) {
            if (t != selected_tab && tab_shown(t)) draw_tab(gfx, t);
        }
        gui_blit(gfx, backgrounds[mc_tabs[selected_tab].background], 195, 136, 0, 0, 195, 136, left_pos, top_pos, 195, 136);
        if (mc_tabs[selected_tab].type == TAB_SEARCH) {
            // the EditBox at (82, 6), no border, white, cursor "_" at the end
            s32 tx = left_pos + 82, ty = top_pos + 6;
            s32 w = gui_text(gfx, search_text, tx, ty, 0xFFFFFF, TRUE);
            if ((gSteve.tick_count / 6) % 2 == 0) {
                gui_text(gfx, "_", tx + w, ty, 0xFFFFFF, TRUE);
            }
        }
        if (mc_tabs[selected_tab].flags & TABF_CAN_SCROLL) {
            s32 yscr = top_pos + 18;
            s32 sy = yscr + (s32)((112 - 17) * scroll_offs);
            if (creative_can_scroll()) {
                gui_blit(gfx, mc_spr_scroller, 12, 15, 0, 0, 12, 15, left_pos + 175, sy, 12, 15);
            } else {
                gui_blit(gfx, mc_spr_scroller_disabled, 12, 15, 0, 0, 12, 15, left_pos + 175, sy, 12, 15);
            }
        }
        draw_tab(gfx, selected_tab);
        if (mc_tabs[selected_tab].type == TAB_INVENTORY) {
            // extractEntityInInventoryFollowsMouse(73, 6, 105, 49, 20)
            steve_draw_gui(gfx, left_pos + 73, top_pos + 6, left_pos + 105, top_pos + 49, 20.0f, mouse_x, mouse_y);
            gui_restore_state(gfx);
        }
    } else if (screen == SCREEN_CHEST) {
        // ContainerScreen.extractBackground: the rows part, then the inventory part
        gui_blit(gfx, mc_gui_generic_54, 176, 222, 0, 0, 176, 3 * 18 + 17, left_pos, top_pos, 176, 3 * 18 + 17);
        gui_blit(gfx, mc_gui_generic_54, 176, 222, 0, 126, 176, 96, left_pos, top_pos + 3 * 18 + 17, 176, 96);
    } else if (screen == SCREEN_FURNACE) {
        gui_blit(gfx, mc_gui_furnace, 176, 166, 0, 0, 176, 166, left_pos, top_pos, 176, 166);
        // AbstractFurnaceScreen: the flame (lit_progress, 14x14 at 56,36)
        // shrinks from the top, the arrow (burn_progress, 24x16 at 79,34) grows
        if (cur_furnace->lit_remaining > 0) {
            f32 lit = (cur_furnace->lit_total > 0) ? (f32)cur_furnace->lit_remaining / (f32)cur_furnace->lit_total : 0.0f;
            s32 hgt = (s32)(lit * 13.0f + 0.999f) + 1;
            if (lit > 1.0f) hgt = 14;
            gui_blit(gfx, mc_spr_lit_progress, MC_SPR_lit_progress_W, MC_SPR_lit_progress_H, 0, 14 - hgt, 14, hgt,
                left_pos + 56, top_pos + 36 + 14 - hgt, 14, hgt);
        }
        {
            f32 burn = (cur_furnace->cook_total > 0) ? (f32)cur_furnace->cook_timer / (f32)cur_furnace->cook_total : 0.0f;
            s32 wid = (s32)(burn * 24.0f + 0.999f);
            if (wid > 24) wid = 24;
            if (wid > 0) {
                gui_blit(gfx, mc_spr_burn_progress, MC_SPR_burn_progress_W, MC_SPR_burn_progress_H, 0, 0, wid, 16,
                    left_pos + 79, top_pos + 34, wid, 16);
            }
        }
    } else {
        gui_blit(gfx, screen == SCREEN_INVENTORY ? mc_gui_inventory : mc_gui_crafting_table, IMAGE_W, IMAGE_H,
            0, 0, IMAGE_W, IMAGE_H, left_pos, top_pos, IMAGE_W, IMAGE_H);
    }

#define SPR(name, x, y) gui_blit(gfx, mc_spr_##name, MC_SPR_##name##_W, MC_SPR_##name##_H, 0, 0, \
        MC_SPR_##name##_W, MC_SPR_##name##_H, x, y, MC_SPR_##name##_W, MC_SPR_##name##_H)

    if (screen == SCREEN_INVENTORY) {
        // empty armor / offhand slots show their outline icons (ArmorSlot noItemIcon)
        if (gSurv.inv[INV_ARMOR(0)].count == 0) SPR(slot_helmet, left_pos + 8, top_pos + 8);
        if (gSurv.inv[INV_ARMOR(1)].count == 0) SPR(slot_chestplate, left_pos + 8, top_pos + 26);
        if (gSurv.inv[INV_ARMOR(2)].count == 0) SPR(slot_leggings, left_pos + 8, top_pos + 44);
        if (gSurv.inv[INV_ARMOR(3)].count == 0) SPR(slot_boots, left_pos + 8, top_pos + 62);
        if (gSurv.inv[INV_OFFHAND].count == 0) SPR(slot_shield, left_pos + 77, top_pos + 62);
        // the player, following the mouse
        steve_draw_gui(gfx, left_pos + 26, top_pos + 8, left_pos + 75, top_pos + 78, 30.0f, mouse_x, mouse_y);
        gui_restore_state(gfx);
    }

    // recipe book button (ImageButton, 20x18), not functional yet. only the
    // screens with a RecipeBookComponent have one (ContainerScreen doesn't)
    if (screen == SCREEN_INVENTORY || screen == SCREEN_CRAFTING) {
        s32 bx = (screen == SCREEN_INVENTORY) ? left_pos + 104 : left_pos + 5;
        s32 by = (screen == SCREEN_INVENTORY) ? h / 2 - 22 : h / 2 - 49;
        if (mouse_x >= bx && mouse_x < bx + 20 && mouse_y >= by && mouse_y < by + 18) {
            SPR(recipe_button_highlighted, bx, by);
        } else {
            SPR(recipe_button, bx, by);
        }
        recipebook_draw(gfx, w, h, mouse_x, mouse_y, craft, craft_w);
    }

    // slot highlight back (under the item), 24x24 at slot - 4
    if (hovered >= 0) {
        SPR(slot_highlight_back, left_pos + slots[hovered].x - 4, top_pos + slots[hovered].y - 4);
    }
    for (i = 0; i < slot_count; i++) {
        ItemStack *st = slot_stack(i);
        if (st != NULL) {
            gui_item(gfx, st, left_pos + slots[i].x, top_pos + slots[i].y, TRUE);
        }
    }
    // the recipe book's ghost recipe in the grid
    if (screen == SCREEN_INVENTORY) recipebook_draw_ghost(gfx, left_pos, top_pos, craft, craft_w, 98, 18, 154, 28);
    else if (screen == SCREEN_CRAFTING) recipebook_draw_ghost(gfx, left_pos, top_pos, craft, craft_w, 30, 17, 124, 35);
    // and front (over the item)
    if (hovered >= 0) {
        SPR(slot_highlight_front, left_pos + slots[hovered].x - 4, top_pos + slots[hovered].y - 4);
    }

    // titles, 0x404040 without shadow
    if (screen == SCREEN_CHEST) {
        // AbstractContainerScreen labels: title (8, 6), inventory (8, imageHeight - 94)
        gui_text(gfx, "Chest", left_pos + 8, top_pos + 6, 0x404040, FALSE);
        gui_text(gfx, "Inventory", left_pos + 8, top_pos + IMAGE_H - 94, 0x404040, FALSE);
    } else if (screen == SCREEN_FURNACE) {
        gui_text(gfx, "Furnace", left_pos + (IMAGE_W - gui_text_width("Furnace")) / 2, top_pos + 6, 0x404040, FALSE);
        gui_text(gfx, "Inventory", left_pos + 8, top_pos + IMAGE_H - 94, 0x404040, FALSE);
    } else if (screen == SCREEN_CREATIVE) {
        // extractLabels: the tab's name unless hideTitle (-12566464 = 0x404040)
        if (mc_tabs[selected_tab].flags & TABF_SHOW_TITLE) {
            gui_text(gfx, mc_tabs[selected_tab].title, left_pos + 8, top_pos + 6, 0x404040, FALSE);
        }
    } else if (screen == SCREEN_INVENTORY) {
        gui_text(gfx, "Crafting", left_pos + 97, top_pos + 8, 0x404040, FALSE);
    } else {
        gui_text(gfx, "Crafting", left_pos + 29, top_pos + 6, 0x404040, FALSE);
        gui_text(gfx, "Inventory", left_pos + 8, top_pos + IMAGE_H - 94, 0x404040, FALSE);
    }

    // the item on the cursor
    if (carried.count > 0) {
        gui_item(gfx, &carried, mouse_x - 8, mouse_y - 8, TRUE);
    } else if (screen == SCREEN_CREATIVE && hovered < 0) {
        // checkTabHovering: the tab's name over (x + 3, y + 3, 21, 27)
        int t;
        for (t = 0; t < MC_TAB_COUNT; t++) {
            s32 x = left_pos + tab_x(t) + 3, y = top_pos + tab_y(t) + 3;
            if (tab_shown(t) && mouse_x >= x - 1 && mouse_x < x + 21 + 1 && mouse_y >= y - 1 && mouse_y < y + 27 + 1) {
                const char *line = mc_tabs[t].title;
                u32 color = 0xFFFFFF;
                draw_tooltip(gfx, &line, &color, 1);
                break;
            }
        }
    } else if (screen == SCREEN_CREATIVE && slots[hovered].kind == SLOT_DESTROY) {
        const char *line = MC_STR_BIN_SLOT;
        u32 color = 0xFFFFFF;
        draw_tooltip(gfx, &line, &color, 1);
    } else if (hovered >= 0 && slot_stack(hovered) != NULL && slot_stack(hovered)->count > 0) {
        // tooltip (TooltipRenderUtil): 9-slice background + frame, padded 3 + 9
        const char *lines[16];
        u32 colors[16];
        static char info[64];
        ItemId id = slot_stack(hovered)->id;
        int n = 0;

        lines[n] = item_name(id);
        colors[n++] = 0xFFFFFF;
        if (screen == SCREEN_CREATIVE && slots[hovered].kind == SLOT_PICKER && picker_locked[slots[hovered].index]) {
            // the placeholder's ITEM_NAME: "Save hotbar with C + 1" (inventory.hotbarInfo)
            int row = (int)(scroll_offs * creative_row_count() + 0.5f) + slots[hovered].index / 9, k = 0;
            const char *f = MC_STR_HOTBAR_INFO;
            while (*f && k < 60) {
                if (f[0] == '%' && f[1] == '1') { info[k++] = 'C'; f += 4; }
                else if (f[0] == '%' && f[1] == '2') { info[k++] = (char)('1' + row % 9); f += 4; }
                else info[k++] = *f++;
            }
            info[k] = 0;
            lines[0] = info;
        } else if ((n = attribute_tooltip(id, lines, colors, n, 16)) >= 0
            && screen == SCREEN_CREATIVE && !(mc_tabs[selected_tab].type == TAB_CATEGORY && slots[hovered].kind == SLOT_PICKER)) {
            // getTooltipFromContainerItem: every category tab that has it, in blue
            int t, i;
            for (t = 0; t < MC_TAB_COUNT && n < 16; t++) {
                if (mc_tabs[t].type == TAB_SEARCH) continue;
                for (i = 0; i < mc_tabs[t].count; i++) {
                    if (mc_tab_items[mc_tabs[t].first + i] == id) {
                        lines[n] = mc_tabs[t].title;
                        colors[n++] = 0x5555FF;
                        break;
                    }
                }
            }
        }
        draw_tooltip(gfx, lines, colors, n);
    } else if ((screen == SCREEN_INVENTORY || screen == SCREEN_CRAFTING) && recipebook_tooltip() != NULL) {
        const char *line = recipebook_tooltip();
        u32 color = 0xFFFFFF;
        draw_tooltip(gfx, &line, &color, 1);
    }
#endif
}
