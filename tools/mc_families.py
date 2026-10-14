"""the shaped block families (26.3): slabs, stairs, doors, trapdoors, buttons,
pressure plates, fences, fence gates, panes / bars, carpets, plants, ladders,
lanterns, chains, snow layers, torches.

each family keeps the blockstate properties the mod stores in a cell's 6
state bits (state index = these in order, the last fastest), the shapes come
from the blocks' java (VoxelShapes: outline + collision boxes per state, in
1/16 block), and every block of a family shares the family's baked quads with
its own textures (texture slots: the model's #variables)."""

import itertools

import mc_models
from mc_data import model_chain, resolve_tex, _parse_key

H4 = ["south", "north", "east", "west"]  # the mod's horizontal facing order
FT = ["false", "true"]

# family -> (C id, the properties kept)
FAMILIES = {
    "slab": (1, [("type", ["bottom", "top", "double"])]),
    "stairs": (2, [("facing", H4), ("half", ["bottom", "top"]),
                   ("shape", ["straight", "inner_left", "inner_right", "outer_left", "outer_right"])]),
    "door": (3, [("facing", H4), ("half", ["lower", "upper"]), ("hinge", ["left", "right"]), ("open", FT)]),
    "trapdoor": (4, [("facing", H4), ("half", ["bottom", "top"]), ("open", FT)]),
    "button": (5, [("face", ["floor", "wall", "ceiling"]), ("facing", H4), ("powered", FT)]),
    "plate": (6, [("powered", FT)]),
    "fence": (7, None),   # multipart: north, east, south, west (false / true)
    "gate": (8, [("facing", H4), ("in_wall", FT), ("open", FT)]),
    "pane": (9, None),    # multipart, like the fence
    "carpet": (10, []),
    "plant": (11, []),
    "ladder": (12, [("facing", H4)]),
    "lantern": (13, [("hanging", FT)]),
    "chain": (14, [("axis", ["y", "x", "z"])]),
    "snow": (15, [("layers", [str(i) for i in range(1, 9)])]),
    "torch": (16, []),
    "wall_torch": (17, [("facing", H4)]),
    "lever": (18, None),  # (the existing model blocks, for the c side)
}
FAMILY_IDS = {k: v[0] for k, v in FAMILIES.items()}
MULTIPART_SIDES = ["north", "east", "south", "west"]

# the model at the bottom of a blockstate's default variant -> family
PARENTS = {
    "block/slab": "slab", "block/inner_stairs": "stairs", "block/stairs": "stairs", "block/outer_stairs": "stairs",
    "block/door_bottom_left": "door", "block/template_orientable_trapdoor_bottom": "trapdoor",
    "block/template_trapdoor_bottom": "trapdoor", "block/button": "button", "block/pressure_plate_up": "plate",
    "block/fence_post": "fence", "block/template_fence_gate": "gate", "block/template_glass_pane_post": "pane",
    "block/template_bars_post_ends": "pane", "block/carpet": "carpet", "block/cross": "plant",
    "block/tinted_cross": "plant", "block/ladder": "ladder", "block/template_lantern": "lantern",
    "block/template_chain": "chain", "block/snow_height2": "snow", "block/template_torch": "torch",
}
# blocks that look like a family but behave differently (left out)
EXCLUDE = {"light_weighted_pressure_plate", "heavy_weighted_pressure_plate", "petrified_oak_slab",
           "bamboo_sapling", "big_dripleaf", "small_dripleaf"}


def _bottom_parent(jar, model):
    m = model.replace("minecraft:", "")
    last = m
    for _ in range(8):
        d = jar.json("assets/minecraft/models/%s.json" % m)
        if d is None:
            break
        last = m
        if "elements" in d or not d.get("parent"):
            break
        m = d["parent"].replace("minecraft:", "")
    return last


def _first_model(state):
    if "variants" in state:
        v = list(state["variants"].values())[0]
    elif "multipart" in state:
        v = state["multipart"][0]["apply"]
    else:
        return None
    if isinstance(v, list):
        v = v[0]
    return v.get("model")


def family_of(jar, name, state):
    if name in EXCLUDE or state is None:
        return None
    m = _first_model(state)
    if m is None:
        return None
    fam = PARENTS.get("block/" + _bottom_parent(jar, m).split("block/")[-1])
    if fam == "torch" and name.endswith("wall_torch"):
        fam = "wall_torch"
    if fam is None and name.endswith("wall_torch"):
        p = _bottom_parent(jar, m)
        if p.endswith("template_torch_wall") or p.endswith("torch_wall"):
            fam = "wall_torch"
    return fam


# --- shapes (VoxelShape boxes, 1/16) ---

def _rot_h(box, facing):
    """a box given for north (Shapes.rotateHorizontal) turned to facing"""
    (x0, y0, z0), (x1, y1, z1) = box
    if facing == "north":
        return box
    if facing == "south":
        return ((16 - x1, y0, 16 - z1), (16 - x0, y1, 16 - z0))
    if facing == "east":
        return ((16 - z1, y0, x0), (16 - z0, y1, x1))
    return ((z0, y0, 16 - x1), (z1, y1, 16 - x0))  # west


def _column(w, y0, y1):
    return ((8 - w / 2, y0, 8 - w / 2), (8 + w / 2, y1, 8 + w / 2))


def cross_shape(mask, post_w, post_h, wall_w, wall_h):
    """CrossCollisionBlock.makeShapes: the post + an arm per connected side
    (mask bits: north, east, south, west)"""
    boxes = [_column(post_w, 0, post_h)]
    arm = ((8 - wall_w / 2, 0, 0), (8 + wall_w / 2, wall_h, 8))  # boxZ(wallWidth, 0, wallTop, 0, 8): north
    for i, d in enumerate(MULTIPART_SIDES):
        if mask & (1 << i):
            boxes.append(_rot_h(arm, d))
    return boxes


def _element_boxes(jar, model, x, y):
    """the model's elements as boxes, turned like the variant (BlockModelRotation)"""
    chain = model_chain(jar, model)
    elements = None
    for _, m in chain:
        if "elements" in m:
            elements = m["elements"]
            break
    out = []
    T = mc_models.block_rotation(x, y)
    for e in elements or []:
        f, t = e["from"], e["to"]
        pts = [mc_models.mvec(T, (a / 16 - 0.5, b / 16 - 0.5, c / 16 - 0.5)) for a in (f[0], t[0]) for b in (f[1], t[1]) for c in (f[2], t[2])]
        lo = tuple(round((min(p[k] for p in pts) + 0.5) * 16, 3) for k in range(3))
        hi = tuple(round((max(p[k] for p in pts) + 0.5) * 16, 3) for k in range(3))
        out.append((lo, hi))
    return out


def state_shapes(fam, name, want, jar, variant):
    """(outline boxes, collision boxes) of one state"""
    if fam == "gate":
        axis_x = want["facing"] in ("east", "west")
        shape = ((0, 0, 6), (16, 13 if want["in_wall"] == "true" else 16, 10))
        coll = ((0, 0, 6), (16, 24, 10))
        if axis_x:
            shape = ((6, shape[0][1], 0), (10, shape[1][1], 16))
            coll = ((6, 0, 0), (10, 24, 16))
        return [shape], ([] if want["open"] == "true" else [coll])
    if fam == "ladder":
        # LadderBlock.SHAPES = rotateHorizontal(boxZ(16, 13, 16)), by its facing
        b = _rot_h(((0, 0, 13), (16, 16, 16)), want["facing"])
        return [b], [b]
    if fam == "snow":
        n = int(want["layers"])
        return [((0, 0, 0), (16, n * 2, 16))], ([((0, 0, 0), (16, (n - 1) * 2, 16))] if n > 1 else [])
    if fam == "chain":
        a = want["axis"]
        b = {"y": ((6.5, 0, 6.5), (9.5, 16, 9.5)), "x": ((0, 6.5, 6.5), (16, 9.5, 9.5)), "z": ((6.5, 6.5, 0), (9.5, 9.5, 16))}[a]
        return [b], [b]
    boxes = _element_boxes(jar, variant["model"], variant.get("x", 0), variant.get("y", 0)) if variant else []
    if fam == "plant":
        # the plant classes' SHAPE (Block.column(width, 0, height))
        if name.endswith("_sapling") or name.endswith("propagule"):
            w, h = 12, 12        # SaplingBlock
        elif name.endswith("mushroom"):
            w, h = 6, 6          # MushroomBlock
        elif name in ("short_grass", "fern", "dead_bush", "bush", "short_dry_grass", "tall_dry_grass"):
            w, h = 12, 13        # TallGrassBlock / DryVegetationBlock
        elif name.endswith("_fungus"):
            w, h = 8, 9          # FungusBlock
        elif name.endswith("_roots") or name == "nether_sprouts":
            w, h = 12, 13 if name.endswith("_roots") else 3
        else:
            w, h = 6, 10         # FlowerBlock
        return [_column(w, 0, h)], []
    if fam in ("button", "plate", "torch", "wall_torch"):
        return boxes[:1] if fam != "plate" else boxes, []
    return boxes, boxes


def variant_states(jar, name, state, props):
    """[(want, variant)] in the mod's state order"""
    variants = state.get("variants", {})
    keys = [(k, _parse_key(k)) for k in variants]
    out = []
    for combo in itertools.product(*[vals for _, vals in props]):
        want = {p: v for (p, _), v in zip(props, combo)}
        found = None
        for k, kp in keys:
            if all(want.get(p, kp[p]) == kp[p] for p in kp):
                found = mc_models._variant(variants[k])
                break
        out.append((want, found))
    return out


def block_textures(jar, model):
    chain = model_chain(jar, model)
    textures = {}
    for _, m in reversed(chain):
        textures.update(m.get("textures", {}))
    return textures
