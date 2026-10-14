# block models with elements (levers, buttons, torches, redstone dust,
# trapdoors...), baked like minecraft's FaceBakery (26.3):
#   - FaceInfo vertex order, defaultFaceUV, CuboidFace.getU/getV with the
#     face rotation (Quadrant.rotateVertexIndex)
#   - element rotation (CuboidRotation: single axis or euler xyz, rescale)
#     about its origin
#   - the variant's x/y rotation (BlockModelRotation: BLOCK_ROT_X_90 =
#     -90 about x, BLOCK_ROT_Y_90 = -90 about y, y applied after x) about the
#     block middle
#   - uvlock: the uv goes through inverseFaceTransformation
#     (BlockMath.getFaceTransformation)
#   - cullface rotated with the model (Direction.rotate)
# the result is plain quads (positions 0..1, uvs in texels) per block state.

import itertools
import math

from mc_data import model_chain, resolve_tex, _parse_key

DIRS = ["up", "down", "south", "north", "east", "west"]  # the mod's face order
UNIT = {"up": (0, 1, 0), "down": (0, -1, 0), "south": (0, 0, 1), "north": (0, 0, -1), "east": (1, 0, 0), "west": (-1, 0, 0)}

# FaceInfo: per face, 4 vertices as (x, y, z) picks: 0 = min, 1 = max
FACE_INFO = {
    "down": [(0, 0, 1), (0, 0, 0), (1, 0, 0), (1, 0, 1)],
    "up": [(0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)],
    "north": [(1, 1, 0), (1, 0, 0), (0, 0, 0), (0, 1, 0)],
    "south": [(0, 1, 1), (0, 0, 1), (1, 0, 1), (1, 1, 1)],
    "west": [(0, 1, 0), (0, 0, 0), (0, 0, 1), (0, 1, 1)],
    "east": [(1, 1, 1), (1, 0, 1), (1, 0, 0), (1, 1, 0)],
}


def default_face_uv(f, t, d):
    return {
        "down": (f[0], 16 - t[2], t[0], 16 - f[2]),
        "up": (f[0], f[2], t[0], t[2]),
        "north": (16 - t[0], 16 - t[1], 16 - f[0], 16 - f[1]),
        "south": (f[0], 16 - t[1], t[0], 16 - f[1]),
        "west": (f[2], 16 - t[1], t[2], 16 - f[1]),
        "east": (16 - t[2], 16 - t[1], 16 - f[2], 16 - f[1]),
    }[d]


def _vu(uvs, i):  # UVs.getVertexU / getVertexV
    return uvs[0] if i in (0, 1) else uvs[2]


def _vv(uvs, i):
    return uvs[1] if i in (0, 3) else uvs[3]


# 3x3 matrices
def mmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def mvec(m, v):
    return tuple(sum(m[i][k] * v[k] for k in range(3)) for i in range(3))


def rot_x(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return [[1, 0, 0], [0, c, -s], [0, s, c]]


def rot_y(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return [[c, 0, s], [0, 1, 0], [-s, 0, c]]


def rot_z(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


IDENT = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]


def _snap(m):
    return [[round(x) if abs(x - round(x)) < 1e-6 else x for x in row] for row in m]


def block_rotation(x, y):
    # Quadrant.fromXYZAngles: rotationY.compose(rotationX), each -angle
    return _snap(mmul(rot_y(-y), rot_x(-x)))


def nearest_dir(v):
    best, bp = None, 0.0
    for d in DIRS:
        u = UNIT[d]
        p = v[0] * u[0] + v[1] * u[1] + v[2] * u[2]
        if p > bp + 1e-6:
            best, bp = d, p
    return best


# BlockMath.VANILLA_UV_TRANSFORM_LOCAL_TO_GLOBAL (joml rotateY / rotateX)
L2G = {"south": IDENT, "east": rot_y(90), "west": rot_y(-90), "north": rot_y(180), "up": rot_x(-90), "down": rot_x(90)}


def _inv(m):  # rotations: transpose
    return [[m[j][i] for j in range(3)] for i in range(3)]


def face_transformation(T, side):
    """BlockMath.getFaceTransformation"""
    fa = mmul(T, L2G[side])
    n = mvec(fa, (0, 0, 1))
    new_side = nearest_dir(n)
    return mmul(_inv(L2G[new_side]), fa)


def element_rotation(rot):
    """CuboidRotation transform (3x3) and origin (blocks)"""
    if rot is None:
        return None
    origin = tuple(o / 16.0 for o in rot.get("origin", [8, 8, 8]))
    if "axis" in rot:
        ax = rot["axis"]
        ang = rot.get("angle", 0)
        m = {"x": rot_x, "y": rot_y, "z": rot_z}[ax](ang) if ang else IDENT
    else:
        # EulerXYZRotation: rotationZYX(z, y, x)
        m = mmul(rot_z(rot.get("z", 0)), mmul(rot_y(rot.get("y", 0)), rot_x(rot.get("x", 0))))
    if rot.get("rescale") and m is not IDENT:
        sc = []
        for a in range(3):
            unit = [0, 0, 0]
            unit[a] = 1
            t = mvec(m, unit)
            sc.append(1.0 / max(abs(c) for c in t))
        m = [[m[i][j] * sc[j] for j in range(3)] for i in range(3)]  # result.scale(s)
    return m, origin


def bake(jar, model, x=0, y=0, uvlock=False, keep_vars=False):
    """quads of a model: [(verts[4] (x,y,z) 0..1, uv[4] (u,v) texels,
    texture name, tinted, cullface dir or None, facing dir)] (keep_vars: + the
    face's own texture reference, "#side", for shared family quads)"""
    chain = model_chain(jar, model)
    textures = {}
    for _, m in reversed(chain):
        textures.update(m.get("textures", {}))
    elements = None
    for _, m in chain:
        if "elements" in m:
            elements = m["elements"]
            break
    if not elements:
        return []
    T = block_rotation(x, y)
    rotated = (x % 360) != 0 or (y % 360) != 0
    quads = []
    for e in elements:
        f, t = e["from"], e["to"]
        er = element_rotation(e.get("rotation"))
        for d, face in e.get("faces", {}).items():
            tex = resolve_tex(textures, face.get("texture", ""))
            if tex is None:
                continue
            uvs = tuple(face["uv"]) if "uv" in face else default_face_uv(f, t, d)
            shift = (face.get("rotation", 0) // 90) % 4
            if uvlock and rotated:
                inv = _inv(face_transformation(T, d))
            else:
                inv = None
            verts, uv = [], []
            for i in range(4):
                pick = FACE_INFO[d][i]
                v = tuple((t[k] if pick[k] else f[k]) / 16.0 for k in range(3))
                if er is not None:
                    m, o = er
                    v = tuple(a + b for a, b in zip(mvec(m, tuple(v[k] - o[k] for k in range(3))), o))
                if rotated:
                    v = tuple(a + 0.5 for a in mvec(T, tuple(c - 0.5 for c in v)))
                verts.append(v)
                ri = (i + shift) % 4
                ru, rv = _vu(uvs, ri) / 16.0, _vv(uvs, ri) / 16.0
                if inv is not None:
                    p = mvec(inv, (ru - 0.5, rv - 0.5, 0.0))
                    ru, rv = p[0] + 0.5, p[1] + 0.5
                uv.append((ru * 16.0, rv * 16.0))
            # the quad's facing from its vertices (calculateFacing)
            a, b, c = verts[0], verts[1], verts[2]
            e1 = tuple(b[k] - a[k] for k in range(3))
            e2 = tuple(c[k] - a[k] for k in range(3))
            n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            facing = nearest_dir(n) or "up"
            cull = face.get("cullface")
            if cull and rotated:
                cull = nearest_dir(mvec(T, UNIT[cull]))
            shade = e.get("shade_direction_override") or facing
            if keep_vars:
                quads.append((verts, uv, tex, "tintindex" in face, cull, shade, face.get("texture", "")))
            else:
                quads.append((verts, uv, tex, "tintindex" in face, cull, shade))
    return quads


def _variant(v):
    return v[0] if isinstance(v, list) else v


# the model blocks: which blockstate properties the mod keeps (state index =
# these in order, the last one fastest), with their value order
MODEL_BLOCKS = {
    "lever": [("face", ["floor", "wall", "ceiling"]), ("facing", ["south", "north", "east", "west"]), ("powered", ["false", "true"])],
    "stone_button": [("face", ["floor", "wall", "ceiling"]), ("facing", ["south", "north", "east", "west"]), ("powered", ["false", "true"])],
    "oak_button": [("face", ["floor", "wall", "ceiling"]), ("facing", ["south", "north", "east", "west"]), ("powered", ["false", "true"])],
    "redstone_torch": [("lit", ["false", "true"])],
    "redstone_wall_torch": [("facing", ["south", "north", "east", "west"]), ("lit", ["false", "true"])],
    "redstone_lamp": [("lit", ["false", "true"])],
    "oak_trapdoor": [("facing", ["south", "north", "east", "west"]), ("half", ["bottom", "top"]), ("open", ["false", "true"])],
    "iron_trapdoor": [("facing", ["south", "north", "east", "west"]), ("half", ["bottom", "top"]), ("open", ["false", "true"])],
    "redstone_wire": [("power", [str(i) for i in range(16)])],
    "torch": [],
    "fire": [],
    "wall_torch": [("facing", ["south", "north", "east", "west"])],
}


# blocks drawn with one fixed model (multipart blocks whose parts aren't done)
MODEL_FIXED = {}

# multipart properties in the order the mod passes them (wire: none/side/up,
# fire: false/true)
MULTIPART_PROPS = {
    "redstone_wire": ["north", "east", "south", "west"],
    "fire": ["north", "east", "south", "west", "up"],
}


def _matches(key_props, want):
    return all(key_props.get(k) == v for k, v in want.items())


def model_block_states(jar, name, state):
    """[quads per state] for a variants blockstate, in MODEL_BLOCKS order"""
    props = MODEL_BLOCKS[name]
    variants = state.get("variants", {})
    keys = [(k, _parse_key(k)) for k in variants]
    out = []
    for combo in itertools.product(*[vals for _, vals in props]):
        want = {p: v for (p, _), v in zip(props, combo)}
        found = None
        for k, kp in keys:
            # a key matches when every property it names agrees
            if all(want.get(p, kp[p]) == kp[p] for p in kp):
                found = _variant(variants[k])
                break
        if found is None:
            out.append([])
            continue
        out.append(bake(jar, found["model"], found.get("x", 0), found.get("y", 0), found.get("uvlock", False)))
    return out


def multipart_parts(jar, state, keep_vars=False):
    """multipart blocks (dust, fire): [(OR list of {prop: [values]}, quads,
    alternative index, alternative count)]. an "apply" list is a weighted
    random pick per block position (all weights 1 here)"""
    parts = []
    for case in state.get("multipart", []):
        when = case.get("when")
        if when is None:
            terms = [{}]
        elif "OR" in when:
            terms = when["OR"]
        else:
            terms = [when]
        conds = [{k: str(v).split("|") for k, v in t.items()} for t in terms]
        alts = case["apply"] if isinstance(case["apply"], list) else [case["apply"]]
        for i, ap in enumerate(alts):
            parts.append((conds, bake(jar, ap["model"], ap.get("x", 0), ap.get("y", 0), ap.get("uvlock", False), keep_vars), i, len(alts)))
    return parts
