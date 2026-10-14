"""minecraft's sounds for the mod (sounds.json, SoundEvents.java, SoundType.java).

the sound files aren't in the client jar: they're in the launcher's asset
store (assets/indexes/<asset index>.json + assets/objects/<hash>). this finds
an installed launcher's store, decodes every .ogg the mod plays with ffmpeg
into 16 bit mono pcm (generated/sounds/<n>.pcm, 44100 hz) and describes them
for the c side (WeighedSoundEvents entries: volume, pitch, weight,
attenuation distance). nothing here is committed: generated/ is gitignored.
"""

import json
import os
import re
import shutil
import subprocess

RATE = 44100

# the sound events the mod plays besides the blocks' SoundTypes
EXTRA_EVENTS = [
    "ui.button.click", "ui.toast.in", "ui.toast.out", "ui.toast.challenge_complete",
    "entity.player.hurt", "entity.player.hurt_drown", "entity.player.hurt_on_fire", "entity.player.death",
    "entity.player.big_fall", "entity.player.small_fall",
    "entity.player.attack.strong", "entity.player.attack.weak", "entity.player.attack.crit",
    "entity.player.attack.sweep", "entity.player.attack.knockback", "entity.player.attack.nodamage",
    "entity.generic.eat", "entity.player.burp",
    "entity.item.pickup", "entity.experience_orb.pickup", "entity.player.levelup", "entity.item.break",
    "entity.arrow.shoot", "entity.arrow.hit", "entity.arrow.hit_player",
    "entity.tnt.primed", "entity.generic.explode",
    "item.bucket.fill", "item.bucket.empty", "item.bucket.fill_lava", "item.bucket.empty_lava",
    "entity.player.splash", "entity.player.splash.high_speed", "entity.player.swim", "entity.generic.splash",
    "block.chest.open", "block.chest.close", "block.furnace.fire_crackle",
    "item.armor.equip_generic", "item.armor.equip_leather", "item.armor.equip_chain", "item.armor.equip_iron",
    "item.armor.equip_gold", "item.armor.equip_diamond", "item.armor.equip_netherite", "item.armor.equip_turtle",
    "item.armor.equip_copper",
    "item.shield.block", "item.shield.break",
    "block.fire.extinguish", "block.lava.pop", "block.lava.ambient", "item.flintandsteel.use",
    "entity.boat.paddle_water",
    "block.lever.click", "block.stone_button.click_on", "block.stone_button.click_off",
    "block.wooden_button.click_on", "block.wooden_button.click_off",
    "block.piston.extend", "block.piston.contract",
    "block.stone_pressure_plate.click_on", "block.stone_pressure_plate.click_off",
    "block.wooden_pressure_plate.click_on", "block.wooden_pressure_plate.click_off",
    "block.wooden_door.open", "block.wooden_door.close", "block.wooden_trapdoor.open", "block.wooden_trapdoor.close",
    "block.iron_door.open", "block.iron_door.close", "block.fence_gate.open", "block.fence_gate.close",
    # the mobs bk's enemies count as (hurt / death: LivingEntity.handleDamageEvent, hurtServer)
    "entity.zombie.hurt", "entity.zombie.death", "entity.cow.hurt", "entity.cow.death",
    "entity.cod.hurt", "entity.cod.death", "entity.spider.hurt", "entity.spider.death",
    "entity.chicken.hurt", "entity.chicken.death", "entity.skeleton.hurt", "entity.skeleton.death",
    "entity.creeper.hurt", "entity.creeper.death", "entity.husk.hurt", "entity.husk.death",
    "entity.drowned.hurt", "entity.drowned.death", "entity.snow_golem.hurt", "entity.snow_golem.death",
    "entity.bat.hurt", "entity.bat.death",
]


def find_asset_store(index_id):
    """-> (objects dir, {name: hash}) of a launcher that has this asset index"""
    home = os.path.expanduser("~")
    appdata = os.environ.get("APPDATA", os.path.join(home, "AppData", "Roaming"))
    mac = os.path.join(home, "Library", "Application Support")
    roots = [
        os.environ.get("MC_ASSETS", ""),
        os.path.join(appdata, ".minecraft", "assets"),
        os.path.join(appdata, "PrismLauncher", "assets"),
        os.path.join(mac, "minecraft", "assets"),
        os.path.join(mac, "PrismLauncher", "assets"),
        os.path.join(home, ".local/share/PrismLauncher/assets"),
        os.path.join(home, ".var/app/org.prismlauncher.PrismLauncher/data/PrismLauncher/assets"),
        os.path.join(home, ".minecraft/assets"),
        os.path.join(home, ".local/share/ModrinthApp/meta/assets"),
        os.path.join(home, ".local/share/multimc/assets"),
        os.path.join(home, ".gradle/caches/fabric-loom/assets"),
    ]
    for r in roots:
        idx = os.path.join(r, "indexes", "%s.json" % index_id)
        if r and os.path.isfile(idx):
            objs = json.load(open(idx))["objects"]
            return os.path.join(r, "objects"), {k: v["hash"] for k, v in objs.items()}
    return None, None


def read_sound_events(path):
    """SoundEvents.java: CONSTANT -> event id"""
    src = open(path, encoding="utf-8").read()
    out = {}
    for m in re.finditer(r'public static final [\w.<>]+ (\w+) = register(?:ForHolder)?\("([\w.]+)"', src):
        out[m.group(1)] = m.group(2)
    return out


def read_sound_types(path, events):
    """SoundType.java: NAME -> (volume, pitch, [break, step, place, hit, fall] event ids)"""
    src = open(path, encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"public static final SoundType (\w+) = new SoundType\((.*?)\);", src, re.S):
        args = [a.strip() for a in m.group(2).split(",")]
        if len(args) != 7:
            continue
        vol = float(args[0].rstrip("F"))
        pitch = float(args[1].rstrip("F"))
        evs = [events.get(a.replace("SoundEvents.", ""), "") for a in args[2:]]
        out[m.group(1)] = (vol, pitch, evs)
    return out


def read_block_sound_types(path):
    """Blocks.java: registry name -> SoundType NAME (Properties default: STONE,
    copies and helper methods followed like the strength parse)"""
    src = open(path, encoding="utf-8").read()
    helpers = {}
    for m in re.finditer(r"static BlockBehaviour\.Properties (\w+)\([^)]*\)\s*\{(.*?)\n\t\}", src, re.S):
        helpers[m.group(1)] = m.group(2)
    stmts = {}
    for m in re.finditer(r"public static final Block (\w+) = ", src):
        i = j = m.end()
        depth = 0
        while j < len(src):
            c = src[j]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            elif c == ";" and depth == 0:
                break
            j += 1
        stmts[m.group(1)] = src[i:j]
    out = {}

    def sound_of(text):
        s = None
        m = re.search(r"of(?:Full|Legacy)Copy\((\w+)\)", text)
        if m and m.group(1).lower() in out:
            s = out[m.group(1).lower()]
        for name, body in helpers.items():
            if re.search(r"\b%s\(" % name, text):
                h = re.findall(r"\.sound\(SoundType\.(\w+)\)", body)
                if h:
                    s = h[-1]
        own = re.findall(r"\.sound\(SoundType\.(\w+)\)", text)
        if own:
            s = own[-1]
        return s or "STONE"

    for name, text in stmts.items():
        out[name.lower()] = sound_of(text)
    return out


def resolve_event(sounds_json, ev, depth=0):
    """an event's WeighedSoundEvents entries: [(file, volume, pitch, weight, attenuation)]"""
    d = sounds_json.get(ev)
    if d is None or depth > 4:
        return []
    out = []
    for e in d.get("sounds", []):
        if isinstance(e, str):
            e = {"name": e}
        if e.get("type", "file") == "event":
            for f, v, p, w, a in resolve_event(sounds_json, e["name"].replace("minecraft:", ""), depth + 1):
                out.append((f, v * e.get("volume", 1.0), p * e.get("pitch", 1.0), w * e.get("weight", 1), a))
            continue
        out.append((e["name"].replace("minecraft:", ""), e.get("volume", 1.0), e.get("pitch", 1.0),
                    e.get("weight", 1), e.get("attenuation_distance", 16)))
    return out


def export(java_root, index_id, out_dir):
    """-> (events [(id, [entry indices])], entries [(file index, vol, pitch, weight, att)],
    files [name], sound_types {NAME: (vol, pitch, [5 event ids])}, block sound type names)
    or None if no launcher has the sounds"""
    objects, index = find_asset_store(index_id)
    if objects is None:
        print("no minecraft asset index %s found (install minecraft 26.3 in a launcher, or set MC_ASSETS): no sounds" % index_id)
        return None
    if shutil.which("ffmpeg") is None:
        print("ffmpeg not found: no sounds")
        return None
    h = index["minecraft/sounds.json"]
    sounds_json = json.load(open(os.path.join(objects, h[:2], h)))
    events = read_sound_events(os.path.join(java_root, "sounds", "SoundEvents.java"))
    types = read_sound_types(os.path.join(java_root, "world", "level", "block", "SoundType.java"), events)
    block_types = read_block_sound_types(os.path.join(java_root, "world", "level", "block", "Blocks.java"))
    return sounds_json, index, objects, types, block_types


def build(sounds_json, index, objects, wanted_events, out_dir):
    """decode the wanted events' files -> (events, entries, files)"""
    os.makedirs(out_dir, exist_ok=True)
    cache = os.path.join(out_dir, "cache")
    os.makedirs(cache, exist_ok=True)
    files, file_ix, entries, ev_rows = [], {}, [], []
    for ev in wanted_events:
        ents = resolve_event(sounds_json, ev)
        ix = []
        for f, v, p, w, a in ents:
            key = "minecraft/sounds/%s.ogg" % f
            if key not in index:
                continue
            if f not in file_ix:
                file_ix[f] = len(files)
                files.append(f)
            ix.append(len(entries))
            entries.append((file_ix[f], v, p, w, a))
        ev_rows.append((ev, ix))
    # decode (cached by hash), then the n.pcm names the native side loads
    total = 0
    for n, f in enumerate(files):
        h = index["minecraft/sounds/%s.ogg" % f]
        cached = os.path.join(cache, h + ".pcm")
        if not os.path.isfile(cached):
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", os.path.join(objects, h[:2], h),
                            "-f", "s16le", "-ac", "1", "-ar", str(RATE), cached], check=True)
        dst = os.path.join(out_dir, "%d.pcm" % n)
        shutil.copyfile(cached, dst)
        total += os.path.getsize(dst)
    print("sounds: %d events, %d entries, %d files (%.1f MB pcm)" % (len(ev_rows), len(entries), len(files), total / 1e6))
    return ev_rows, entries, files
