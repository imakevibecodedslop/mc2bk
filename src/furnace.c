// furnaces, ported from AbstractFurnaceBlockEntity.serverTick (26.3): fuel
// burns while there's something it can smelt, cookingTimer counts up to the
// recipe's cooking time, the result stacks up in slot 2. the recipes and
// fuel burn times come from minecraft's data (mcdata.h).

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "world.h"
#include "items.h"
#include "furnace.h"
#include "sound.h"
#include "steve.h"
#include "xp.h"

static u32 frng = 0xF00D5EEDu;
static f32 furnace_rand(void) {
    frng = frng * 1664525u + 1013904223u;
    return (f32)(frng >> 8) / 16777216.0f;
}

#define MAX_FURNACES 32
static Furnace furnaces[MAX_FURNACES];

// the single-item recipes: the first smelting recipe whose ingredient matches
static const McSmelting *smelting_for(ItemId id) {
    int i, k;

    if (id == 0) return NULL;
    for (i = 0; i < MC_SMELTING_COUNT; i++) {
        const McSmelting *r = &mc_smelting[i];
        for (k = 0; k < r->ing.count; k++) {
            if (mc_ingredient_ids[r->ing.offset + k] == id) return r;
        }
    }
    return NULL;
}

// FuelValues.burnDuration
static int burn_duration(ItemId id) {
    int i;
    for (i = 0; i < MC_FUEL_COUNT; i++) {
        if (mc_fuels[i].item == id) return mc_fuels[i].ticks;
    }
    return 0;
}

int furnace_is_fuel(ItemId id) {
    return id != 0 && burn_duration(id) > 0;
}

int furnace_can_smelt(ItemId id) {
    return smelting_for(id) != NULL;
}

Furnace *furnace_at(s32 pos[3], int create) {
    int i, free_slot = -1;

    for (i = 0; i < MAX_FURNACES; i++) {
        Furnace *f = &furnaces[i];
        if (f->used && f->pos[0] == pos[0] && f->pos[1] == pos[1] && f->pos[2] == pos[2]) return f;
        if (!f->used && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return NULL;
    {
        Furnace *f = &furnaces[free_slot];
        int k;
        f->used = TRUE;
        for (k = 0; k < 3; k++) {
            f->pos[k] = pos[k];
            f->items[k].id = 0;
            f->items[k].count = 0;
            f->items[k].damage = 0;
        }
        f->lit_remaining = f->lit_total = 0;
        f->cook_timer = 0;
        f->cook_total = 200;
        return f;
    }
}

void furnace_set_input(Furnace *f, const ItemStack *st) {
    int same = f->items[0].count > 0 && st->count > 0 && f->items[0].id == st->id;

    f->items[0] = *st;
    if (!same) {
        const McSmelting *r = smelting_for(st->id);
        f->cook_total = r ? r->time : 200; // getTotalCookTime
        f->cook_timer = 0;
    }
}

void furnaces_clear(void) {
    int i;
    for (i = 0; i < MAX_FURNACES; i++) furnaces[i].used = FALSE;
}

// AbstractFurnaceBlockEntity.canBurn
static int can_burn(Furnace *f, const McSmelting *r) {
    ItemStack *res = &f->items[2];
    int max;

    if (res->count == 0) return TRUE;
    if (res->id != r->result) return FALSE;
    max = item_max_stack(r->result);
    if (max > 64) max = 64;
    return res->count + r->count <= max;
}

// setRecipeUsed: recipesUsed.addTo(recipe, 1)
static void record_used(Furnace *f, const McSmelting *r) {
    u16 idx = (u16)(r - mc_smelting) + 1;
    int i;
    for (i = 0; i < 4; i++) {
        if (f->used_recipe[i] == idx) { f->used_count[i]++; return; }
    }
    for (i = 0; i < 4; i++) {
        if (f->used_recipe[i] == 0) { f->used_recipe[i] = idx; f->used_count[i] = 1; return; }
    }
    f->used_count[3]++; // (more than 4 different recipes: lumped into the last)
}

// getRecipesToAwardAndPopExperience -> createExperience per recipe:
// floor(amount * xp) plus one more with chance frac(amount * xp)
void furnace_award_xp(Furnace *f, s32 pos[3]) {
    f32 c[3] = { (pos[0] + 0.5f) * MC_BLOCK, (pos[1] + 0.5f) * MC_BLOCK, (pos[2] + 0.5f) * MC_BLOCK };
    int i;
    for (i = 0; i < 4; i++) {
        if (f->used_recipe[i] != 0) {
            f32 v = (f32)f->used_count[i] * mc_smelting[f->used_recipe[i] - 1].experience;
            s32 whole = (s32)v;
            f32 frac = v - (f32)whole;
            if (frac != 0.0f && furnace_rand() < frac) whole++;
            xp_award(c, whole);
        }
        f->used_recipe[i] = 0;
        f->used_count[i] = 0;
    }
}

static void tick_one(Furnace *f) {
    int is_lit;
    ItemStack *fuel = &f->items[1], *input = &f->items[0];
    int has_input = input->count > 0, has_fuel = fuel->count > 0;

    if (f->lit_remaining > 0) {
        f->lit_remaining--;
        is_lit = f->lit_remaining > 0;
    } else {
        is_lit = FALSE;
    }

    if (is_lit || (has_fuel && has_input)) {
        const McSmelting *r = has_input ? smelting_for(input->id) : NULL;

        if (!has_input) {
            f->cook_timer = 0;
        } else if (r != NULL) {
            if (can_burn(f, r)) {
                if (!is_lit) {
                    int t = burn_duration(fuel->id);
                    f->lit_remaining = t;
                    f->lit_total = t;
                    if (f->cook_total > 0 && f->cook_timer < f->cook_total) {
                        f32 ratio = (f32)f->cook_timer / (f32)f->cook_total;
                        f->cook_total = r->time;
                        f->cook_timer = (s32)(ratio * f->cook_total + 0.999f);
                    }
                    if (t > 0) {
                        // consumeFuel (no buckets yet: nothing is left behind)
                        if (--fuel->count == 0) {
                            fuel->id = 0;
                            fuel->damage = 0;
                        }
                        is_lit = TRUE;
                    }
                }
                if (is_lit) {
                    if (++f->cook_timer >= f->cook_total) {
                        f->cook_timer = 0;
                        f->cook_total = r->time;
                        // burn: the result grows, one input is used
                        if (f->items[2].count == 0) {
                            f->items[2].id = r->result;
                            f->items[2].count = r->count;
                            f->items[2].damage = 0;
                        } else {
                            f->items[2].count += r->count;
                        }
                        record_used(f, r);
                        if (--input->count == 0) {
                            input->id = 0;
                            input->damage = 0;
                        }
                    }
                } else {
                    f->cook_timer = 0;
                }
            } else {
                f->cook_timer = 0;
            }
        }
    } else if (f->cook_timer > 0) {
        f->cook_timer -= 2;
        if (f->cook_timer < 0) f->cook_timer = 0;
        if (f->cook_timer > f->cook_total) f->cook_timer = f->cook_total;
    }
}

void furnaces_tick(void) {
    int i;
    for (i = 0; i < MAX_FURNACES; i++) {
        if (furnaces[i].used) {
            tick_one(&furnaces[i]);
            // FurnaceBlock.animateTick (lit: 0.1 of the times ClientLevel's
            // animateTick picks the block, ~0.021 a tick near the player)
            if (furnaces[i].lit_remaining > 0 && sound_random() < 0.0021f) {
                s32 *p = furnaces[i].pos;
                sound_play_cell(SND(BLOCK_FURNACE_FIRE_CRACKLE), p[0], p[1], p[2], 1.0f, 1.0f);
            }
        }
    }
}

// Containers.dropContents when the block goes: done by survival.c through
// surv_inv_add-like drops, so here the contents are only handed back
void furnace_removed(s32 pos[3]) {
    Furnace *f = furnace_at(pos, FALSE);
    if (f != NULL) f->used = FALSE;
}
