"""minecraft's own data -> the mod's block / item / recipe tables.

swirly's approach (see CLAUDE.md): instead of typing blocks in by hand, read
them out of the game. sources:
  - the client jar: blockstates + block/item models (which blocks are plain
    full cubes, their face textures and tints), item models (flat sprites),
    tags (mineable/*, incorrect_for_*_tool), loot tables (drops), recipes.
  - the decompiled source (mc-src/decomp, see README): Blocks.java for
    strength / requiresCorrectToolForDrops, Items.java for stack sizes,
    durability and tool stats, ToolMaterial.java for the tool tiers.

only full-cube blocks are taken (the mod's renderer and collision are cubes).
everything here is derived from minecraft files, so the output is generated
and never committed.
"""

import json
import os
import re

NS = "minecraft:"

# blocks that make no sense in banjo-kazooie (technical / admin blocks)
SKIP_BLOCKS = {
    "command_block", "chain_command_block", "repeating_command_block", "structure_block",
    "jigsaw", "barrier", "light", "test_block", "test_instance_block", "structure_void",
    "spawner", "trial_spawner", "vault", "petrified_oak_slab",
}

# blocks baked from their elements instead (mc_models.MODEL_BLOCKS)
_MODEL_BLOCK_NAMES = {"lever", "stone_button", "oak_button", "redstone_torch", "redstone_wall_torch",
                      "redstone_lamp", "oak_trapdoor", "iron_trapdoor", "redstone_wire", "torch", "wall_torch", "fire"}

# ToolMaterial.java order (also the tier bit order in the mod)
MATERIALS = ["wooden", "stone", "copper", "iron", "golden", "diamond", "netherite"]
MATERIAL_JAVA = {"WOOD": "wooden", "STONE": "stone", "COPPER": "copper", "IRON": "iron",
                 "GOLD": "golden", "DIAMOND": "diamond", "NETHERITE": "netherite"}
TOOL_KINDS = ["sword", "shovel", "pickaxe", "axe", "hoe"]  # 1..5 in the mod (0 = none)

GRASS_TINT = 0x91BD59
FOLIAGE_TINT = 0x77AB2F


def strip(name):
    return name[len(NS):] if name.startswith(NS) else name


class Jar:
    def __init__(self, zf):
        self.z = zf
        self.names = set(zf.namelist())
        self._json = {}

    def has(self, path):
        return path in self.names

    def json(self, path):
        if path not in self._json:
            self._json[path] = json.loads(self.z.read(path)) if path in self.names else None
        return self._json[path]

    def listdir(self, prefix, ext=".json"):
        out = []
        for n in self.names:
            if n.startswith(prefix) and n.endswith(ext) and "/" not in n[len(prefix):]:
                out.append(n[len(prefix):-len(ext)])
        return sorted(out)


# --- tags ---

class Tags:
    def __init__(self, jar, kind):
        self.jar = jar
        self.kind = kind
        self.cache = {}

    def get(self, name):
        name = strip(name)
        if name in self.cache:
            return self.cache[name]
        self.cache[name] = set()  # (cycles)
        data = self.jar.json("data/minecraft/tags/%s/%s.json" % (self.kind, name))
        out = set()
        if data:
            for v in data.get("values", []):
                if isinstance(v, dict):
                    v = v.get("id", "")
                if v.startswith("#"):
                    out |= self.get(v[1:])
                else:
                    out.add(strip(v))
        self.cache[name] = out
        return out


# --- models ---

def model_chain(jar, name):
    """the model and its parents, child first"""
    chain = []
    while name:
        name = strip(name)
        m = jar.json("assets/minecraft/models/%s.json" % name)
        if m is None:
            break
        chain.append((name, m))
        name = m.get("parent")
    return chain


def _tex_str(v):
    # 26.x models can give a texture as {"sprite": ..., "force_translucent": ...}
    if isinstance(v, dict):
        return v.get("sprite", "")
    return v or ""


def resolve_tex(textures, ref, depth=0):
    ref = _tex_str(ref)
    while ref.startswith("#") and depth < 10:
        ref = _tex_str(textures.get(ref[1:], ""))
        depth += 1
    return strip(ref) if ref and not ref.startswith("#") else None


FACE_ORDER = ["up", "down", "south", "north", "east", "west"]  # the mod's +y -y +z -z +x -x


def cube_faces(jar, model):
    """face textures + tint flags if the model is one full 16x16x16 box"""
    chain = model_chain(jar, model)
    textures = {}
    for _, m in reversed(chain):
        textures.update(m.get("textures", {}))
    elements = None
    for _, m in chain:
        if "elements" in m:
            elements = m["elements"]
            break
    if not elements or len(elements) != 1:
        return None
    e = elements[0]
    if e.get("from") != [0, 0, 0] or e.get("to") != [16, 16, 16] or "rotation" in e:
        return None
    faces = e.get("faces", {})
    if any(f not in faces for f in FACE_ORDER):
        return None
    out = []
    for f in FACE_ORDER:
        t = resolve_tex(textures, faces[f].get("texture", ""))
        if t is None:
            return None
        out.append((t, "tintindex" in faces[f], (faces[f].get("rotation", 0) // 90) % 4))
    return out


# the mod's face order: up, down, south, north, east, west
_DIRS = ["up", "down", "south", "north", "east", "west"]
_ROT_X = {"up": "north", "north": "down", "down": "south", "south": "up", "east": "east", "west": "west"}
_ROT_Y = {"north": "east", "east": "south", "south": "west", "west": "north", "up": "up", "down": "down"}


# the renderer's texture frame per face (world_draw.c corner_uv): the way the
# texture's top points and the way its u runs
_VEC = {"up": (0, 1, 0), "down": (0, -1, 0), "south": (0, 0, 1), "north": (0, 0, -1), "east": (1, 0, 0), "west": (-1, 0, 0)}
_TEX_UP = {"up": (0, 0, -1), "down": (0, 0, 1), "south": (0, 1, 0), "north": (0, 1, 0), "east": (0, 1, 0), "west": (0, 1, 0)}
_TEX_RIGHT = {"up": (1, 0, 0), "down": (1, 0, 0), "south": (1, 0, 0), "north": (-1, 0, 0), "east": (0, 0, -1), "west": (0, 0, 1)}


def _neg(v):
    return tuple(-a for a in v)


def _rotate_vec(v, x, y):
    # same turns as _ROT_X / _ROT_Y (up -> north, north -> east)
    for _ in range((x // 90) % 4):
        v = (v[0], v[2], -v[1])
    for _ in range((y // 90) % 4):
        v = (-v[2], v[1], v[0])
    return v


def _uv_turns(d, face_rot, x, y):
    """quarter turns (clockwise) of the texture on the face `d` lands on: the
    face's own rotation carried through the model's x/y rotation (uvlock off)"""
    frame = [_TEX_UP[d], _TEX_RIGHT[d], _neg(_TEX_UP[d]), _neg(_TEX_RIGHT[d])]
    up = _rotate_vec(frame[face_rot % 4], x, y)
    nd = _rotate(d, x, y)
    nframe = [_TEX_UP[nd], _TEX_RIGHT[nd], _neg(_TEX_UP[nd]), _neg(_TEX_RIGHT[nd])]
    return nframe.index(up)


def _rotate(d, x, y):
    """where a model face ends up after the blockstate's x then y rotation"""
    for _ in range((x // 90) % 4):
        d = _ROT_X[d]
    for _ in range((y // 90) % 4):
        d = _ROT_Y[d]
    return d


def _parse_key(k):
    return dict(p.split("=", 1) for p in k.split(",") if "=" in p)


def block_states(jar, name, state):
    """rotatable blocks: (kind, [faces per state], default index).
    kind 1: horizontal facing (index = _DIRS position), 2: facing (6 dirs),
    3: axis (y, z, x), 0: none"""
    v = state.get("variants")
    if not v:
        return 0, [], 0
    keys = list(v.keys())
    parsed = [_parse_key(k) for k in keys]
    dkey = "" if "" in v else next((k for k in keys if "axis=y" in k), keys[0])
    dprops = _parse_key(dkey)
    if "facing" in dprops:
        prop = "facing"
        values = sorted({p["facing"] for p in parsed if "facing" in p})
        kind = 2 if ("up" in values or "down" in values) else 1
        order = _DIRS
    elif "axis" in dprops:
        prop = "axis"
        kind = 3
        order = ["y", "z", "x"]
    else:
        return 0, [], 0
    out = []
    for val in order:
        want = dict(dprops)
        want[prop] = val
        key = next((k for k, p in zip(keys, parsed) if p == want), None)
        if key is None:
            out.append(None)
            continue
        m = v[key]
        if isinstance(m, list):
            m = m[0]
        faces = cube_faces(jar, m.get("model"))
        if faces is None:
            out.append(None)
            continue
        rot = [None] * 6
        mx, my = m.get("x", 0), m.get("y", 0)
        for i, d in enumerate(_DIRS):
            t, tint, fr = faces[i]
            rot[_DIRS.index(_rotate(d, mx, my))] = (t, tint, _uv_turns(d, fr, mx, my))
        out.append(rot)
    return kind, out, order.index(dprops[prop]) if dprops[prop] in order else 0


def _variant_model(state):
    """any model the blockstate uses (variants or the first multipart part)"""
    if "variants" in state:
        v = next(iter(state["variants"].values()))
    else:
        v = state["multipart"][0]["apply"]
    if isinstance(v, list):
        v = v[0]
    return v.get("model")


def default_variant_model(state):
    if "variants" not in state:
        return None
    v = state["variants"]
    key = None
    if "" in v:
        key = ""
    else:
        for k in v:
            if "axis=y" in k:
                key = k
                break
        if key is None:
            key = next(iter(v))
    m = v[key]
    if isinstance(m, list):
        m = m[0]
    if m.get("x") or m.get("y"):
        # rotated defaults (e.g. some facing blocks): only fine if all sides match
        pass
    return m.get("model")


def item_tint(jar, name):
    data = jar.json("assets/minecraft/items/%s.json" % name)
    if not data:
        return None
    for t in data.get("model", {}).get("tints", []):
        typ = strip(t.get("type", ""))
        if typ == "constant":
            return t.get("value", 0) & 0xFFFFFF
        if typ == "grass":
            return GRASS_TINT
        if typ == "foliage":
            return FOLIAGE_TINT
    return None


def flat_item_sprite(jar, name):
    """layer0 texture if the item is a plain flat sprite (item/generated)"""
    data = jar.json("assets/minecraft/items/%s.json" % name)
    if not data:
        return None
    m = data.get("model", {})
    # the plain case of the item model "select"/"condition"/"range_dispatch"
    # trees: fallback / on_false / the first range entry (a compass with no
    # target, a clock at time 0...)
    for _ in range(4):
        t = strip(m.get("type", ""))
        if t == "condition":
            m = m.get("on_false", {})
        elif t == "select":
            # display_context: the case used in the gui (spears have a
            # different in-hand model)
            pick = None
            if strip(m.get("property", "")) == "display_context":
                for case in m.get("cases", []):
                    when = case.get("when", [])
                    when = when if isinstance(when, list) else [when]
                    if "gui" in when:
                        pick = case.get("model")
            m = pick if pick is not None else m.get("fallback", {})
        elif t == "range_dispatch":
            entries = m.get("entries") or []
            m = entries[0]["model"] if entries else m.get("fallback", {})
        else:
            break
    if strip(m.get("type", "")) != "model" or m.get("tints"):
        return None
    chain = model_chain(jar, m.get("model", ""))
    if not any(n == "item/generated" for n, _ in chain):
        return None
    textures = {}
    for _, mm in reversed(chain):
        textures.update(mm.get("textures", {}))
    if "layer1" in textures:
        return None
    t = _tex_str(textures.get("layer0"))
    return strip(t) if t else None


def item_is_handheld(jar, name):
    data = jar.json("assets/minecraft/items/%s.json" % name)
    if not data:
        return False
    return any(n == "item/handheld" for n, _ in model_chain(jar, data.get("model", {}).get("model", "")))


# --- java sources ---

def java_statements(src, decl):
    """`public static final <decl> NAME = ...;` -> {NAME: text}"""
    out = {}
    for m in re.finditer(r"public static final %s (\w+) = " % decl, src):
        i = m.end()
        depth = 0
        j = i
        while j < len(src):
            c = src[j]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            elif c == ";" and depth == 0:
                break
            j += 1
        out[m.group(1)] = src[i:j]
    return out


def parse_props(text, helpers, blocks_props):
    """BlockBehaviour.Properties chain -> {hardness, requires_tool, no_occlusion}"""
    p = {"hardness": None, "requires_tool": False}
    # copies first, their values get overridden by what follows
    m = re.search(r"of(?:Full|Legacy)Copy\((\w+)\)", text)
    if m and m.group(1) in blocks_props:
        p.update(blocks_props[m.group(1)])
    for name, body in helpers.items():
        if re.search(r"\b%s\(" % name, text):
            p.update({k: v for k, v in parse_props(body, {}, blocks_props).items() if v is not None and v is not False})
    for m in re.finditer(r"\.strength\(\s*(-?[\d.]+)F?(?:\s*,\s*(-?[\d.]+)F?)?\s*\)", text):
        p["hardness"] = float(m.group(1))
        # strength(destroyTime, explosionResistance); strength(t) = both t
        p["resist"] = max(0.0, float(m.group(2) if m.group(2) else m.group(1)))
    for m in re.finditer(r"\.explosionResistance\(\s*([\d.]+)F?", text):
        p["resist"] = float(m.group(1))
    for m in re.finditer(r"\.destroyTime\(\s*([\d.\-]+)F?", text):
        p["hardness"] = float(m.group(1))
    if ".instabreak()" in text:
        p["hardness"] = 0.0
        p["resist"] = 0.0
    if ".noCollision()" in text:
        p["no_collision"] = True
    # BlockBehaviour.OffsetType (flowers XZ, grass XYZ): the random draw offset
    m = re.search(r"\.offsetType\(BlockBehaviour\.OffsetType\.(XYZ|XZ)\)", text)
    if m:
        p["offset"] = 2 if m.group(1) == "XYZ" else 1
    if ".requiresCorrectToolForDrops()" in text:
        p["requires_tool"] = True
    return p


def read_blocks_java(path):
    src = open(path, encoding="utf-8").read()
    # helper methods returning Properties (logProperties, leavesProperties, ...)
    helpers = {}
    for m in re.finditer(r"static BlockBehaviour\.Properties (\w+)\([^)]*\)\s*\{(.*?)\n\t\}", src, re.S):
        helpers[m.group(1)] = m.group(2)
    stmts = java_statements(src, "Block")
    props = {}
    for name, text in stmts.items():
        props[name] = parse_props(text, helpers, props)
        # DropExperienceBlock(xpRange) / RedStoneOreBlock.spawnAfterBreak (1..5)
        m = re.search(r"DropExperienceBlock\(\s*UniformInt\.of\((\d+),\s*(\d+)\)", text)
        c = re.search(r"DropExperienceBlock\(\s*ConstantInt\.of\((\d+)\)", text)
        if m:
            props[name]["xp"] = (int(m.group(1)), int(m.group(2)))
        elif c:
            props[name]["xp"] = (int(c.group(1)), int(c.group(1)))
        elif "RedStoneOreBlock" in text:
            props[name]["xp"] = (1, 5)
    # java constant -> registry name via BlockItemIds.NAME (same spelling, lowercase)
    return {k.lower(): v for k, v in props.items()}


def read_item_places(path):
    """Items.java: items that place a block under another name
    (createBlockItemWithCustomItemName) and StandingAndWallBlockItem's wall
    block -> [(item, block, wall block or None)]"""
    src = open(path, encoding="utf-8").read()
    # BlockItemIds: NAME = BlockItemId.create("block id", "item id") (one name: both)
    ids_src = open(os.path.join(os.path.dirname(path), "..", "..", "references", "BlockItemIds.java"), encoding="utf-8").read()
    item_of = {}
    for m in re.finditer(r'(\w+) = BlockItemId\.create\("(\w+)"(?:,\s*"(\w+)")?\)', ids_src):
        item_of[m.group(1)] = m.group(3) or m.group(2)
    out = []
    for m in re.finditer(r"BlockItemIds\.(\w+),\s*createBlockItemWithCustomItemName\(Blocks\.(\w+)\)", src):
        out.append((item_of.get(m.group(1), m.group(1).lower()), m.group(2).lower(), None))
    for m in re.finditer(r"BlockItemIds\.(\w+),\s*Blocks\.(\w+),\s*\(b, p\) -> new StandingAndWallBlockItem\(b, Blocks\.(\w+), Direction\.DOWN", src):
        out.append((item_of.get(m.group(1), m.group(1).lower()), m.group(2).lower(), m.group(3).lower()))
    return out


def read_items_java(path):
    src = open(path, encoding="utf-8").read()
    out = {}
    for name, text in java_statements(src, "Item").items():
        it = {"stack": 64, "durability": 0, "tool": None}
        m = re.search(r"\.stacksTo\((\d+)\)", text)
        if m:
            it["stack"] = int(m.group(1))
        m = re.search(r"\.durability\((\d+)\)", text)
        if m:
            it["durability"] = int(m.group(1))
            it["stack"] = 1
        m = re.search(r"\.(sword|pickaxe|axe|shovel|hoe)\(ToolMaterial\.(\w+),\s*([-\d.]+)F,\s*([-\d.]+)F", text)
        if m:
            it["tool"] = (m.group(1), MATERIAL_JAVA.get(m.group(2)), float(m.group(3)), float(m.group(4)))
            it["stack"] = 1
        # Item.Properties.spear(ToolMaterial.X, attackDuration, ...): durability,
        # ATTACK_DAMAGE 0 + bonus, ATTACK_SPEED 1 / attackDuration - 4
        m = re.search(r"\.spear\(ToolMaterial\.(\w+),\s*([-\d.]+)F", text)
        if m:
            it["spear"] = (MATERIAL_JAVA.get(m.group(1)), float(m.group(2)))
            it["stack"] = 1
        # Item.Properties.humanoidArmor(ArmorMaterials.X, ArmorType.Y)
        m = re.search(r"\.humanoidArmor\(ArmorMaterials\.(\w+),\s*ArmorType\.(\w+)\)", text)
        if m:
            it["armor"] = (m.group(1), m.group(2))
            it["stack"] = 1
        out[name.lower()] = it
    return out


# ArmorType: slot index in the mod (0 head, 1 chest, 2 legs, 3 feet) and unitDurability
ARMOR_TYPES = {"HELMET": (0, 11), "CHESTPLATE": (1, 16), "LEGGINGS": (2, 15), "BOOTS": (3, 13)}


def read_armor_materials(path):
    """ArmorMaterials.java: durability multiplier, makeDefense(boots, legs,
    chest, helm, body), toughness, knockback resistance, EquipmentAssets id"""
    src = open(path, encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"ArmorMaterial (\w+) = new ArmorMaterial\(\s*(\d+),\s*makeDefense\((\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\),"
                         r"\s*\d+,\s*SoundEvents\.\w+,\s*([-\d.]+)F,\s*([-\d.]+)F,\s*ItemTags\.\w+,\s*EquipmentAssets\.(\w+)", src):
        boots, legs, chest, helm = int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6))
        out[m.group(1)] = {"durability": int(m.group(2)), "defense": {"HELMET": helm, "CHESTPLATE": chest, "LEGGINGS": legs, "BOOTS": boots},
                           "toughness": float(m.group(8)), "kb": float(m.group(9)), "asset": m.group(10).lower()}
    return out


def read_fuels(java_root, items_path):
    """Items.java cookingFuel(ContextIntProviders.X) + the burn ticks registered
    for X in ContextIntProviders.java -> {item name: ticks}"""
    cip = os.path.join(java_root, "level", "storage", "loot", "providers", "number", "ints", "ContextIntProviders.java")
    src = open(cip, encoding="utf-8").read()
    ticks = {}
    for m in re.finditer(r"context\.register\((COOKING_TIME_\w+),\s*cooking\([^)]*?,\s*(\d+)\)\)", src):
        ticks[m.group(1)] = int(m.group(2))
    out = {}
    isrc = open(items_path, encoding="utf-8").read()
    for name, text in java_statements(isrc, "Item").items():
        m = re.search(r"cookingFuel\(ContextIntProviders\.(COOKING_TIME_\w+)\)", text)
        if m and m.group(1) in ticks:
            out[name.lower()] = ticks[m.group(1)]
    return out


def read_foods(java_root, items_path):
    """Foods.java (nutrition, saturationModifier, alwaysEdible) + Items.java
    .food(Foods.X[, Consumables.Y]) -> {item: (nutrition, sat_mod, always, eat_ticks)}"""
    src = open(os.path.join(java_root, "food", "Foods.java"), encoding="utf-8").read()
    foods = {}
    for m in re.finditer(r"public static final FoodProperties (\w+) = (.*?);", src, re.S):
        text = m.group(2)
        n = re.search(r"nutrition\((\d+)\)", text)
        st = re.search(r"stew\((\d+)\)", text)
        sat = re.search(r"saturationModifier\(([\d.]+)F\)", text)
        if n:
            nut = int(n.group(1))
        elif st:
            nut = int(st.group(1))
        else:
            continue
        sm = float(sat.group(1)) if sat else (0.6 if st else 0.0)
        foods[m.group(1)] = (nut, sm, ".alwaysEdible()" in text)
    csrc = open(os.path.join(java_root, "item", "component", "Consumables.java"), encoding="utf-8").read()
    secs = {}
    for m in re.finditer(r"public static final Consumable (\w+) = (.*?);", csrc, re.S):
        c = re.search(r"consumeSeconds\(([\d.]+)F\)", m.group(2))
        secs[m.group(1)] = float(c.group(1)) if c else 1.6
    out = {}
    isrc = open(items_path, encoding="utf-8").read()
    for name, text in java_statements(isrc, "Item").items():
        m = re.search(r"\.food\(Foods\.(\w+)(?:,\s*Consumables\.(\w+))?\)", text)
        if m and m.group(1) in foods:
            nut, sm, always = foods[m.group(1)]
            sec = secs.get(m.group(2), 1.6) if m.group(2) else 1.6
            out[name.lower()] = (nut, sm, always, int(round(sec * 20)))
    return out


COLORS = ["white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray", "light_gray", "cyan",
          "purple", "blue", "brown", "green", "red", "black"]


def _family_item(family, color):
    """Items.WOOL.pick(CYAN) -> cyan_wool (DYED_x families drop the DYED_)"""
    fam = family.lower()
    if fam.startswith("dyed_"):
        fam = fam[5:]
    return "%s_%s" % (color, fam)


def read_creative_tabs(java_root):
    """CreativeModeTabs.java: every tab's row/column/type/flags/icon and its
    displayItems in order (accept(Items.X), accept(Items.FAMILY.color()),
    registerColoredItems(out, gameplayColorOrder, Items.FAMILY))"""
    src = open(os.path.join(java_root, "item", "CreativeModeTabs.java"), encoding="utf-8").read()
    m = re.search(r"List<DyeColor> gameplayColorOrder = List\.of\((.*?)\);", src, re.S)
    order = [c.lower() for c in re.findall(r"DyeColor\.(\w+)", m.group(1))] if m else COLORS
    tabs = []
    for m in re.finditer(r"CreativeModeTab\.builder\(CreativeModeTab\.Row\.(TOP|BOTTOM),\s*(\d+)\)(.*?)\.build\(\)", src, re.S):
        body = m.group(3)
        t = {"row": 0 if m.group(1) == "TOP" else 1, "col": int(m.group(2))}
        tm = re.search(r'\.title\(Component\.translatable\("([^"]+)"\)\)', body)
        t["title"] = tm.group(1) if tm else ""
        im = re.search(r"\.icon\(\(\) -> new ItemStack\((?:Items|Blocks)\.(\w+)(?:\.(\w+)\(\))?\)\)", body)
        t["icon"] = (_family_item(im.group(1), im.group(2)) if im.group(2) else im.group(1).lower()) if im else ""
        ty = re.search(r"\.type\(CreativeModeTab\.Type\.(\w+)\)", body)
        t["type"] = ty.group(1) if ty else "CATEGORY"
        t["aligned_right"] = ".alignedRight()" in body
        t["show_title"] = ".hideTitle()" not in body
        t["can_scroll"] = ".noScrollBar()" not in body
        bg = re.search(r"\.backgroundTexture\((\w+)\)", body)
        t["background"] = {"INVENTORY_BACKGROUND": "inventory", "SEARCH_BACKGROUND": "item_search"}.get(bg.group(1) if bg else "", "items")
        items = []
        for e in re.finditer(r"\.accept\((?:Items|Blocks)\.(\w+)(?:\.(\w+)\(\))?\)|registerColoredItems\(\w+,\s*gameplayColorOrder,\s*(?:Items|Blocks)\.(\w+)\)", body):
            if e.group(3):
                items += [_family_item(e.group(3), c) for c in order]
            elif e.group(2):
                items.append(_family_item(e.group(1), e.group(2)))
            else:
                items.append(e.group(1).lower())
        t["items"] = items
        tabs.append(t)
    return tabs


def read_tool_materials(path):
    src = open(path, encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"ToolMaterial (\w+) = new ToolMaterial\(BlockTags\.\w+,\s*(\d+),\s*([\d.]+)F,\s*([\d.]+)F", src):
        out[MATERIAL_JAVA[m.group(1)]] = {"durability": int(m.group(2)), "speed": float(m.group(3)), "bonus": float(m.group(4))}
    return out


# --- loot tables ---

def _silk_or_shear(cond):
    if cond is None:
        return False
    if isinstance(cond, str):
        return "silk_touch" in cond or "can_shear" in cond
    t = strip(cond.get("type", "")) if isinstance(cond, dict) else ""
    if t in ("any_of", "all_of"):
        return any(_silk_or_shear(c) for c in cond.get("terms", []))
    if t == "match_tool":
        return True
    if t == "inverted":
        return False
    return False


def _chance(cond):
    """probability from random_chance / table_bonus (fortune 0); 1 otherwise"""
    if cond is None or isinstance(cond, str):
        return 1.0
    t = strip(cond.get("type", ""))
    if t == "random_chance":
        return float(cond.get("chance", 1.0)) if not isinstance(cond.get("chance"), dict) else 1.0
    if t == "random_chance_with_enchanted_bonus":
        return float(cond.get("unenchanted_chance", 1.0))  # (no looting)
    if t == "table_bonus":
        return float(cond.get("chances", [1.0])[0])
    if t in ("all_of",):
        p = 1.0
        for c in cond.get("terms", []):
            p *= _chance(c)
        return p
    return 1.0


def _count(entry):
    lo = hi = 1
    fs = []
    for key in ("functions", "modifier", "modifiers"):
        v = entry.get(key, [])
        fs += v if isinstance(v, list) else [v]
    for f in fs:
        if not isinstance(f, dict):
            continue
        if strip(f.get("function", f.get("type", ""))) == "set_count":
            c = f.get("count")
            if isinstance(c, (int, float)):
                lo = hi = int(c)
            elif isinstance(c, dict):
                if strip(c.get("type", "")) == "uniform":
                    lo, hi = int(c.get("min", 1)), int(c.get("max", 1))
                elif "value" in c:
                    lo = hi = int(c["value"])
    return lo, hi


def _entry_drops(e, out, pool_chance):
    t = strip(e.get("type", ""))
    cond = e.get("condition") or (e.get("conditions") or [None])[0]
    if _silk_or_shear(cond):
        return False
    if t == "alternatives":
        for c in e.get("children", []):
            if _entry_drops(c, out, pool_chance):
                return True
        return False
    if t == "item":
        lo, hi = _count(e)
        out.append((strip(e["name"]), lo, hi, pool_chance * _chance(cond)))
        return True
    return False


# minecraft mobs whose loot tables bk's enemies use
MOB_LOOT = ["zombie", "cow", "pig", "chicken", "cod", "salmon", "silverfish", "spider",
            "skeleton", "creeper", "husk", "drowned", "snow_golem", "bat"]


def entity_drops(jar, name):
    """an entity loot table: [(item, min, max, chance)] (no looting, not on fire)"""
    data = jar.json("data/minecraft/loot_table/entities/%s.json" % name)
    if not data:
        return []
    out = []
    ok = ("killed_by_player", "random_chance", "random_chance_with_enchanted_bonus")
    for pool in data.get("pools", []):
        cond = pool.get("condition") or (pool.get("conditions") or [None])[0]
        terms = [cond] if cond is not None else []
        if cond is not None and not isinstance(cond, str) and strip(cond.get("type", "")) == "all_of":
            terms = cond.get("terms", [])
        # steve kills them: killed_by_player holds; anything about the
        # entity itself (a baby, on fire, riding) skips the pool
        if any(isinstance(c, str) or strip(c.get("type", "")) not in ok for c in terms):
            continue
        chance = 1.0
        for c in terms:
            chance *= _chance(c)
        entries = []
        for e in pool.get("entries", []):
            ec = e.get("condition")
            if ec is not None and not isinstance(ec, str) and strip(ec.get("type", "")) == "entity_properties":
                continue  # (e.g. zombie riding a chicken)
            entries.append(e)
        # LootPool: one roll picks one entry by weight (zombie: iron ingot,
        # carrot or potato, 2.5% together)
        rolls = pool.get("rolls", 1)
        total = sum(int(e.get("weight", 1)) for e in entries)
        for e in entries:
            share = int(e.get("weight", 1)) / total if (rolls == 1 and total > 0) else 1.0
            _entry_drops(e, out, chance * share)
    return out


def block_drops(jar, name):
    """what breaking the block gives without silk touch: [(item, min, max, chance)]"""
    data = jar.json("data/minecraft/loot_table/blocks/%s.json" % name)
    if not data:
        return []
    out = []
    for pool in data.get("pools", []):
        cond = pool.get("condition") or (pool.get("conditions") or [None])[0]
        if _silk_or_shear(cond):
            continue
        for e in pool.get("entries", []):
            _entry_drops(e, out, 1.0)
    return out


# --- recipes ---

def ingredient_options(spec, item_tags):
    if isinstance(spec, list):
        out = set()
        for s in spec:
            out |= ingredient_options(s, item_tags)
        return out
    if isinstance(spec, dict):
        spec = spec.get("item") or ("#" + spec["tag"] if "tag" in spec else "")
    if spec.startswith("#"):
        return set(item_tags.get(spec[1:]))
    return {strip(spec)}


ADV_RECIPE_DIRS = ["building_blocks", "decorations", "redstone", "tools", "combat", "food", "misc", "transportation", "brewing"]


def recipe_unlock_items(jar, name, item_tags):
    """data/minecraft/advancement/recipes/<dir>/<name>.json: the items of its
    inventory_changed criteria (picking any of them up unlocks the recipe)"""
    out = set()
    for d in ADV_RECIPE_DIRS:
        a = jar.json("data/minecraft/advancement/recipes/%s/%s.json" % (d, name))
        if not a:
            continue
        for crit in a.get("criteria", {}).values():
            if strip(crit.get("trigger", "")) != "inventory_changed":
                continue
            for pred in crit.get("conditions", {}).get("items", []):
                it = pred.get("items")
                if it:
                    out |= ingredient_options(it, item_tags)
        break
    return out


def read_recipes(jar, item_tags):
    out = []
    for name in jar.listdir("data/minecraft/recipe/"):
        r = jar.json("data/minecraft/recipe/%s.json" % name)
        typ = strip(r.get("type", ""))
        book = {"category": r.get("category", "misc"), "group": r.get("group", "")}
        res = r.get("result", {})
        result = strip(res.get("id", res.get("item", ""))) if isinstance(res, dict) else strip(res)
        count = res.get("count", 1) if isinstance(res, dict) else 1
        if typ == "crafting_shaped":
            pat = r["pattern"]
            key = {k: ingredient_options(v, item_tags) for k, v in r["key"].items()}
            grid = [[key.get(c) if c != " " else None for c in row] for row in pat]
            out.append(dict(book, name=name, shaped=True, grid=grid, result=result, count=count,
                            unlock=recipe_unlock_items(jar, name, item_tags)))
        elif typ == "smelting":
            out.append({"name": name, "smelting": True, "ing": ingredient_options(r["ingredient"], item_tags),
                        "result": result, "count": count, "time": r.get("cookingtime", 200),
                        "xp": r.get("experience", 0.0)})
        elif typ == "crafting_shapeless":
            ings = [ingredient_options(i, item_tags) for i in r["ingredients"]]
            out.append(dict(book, name=name, shaped=False, ings=ings, result=result, count=count,
                            unlock=recipe_unlock_items(jar, name, item_tags)))
    return out


# --- everything ---

def collect(jar, src_root):
    block_tags = Tags(jar, "block")
    item_tags = Tags(jar, "item")
    java = os.path.join(src_root, "net", "minecraft", "world")
    bprops = read_blocks_java(os.path.join(java, "level", "block", "Blocks.java"))
    iprops = read_items_java(os.path.join(java, "item", "Items.java"))
    materials = read_tool_materials(os.path.join(java, "item", "ToolMaterial.java"))
    armor_mats = read_armor_materials(os.path.join(java, "item", "equipment", "ArmorMaterials.java"))
    fuels = read_fuels(java, os.path.join(java, "item", "Items.java"))
    foods = read_foods(java, os.path.join(java, "item", "Items.java"))
    tabs = read_creative_tabs(java)

    # blocks: every blockstate whose default model is one full box
    blocks = []
    for name in jar.listdir("assets/minecraft/blockstates/"):
        if name in SKIP_BLOCKS or name in _MODEL_BLOCK_NAMES or not jar.has("assets/minecraft/items/%s.json" % name):
            continue
        state = jar.json("assets/minecraft/blockstates/%s.json" % name)
        model = default_variant_model(state)
        if model is None:
            continue
        if name == "grass_block":
            faces = [("block/grass_block_top", True), ("block/dirt", False)] + [("block/grass_block_side", True)] * 4
        else:
            faces = cube_faces(jar, model)
        if faces is None:
            continue
        p = bprops.get(name, {"hardness": 1.0, "requires_tool": False})
        kind, states, dstate = (0, [], 0) if name == "grass_block" else block_states(jar, name, state)
        if name == "observer" and kind == 2:
            kind = 4  # ObserverBlock: faces where you look (getNearestLookingDirection)
        blocks.append({"name": name, "faces": faces, "tint": item_tint(jar, name),
                       "place": kind, "states": states, "default_state": dstate,
                       "hardness": p["hardness"] if p["hardness"] is not None else 1.0,
                       "requires_tool": p["requires_tool"], "xp": p.get("xp", (0, 0)),
                       "resist": p.get("resist", 0.0)})
    # model blocks (elements, not one box): levers, buttons, torches, dust...
    import mc_models
    for name in sorted(mc_models.MODEL_BLOCKS):
        state = jar.json("assets/minecraft/blockstates/%s.json" % name)
        if state is None:
            continue
        p = bprops.get(name, {"hardness": 1.0, "requires_tool": False})
        if name in mc_models.MODEL_FIXED:
            # one model for every state (fire: the floor fire, no sides yet)
            mstates, parts = [mc_models.bake(jar, mc_models.MODEL_FIXED[name])], None
            first = mstates[0][0] if mstates[0] else None
        elif "multipart" in state:
            mstates, parts = None, mc_models.multipart_parts(jar, state)
            first = next((q for pt in parts for q in pt[1]), None)
        else:
            mstates, parts = mc_models.model_block_states(jar, name, state), None
            first = next((q for qs in mstates for q in qs), None)
        if first is None:
            continue
        # the particle texture stands in for the 6 cube faces (break particles)
        chain = model_chain(jar, _variant_model(state))
        textures = {}
        for _, m in reversed(chain):
            textures.update(m.get("textures", {}))
        particle = resolve_tex(textures, textures.get("particle", "")) or first[2]
        blocks.append({"name": name, "faces": [(particle, False)] * 6, "tint": None,
                       "place": 5, "states": [], "default_state": 0,
                       "model_states": mstates, "model_parts": parts,
                       "no_collision": p.get("no_collision", False),
                       "opaque_model": name == "redstone_lamp",
                       "hardness": p["hardness"] if p["hardness"] is not None else 1.0,
                       "requires_tool": p["requires_tool"], "xp": p.get("xp", (0, 0)),
                       "resist": p.get("resist", 0.0)})
    # the shaped families (tools/mc_families.py): stairs, slabs, doors, fences...
    import mc_families
    have = set(b["name"] for b in blocks) | set(mc_models.MODEL_BLOCKS)
    for name in jar.listdir("assets/minecraft/blockstates/"):
        if name in have or name in SKIP_BLOCKS:
            continue
        if not jar.has("assets/minecraft/items/%s.json" % name) and not name.endswith("_wall_torch"):
            continue  # (wall torches: the torch item places them)
        state = jar.json("assets/minecraft/blockstates/%s.json" % name)
        fam = mc_families.family_of(jar, name, state)
        if fam is None:
            continue
        props = mc_families.FAMILIES[fam][1]
        p = bprops.get(name, {"hardness": 1.0, "requires_tool": False})
        if props is None:
            # fence / pane: multipart, the cell's state is the sides mask
            mstates, parts = None, mc_models.multipart_parts(jar, state, keep_vars=True)
            if fam == "fence":
                shapes = [(mc_families.cross_shape(m, 4, 16, 4, 16), mc_families.cross_shape(m, 4, 24, 4, 24)) for m in range(16)]
            else:
                shapes = [(mc_families.cross_shape(m, 2, 16, 2, 16),) * 2 for m in range(16)]
            first = next((q for pt in parts for q in pt[1]), None)
        else:
            sts = mc_families.variant_states(jar, name, state, props)
            if len(sts) > 64:
                continue  # (6 state bits)
            mstates = [mc_models.bake(jar, v["model"], v.get("x", 0), v.get("y", 0), v.get("uvlock", False), True) if v else []
                       for _, v in sts]
            parts = None
            shapes = [mc_families.state_shapes(fam, name, want, jar, v) for want, v in sts]
            first = next((q for qs in mstates for q in qs), None)
        if first is None:
            continue
        textures = mc_families.block_textures(jar, mc_families._first_model(state))
        particle = resolve_tex(textures, textures.get("particle", "")) or first[2]
        blocks.append({"name": name, "faces": [(particle, False)] * 6, "tint": None,
                       "place": 5, "states": [], "default_state": 0, "family": fam, "offset": p.get("offset", 0),
                       "model_states": mstates, "model_parts": parts, "shapes": shapes,
                       "no_collision": p.get("no_collision", False) or fam in ("plant", "button", "plate", "torch", "wall_torch"),
                       "opaque_model": False,
                       "hardness": p["hardness"] if p["hardness"] is not None else 1.0,
                       "requires_tool": p["requires_tool"], "xp": p.get("xp", (0, 0)),
                       "resist": p.get("resist", 0.0)})
    for b in blocks:
        if "family" not in b:
            n = b["name"]
            b["family"] = ("trapdoor" if n.endswith("_trapdoor") else "button" if n.endswith("_button") else
                           "torch" if n == "torch" else "wall_torch" if n == "wall_torch" else
                           "lever" if n == "lever" else None) if b.get("model_states") is not None else None
    # the chest (ChestBlock): drawn by chest.c with ChestModel, oak planks for
    # the break particles (its block model only names a particle texture)
    p = bprops.get("chest", {"hardness": 2.5, "requires_tool": False})
    blocks.append({"name": "chest", "faces": [("block/oak_planks", False)] * 6, "tint": None,
                   "place": 1, "states": [], "default_state": 0, "chest": True,
                   "hardness": p["hardness"] if p["hardness"] is not None else 2.5,
                   "requires_tool": False, "xp": (0, 0), "resist": p.get("resist", 2.5)})
    # fluids (LiquidBlock): drawn by fluids.c (LiquidBlockRenderer), the
    # still texture stands in for the faces
    for name in ("water", "lava"):
        p = bprops.get(name, {"hardness": 100.0, "requires_tool": False})
        blocks.append({"name": name, "faces": [("block/%s_still" % name, False)] * 6, "tint": None,
                       "place": 6, "states": [], "default_state": 0, "fluid": True, "no_collision": True,
                       "hardness": p["hardness"] if p["hardness"] is not None else 100.0,
                       "requires_tool": False, "xp": (0, 0), "resist": p.get("resist", 100.0)})
    blocks.sort(key=lambda b: b["name"])
    # grass first etc. doesn't matter: ids are by name order (stable per version)
    block_names = [b["name"] for b in blocks]
    block_set = set(block_names)

    # tool rules
    mine = {k: block_tags.get("mineable/" + k) for k in ("pickaxe", "axe", "shovel", "hoe")}
    sword_eff = block_tags.get("sword_efficient")
    washed = block_tags.get("washed_away_by_fluids")  # FlowingFluid.canHoldAnyFluid
    incorrect = {m: block_tags.get("incorrect_for_%s_tool" % m.replace("en", "") if m in ("wooden", "golden") else
                                   "incorrect_for_%s_tool" % m) for m in MATERIALS}
    # the tag names are incorrect_for_wooden_tool / _gold_tool
    incorrect["wooden"] = block_tags.get("incorrect_for_wooden_tool")
    incorrect["golden"] = block_tags.get("incorrect_for_gold_tool")
    for b in blocks:
        n = b["name"]
        b["mine"] = [k for k in ("pickaxe", "axe", "shovel", "hoe") if n in mine[k]]
        b["sword_efficient"] = n in sword_eff
        b["incorrect"] = [m for m in MATERIALS if n in incorrect[m]]
        b["drops"] = [] if b.get("fluid") else block_drops(jar, n)
        b["washed"] = n in washed

    # items: flat sprites that matter (dropped, crafted, tools)
    recipes = read_recipes(jar, item_tags)
    wanted = set()
    for b in blocks:
        for d in b["drops"]:
            wanted.add(d[0])
    for r in recipes:
        wanted.add(r["result"])
    for name, it in iprops.items():
        if it["tool"]:
            wanted.add(name)
    wanted |= {"water_bucket", "lava_bucket", "bucket", "redstone", "egg", "stick", "paper", "iron_sword", "buried_treasure_map", "ender_eye", "bow", "arrow"}
    # mob loot (bk enemies drop like these minecraft mobs, see mob_drops)
    mob_loot = {}
    for mob in MOB_LOOT:
        mob_loot[mob] = entity_drops(jar, mob)
        for d in mob_loot[mob]:
            wanted.add(d[0])
    for t in tabs:
        wanted.add(t["icon"])
    items = []
    for name in sorted(wanted):
        if name in block_set:
            continue
        sprite = flat_item_sprite(jar, name)
        # the shield's item model is special (ShieldModel), no flat sprite:
        # the mod draws the model itself
        if name != "shield" and (sprite is None or not jar.has("assets/minecraft/textures/%s.png" % sprite)):
            continue
        ip = iprops.get(name, {"stack": 64, "durability": 0, "tool": None})
        tool = ip["tool"]
        entry = {"name": name, "sprite": sprite, "stack": ip["stack"], "durability": ip["durability"],
                 "tool": None, "handheld": item_is_handheld(jar, name)}
        if tool:
            kind, mat, dmg, spd = tool
            mm = materials.get(mat, {"durability": 0, "speed": 1.0, "bonus": 0.0})
            entry["tool"] = {"kind": kind, "material": mat, "speed": mm["speed"],
                             "damage": 1.0 + dmg + mm["bonus"], "attack_speed": 4.0 + spd}
            entry["durability"] = mm["durability"]
        if ip.get("spear"):
            mat, dur = ip["spear"]
            mm = materials.get(mat, {"durability": 0, "speed": 1.0, "bonus": 0.0})
            entry["weapon"] = {"material": mat, "damage": 1.0 + mm["bonus"], "attack_speed": 1.0 / dur}
            entry["durability"] = mm["durability"]
        if ip.get("armor") and ip["armor"][0] in armor_mats and ip["armor"][1] in ARMOR_TYPES:
            am = armor_mats[ip["armor"][0]]
            slot, unit = ARMOR_TYPES[ip["armor"][1]]
            entry["armor"] = {"slot": slot, "defense": am["defense"][ip["armor"][1]], "toughness": am["toughness"],
                              "kb": am["kb"], "asset": am["asset"]}
            entry["durability"] = unit * am["durability"]  # ArmorType.getDurability
        items.append(entry)

    item_names = block_names + [i["name"] for i in items]
    item_set = set(item_names)

    # recipes that only use what we have
    ok = []
    smelting = []
    for r in recipes:
        if r["result"] not in item_set:
            continue
        if r.get("smelting"):
            ing = r["ing"] & item_set
            if ing:
                r["ing"] = ing
                smelting.append(r)
            continue
        cells = [c for row in r["grid"] for c in row] if r["shaped"] else r["ings"]
        good = True
        for c in cells:
            if c is None:
                continue
            c &= item_set
            if not c:
                good = False
                break
        if not good:
            continue
        if r["shaped"]:
            r["grid"] = [[(c & item_set) if c is not None else None for c in row] for row in r["grid"]]
            if len(r["grid"]) > 3 or any(len(row) > 3 for row in r["grid"]):
                continue
        else:
            r["ings"] = [c & item_set for c in r["ings"]]
            if len(r["ings"]) > 9:
                continue
        ok.append(r)

    # drops of things we don't have are dropped
    for b in blocks:
        b["drops"] = [d for d in b["drops"] if d[0] in item_set][:2]

    fuel = {n: t for n, t in fuels.items() if n in item_set}
    food = {n: f for n, f in foods.items() if n in item_set}
    for mob in mob_loot:
        mob_loot[mob] = [d for d in mob_loot[mob] if d[0] in item_set]
    for t in tabs:
        t["items"] = [n for n in t["items"] if n in item_set]
    tabs.append({"mob_loot": mob_loot})
    # FireBlock.bootStrap: setFlammable(block, igniteOdds, burnOdds)
    global FLAMMABLE
    fsrc = open(os.path.join(java, "level", "block", "FireBlock.java"), encoding="utf-8").read()
    FLAMMABLE = {m.group(1).lower(): (int(m.group(2)), int(m.group(3)))
                 for m in re.finditer(r"setFlammable\(Blocks\.(\w+),\s*(\d+),\s*(\d+)\)", fsrc)}
    global ITEM_PLACES
    ITEM_PLACES = [(i, b, w) for i, b, w in read_item_places(os.path.join(java, "item", "Items.java"))
                   if i in item_set and b in block_set and (w is None or w in block_set)]
    return blocks, items, ok, smelting, fuel, food, tabs
