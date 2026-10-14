# minecraft's advancements (data/minecraft/advancement/<tab>/*.json) for the
# mod's advancement screen + toasts: display info, the criteria the mod can
# tell apart, requirement groups, and the tree layout
# (TreeNodePosition.run, ported as is).

TABS = ["story", "nether", "end", "adventure", "husbandry"]  # the screen's tab order
TRIG = {"inventory_changed": 1, "consume_item": 2, "player_killed_entity": 3, "entity_killed_player": 4}
FRAMES = {"task": 0, "goal": 1, "challenge": 2}
# the minecraft mobs bk enemies count as (survival.c mob_table), in mask bit order
# the mobs bk enemies count as, in survival.c McMob order (mask bit = index)
KILLABLE = ["zombie", "cow", "cod", "spider", "chicken", "skeleton", "creeper", "husk", "drowned", "snow_golem", "bat"]


def strip(s):
    return s.split(":", 1)[1] if isinstance(s, str) and ":" in s else s


class Node:
    def __init__(self, adv, parent, prev, child_index, depth):
        self.adv = adv
        self.parent = parent
        self.prev = prev
        self.child_index = child_index
        self.children = []
        self.ancestor = self
        self.thread = None
        self.x = depth
        self.y = -1.0
        self.mod = 0.0
        self.change = 0.0
        self.shift = 0.0
        prev_child = None
        for c in adv["children"]:
            prev_child = self.add_child(c, prev_child)

    def add_child(self, adv, prev):
        if adv.get("display") is not None:
            prev = Node(adv, self, prev, len(self.children) + 1, self.x + 1)
            self.children.append(prev)
        else:
            for g in adv["children"]:
                prev = self.add_child(g, prev)
        return prev

    def first_walk(self):
        if not self.children:
            self.y = self.prev.y + 1.0 if self.prev is not None else 0.0
        else:
            default = None
            for c in self.children:
                c.first_walk()
                default = c.apportion(c if default is None else default)
            self.execute_shifts()
            mid = (self.children[0].y + self.children[-1].y) / 2.0
            if self.prev is not None:
                self.y = self.prev.y + 1.0
                self.mod = self.y - mid
            else:
                self.y = mid

    def second_walk(self, mod_sum, depth, mn):
        self.y += mod_sum
        self.x = depth
        if self.y < mn:
            mn = self.y
        for c in self.children:
            mn = c.second_walk(mod_sum + self.mod, depth + 1, mn)
        return mn

    def third_walk(self, off):
        self.y += off
        for c in self.children:
            c.third_walk(off)

    def execute_shifts(self):
        shift = 0.0
        change = 0.0
        for c in reversed(self.children):
            c.y += shift
            c.mod += shift
            change += c.change
            shift += c.shift + change

    def prev_or_thread(self):
        if self.thread is not None:
            return self.thread
        return self.children[0] if self.children else None

    def next_or_thread(self):
        if self.thread is not None:
            return self.thread
        return self.children[-1] if self.children else None

    def apportion(self, default):
        if self.prev is None:
            return default
        vir = self
        vor = self
        vil = self.prev
        vol = self.parent.children[0]
        sir = self.mod
        sor = self.mod
        sil = vil.mod
        sol = vol.mod
        while vil.next_or_thread() is not None and vir.prev_or_thread() is not None:
            vil = vil.next_or_thread()
            vir = vir.prev_or_thread()
            vol = vol.prev_or_thread()
            vor = vor.next_or_thread()
            vor.ancestor = self
            shift = vil.y + sil - (vir.y + sir) + 1.0
            if shift > 0.0:
                vil.get_ancestor(self, default).move_subtree(self, shift)
                sir += shift
                sor += shift
            sil += vil.mod
            sir += vir.mod
            sol += vol.mod
            sor += vor.mod
        if vil.next_or_thread() is not None and vor.next_or_thread() is None:
            vor.thread = vil.next_or_thread()
            vor.mod += sil - sor
        else:
            if vir.prev_or_thread() is not None and vol.prev_or_thread() is None:
                vol.thread = vir.prev_or_thread()
                vol.mod += sir - sol
            default = self
        return default

    def move_subtree(self, right, shift):
        subtrees = float(right.child_index - self.child_index)
        if subtrees != 0.0:
            right.change -= shift / subtrees
            self.change += shift / subtrees
        right.shift += shift
        right.y += shift
        right.mod += shift

    def get_ancestor(self, other, default):
        return self.ancestor if self.ancestor is not None and self.ancestor in other.parent.children else default

    def finalize(self):
        self.adv["pos"] = (self.x, self.y)
        for c in self.children:
            c.finalize()


def layout(root):
    r = Node(root, None, None, 1, 0)
    r.first_walk()
    mn = r.second_walk(0.0, 0, r.y)
    if mn < 0.0:
        r.third_walk(-mn)
    r.finalize()


def item_opts(spec, item_tags):
    if isinstance(spec, list):
        out = set()
        for s in spec:
            out |= item_opts(s, item_tags)
        return out
    if isinstance(spec, str):
        if spec.startswith("#"):
            return set(item_tags.get(strip(spec[1:])))
        return {strip(spec)}
    return set()


def entity_mask(ent, entity_tags):
    """an entity condition -> the KILLABLE mobs it accepts (bit mask), None if
    it asks for something the mod can't tell (anything but the type)"""
    if not ent:
        return (1 << len(KILLABLE)) - 1
    mask = (1 << len(KILLABLE)) - 1
    for e in (ent if isinstance(ent, list) else [ent]):
        if not isinstance(e, dict):
            return None
        if e.get("type") and strip(e["type"]) != "entity_properties":
            return None
        pred = e.get("predicate", e)
        keys = set(pred) - {"type", "minecraft:entity_type", "entity_type"}
        if keys:
            return None  # flags, location, equipment...
        t = pred.get("minecraft:entity_type", pred.get("entity_type"))
        if t is None:
            continue
        names = set()
        for x in (t if isinstance(t, list) else [t]):
            if isinstance(x, str) and x.startswith("#"):
                names |= {strip(n) for n in entity_tags.get(strip(x[1:]))}
            else:
                names.add(strip(x))
        mask &= sum(1 << i for i, k in enumerate(KILLABLE) if k in names)
    return mask


def criterion(c, item_tags, entity_tags=None):
    """(trigger code, [option sets], mob mask) ; trigger 0 = the mod can't tell"""
    trig = TRIG.get(strip(c.get("trigger", "")), 0)
    cond = c.get("conditions", {}) or {}
    if trig in (3, 4):
        # KilledTrigger: the entity (killed / killer), no killing_blow etc.
        if set(cond) - {"entity", "player"}:
            return (0, [], 0)
        mask = entity_mask(cond.get("entity"), entity_tags)
        return (trig, [], mask) if mask else (0, [], 0)
    r = _criterion(trig, cond, item_tags)
    return (r[0], r[1], 0)


def _criterion(trig, cond, item_tags):
    if trig == 1:
        preds = []
        for p in cond.get("items", []):
            if "items" not in p:
                return (0, [])
            preds.append(item_opts(p["items"], item_tags))
        return (1, preds) if preds else (0, [])
    if trig == 2:
        it = cond.get("item")
        if it is None:
            return (2, [])  # anything eaten
        if "items" not in it:
            return (0, [])
        return (2, [item_opts(it["items"], item_tags)])
    return (0, [])


def read_advancements(jar, item_tags, lang):
    import mc_data
    entity_tags = mc_data.Tags(jar, "entity_type")
    advs = {}
    for tab in TABS:
        for name in jar.listdir("data/minecraft/advancement/%s/" % tab):
            d = jar.json("data/minecraft/advancement/%s/%s.json" % (tab, name))
            if not d:
                continue
            aid = "%s/%s" % (tab, name)
            disp = d.get("display")
            a = {"id": aid, "tab": tab, "parent": strip(d.get("parent")) if d.get("parent") else None, "children": [],
                 "display": None, "crit": [], "req": []}
            if disp:
                def tr(t):
                    if isinstance(t, dict):
                        return lang.get(t.get("translate", ""), t.get("text", t.get("translate", "")))
                    return str(t)
                a["display"] = {
                    "title": tr(disp.get("title", "")), "desc": tr(disp.get("description", "")),
                    "icon": strip(disp.get("icon", {}).get("id", "")) if isinstance(disp.get("icon"), dict) else strip(disp.get("icon", "")),
                    "frame": FRAMES.get(disp.get("frame", "task"), 0),
                    "toast": disp.get("show_toast", True), "hidden": disp.get("hidden", False),
                    "background": strip(disp.get("background", "")) if disp.get("background") else None,
                }
            names = list(d.get("criteria", {}).keys())
            for cn in names:
                trig, preds, mask = criterion(d["criteria"][cn], item_tags, entity_tags)
                a["crit"].append({"name": cn, "trig": trig, "preds": preds, "mask": mask})
            req = d.get("requirements") or [[cn] for cn in names]
            a["req"] = [[names.index(cn) for cn in g if cn in names] for g in req]
            advs[aid] = a
    for aid in sorted(advs):
        p = advs[aid]["parent"]
        if p in advs:
            advs[p]["children"].append(advs[aid])
    roots = [advs[t + "/root"] for t in TABS if (t + "/root") in advs]
    for r in roots:
        if r["display"] is not None:
            layout(r)
    order = []
    def walk(a):
        order.append(a)
        for c in a["children"]:
            walk(c)
    for r in roots:
        walk(r)
    return roots, order
