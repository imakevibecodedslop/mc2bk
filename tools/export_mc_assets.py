#!/usr/bin/env python3
# exports the minecraft art the mod uses from YOUR OWN minecraft client jar
# into generated/mc_textures.h (gitignored, never commit it).
#
#   python3 tools/export_mc_assets.py [path/to/minecraft-client.jar]
#
# with no path it looks in the usual places (fabric loom cache, ~/.minecraft).
# needs pillow: sudo dnf install python3-pillow
#
# textures become n64 rgba5551 (16 bit) arrays. grass and leaves are grayscale
# in the jar and tinted per biome by the game, so the plains biome tint is
# baked in here (BiomeColors: grass 0x91BD59, foliage 0x77AB2F).

MOB_DROPS = 5  # mc_loot_* rows (survival.c MOB_DROPS)
import glob
import io
import json
import os
import sys
import zipfile

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mc_data  # noqa: E402
import mc_models  # noqa: E402
import mc_families  # noqa: E402
mc_families_sides = mc_families.MULTIPART_SIDES

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "generated")
OUT = os.path.join(OUT_DIR, "mc_textures.h")

GRASS_TINT = (0x91, 0xBD, 0x59)
FOLIAGE_TINT = (0x77, 0xAB, 0x2F)

HUD_SPRITES = [
    ("hotbar", "hotbar"),
    ("hotbar_selection", "hotbar_selection"),
    ("crosshair", "crosshair"),
    ("heart_container", "heart/container"),
    ("heart_full", "heart/full"),
    ("heart_half", "heart/half"),
    ("food_empty", "food_empty"),
    ("food_full", "food_full"),
    ("food_half", "food_half"),
    ("xp_bg", "experience_bar_background"),
    ("xp_progress", "experience_bar_progress"),
    ("heart_container_blinking", "heart/container_blinking"),
    ("heart_full_blinking", "heart/full_blinking"),
    ("heart_half_blinking", "heart/half_blinking"),
    ("air", "air"),
    ("air_bursting", "air_bursting"),
    ("air_empty", "air_empty"),
    ("attack_full", "crosshair_attack_indicator_full"),
    ("attack_bg", "crosshair_attack_indicator_background"),
    ("attack_progress", "crosshair_attack_indicator_progress"),
    ("armor_empty", "armor_empty"),
    ("armor_half", "armor_half"),
    ("armor_full", "armor_full"),
]

# EquipmentAssets with a humanoid layer, in the mod's order (McArmor.asset)
ARMOR_ASSETS = ["leather", "copper", "chainmail", "iron", "gold", "diamond", "turtle_scute", "netherite"]

# 26.3 container sprites (textures/gui/sprites/...)
GUI_SPRITES = [
    ("slot_highlight_back", "container/slot_highlight_back"),
    ("slot_highlight_front", "container/slot_highlight_front"),
    ("slot_helmet", "container/slot/helmet"),
    ("slot_chestplate", "container/slot/chestplate"),
    ("slot_leggings", "container/slot/leggings"),
    ("slot_boots", "container/slot/boots"),
    ("slot_shield", "container/slot/shield"),
    ("recipe_button", "recipe_book/button"),
    ("recipe_button_highlighted", "recipe_book/button_highlighted"),
    ("tooltip_background", "tooltip/background"),
    ("tooltip_frame", "tooltip/frame"),
    ("scroller", "container/creative_inventory/scroller"),
    ("gm_slot", "gamemode_switcher/slot"),
    ("gm_selection", "gamemode_switcher/selection"),
    ("tab_top_selected_1", "container/creative_inventory/tab_top_selected_1"),
    ("tab_top_selected_2", "container/creative_inventory/tab_top_selected_2"),
    ("tab_top_selected_3", "container/creative_inventory/tab_top_selected_3"),
    ("tab_top_selected_4", "container/creative_inventory/tab_top_selected_4"),
    ("tab_top_selected_5", "container/creative_inventory/tab_top_selected_5"),
    ("tab_top_selected_6", "container/creative_inventory/tab_top_selected_6"),
    ("tab_top_selected_7", "container/creative_inventory/tab_top_selected_7"),
    ("tab_top_unselected_1", "container/creative_inventory/tab_top_unselected_1"),
    ("tab_top_unselected_2", "container/creative_inventory/tab_top_unselected_2"),
    ("tab_top_unselected_3", "container/creative_inventory/tab_top_unselected_3"),
    ("tab_top_unselected_4", "container/creative_inventory/tab_top_unselected_4"),
    ("tab_top_unselected_5", "container/creative_inventory/tab_top_unselected_5"),
    ("tab_top_unselected_6", "container/creative_inventory/tab_top_unselected_6"),
    ("tab_top_unselected_7", "container/creative_inventory/tab_top_unselected_7"),
    ("tab_bottom_selected_1", "container/creative_inventory/tab_bottom_selected_1"),
    ("tab_bottom_selected_2", "container/creative_inventory/tab_bottom_selected_2"),
    ("tab_bottom_selected_3", "container/creative_inventory/tab_bottom_selected_3"),
    ("tab_bottom_selected_4", "container/creative_inventory/tab_bottom_selected_4"),
    ("tab_bottom_selected_5", "container/creative_inventory/tab_bottom_selected_5"),
    ("tab_bottom_selected_6", "container/creative_inventory/tab_bottom_selected_6"),
    ("tab_bottom_selected_7", "container/creative_inventory/tab_bottom_selected_7"),
    ("tab_bottom_unselected_1", "container/creative_inventory/tab_bottom_unselected_1"),
    ("tab_bottom_unselected_2", "container/creative_inventory/tab_bottom_unselected_2"),
    ("tab_bottom_unselected_3", "container/creative_inventory/tab_bottom_unselected_3"),
    ("tab_bottom_unselected_4", "container/creative_inventory/tab_bottom_unselected_4"),
    ("tab_bottom_unselected_5", "container/creative_inventory/tab_bottom_unselected_5"),
    ("tab_bottom_unselected_6", "container/creative_inventory/tab_bottom_unselected_6"),
    ("tab_bottom_unselected_7", "container/creative_inventory/tab_bottom_unselected_7"),
    ("burn_progress", "container/furnace/burn_progress"),
    ("lit_progress", "container/furnace/lit_progress"),
    ("scroller_disabled", "container/creative_inventory/scroller_disabled"),
    # RecipeBookComponent / RecipeBookPage / RecipeButton / RecipeBookTabButton
    ("rb_tab", "recipe_book/tab"),
    ("rb_tab_selected", "recipe_book/tab_selected"),
    ("rb_slot_craftable", "recipe_book/slot_craftable"),
    ("rb_slot_uncraftable", "recipe_book/slot_uncraftable"),
    ("rb_page_forward", "recipe_book/page_forward"),
    ("rb_page_forward_highlighted", "recipe_book/page_forward_highlighted"),
    ("rb_page_backward", "recipe_book/page_backward"),
    ("rb_page_backward_highlighted", "recipe_book/page_backward_highlighted"),
    ("rb_filter_enabled", "recipe_book/filter_enabled"),
    ("rb_filter_disabled", "recipe_book/filter_disabled"),
    ("rb_filter_enabled_highlighted", "recipe_book/filter_enabled_highlighted"),
    ("rb_filter_disabled_highlighted", "recipe_book/filter_disabled_highlighted"),
    # AdvancementsScreen / AdvancementWidget / AdvancementTabType, toasts
    ("adv_box_obtained", "advancements/box_obtained"),
    ("adv_box_unobtained", "advancements/box_unobtained"),
    ("adv_title_box", "advancements/title_box"),
    ("adv_task_frame_obtained", "advancements/task_frame_obtained"),
    ("adv_task_frame_unobtained", "advancements/task_frame_unobtained"),
    ("adv_goal_frame_obtained", "advancements/goal_frame_obtained"),
    ("adv_goal_frame_unobtained", "advancements/goal_frame_unobtained"),
    ("adv_challenge_frame_obtained", "advancements/challenge_frame_obtained"),
    ("adv_challenge_frame_unobtained", "advancements/challenge_frame_unobtained"),
    ("adv_tab_above_left", "advancements/tab_above_left"),
    ("adv_tab_above_middle", "advancements/tab_above_middle"),
    ("adv_tab_above_right", "advancements/tab_above_right"),
    ("adv_tab_above_left_selected", "advancements/tab_above_left_selected"),
    ("adv_tab_above_middle_selected", "advancements/tab_above_middle_selected"),
    ("adv_tab_above_right_selected", "advancements/tab_above_right_selected"),
    ("toast_advancement", "toast/advancement"),
    ("toast_recipe", "toast/recipe"),
    ("button", "widget/button"),
    ("text_field", "widget/text_field"),
    ("text_field_highlighted", "widget/text_field_highlighted"),
    ("button_highlighted", "widget/button_highlighted"),
]




def launcher_dirs():
    """the official launcher's and prism launcher's folders on linux, windows and macos"""
    home = os.path.expanduser("~")
    appdata = os.environ.get("APPDATA", os.path.join(home, "AppData", "Roaming"))
    mac = os.path.join(home, "Library", "Application Support")
    return {
        "official": [os.path.join(home, ".minecraft"), os.path.join(appdata, ".minecraft"), os.path.join(mac, "minecraft")],
        "prism": [os.path.join(home, ".local/share/PrismLauncher"), os.path.join(appdata, "PrismLauncher"),
                  os.path.join(mac, "PrismLauncher")],
    }


def find_jar():
    home = os.path.expanduser("~")
    patterns = [home + "/.gradle/caches/fabric-loom/26.3/minecraft-client.jar"]
    for d in launcher_dirs()["official"]:
        patterns.append(os.path.join(d, "versions", "26.3", "26.3.jar"))
    for d in launcher_dirs()["prism"]:
        patterns.append(os.path.join(d, "libraries", "com", "mojang", "minecraft", "26.3", "minecraft-26.3-client.jar"))
    patterns += [
        home + "/.gradle/caches/fabric-loom/*/minecraft-client.jar",
        ROOT + "/../mc-src/.gradle/loom-cache/**/minecraft-clientOnly-*.jar",
    ]
    for pat in patterns:
        found = sorted(p for p in glob.glob(pat, recursive=True) if "sources" not in p)
        if found:
            return found[-1]
    return None


def load(z, path):
    return Image.open(io.BytesIO(z.read(path))).convert("RGBA")


def tint(img, color):
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = px[x, y]
            px[x, y] = (r * color[0] // 255, g * color[1] // 255, b * color[2] // 255, a)
    return img


def rgba5551(img):
    out = []
    data = img.tobytes()
    for i in range(0, len(data), 4):
        r, g, b, a = data[i:i + 4]
        out.append(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (1 if a >= 128 else 0))
    return out


def rgba8888(img):
    data = img.tobytes()
    return [(data[i] << 24) | (data[i + 1] << 16) | (data[i + 2] << 8) | data[i + 3] for i in range(0, len(data), 4)]


def c_array32(name, values):
    lines = []
    for i in range(0, len(values), 8):
        lines.append("    " + ", ".join("0x%08X" % v for v in values[i:i + 8]) + ",")
    return "static u32 %s[%d] __attribute__((aligned(8))) = {\n%s\n};\n" % (name, len(values), "\n".join(lines))


def c_array(name, values):
    lines = []
    for i in range(0, len(values), 16):
        lines.append("    " + ", ".join("0x%04X" % v for v in values[i:i + 16]) + ",")
    return "static u16 %s[%d] __attribute__((aligned(8))) = {\n%s\n};\n" % (name, len(values), "\n".join(lines))


DATA_OUT = os.path.join(OUT_DIR, "mc_data.h")
TOOL_KIND_IDS = {"sword": 1, "shovel": 2, "pickaxe": 3, "axe": 4, "hoe": 5}


def c_ident(name):
    return name.upper().replace("-", "_")


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def write_data_header(blocks, block_faces, tex_opaque, items, recipes, lang, smelting, fuel, food, tabs):
    """generated/mc_data.h: the tables src/mcdata.h describes"""
    nb = len(blocks)
    ids = {}
    for i, b in enumerate(blocks):
        ids[b["name"]] = 1 + i
    first_item = nb + 1
    for i, it in enumerate(items):
        ids[it["name"]] = first_item + i

    out = ["// generated by tools/export_mc_assets.py from minecraft's data. do NOT commit.\n",
           "#define MC_BLOCK_COUNT %d\n" % (nb + 1),
           "#define MC_ITEM_FIRST %d\n" % first_item,
           "#define MC_ITEM_END %d\n" % (first_item + len(items)),
           "#define BLOCK_AIR 0\n"]
    for b in blocks:
        out.append("#define BLOCK_%s %d\n" % (c_ident(b["name"]), ids[b["name"]]))
    for it in items:
        out.append("#define ITEM_%s %d\n" % (c_ident(it["name"]), ids[it["name"]]))

    # registry names by id (save files store these, so ids can change
    # between exports without scrambling inventories)
    reg = [""] + [b["name"] for b in blocks] + [it["name"] for it in items]
    out.append("static const char *const mc_registry_names[MC_ITEM_END] = {\n%s\n};\n"
               % ",\n".join("    " + c_str(n) for n in reg))

    # Item.getName: the item's description id is item.minecraft.x, but block
    # items (doors, signs, saplings...) use their block's block.minecraft.x
    def display(name, kind):
        first = "%s.minecraft.%s" % (kind, name)
        other = "%s.minecraft.%s" % ("item" if kind == "block" else "block", name)
        return lang.get(first) or lang.get(other) or name

    mats = mc_data.MATERIALS
    out.append("static const McBlock mc_blocks[MC_BLOCK_COUNT] = {\n")
    out.append('    { "Air", { 0, 0, 0, 0, 0, 0 }, 0.0f, 0, 0, { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } } },\n')
    for b, faces in zip(blocks, block_faces):
        flags = []
        if b["place"] == 5:
            flags.append("MCB_MODEL")
            if b.get("opaque_model"):
                flags.append("MCB_OPAQUE")
        elif all(tex_opaque[t] for t in faces) and not b.get("fluid") and not b.get("chest"):
            flags.append("MCB_OPAQUE")
        if b.get("no_collision"):
            flags.append("MCB_NO_COLLISION")
        if b.get("fluid"):
            flags.append("MCB_FLUID")
        if b.get("chest"):
            flags.append("MCB_CHEST")
        if b.get("washed"):
            flags.append("MCB_WASHED")
        if b.get("shapes") is not None:
            flags.append("MCB_TEX_SLOTS")
        if b["requires_tool"]:
            flags.append("MCB_REQUIRES_TOOL")
        if b["sword_efficient"]:
            flags.append("MCB_SWORD_EFFICIENT")
        for k in b["mine"]:
            flags.append("MCB_MINE_" + k.upper())
        if b["hardness"] < 0:
            flags.append("MCB_UNBREAKABLE")
        incorrect = 0
        for m in b["incorrect"]:
            incorrect |= 1 << mats.index(m)
        drops = []
        for d in b["drops"]:
            drops.append("{ %d, %d, %d, %d }" % (ids[d[0]], max(0, d[1]), max(0, d[2]), min(255, int(round(d[3] * 255)))))
        while len(drops) < 2:
            drops.append("{ 0, 0, 0, 0 }")
        out.append("    { %s, { %s }, %.3ff, %s, 0x%02X, { %s } },\n" % (
            c_str(display(b["name"], "block")), ", ".join(str(t) for t in faces), max(b["hardness"], 0.0),
            " | ".join(flags) or "0", incorrect, ", ".join(drops)))
    out.append("};\n")

    # BlockItem.getBlock for items named differently (redstone -> redstone_wire)
    # and StandingAndWallBlockItem's wall block (torches)
    out.append("#define MC_ITEM_PLACE_COUNT %d\n" % len(mc_data.ITEM_PLACES))
    out.append("static const u16 mc_item_places[%d][3] = { %s };\n" % (max(1, len(mc_data.ITEM_PLACES)),
               ", ".join("{ %d, %d, %d }" % (ids[i], ids[b], ids[w] if w else 0) for i, b, w in mc_data.ITEM_PLACES) or "{ 0 }"))
    # FireBlock: ignite / burn odds per block
    out.append("static const u8 mc_block_flame[MC_BLOCK_COUNT][2] = { { 0, 0 }, %s };\n"
               % ", ".join("{ %d, %d }" % mc_data.FLAMMABLE.get(b["name"], (0, 0)) for b in blocks))
    # Block.getExplosionResistance (Properties.strength / explosionResistance)
    out.append("static const f32 mc_block_resist[MC_BLOCK_COUNT] = { 0.0f, %s };\n"
               % ", ".join("%.1ff" % min(b.get("resist", 0.0), 3600000.0) for b in blocks))
    # Block.spawnAfterBreak -> tryDropExperience: UniformInt xp range per block
    out.append("static const u8 mc_block_xp[MC_BLOCK_COUNT][2] = { { 0, 0 }, %s };\n"
               % ", ".join("{ %d, %d }" % tuple(b["xp"]) for b in blocks))
    # block states: placement rule + the 6 face textures of each state
    out.append("static const u8 mc_block_place[MC_BLOCK_COUNT] = { 0, %s };\n"
               % ", ".join(str(b["place"]) for b in blocks))
    rows = []
    for b in blocks:
        sts = b.get("state_tex") or []
        cells = []
        for i in range(6):
            t = sts[i] if i < len(sts) else block_faces[blocks.index(b)]
            cells.append("{ %s }" % ", ".join(str(x) for x in t))
        rows.append("    { %s },\n" % ", ".join(cells))
    out.append("static const u16 mc_state_tex[MC_BLOCK_COUNT][6][6] = {\n    { { 0 } },\n%s};\n" % "".join(rows))
    # quarter turns of each face's texture (the model's face rotation carried
    # through the blockstate's x/y rotation)
    rows = []
    for b in blocks:
        srs = b.get("state_rot") or []
        cells = ["{ %s }" % ", ".join(str(x) for x in (srs[i] if i < len(srs) else [0] * 6)) for i in range(6)]
        rows.append("    { %s },\n" % ", ".join(cells))
    out.append("static const u8 mc_state_rot[MC_BLOCK_COUNT][6][6] = {\n    { { 0 } },\n%s};\n" % "".join(rows))

    out.append("static const McItem mc_items[MC_ITEM_END - MC_ITEM_FIRST + 1] = {\n")
    for it in items:
        t = it["tool"]
        if t:
            out.append("    { %s, %d, %d, %d, %d, %d, %.3ff, %.3ff, %.3ff },\n" % (
                c_str(display(it["name"], "item")), it["stack"], it["durability"], TOOL_KIND_IDS[t["kind"]],
                mats.index(t["material"]) if t["material"] in mats else 0, 1, t["speed"], t["damage"], t["attack_speed"]))
        elif it.get("weapon"):
            w = it["weapon"]  # spears: no mining kind, but a weapon's stats
            out.append("    { %s, %d, %d, 0, %d, %d, 1.0f, %.3ff, %.3ff },\n" % (
                c_str(display(it["name"], "item")), it["stack"], it["durability"],
                mats.index(w["material"]) if w["material"] in mats else 0, 1 if it["handheld"] else 0, w["damage"], w["attack_speed"]))
        else:
            out.append("    { %s, %d, %d, 0, 0, %d, 1.0f, 1.0f, 4.0f },\n" % (
                c_str(display(it["name"], "item")), it["stack"], it["durability"], 1 if it["handheld"] else 0))
    out.append('    { "", 0, 0, 0, 0, 0, 0, 0, 0 },\n};\n')

    # armor (Equippable + ArmorMaterial): slot 0 head .. 3 feet, 255 = not armor
    out.append("static const McArmor mc_item_armor[MC_ITEM_END - MC_ITEM_FIRST + 1] = {\n")
    for it in items:
        a = it.get("armor")
        if a and a["asset"] in ARMOR_ASSETS:
            out.append("    { %d, %d, %.2ff, %.2ff, %d },\n" % (a["slot"], a["defense"], a["toughness"], a["kb"], ARMOR_ASSETS.index(a["asset"])))
        else:
            out.append("    { 255, 0, 0.0f, 0.0f, 0 },\n")
    out.append("    { 255, 0, 0.0f, 0.0f, 0 },\n};\n")

    # recipes: ingredient sets are deduped lists of ids
    sets = {}
    set_data = []

    def set_id(opts):
        key = tuple(sorted(ids[o] for o in opts))
        if key not in sets:
            sets[key] = (len(set_data), len(key))
            set_data.extend(key)
        return sets[key]

    rows = []
    book_rows = []
    book_names = []
    for r in recipes:
        cells = []
        if r["shaped"]:
            h = len(r["grid"])
            w = max(len(row) for row in r["grid"])
            for row in r["grid"]:
                for x in range(w):
                    c = row[x] if x < len(row) else None
                    cells.append(c)
        else:
            w, h = 0, len(r["ings"])
            cells = list(r["ings"])
        refs = []
        for c in cells:
            if c is None:
                refs.append("{ 0, 0 }")
            else:
                off, n = set_id(c)
                refs.append("{ %d, %d }" % (off, n))
        while len(refs) < 9:
            refs.append("{ 0, 0 }")
        rows.append("    { %d, %d, %d, %d, { %s } },\n" % (w, h, ids[r["result"]], r["count"], ", ".join(refs)))
        # the recipe book: category (CraftingBookCategory), the items that
        # unlock it (its recipes/ advancement), its group
        cat = {"building": 1, "redstone": 2, "equipment": 3}.get(r.get("category", "misc"), 0)
        unl = [o for o in r.get("unlock", set()) if o in ids]
        uoff, un = set_id(unl) if unl else (0, 0)
        book_rows.append("    { %d, { %d, %d } },\n" % (cat, uoff, un))
        book_names.append(r["name"])
    # furnace: smelting recipes (ingredient set -> result) and fuel burn ticks
    srows = []
    for r in smelting:
        off, n = set_id(r["ing"])
        srows.append("    { { %d, %d }, %d, %d, %d, %.3ff },\n" % (off, n, ids[r["result"]], r["count"], r["time"], r["xp"]))
    frows = ["    { %d, %d },\n" % (ids[n], t) for n, t in sorted(fuel.items(), key=lambda kv: ids[kv[0]])]
    out.append("#define MC_SMELTING_COUNT %d\n" % len(srows))
    out.append("static const McSmelting mc_smelting[MC_SMELTING_COUNT + 1] = {\n%s    { { 0, 0 }, 0, 0, 0, 0.0f },\n};\n" % "".join(srows))
    food_rows = ["    { %d, %d, %.3ff, %d, %d },\n" % (ids[n], f[0], f[1], 1 if f[2] else 0, f[3])
                 for n, f in sorted(food.items(), key=lambda kv: ids[kv[0]])]
    out.append("#define MC_FOOD_COUNT %d\n" % len(food_rows))
    out.append("static const McFood mc_foods[MC_FOOD_COUNT + 1] = {\n%s    { 0, 0, 0.0f, 0, 0 },\n};\n" % "".join(food_rows))
    # creative tabs (CreativeModeTabs.java). the search tab gets every
    # category tab's items in tab order, without repeats (ItemStackLinkedSet)
    types = {"CATEGORY": 0, "HOTBAR": 1, "SEARCH": 2, "INVENTORY": 3}
    tab_items = []
    tab_rows = []
    search_list = []
    tabs_real = [t for t in tabs if "mob_loot" not in t]
    for t in tabs_real:
        if t["type"] == "CATEGORY" and t["title"] != "itemGroup.op":
            for n in t["items"]:
                if n not in search_list:
                    search_list.append(n)
    for t in tabs_real:
        if t["title"] == "itemGroup.op":
            continue  # only with the operator items tab option (off by default)
        lst = search_list if t["type"] == "SEARCH" else t["items"]
        first = len(tab_items)
        tab_items += [ids[n] for n in lst]
        flags = (1 if t["aligned_right"] else 0) | (2 if t["show_title"] else 0) | (4 if t["can_scroll"] else 0)
        bg = {"items": 0, "item_search": 1, "inventory": 2}[t["background"]]
        tab_rows.append("    { %s, %d, %d, %d, %d, %d, %d, %d, %d },\n" % (
            c_str(lang.get(t["title"], t["title"])), t["row"], t["col"], types[t["type"]], flags, bg,
            ids.get(t["icon"], 0), first, len(lst)))
    out.append("#define MC_TAB_COUNT %d\n" % len(tab_rows))
    out.append("static const McTab mc_tabs[MC_TAB_COUNT] = {\n%s};\n" % "".join(tab_rows))
    out.append("static const u16 mc_tab_items[%d] = { %s };\n" % (max(1, len(tab_items)), ", ".join(str(v) for v in tab_items) or "0"))
    out.append("#define MC_STR_HOTBAR_INFO %s\n" % c_str(lang.get("inventory.hotbarInfo", "Save hotbar with %1$s + %2$s")))
    out.append("#define MC_STR_BIN_SLOT %s\n" % c_str(lang.get("inventory.binSlot", "Destroy Item")))
    out.append("#define MC_STR_HOTBAR_SAVED %s\n" % c_str(lang.get("inventory.hotbarSaved", "Item hotbar saved (restore with %1$s + %2$s)")))
    # bk enemies -> minecraft mob loot (mob_drops in survival.c picks the mob)
    mob_loot = next((t["mob_loot"] for t in tabs if "mob_loot" in t), {})
    for mob, drops in mob_loot.items():
        # (uniform -1..1 rolls below 0 count as none: clamp to 0)
        # (a chance under 1/255 still gets 1: the zombie's 2.5% / 3)
        lrows = ["{ %d, %d, %d, %d }" % (ids[d[0]], max(0, d[1]), max(0, d[2]), max(1, min(255, int(round(d[3] * 255))))) for d in drops][:MOB_DROPS]
        while len(lrows) < MOB_DROPS:
            lrows.append("{ 0, 0, 0, 0 }")
        out.append("static const McDrop mc_loot_%s[%d] = { %s };\n" % (mob, MOB_DROPS, ", ".join(lrows)))
    # game mode switcher (GameModeSwitcherScreen)
    for key in ("gameMode.creative", "gameMode.survival", "gameMode.adventure", "gameMode.spectator",
                "debug.gamemodes.select_next", "debug.gamemodes.press_f4"):
        out.append("#define MC_STR_%s %s\n" % (key.replace(".", "_").upper(), c_str(lang.get(key, key))))
    out.append("#define MC_FUEL_COUNT %d\n" % len(frows))
    out.append("static const McFuel mc_fuels[MC_FUEL_COUNT + 1] = {\n%s    { 0, 0 },\n};\n" % "".join(frows))
    out.append("#define MC_RECIPE_COUNT %d\n" % len(rows))
    out.append("static const McRecipe mc_recipes[MC_RECIPE_COUNT] = {\n%s};\n" % "".join(rows))
    out.append("static const McRecipeBook mc_recipe_book[MC_RECIPE_COUNT] = {\n%s};\n" % "".join(book_rows))
    out.append("static const char *const mc_recipe_names[MC_RECIPE_COUNT] = {\n%s\n};\n" % ",\n".join("    " + c_str(n) for n in book_names))
    # advancements (tools/mc_advancements.py): display, criteria the mod
    # tells apart (trigger 1 inventory_changed, 2 consume_item, 3
    # player_killed_entity, 4 entity_killed_player, 0 never), requirement
    # groups, TreeNodePosition layout
    roots, order = ADV
    index = {a["id"]: i for i, a in enumerate(order)}
    adv_rows, crit_rows, pred_rows, req_groups, req_items, adv_names, crit_names = [], [], [], [], [], [], []
    tab_of = {}
    for ti, r in enumerate(roots):
        def mark(a, ti=ti):
            tab_of[a["id"]] = ti
            for c in a["children"]:
                mark(c)
        mark(r)
    for a in order:
        d = a["display"] or {"title": "", "desc": "", "icon": "", "frame": 0, "toast": False, "hidden": True}
        cf = len(crit_rows)
        for c in a["crit"]:
            pf = len(pred_rows)
            for opts in c["preds"]:
                known = [o for o in opts if o in ids]
                if known:
                    off, n = set_id(known)
                    pred_rows.append("{ %d, %d }" % (off, n))
                else:
                    pred_rows.append("{ 0, 0 }")
            if c["trig"] in (3, 4):
                # kills: pred_first is the mob mask (zombie 1, cow 2, cod 4)
                crit_rows.append("    { %d, %d, 0 },\n" % (c["trig"], c.get("mask", 7)))
            else:
                crit_rows.append("    { %d, %d, %d },\n" % (c["trig"], pf, len(pred_rows) - pf))
            crit_names.append(c["name"])
        rf = len(req_groups)
        for g in a["req"]:
            req_groups.append("{ %d, %d }" % (len(req_items), len(g)))
            req_items.extend(g)
        x, y = a.get("pos", (0, 0))
        flags = (1 if d.get("toast", True) else 0) | (2 if d.get("hidden") else 0) | (4 if a["display"] is not None else 0)
        parent = index.get(a["parent"], -1) if a["parent"] else -1
        adv_rows.append("    { %s, %s, %d, %d, %d, %d, %.4ff, %.4ff, %d, %d, %d, %d, %d },\n" % (
            c_str(d["title"]), c_str(d["desc"]), ids.get(d["icon"], 0), d["frame"], flags, parent, x, y,
            tab_of.get(a["id"], 0), cf, len(a["crit"]), rf, len(a["req"])))
        adv_names.append(a["id"])
    out.append("#define MC_ADV_COUNT %d\n#define MC_ADV_TAB_COUNT %d\n" % (len(adv_rows), len(roots)))
    out.append("static const McAdvancement mc_advs[MC_ADV_COUNT] = {\n%s};\n" % "".join(adv_rows))
    out.append("#define MC_ADV_CRIT_COUNT %d\n" % len(crit_rows))
    out.append("static const McAdvCrit mc_adv_crits[MC_ADV_CRIT_COUNT] = {\n%s};\n" % "".join(crit_rows))
    out.append("static const McIngredient mc_adv_preds[%d] = { %s };\n" % (max(1, len(pred_rows)), ", ".join(pred_rows) or "{ 0, 0 }"))
    out.append("static const McIngredient mc_adv_req_groups[%d] = { %s };\n" % (max(1, len(req_groups)), ", ".join(req_groups) or "{ 0, 0 }"))
    out.append("static const u8 mc_adv_req_items[%d] = { %s };\n" % (max(1, len(req_items)), ", ".join(str(v) for v in req_items) or "0"))
    out.append("static const char *const mc_adv_names[MC_ADV_COUNT] = {\n%s\n};\n" % ",\n".join("    " + c_str(n) for n in adv_names))
    out.append("static const char *const mc_adv_crit_names[MC_ADV_CRIT_COUNT] = {\n%s\n};\n" % ",\n".join("    " + c_str(n) for n in crit_names))
    out.append("static const s16 mc_adv_tab_roots[MC_ADV_TAB_COUNT] = { %s };\n" % ", ".join(str(index[r["id"]]) for r in roots))
    for key in ("advancements.toast.task", "advancements.toast.goal", "advancements.toast.challenge", "recipe.toast.title",
                "recipe.toast.description", "gui.advancements", "gui.done", "advancements.empty", "advancements.sad_label",
                "item.modifiers.head", "item.modifiers.chest", "item.modifiers.legs", "item.modifiers.feet", "item.modifiers.mainhand",
                "attribute.name.armor", "attribute.name.armor_toughness", "attribute.name.knockback_resistance",
                "attribute.name.attack_damage", "attribute.name.attack_speed", "attribute.modifier.plus.0",
                "attribute.modifier.equals.0", "gui.recipebook.search_hint"):
        out.append("#define MC_STR_%s %s\n" % (key.replace(".", "_").upper(), c_str(lang.get(key, key))))
    out.append("static const u16 mc_ingredient_ids[%d] = { %s };\n" % (max(1, len(set_data)),
               ", ".join(str(v) for v in set_data) or "0"))

    with open(DATA_OUT, "w") as f:
        f.write("".join(out))


def write_sounds_header(blocks, src_root, jar):
    """generated/mc_sounds.h + generated/sounds/<n>.pcm (tools/mc_sounds.py):
    the blocks' SoundTypes and the events the mod plays"""
    import mc_sounds
    info = os.path.join(os.path.dirname(jar), "mojang_minecraft_info.json")
    index_id = json.load(open(info))["assetIndex"]["id"] if os.path.isfile(info) else os.environ.get("MC_ASSET_INDEX", "34")
    java = os.path.join(src_root, "net", "minecraft")
    out = ["// generated by tools/export_mc_assets.py from minecraft's sounds. do NOT commit.\n#ifndef MC_SOUNDS_GENERATED\n#define MC_SOUNDS_GENERATED\n"]
    got = mc_sounds.export(java, index_id, os.path.join(OUT_DIR, "sounds"))
    if got is None:
        open(os.path.join(OUT_DIR, "mc_sounds.h"), "w").write("".join(out) + "#endif\n")
        return
    sounds_json, index, objects, types, block_types = got
    used_types = []
    bt = []
    for b in blocks:
        t = block_types.get(b["name"], "STONE")
        if t not in types:
            t = "STONE"
        if t not in used_types:
            used_types.append(t)
        bt.append(used_types.index(t))
    wanted = []
    for t in used_types:
        for ev in types[t][2]:
            if ev and ev not in wanted:
                wanted.append(ev)
    for ev in mc_sounds.EXTRA_EVENTS:
        if ev not in wanted:
            wanted.append(ev)
    ev_rows, entries, files = mc_sounds.build(sounds_json, index, objects, wanted, os.path.join(OUT_DIR, "sounds"))
    ev_index = {ev: i for i, (ev, _) in enumerate(ev_rows)}
    out.append("#define MC_HAVE_SOUNDS 1\n#define MC_SOUND_FILE_COUNT %d\n#define MC_SOUND_EVENT_COUNT %d\n" % (len(files), len(ev_rows)))
    for i, (ev, ix) in enumerate(ev_rows):
        out.append("#define SND_%s %d\n" % (c_ident(ev.replace(".", "_")), i))
    out.append("static const McSoundEntry mc_sound_entries[%d] = {\n%s};\n" % (max(1, len(entries)), "".join(
        "    { %d, %d, %d, %.4ff, %.4ff },\n" % (f, min(255, w), min(255, a), v, p) for f, v, p, w, a in entries) or "    { 0 },\n"))
    out.append("static const McSoundEvent mc_sound_events[MC_SOUND_EVENT_COUNT] = {\n%s};\n" % "".join(
        "    { %d, %d }, // %s\n" % (ix[0] if ix else 0, len(ix), ev) for ev, ix in ev_rows))
    rows = []
    for t in used_types:
        vol, pitch, evs = types[t]
        rows.append("    { %.4ff, %.4ff, { %s } }, // %s\n" % (vol, pitch, ", ".join(str(ev_index.get(e, -1)) for e in evs), t))
    out.append("#define MC_SOUND_TYPE_COUNT %d\n" % len(used_types))
    out.append("static const McSoundType mc_sound_types[MC_SOUND_TYPE_COUNT] = {\n%s};\n" % "".join(rows))
    for i, t in enumerate(used_types):
        out.append("#define SOUNDTYPE_%s %d\n" % (t, i))
    out.append("static const u8 mc_block_sound_type[MC_BLOCK_COUNT] = { %d, %s };\n" % (used_types.index("STONE") if "STONE" in used_types else 0,
               ", ".join(str(v) for v in bt)))
    missing = [ev for ev, ix in ev_rows if not ix]
    if missing:
        print("sounds with no files in this version:", ", ".join(missing))
    open(os.path.join(OUT_DIR, "mc_sounds.h"), "w").write("".join(out) + "#endif\n")


def main():
    jar = sys.argv[1] if len(sys.argv) > 1 else find_jar()
    if jar is None or not os.path.isfile(jar):
        print("couldn't find a minecraft client jar. pass its path as an argument.")
        sys.exit(1)
    z = zipfile.ZipFile(jar)
    tex = "assets/minecraft/textures/"

    # blocks / items / recipes straight from minecraft's data (tools/mc_data.py)
    src_root = os.environ.get("MC_SRC", os.path.join(ROOT, "..", "mc-src", "decomp"))
    if not os.path.isdir(src_root):
        print("need the decompiled minecraft source (Blocks.java etc) at %s (or set MC_SRC)" % src_root)
        sys.exit(1)
    jar_data = mc_data.Jar(z)
    blocks, items, recipes, smelting, fuel, food, tabs = mc_data.collect(jar_data, src_root)
    lang = json.loads(z.read("assets/minecraft/lang/en_us.json"))
    global ADV
    import mc_advancements
    ADV = mc_advancements.read_advancements(jar_data, mc_data.Tags(jar_data, "item"), lang)

    parts = [
        "// generated by tools/export_mc_assets.py from %s\n" % os.path.basename(jar),
        "// minecraft art: do NOT commit this file.\n",
        "#define MC_HAVE_TEXTURES 1\n",
    ]

    # block textures, deduped by (texture, tint). tinted ones get the tint
    # baked in (grass/leaves are grayscale in the jar)
    tex_index = {}
    tex_order = []
    tex_opaque = []
    all_px = []

    def block_texture(name, tint_rgb):
        key = (name, tint_rgb)
        if key in tex_index:
            return tex_index[key]
        img = load(z, tex + "%s.png" % name)
        img = img.crop((0, 0, img.width, img.width))  # animated: frame 0
        if img.width != 16:
            img = img.resize((16, 16), Image.NEAREST)
        if tint_rgb is not None:
            img = tint(img, ((tint_rgb >> 16) & 0xFF, (tint_rgb >> 8) & 0xFF, tint_rgb & 0xFF))
        if name == "block/grass_block_side":
            overlay = tint(load(z, tex + "block/grass_block_side_overlay.png"), GRASS_TINT)
            img = Image.alpha_composite(load(z, tex + "block/grass_block_side.png"), overlay)
        tex_index[key] = len(tex_order)
        tex_order.append(key)
        tex_opaque.append(all(px[3] == 255 for px in img.getdata()))
        all_px.extend(rgba5551(img))
        return tex_index[key]

    def face_list(b, faces_in):
        faces = []
        for fe in faces_in:
            t, tinted = fe[0], fe[1]
            tint_rgb = None
            if tinted and t != "block/grass_block_side":
                tint_rgb = b["tint"] if b["tint"] is not None else mc_data.FOLIAGE_TINT
            faces.append(block_texture(t, tint_rgb))
        return faces

    block_faces = []
    for b in blocks:
        block_faces.append(face_list(b, b["faces"]))
        # rotated variants (block states): textures per state, default if missing
        b["state_tex"] = [face_list(b, st) if st else block_faces[-1] for st in b["states"]]
        b["state_rot"] = [[(fe[2] if len(fe) > 2 else 0) for fe in (st or b["faces"])] for st in b["states"]]
    # model blocks: baked quads per state (mc_models.bake), and for multipart
    # blocks (redstone dust) the parts with their conditions
    DIRI = {d: i for i, d in enumerate(mc_models.DIRS)}
    q_rows, st_rows, box_rows, part_rows = [], [], [], []
    blk_model = ["{ 0, 0, 0 }"]  # air

    def quad_rows(qs):
        first = len(q_rows)
        for verts, uv, t, tinted, cull, shade in qs:
            ti = block_texture(t, None)
            vs = ", ".join("{ %d, %d, %d }" % tuple(int(round(c * 256)) for c in v) for v in verts)
            us = ", ".join("{ %d, %d }" % (int(round(a * 32)), int(round(c * 32))) for a, c in uv)
            q_rows.append("    { { %s }, { %s }, %d, %d, %d },\n" % (
                vs, us, ti, DIRI[cull] if cull else 6, DIRI[shade] | (0x80 if tinted else 0)))
        return first, len(q_rows) - first

    def bbox(qs):
        lo, hi = [16, 16, 16], [0, 0, 0]
        for verts, *_ in qs:
            for v in verts:
                for k in range(3):
                    c = min(16, max(0, v[k] * 16))
                    lo[k] = min(lo[k], int(c))
                    hi[k] = max(hi[k], int(-(-c // 1)))
        if lo[0] > hi[0]:
            lo, hi = [0, 0, 0], [16, 16, 16]
        for k in range(3):
            if hi[k] - lo[k] < 1:  # flat (dust): 1/16 thick to aim at
                hi[k] = min(16, lo[k] + 1)
                lo[k] = hi[k] - 1
        return "{ { %d, %d, %d }, { %d, %d, %d } }" % tuple(lo + hi)

    # shaped families (mc_families.py): every block of a family shares the
    # family's quads, a quad's tex is the family's texture slot (its
    # #variable), each block has its own textures per slot
    fam_vars = {}
    tmpl_rows = {}
    slot_rows = []
    sbox_rows = []  # (one per state: outline boxes then collision boxes)

    def tmpl_quad_rows(fam, qs):
        vs_ = fam_vars.setdefault(fam, [])
        for q in qs:
            if q[6] not in vs_:
                vs_.append(q[6])
        key = (fam, tuple((tuple(tuple(round(c, 4) for c in v) for v in q[0]), tuple(tuple(round(c, 3) for c in u) for u in q[1]),
                           q[6], q[3], q[4], q[5]) for q in qs))
        if key in tmpl_rows:
            return tmpl_rows[key]
        first = len(q_rows)
        for verts, uv, t, tinted, cull, shade, var in qs:
            vs = ", ".join("{ %d, %d, %d }" % tuple(int(round(c * 256)) for c in v) for v in verts)
            us = ", ".join("{ %d, %d }" % (int(round(a * 32)), int(round(c * 32))) for a, c in uv)
            q_rows.append("    { { %s }, { %s }, %d, %d, %d },\n" % (
                vs, us, vs_.index(var), DIRI[cull] if cull else 6, DIRI[shade] | (0x80 if tinted else 0)))
        tmpl_rows[key] = (first, len(q_rows) - first)
        return tmpl_rows[key]

    def box_c(bx):
        (x0, y0, z0), (x1, y1, z1) = bx
        cl = lambda v, hi=16: int(round(max(0, min(hi, v))))
        return "{ { %d, %d, %d }, { %d, %d, %d } }" % (cl(x0), cl(y0), cl(z0), cl(x1), cl(y1, 24), cl(z1))

    def shape_rows(outline, coll):
        f = len(sbox_rows)
        for bx in list(outline) + list(coll):
            sbox_rows.append("    %s,\n" % box_c(bx))
        return f, len(outline), len(coll)

    def block_slots(b, quads):
        """per slot: the block's own texture (from its own quads' resolved names)"""
        fam = b["family"]
        res = {}
        for q in quads:
            res.setdefault(q[6], q[2])
        return res

    VAL = {"none": 1, "side": 2, "up": 4, "false": 1, "true": 2}
    block_slot_map = {}
    for b in blocks:
        if b.get("model_states") is not None and b.get("shapes") is not None:
            # a family block: shared quads, its boxes per state
            fs = len(st_rows)
            allq = []
            for qs, sh in zip(b["model_states"], b["shapes"]):
                f, n = tmpl_quad_rows(b["family"], qs)
                allq += qs
                bf, nb, nc = shape_rows(*sh)
                st_rows.append("    { %d, %d, %d, %d, %d },\n" % (f, n, bf, nb, nc))
                box_rows.append("    %s,\n" % (box_c(sh[0][0]) if sh[0] else "{ { 0, 0, 0 }, { 16, 16, 16 } }"))
            block_slot_map[b["name"]] = block_slots(b, allq)
            blk_model.append("{ %d, %d, 0 }" % (fs, len(b["model_states"])))
        elif b.get("model_parts") is not None and b.get("shapes") is not None:
            # fence / pane: parts by the sides mask (north, east, south, west)
            fp = len(part_rows)
            allq = []
            for conds, qs, alt, alts in b["model_parts"]:
                f, n = tmpl_quad_rows(b["family"], qs)
                allq += qs
                terms = []
                for c in conds[:5]:
                    vals = [str(sum(VAL[v] for v in c[d]) if d in c else 7) for d in mc_families_sides]
                    vals += ["7"] * (5 - len(vals))
                    terms.append("{ %s }" % ", ".join(vals))
                while len(terms) < 5:
                    terms.append("{ 0, 0, 0, 0, 0 }")
                part_rows.append("    { %d, %d, %d, %d, %d, { %s } },\n" % (f, n, len(conds), alt, alts, ", ".join(terms)))
            srow = len(st_rows)
            for sh in b["shapes"]:
                bf, nb, nc = shape_rows(*sh)
                st_rows.append("    { 0, 0, %d, %d, %d },\n" % (bf, nb, nc))
                box_rows.append("    %s,\n" % box_c(sh[0][0]))
            block_slot_map[b["name"]] = block_slots(b, allq)
            blk_model.append("{ %d, %d, %d }" % (fp, len(b["model_parts"]), srow))
        elif b.get("model_states") is not None:
            fs = len(st_rows)
            for qs in b["model_states"]:
                f, n = quad_rows(qs)
                st_rows.append("    { %d, %d, 0, 0, 0 },\n" % (f, n))
                box_rows.append("    %s,\n" % bbox(qs))
            blk_model.append("{ %d, %d, 0 }" % (fs, len(b["model_states"])))
        elif b.get("model_parts") is not None:
            fp = len(part_rows)
            allq = []
            props = mc_models.MULTIPART_PROPS[b["name"]]
            for conds, qs, alt, alts in b["model_parts"]:
                f, n = quad_rows(qs)
                allq += qs
                terms = []
                for c in conds[:5]:
                    vals = [str(sum(VAL[v] for v in c[d]) if d in c else 7) for d in props]
                    vals += ["7"] * (5 - len(vals))
                    terms.append("{ %s }" % ", ".join(vals))
                while len(terms) < 5:
                    terms.append("{ 0, 0, 0, 0, 0 }")
                part_rows.append("    { %d, %d, %d, %d, %d, { %s } },\n" % (f, n, len(conds), alt, alts, ", ".join(terms)))
            # one shape box for every state (RedstoneWireBlock's dot shape,
            # BaseFireBlock.SHAPE = a 1 tall floor box)
            st_rows.append("    { 0, 0, 0, 0, 0 },\n")
            box_rows.append("    { { 3, 0, 3 }, { 13, 1, 13 } },\n" if b["name"] == "redstone_wire" else "    { { 0, 0, 0 }, { 16, 1, 16 } },\n")
            blk_model.append("{ %d, %d, %d }" % (fp, len(b["model_parts"]), len(st_rows) - 1))
        else:
            blk_model.append("{ 0, 0, 0 }")
    # item models of the model blocks drawn in 3d (buttons, trapdoors): the
    # items/<name>.json model, unrotated, at most 6 quads (one box)
    item_rows = ["{ 0, 0, 0, 0, 0 }"]
    gui_rows = ["{ { 30, 225, 0 }, 0.625f }"]
    for b in blocks:
        ent = "{ 0, 0, 0, 0, 0 }"
        gui = "{ { 30, 225, 0 }, 0.625f }"
        if b["place"] == 5 and mc_data.flat_item_sprite(jar_data, b["name"]) is None:
            im = jar_data.json("assets/minecraft/items/%s.json" % b["name"])
            mm = (im or {}).get("model", {})
            if mc_data.strip(mm.get("type", "")) == "model" and mm.get("model"):
                qs = mc_models.bake(jar_data, mm["model"])
                if 0 < len(qs) <= 48:
                    f, n = quad_rows(qs)
                    ent = "{ %d, %d, 0, 0, 0 }" % (f, n)
                    # the item model's display.gui (ItemTransform: rotation, scale)
                    for _, m in mc_data.model_chain(jar_data, mm["model"]):
                        g = m.get("display", {}).get("gui")
                        if g:
                            r = g.get("rotation", [0, 0, 0])
                            gui = "{ { %d, %d, %d }, %.4ff }" % (r[0], r[1], r[2], g.get("scale", [1, 1, 1])[0])
                            break
        item_rows.append(ent)
        gui_rows.append(gui)
    parts.append("#define MC_HAVE_MODELS 1\n")
    parts.append("static const McQuad mc_model_quads[%d] = {\n%s};\n" % (max(1, len(q_rows)), "".join(q_rows) or "    { 0 },\n"))
    parts.append("static const McModelState mc_model_states[%d] = {\n%s};\n" % (max(1, len(st_rows)), "".join(st_rows) or "    { 0 },\n"))
    parts.append("static const McBox mc_model_boxes[%d] = {\n%s};\n" % (max(1, len(box_rows)), "".join(box_rows) or "    { 0 },\n"))
    parts.append("static const McModelPart mc_model_parts[%d] = {\n%s};\n" % (max(1, len(part_rows)), "".join(part_rows) or "    { 0 },\n"))
    # the families' shape boxes, the texture slots, the family of each block
    parts.append("static const McShapeBox mc_shape_boxes[%d] = {\n%s};\n" % (max(1, len(sbox_rows)), "".join(sbox_rows) or "    { { 0, 0, 0 }, { 0, 0, 0 } },\n"))
    nslots = max([len(v) for v in fam_vars.values()] + [1])
    parts.append("#define MC_TEX_SLOTS %d\n" % nslots)
    srows = []
    for b in blocks:
        mp = block_slot_map.get(b["name"])
        if mp is None:
            srows.append("{ 0 }")
            continue
        vs_ = fam_vars[b["family"]]
        srows.append("{ %s }" % ", ".join(str(block_texture(mp[v], None) if v in mp else 0) for v in vs_))
    parts.append("static const u16 mc_block_slots[MC_BLOCK_COUNT][MC_TEX_SLOTS] = { { 0 }, %s };\n" % ", ".join(srows))
    parts.append("static const u8 mc_block_offset[MC_BLOCK_COUNT] = { 0, %s };\n" % ", ".join(str(b.get("offset", 0)) for b in blocks))
    parts.append("static const u8 mc_block_family[MC_BLOCK_COUNT] = { 0, %s };\n" % ", ".join(
        str(mc_families.FAMILY_IDS.get(b.get("family"), 0) if b.get("family") else 0) for b in blocks))
    # per block: model blocks -> first state + state count; multipart -> first
    # part + part count + the shared state row
    parts.append("static const McBlockModel mc_block_models[MC_BLOCK_COUNT] = { %s };\n" % ", ".join(blk_model))
    parts.append("static const McModelState mc_block_item_models[MC_BLOCK_COUNT] = { %s };\n" % ", ".join(item_rows))
    parts.append("static const McItemGui mc_block_item_gui[MC_BLOCK_COUNT] = { %s };\n" % ", ".join(gui_rows))

    parts.append("#define MC_TEX_COUNT %d\n" % len(tex_order))
    parts.append(c_array("mc_block_tex", all_px))
    # fluids: the still textures, frame 0, rgba8888 (water keeps its alpha)
    for fl in ("water", "lava"):
        img = load(z, tex + "block/%s_still.png" % fl)
        img = img.crop((0, 0, img.width, img.width)).resize((16, 16), Image.NEAREST)
        parts.append(c_array32("mc_%s_still" % fl, rgba8888(img)))
        img = load(z, tex + "block/%s_flow.png" % fl)
        img = img.crop((0, 0, img.width, img.width)).resize((32, 32), Image.NEAREST)
        parts.append(c_array32("mc_%s_flow" % fl, rgba8888(img)))
    # fire sprites (frame 0) for the screen overlay / burning entities
    for i in (0, 1):
        img = load(z, tex + "block/fire_%d.png" % i)
        img = img.crop((0, 0, img.width, img.width)).resize((16, 16), Image.NEAREST)
        parts.append(c_array32("mc_fire_%d_tex" % i, rgba8888(img)))
    # particle sprite sets (particles/smoke.json, explosion.json, poof.json), 16x16 rgba5551
    ppx = []
    for pset in ("smoke", "explosion", "poof"):
        names = json.loads(z.read("assets/minecraft/particles/%s.json" % pset))["textures"]
        parts.append("#define MC_PSPRITE_%s_FIRST %d\n#define MC_PSPRITE_%s_COUNT %d\n"
                     % (pset.upper(), len(ppx) // 256, pset.upper(), len(names)))
        for n in names:
            img = load(z, tex + "particle/%s.png" % n.split(":")[-1])
            if img.size != (16, 16):
                img = img.resize((16, 16), Image.NEAREST)
            ppx.extend(rgba5551(img))
    parts.append(c_array("mc_particle_tex", ppx))

    # item sprites (16x16), one per plain item in id order
    sprites = []
    for it in items:
        if it["sprite"] is None:
            sprites.extend([0] * 256)  # (drawn as a model: the shield)
            continue
        img = load(z, tex + "%s.png" % it["sprite"])
        img = img.crop((0, 0, img.width, img.width))
        if img.width != 16:
            img = img.resize((16, 16), Image.NEAREST)
        sprites.extend(rgba8888(img))
    parts.append("#define MC_ITEM_SPRITE_COUNT %d\n" % len(items))
    parts.append(c_array32("mc_item_sprites", sprites))
    # model blocks whose item model is a flat sprite (torches, lever): drawn
    # like items in the gui / hand / on the ground
    bsprites, bidx = [], []
    for b in blocks:
        sp = mc_data.flat_item_sprite(jar_data, b["name"]) if b["place"] == 5 else None
        if sp and jar_data.has("assets/minecraft/textures/%s.png" % sp):
            img = load(z, tex + "%s.png" % sp)
            img = img.crop((0, 0, img.width, img.width))
            if img.width != 16:
                img = img.resize((16, 16), Image.NEAREST)
            bsprites.extend(rgba8888(img))
            bidx.append(len(bsprites) // 256)
        else:
            bidx.append(0)
    parts.append(c_array32("mc_block_item_sprites", bsprites or [0] * 256))
    parts.append("static const u8 mc_block_sprite[MC_BLOCK_COUNT] = { 0, %s };\n" % ", ".join(str(i) for i in bidx))

    write_data_header(blocks, block_faces, tex_opaque, items, recipes, lang, smelting, fuel, food, tabs)
    write_sounds_header(blocks, src_root, jar)

    # container screens (only the 176x166 used part)
    for name in ("inventory", "crafting_table"):
        img = load(z, tex + "gui/container/%s.png" % name).crop((0, 0, 176, 166))
        parts.append(c_array32("mc_gui_" + name, rgba8888(img)))
    # boats (BoatModel, entity/boat/<wood>.png, 128 x 64), in boat.c's order
    bpx = []
    for wood in ("oak", "spruce", "birch", "jungle", "acacia", "dark_oak", "mangrove", "cherry", "pale_oak", "poplar"):
        bpx.extend(rgba8888(load(z, tex + "entity/boat/%s.png" % wood).crop((0, 0, 128, 64))))
    parts.append(c_array32("mc_boat_tex", bpx))
    # armor layers (EquipmentLayerRenderer: humanoid / humanoid_leggings, 64 x 32)
    # per asset in ARMOR_ASSETS order, then leather's undyed overlays
    apx = []
    for asset in ARMOR_ASSETS:
        for layer in ("humanoid", "humanoid_leggings"):
            path = tex + "entity/equipment/%s/%s.png" % (layer, asset)
            img = load(z, path) if ("assets/minecraft/textures/entity/equipment/%s/%s.png" % (layer, asset)) in z.namelist() else Image.new("RGBA", (64, 32))
            apx.extend(rgba8888(img.crop((0, 0, 64, 32))))
    for layer in ("humanoid", "humanoid_leggings"):
        apx.extend(rgba8888(load(z, tex + "entity/equipment/%s/leather_overlay.png" % layer).crop((0, 0, 64, 32))))
    parts.append("#define MC_ARMOR_ASSETS %d\n" % len(ARMOR_ASSETS))
    parts.append(c_array32("mc_armor_tex", apx))
    # the advancements window (252 x 140) and each tab's 16 x 16 background tile
    parts.append(c_array32("mc_gui_adv_window", rgba8888(load(z, tex + "gui/advancements/window.png").crop((0, 0, 252, 140)))))
    bgpx = []
    for r in ADV[0]:
        bg = (r["display"] or {}).get("background") or "gui/advancements/backgrounds/stone"
        bgpx.extend(rgba8888(load(z, tex + "%s.png" % bg).crop((0, 0, 16, 16))))
    parts.append(c_array32("mc_adv_bg", bgpx or [0] * 256))
    # the recipe book's background (RECIPE_BOOK_LOCATION, 147 x 166 from 1, 1)
    parts.append(c_array32("mc_gui_recipe_book", rgba8888(load(z, tex + "gui/recipe_book.png").crop((1, 1, 148, 167)))))
    # ContainerScreen (chests): generic_54.png, 176 x 222
    parts.append(c_array32("mc_gui_generic_54", rgba8888(load(z, tex + "gui/container/generic_54.png").crop((0, 0, 176, 222)))))
    img = load(z, tex + "gui/container/furnace.png").crop((0, 0, 176, 166))
    parts.append(c_array32("mc_gui_furnace", rgba8888(img)))
    # the shield (ShieldModel, 64 x 64, no banner pattern)
    parts.append(c_array32("mc_shield_tex", rgba8888(load(z, tex + "entity/shield/shield_base_nopattern.png").crop((0, 0, 64, 64)))))
    # bow: the arrow entity (ArrowModel, 32x32) and the pulling sprites the
    # item model switches to while drawing (use_duration 0 / 0.65 / 0.9)
    parts.append(c_array32("mc_arrow_tex", rgba8888(load(z, tex + "entity/projectiles/arrow.png"))))
    # ExperienceOrbRenderer: experience_orb.png is a 4x4 grid of 16x16 icons (11 used)
    orb = load(z, tex + "entity/experience/experience_orb.png")
    opx = []
    for i in range(11):
        opx.extend(rgba8888(orb.crop((i % 4 * 16, i // 4 * 16, i % 4 * 16 + 16, i // 4 * 16 + 16))))
    parts.append(c_array32("mc_xp_orb_tex", opx))
    pull = []
    for i in range(3):
        pull.extend(rgba8888(load(z, tex + "item/bow_pulling_%d.png" % i).crop((0, 0, 16, 16))))
    parts.append(c_array32("mc_bow_pulling", pull))
    # the chest (ChestModel, 64x64) for the survival inventory tab's icon
    parts.append(c_array32("mc_chest_tex", rgba8888(load(z, tex + "entity/chest/normal.png"))))
    img = load(z, tex + "gui/container/gamemode_switcher.png").crop((0, 0, 125, 75))
    parts.append(c_array32("mc_gui_gamemode_switcher", rgba8888(img)))
    # creative inventory backgrounds (CreativeModeInventoryScreen: 195x136)
    for name in ("items", "item_search", "inventory"):
        img = load(z, tex + "gui/container/creative_inventory/tab_%s.png" % name).crop((0, 0, 195, 136))
        parts.append(c_array32("mc_gui_creative_" + name, rgba8888(img)))

    # the whole default font (ascii.png, 16x16 cells of 8x8), white + alpha.
    # glyph width = rightmost lit column + 1, like the bitmap font provider.
    font = load(z, tex + "font/ascii.png")
    cell = font.width // 16
    font = font.resize((128, 128), Image.NEAREST)
    widths = []
    for ch in range(256):
        gx, gy = (ch % 16) * 8, (ch // 16) * 8
        w = 0
        for x in range(8):
            for y in range(8):
                if font.getpixel((gx + x, gy + y))[3] > 0:
                    w = x + 1
        widths.append(w)
    widths[32] = 3  # space provider: advance 4 = width 3 + 1
    parts.append(c_array32("mc_font", rgba8888(font)))
    parts.append("static const u8 mc_font_width[256] = { %s };\n" % ", ".join(str(w) for w in widths))

    # hud sprites (Gui): name in the mod -> path under textures/gui/sprites/hud/
    for name, path in HUD_SPRITES:
        img = load(z, tex + "gui/sprites/hud/%s.png" % path)
        parts.append("#define MC_HUD_%s_W %d\n#define MC_HUD_%s_H %d\n" % (name.upper(), img.width, name.upper(), img.height))
        # lowercase too, so the hud's SPRITE(name, ...) macro can paste the name
        parts.append("#define MC_HUD_%s_W %d\n#define MC_HUD_%s_H %d\n" % (name, img.width, name, img.height))
        # 32 bit: the gui sprites use real alpha (the hotbar is see-through)
        parts.append(c_array32("mc_hud_" + name, rgba8888(img)))

    for name, path in GUI_SPRITES:
        img = load(z, tex + "gui/sprites/%s.png" % path)
        parts.append("#define MC_SPR_%s_W %d\n#define MC_SPR_%s_H %d\n" % (name, img.width, name, img.height))
        parts.append(c_array32("mc_spr_" + name, rgba8888(img)))

    # block breaking cracks (destroy_stage_0..9), 16x16 each
    cracks = []
    for i in range(10):
        cracks.extend(rgba8888(load(z, tex + "block/destroy_stage_%d.png" % i).crop((0, 0, 16, 16))))
    parts.append(c_array32("mc_destroy_tex", cracks))

    # the digits 0-9 of the default font (ascii.png, 8x8 cells, row 3), white
    # with alpha, for item counts. glyph width = rightmost lit column + 1.
    font = load(z, tex + "font/ascii.png")
    cell = font.width // 16
    digits = font.crop((0, 3 * cell, 10 * cell, 4 * cell)).resize((80, 8), Image.NEAREST)
    widths = []
    for d in range(10):
        w = 0
        for x in range(8):
            for y in range(8):
                if digits.getpixel((d * 8 + x, y))[3] > 0:
                    w = x + 1
        widths.append(w)
    parts.append(c_array32("mc_font_digits", rgba8888(digits)))
    parts.append("static const u8 mc_font_digit_width[10] = { %s };\n" % ", ".join(str(w) for w in widths))

    skin = load(z, tex + "entity/player/wide/steve.png")
    parts.append("#define MC_SKIN_W 64\n#define MC_SKIN_H 64\n")
    parts.append(c_array("mc_skin_tex", rgba5551(skin)))

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT, "w") as f:
        f.write("".join(parts))
    print("wrote %s (%d block textures, %d blocks, %d items, %d recipes) from %s"
          % (OUT, len(tex_order), len(blocks), len(items), len(recipes), jar))


if __name__ == "__main__":
    main()
