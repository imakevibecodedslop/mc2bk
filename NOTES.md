# notes

running notes for the minecraft-in-bk mod. decomp paths are relative to `bk-decomp/`.
"syms ok" means the name exists in `BanjoRecompSyms/bk.us.rev0.syms.toml` (so we can hook it).

## milestone status (2026-10-08)

- m0 setup, m1 research, m2 movement, m3 blocks, m4 place/break, m5 hud, m6 steve model: **done, owner tested.**
- m7 in progress: terrain mining, trees, crafting/tools (built), bk moves -> minecraft (not started).
- the table below is the old per-feature log, kept for history. CLAUDE.md "where we left off" is the current list.

## in-game test status (old log)

| thing | built | tested in game |
|---|---|---|
| m0 hook on player update (`func_80295C14`) + on-screen readout | yes | **no** |
| m0 hide banjo (d-pad down) via `player_draw` hooks | yes | **no** |
| m2 steve mode toggle (d-pad up), input stealing | yes | **no** |
| m2 minecraft movement on 20 tps clock + interpolation | yes | **no** |
| m2 collision vs bk terrain (raycasts) | yes | **no** |
| m2 hand-over to banjo (cutscenes, water, climbing, hurt) | yes | **no** |
| m2 placeholder steve boxes (custom display list) | yes | **no** |
| m3 first person camera (viewport swap around the scene draw) + crosshair | yes | **no** |
| m3 voxel grid, place (r) / break (l) with dual raycast | yes | **no** |
| m3 block meshes (heap display lists, face culling, shading) | yes | **no** |
| m3 grid as a bk collision provider (steve/banjo floor/camera hit blocks) | yes | **no** |
| m3 mouse companion mod (native .so, sdl event watch) | yes, smoke tested outside the game | **no** |
| owner test of m3: blocks + first person work; bk objects ignored steve, fov wrong, no arm | - | **yes (reported)** |
| fix: banjo touch spheres follow steve (`baMarker_update` hooks) | yes | **no** |
| 70° fov + sprint fov, view bob, arm sway | yes | **no** |
| first person hand (minecraft's pose + swing) | yes | **no** |
| third person walk / look / swing animation | yes | **no** |
| ~~left click = banjo's b move~~ replaced by 3 block attack pick | yes | **no** |
| owner test: fov/arm/movement feel right; mouse flicked (wayland), sneak fell off edges, sprint stopped on ctrl release, arm lagged/drifted | - | **yes (reported)** |
| mouse: relative mode via GOT hook of SDL_SetRelativeMouseMode (works in a test program) | yes | **no** |
| sprint latch until forward released, sneak edge back-off | yes | **no** |
| hand on its own fixed projection (rt64 frame interpolation drift) | yes | **no** |
| attack: 3 block pick, bk collision with beak bomb hitbox | yes | **no** |
| minecraft textures from the owner's jar (blocks + steve skin), 10 block types, hotbar 1-9 | yes | **no** |
| steve model ported from HumanoidModel/PlayerModel (skin uvs, setupAnim, whack swing, crouch) | yes | **no** |
| trees/poles as ladders (climbSet hooks, handleOnClimbable) | yes | **no** |

## round 8: terrain mining, trees, particles, flight, swimming, outline

- terrain.c: mining bk's level. a mined grid cell cuts every opaque map model triangle touching it
  (sutherland-hodgman against the cell box). render: the triangle is removed from the model's display list in
  memory (TRI1 -> G_SPNOOP, TRI2 -> TRI1 of the kept one), its outside parts are drawn by us with the texture
  commands captured from the display list (segment 2 resolved to the model's texture data, CI -> TLUT RGBA16),
  the vertex tc/colors interpolated. collision: tris whose AABB touches the cell are made zero size (all corners
  on vertex 0), their outside parts go into world.c's collision provider. the sides of holes get dirt/stone block
  faces that are also solid (bk's ground is a shell). terrain kind by depth below the surface (grass < 1, dirt < 4,
  stone), trees from the learned climbable poles (inside the trunk = oak log, around/above the top = leaves).
  UNTESTED risks: the gfx list count field, the recomp possibly not re-reading the edited display list,
  animated texture segments (those tris are skipped), sphere queries (enemies) don't see the leftover pieces.
- particles.c: TerrainParticle destroy (4x4x4) + crack, Particle tick, quadSize, sprite quarter.
- block outline (submitBlockOutline, black 102, ~2.5px), attack indicator, held item name (40 ticks).
- creative flight (double tap jump, flyingSpeed 0.05, y * 0.6), sprint swimming (look steering, 0.6 tall, eye 0.4).
- pick block (middle click), f1 hides gui + hand, inventory dropped on death.
- respawn camera: steve's camera only after 10 frames in a row of steve driving.
- water: in water, three bk sphere queries (func_80320DB0) push steve out of the level.

## round 7: hud scale, items/tools, crafting screens, fixes

- owner: hotbar too big/stretched vs minecraft, slopes clip while swimming, shift spammed splashes, trees not
  climbable.
- hud: gui.c computes minecraft's gui scale from the window size (Window.calculateScale, needs the mouse mod for
  the window size, else assumes 1080p) and draws with gEXTextureRectangle anchored to G_EX_ORIGIN_CENTER (square
  pixels in widescreen). UNTESTED assumption: rt64 extended rect units = framebuffer units (240 = window height),
  x measured from the screen center.
- icons are pre-rendered by the exporter (block "gui" transform, 64x64 = gui scale 4, side lighting).
- splash spam: func_8028B59C splashes when D_8037BF61 goes 0 -> 1; we now restore the real previous value in a
  pre hook before hiding it again.
- swim slope clip: the upward vertical move now also resolves floor penetration.
- climbing: poles are learned from the climbable actor's own update (func_802D77D4, ch/climbBase.c) plus
  climbSet, and checked against steve's box every tick (radius + half width + 12 units).
- items.c: stick + wood/stone sword/shovel/pickaxe/axe (ToolMaterial WOOD 59/2.0, STONE 131/4.0), tool rules
  (mineable tags, sword_efficient leaves 1.5), damage per block (tools 1, sword 2) / per attack (tools 2,
  sword 1), attack speed for the equip dip. crafting table block (hardness 2.5).
- screen.c: InventoryMenu / CraftingMenu layouts, doClick PICKUP/QUICK_MOVE/SWAP/THROW, ResultSlot, recipes
  (planks, sticks, crafting table, tools; shaped + mirrored). e toggles, esc closes, keyboard e also arrives as
  n64 L so L is ignored until released after closing. mouse lib got gui mode (free cursor, clicks counted),
  window size and cursor position.
- still not done: xp, swimming pose, the player preview in the inventory, armor, recipe book, creative inventory
  screen, drag-splitting, sounds, particles, controller support in screens.

## round 6 (owner away): smoothing, hunger, equip, drop, third person item, swimming

- LAG: banjo: recompiled runs logic at 30/s and rt64 interpolates frames using matrix groups (gEXMatrixGroup with
  stable ids). nothing of ours was tagged, so steve/hand/blocks/items only moved 30 times a second. src/mc_gbi.h
  copies the recomp's macros (BanjoRecomp patches/*_transform_tagging.c) and our ids live at 0x40000000+.
  cpu-built meshes use the "verts" variant (rt64 interpolates vertex positions). the hand gets its own projection
  group + identity view matrix (gEXSetViewMatrixFloat). mouse look itself is still read once per game frame.
- hunger: FoodData.tick (normal difficulty), exhaustion values from FoodConstants (sprint 0.1/block, jump 0.05,
  sprint jump 0.2, mine 0.005, attack 0.1, damage 0.1), no sprint at food <= 6. food source: picking up bk
  honeycombs = honey bottle (nutrition 6, saturation 0.1). healing raises bk honeycombs too.
- equip animation: FirstPersonHandsAndItems.tick (+-0.4/tick, swap scale^3, attack strength 5 ticks), itemUsed on
  place. hand shows hand_item (lags the hotbar).
- drop: q / ctrl+q / d-pad down, LivingEntity.createItemStackToDrop (eye - 0.3, 0.3 throw, pickup delay 40).
- third person: ItemInHandLayer + thirdperson_righthand display, ArmPose.ITEM.
- swimming: water from banjo's floor probe (floor_isCurrentFloorunk59 / floor y = surface), banjo's D_8037BF61
  cleared after func_8028B59C so he never swims. steve: travelInFluid (0.02, 0.8/0.9, gravity/16 falling adjust,
  jumpInLiquid +0.04 above 0.4 deep, shift sinks -0.04, 0.3 pop at ledges), air supply 300 (-1/tick, 2 dmg at -20,
  +4/tick), Hud.extractAirBubbles. untested: whether steve's line queries see water as solid (bk's normal
  collision filter is supposed to skip it).
- still missing: items that aren't blocks (sticks, tools), crafting + the inventory/crafting table screens,
  xp, swimming pose (sprint underwater), bk terrain mining.

## round 5: survival + audit against the 26.3 source

- audit fixes: fall damage is floor(fall + 1e-6 - 3) (was ceil); swing is 26.3's SwingState (no restart in the
  first half, getAnimation lerps through the wrap; body follows the head while animation > 0); camera eye height
  eases 0.5/tick (Camera.tick) when crouching.
- survival.c: Inventory (36 slots, hotbar 0-8, stacks of 64, add = matching stacks then free slots),
  Minecraft.startAttack/continueAttack/startUseItem (missTime 10, rightClickDelay 4),
  MultiPlayerGameMode start/continueDestroyBlock (destroyDelay 5, creative instant), getDestroyProgress
  (hand speed 1, /5 off ground, /30 or /100 when the block needs a tool), Blocks.java hardness, loot (grass->dirt,
  stone->cobble but needs a pickaxe so nothing by hand, glass/leaves nothing), Block.popResource + ItemEntity
  (gravity 0.04, drag 0.98, ground 0.6, bounce -0.5, pickup delay 10, player box inflated 1/0.5/1, 6000 tick
  lifetime), ItemEntityRenderer bob/spin + block "ground" display. reach 4.5 survival / 5 creative, entities 3.
- crack overlay: destroy_stage_N (32 bit) on a cube 1 unit bigger, xlu.
- held block in first person: applyItemArmTransform + swingArm + firstperson_righthand (45°, 0.4).
- hud.c rewritten from Hud.java: 32 bit sprites (alpha), 3d icons via the "gui" display transform (30/225,
  0.625) in an ortho projection, counts in the jar's font with shadow, xp bar background, hurt blinking hearts.
- f3 toggles the debug text, f3+f4 toggles survival/creative. mouse lib reports held buttons (mcm_buttons).
- not yet: tools/crafting (so stone/cobble/bricks give nothing), hunger, xp, the 27 slot inventory screen, item
  dropping (q), swimming, held item in third person, equip animation.

## round 4 (owner: textures fine; f5 wrong, model faced the camera, sneak fell off blocks)

- sneak bug: the edge check sampled the full 0.3 radius, the floor rays use 0.9 of it. steve could hang out
  over the edge further than the floor check can stand on, then fell. both use 0.9 now.
- camera: steve's camera runs in all three CameraTypes while he drives (bk's camera for cutscenes, swimming).
  detached: Camera.setup (front = yaw+180, -pitch) + getMaxZoom(4 blocks, 8 jittered rays). mouse aims in all modes.
- body: LivingEntity.tick walk direction + tickHeadTurn (0.3 ease, head max 50° off the body). renderer uses
  180 - yBodyRot, head yRot = yRot - yBodyRot.
- health: 20 hp, honeycomb = 20/max hp, synced both ways. minecraft fall damage ceil(fall - 3); bk's fall damage
  is cancelled each frame with bafalldamage_start(). 0 honeycombs -> bs_setState(BS_41_DIE) like bs/splat.c.
- hud.c: Gui hotbar/selection/hearts/food/crosshair sprites from the jar, texrects in 64 column strips (tmem).
  food is always full (no hunger yet).
- crouch: 1.5 tall hitbox, can't stand up under a ceiling. double tap w sprint (sprintTriggerTime 7).
- still missing (player): swimming with minecraft water physics (bk drives banjo in water), survival mining
  with crack stages, inventory / item drops, hunger, xp, knockback, creative flight.

## this round (mouse, attack, textures, climbing)

- mouse flicking: owner is on wayland, where SDL_WarpMouseInWindow does nothing, so the cursor hit the window edge.
  the native lib now patches the game executable's GOT entry for SDL_SetRelativeMouseMode (game links sdl2
  dynamically, fedora's is sdl2-compat) so recomp can't turn relative mode off while we're captured. warping is
  only the fallback if the patch fails. the console says `[mc mouse] relative mouse mode hooked` when it worked.
- arm drift: rt64 interpolates matrices between game frames (game logic is 30/s). the hand used the world camera,
  so it swam against the interpolated view. it now has its own constant projection (guPerspective 70°) and
  view-space vertices.
- attack: Minecraft.startAttack/pick. walk the eye ray in 16 unit steps up to 3 blocks (or the nearer block/wall),
  at each step move banjo's marker there and run bk's prop query (func_80320ED8 + func_8032F528). first actor wins.
  then `__baMarker_resolveCollision(prop)` with D_8037BFA0 set, which makes hitbox_getHitboxForActor say beak bomb.
  so enemies react like to banjo's attacks. one punch kills most things (no damage numbers yet).
- textures: tools/export_mc_assets.py -> generated/mc_textures.h (rgba5551, plains tint baked into grass/leaves).
  blocks: mesh sorted by texture, gDPLoadTextureBlock per group, point filter, TEX_EDGE cutout for glass/leaves.
  skin: 8 KB, too big for tmem, so each part loads its own rectangle (gDPLoadTextureTile) with full-skin uvs.
- model: minecraft world axes = bk's with yRot = -bk yaw, so LivingEntityRenderer's chain (rotY(180-bodyRot),
  scale(-1,-1,1), translate(0,-1.501,0), 0.9375) is used unchanged. vertices built on the cpu, camera relative,
  back faces dropped (so the hand works without z).
- climbing: climbable actors (ch/climbBase.c etc) call climbSet every frame banjo is in range. hooks note it for
  steve and clear D_8037C590 so banjo never grabs. steve gets handleOnClimbable (clamp 0.15, up 0.2 when
  pushing into it or jumping, sneak holds).

## minecraft source

- decompiled 26.3 (fabric example mod, `genSources`) unzipped to `../mc-src/decomp/`, outside this repo. never commit.
  files used: `LivingEntity.java` (travel/jump), `client/Camera.java` (fov), `GameRenderer.java` (bobView),
  `FirstPersonHandsAndItemsRenderer.java` (hand), `HumanoidModel.java` (limbs), `ClientAvatarState.java` (bob).
- movement already matched 26.3; only change: jump sets vy = max(0.42, vy).

## why bk objects ignored steve (fixed, untested)

- `baMarker_update` places banjo's touch spheres via `baModel_80292284`, which reads bone positions that only update
  when banjo's model is drawn. hidden banjo = spheres frozen. with the visible flag off it falls back to player
  position + 75 / + 33, so we clear the flag just for the marker update.
- fov: bk 40° (`sViewportFOVy`), minecraft 70°. set before `viewport_update` in the scene pre hook.
- hand: drawn in the `func_80334540` return hook (pre hook's gfx/mtx), after re-emitting the projection. no z buffer,
  back faces dropped on the cpu. view bob is faked by nudging camera position/roll/pitch.

## m3 notes (first person, blocks, mouse)

- **first person** (`src/fpcam.c`): `func_80334540` (the 3d scene draw) pre-hook saves bk's viewport and swaps in
  steve's eye (feet + 1.62 blocks, 1.27 sneaking) and yaw/pitch, calls `viewport_update()`; the return hook puts bk's
  camera back. bk's camera logic keeps running on the hidden banjo and never sees the swap.
  - projection (`viewport_setRenderPerspectiveMatrix`) is rotZ(-roll)·rotX(-pitch)·rotY(-yaw) looking down -z, so a
    camera at yaw y faces (-sin y, -cos y). steve faces (sin yaw, cos yaw), so camera yaw = steve yaw + 180.
    positive pitch looks up for both.
  - near plane: the scene draw calls `viewport_setNearAndFar` right before building the projection; a return hook
    on it lowers `sViewportNear` (data sym) to 8 units while in first person.
  - crosshair: `printbuffer_draw` pre-hook, two `drawRectangle2D` fills at the framebuffer center.
  - look: c buttons (180°/s yaw, 120°/s pitch) or the mouse (0.15°/pixel = minecraft default sensitivity).
    turning right lowers bk yaw.
- **voxel grid** (`src/world.c`): 16³ sections in a flat array (max 256), block (0,0,0) at world 0..80. cleared on
  map load (return hook on `func_8030A078`, the map model init, which runs right after the provider list reset).
  grid walk is amanatides & woo; ignores the cell the ray starts in.
- **grid as collision**: `func_80320B24(line_fn, NULL, NULL)` registers a line-query provider every map load. it
  returns a static fake `BKCollisionTri` (flags 0). callers read `flags`/`unk6`; banjo's floor code also stores the
  "which model" getter result, NULL is a normal case there (`code_12F30.c` checks it). sphere queries (banjo/enemy
  walls) aren't provided yet, so enemies walk through blocks.
- **place/break** (`steve_use_blocks` in `src/steve.c`): ray from the eye, 5 block reach, through
  `func_80320B98` (terrain + grid) and through the grid alone; if the grid hit is the nearest it's a block. break
  removes it; place goes on the hit face, or for terrain on the cell half a block out along the surface normal
  (sinks into slopes, same as swirly). won't place inside steve.
- **block meshes** (`src/world_draw.c`): only faces next to air, 4 verts each, minecraft face shading baked into
  vertex colors (top 1.0, bottom 0.5, z 0.8, x 0.6). flat colors picked by me, no minecraft textures. vertices are
  section-local s16, one matrix per section (camera relative). `recomp_alloc` buffers; replaced meshes are freed 4
  frames late in case the renderer still reads them. drawn from the `player_draw` pre-hook.
- **mouse** (`mouse/`): banjo: recompiled tracks mouse motion (`recompinput::get_mouse_deltas`) but
  `REGISTER_FUNC(recomp_get_mouse_deltas)` is commented out in `src/main/main.cpp`, mouse buttons can't be bound
  ("TODO mouse support" in RecompFrontend input_state.cpp), and bk sets `has_mouse_sensitivity = false` so recomp
  forces `SDL_SetRelativeMouseMode(SDL_FALSE)` on every event. so:
  - companion mod `bk_minecraft_mouse` ships `mc_mouse_native.so` (must sit next to its `.nrm`; runtime loads
    `<mods folder>/<name>.so`, needs `recomp_api_version = 1`). native functions use `(rdram, ctx)`, args in r4,
    return in r2. only ints cross.
  - the .so finds the game's sdl2 with `dlsym`, adds an `SDL_AddEventWatch` (runs on the main/event thread), sums
    motion there, re-centers with `SDL_WarpMouseInWindow` when the cursor drifts a quarter screen, hides it with a
    blank `SDL_CreateCursor`. esc toggles capture; focus loss or the game not polling for 300 ms releases it.
  - native libs can only be imported by the mod that ships them, so the companion re-exports them and the main
    mod lists it as an optional dependency (`recomp_is_dependency_met`). main mod works without it.
  - untested risks: wayland (sdl2 on xwayland should be fine; native wayland can't warp), the blank cursor being
    replaced by recomp's ui.
- recomp default keyboard (RecompFrontend `input_mapping.cpp`): wasd stick, space a, lshift b, q z, e l, r r,
  arrows c buttons, i/j/k/l d-pad, enter start, esc menu.

## m2 notes (steve movement)

- code: `src/steve.c` (physics, collision, hooks), `src/steve_draw.c` (placeholder model).
- hook order inside banjo's update: `bakey_update`(ret) and `bastick_update`(ret) copy input for steve and blank
  a/b/z + the stick for banjo → banjo's state machine runs with no input → `baphysics_update`(pre) detects if the
  game moved banjo (warp/void out/map load) and snaps steve there → `baphysics_update`(ret) runs steve's ticks and
  sets banjo's position/velocity/yaw → `func_8029858C`(pre) adds platform offsets to steve too →
  `func_80293F0C`(ret) puts banjo back on steve after banjo's own terrain collision.
- steve only drives when banjo's physics type is normal/airborne/locked rotation/inverted yaw/unk1, banjo isn't in
  water, and the movement group isn't intr/look/climb/flying/sled/transforming. otherwise the game drives banjo
  (with normal input) and steve follows. so swimming, talking, cutscenes and getting hurt are banjo for now.
- controls: stick = move (camera relative), a = jump (hold to bunny hop like minecraft), b = sprint (hold, stick
  pushed forward), z = sneak. d-pad up = steve on/off. camera is bk's (c buttons).
- physics follows `LivingEntity.aiStep/travel/moveRelative`: 0.98 input scale, ground speed
  `0.1 * 0.216/0.6^3` (×1.3 sprint), air speed 0.02 (0.026 sprint), friction `0.6*0.91` on ground / `0.91` in air,
  gravity `(vy - 0.08) * 0.98`, jump 0.42 (+0.2 forward when sprinting), jump delay 10 ticks, sneak ×0.3,
  velocity < 0.003 zeroed.
- collision (my approximation, minecraft has no triangles): 9 horizontal rays (3 heights above step height × left/
  middle/right) with wall slide; 5 vertical rays (center + corners) for floor/ceiling. floors with normal.y ≥ 0.5,
  walls with |normal.y| < 0.7. step up 0.6 blocks (+4 units). snaps down slopes while on ground.
- not done yet: sneak edge guard, ice/slipperiness from bk floor flags, pushing steve out if something overlaps him,
  fall damage → hearts (banjo's own fall damage still applies).
- unknowns to watch in testing: if left/right are swapped, if steve walks on water surfaces, if bk models look wrong
  after steve draws (render state leak from `steve_draw`), if static vertex data in mod memory draws at all.

## m0 notes

- toolchain used here: ubuntu clang 18.1.3, ld.lld, RecompModTool built from N64Recomp source. the template's
  `-Wno-cast-function-type-mismatch` warning on clang 18 is harmless (that flag only exists on clang 19+).
- `RECOMP_HOOK_RETURN` hooks run right before the function returns, so arg registers may already be clobbered.
  pattern used: a `RECOMP_HOOK` (pre) stashes what it needs, the return hook uses it. couldn't open the hackmd
  docs from this environment to double check, so treat return hooks as "no args".
- the m0 readout (`DT xx MS  UPS yy`) is there to answer the tick rate question below. write down what it says.

## game loop and tick rate

- per-frame game update: `func_80334ECC` (`src/core2/code_AD5B0.c`) calls `func_8028E71C` → `func_80295C14`.
- per-frame 3d draw: `func_80334540` (same file), called from `func_802E39D0` (`code_5C870.c`), which then draws
  hud/pause/dialog/`printbuffer_draw` and calls `finishFrame`.
- **bk has no fixed tick.** game logic uses a variable delta:
  - `time_getDelta()` (`src/core2/time.c`) = seconds since last frame × time multiplier, capped at 0.05 s.
  - set from vi frames (`time_setDeltaReal_frames`, 1 vi frame = 1/60 s). normally 2 vi frames = 1/30 s.
  - banjo's physics scales everything by `time_getDelta()` (e.g. gravity `v.y += dt * -2700`).
  - also `time_getDelta_frames()` = delta × 30, i.e. "30 fps frames".
- **open question:** at high framerate in banjo: recompiled, does logic run more often with a smaller delta, or
  stay at 30 and get interpolated? m0's UPS/DT readout answers this. either way, plan for steve:
  accumulate `time_getDelta()` and run steve's physics in fixed 1/20 s steps, interpolate for drawing.
  never tie steve to "one call = one tick".

## player ("banjo") state

there's no single player struct. state lives in file-level globals behind getters/setters:

- position: `player_position[3]` in `src/core2/code_11460.c`.
  - `player_getPosition(f32[3])` (syms ok), `_player_getPosition`, `player_setPosition(f32[3])`.
  - `func_8028F85C(f32[3])` teleports the player (used by cutscenes/warps).
- velocity: `s_player_velocity[3]` in `src/core2/ba/physics.c`.
  - `baphysics_get_velocity`, `baphysics_set_velocity`, `baphysics_set_vertical_velocity`, `baphysics_set_type`.
  - `baphysics_update()` reads position, integrates by type (normal/airborne/no gravity/climb/goto/...), writes position.
  - default gravity `-2700` units/s², terminal velocity `-4000`.
- facing: `yaw_get()`, `yaw_set()`, `yaw_setIdeal()`, `yaw_applyIdeal()` (`src/core2/yaw.c`). pitch/roll similar.
- banjo state machine: `bs_getState()`, `bs_setState()`, table `D_80363824` in `src/core2/code_E910.c` (one
  init/update/end/interrupt per `BS_*` state). `bs_getIdleState()` gives the right idle for the current transformation.
- transformation: `player_getTransformation()` (termite/pumpkin/croc/walrus/bee).
- full per-frame player update order, `func_80295C14` (`src/core2/code_E910.c`):
  input (`pfsManager_update`, `bakey_update`, `bastick_update`) → `bs_updateState` → `baphysics_update` →
  `func_8029858C` (apply position offset, see platforms) → `bafalldamage_update` → `func_80293F0C` (terrain
  collision) → yaw/pitch/roll → anim → model → `baMarker_update` (object collisions) → carry → hazards →
  iframes → `cameraMode_update` → ... → `func_80295E74` (void out).
- player init/reset per map: `func_80295914` (resets every ba* subsystem), spawn placement `func_8028E4B0`.
- `player_movementGroup()`, `player_getWaterState()`, `player_isDead()` etc. in `src/core2/code_7060.c`.

### platforms / being moved by the game

- `func_8028FAB0(f32 target[3])` sets a one-frame position offset (`D_8037C5C0`) that `func_8029858C` adds after
  physics. used by things like the hot sand clamp, sleds, pumpkin, CC. **this is the "platform moved banjo" signal
  to copy onto steve.**
- also: `func_8029842C` saves last frame's position (`D_8037C5B0`). diffing banjo's position before/after the game
  touched it is the general way to catch any game-driven movement.
- props with collision models (moving platforms etc) live in `src/core2/code_C9F00.c` (`Struct68s`, marker
  collision funcs `func_80350F7C` / `func_803510B4` / `func_80351198`). to confirm in m2 how they carry banjo.

## input

- `bakey_*` (`src/core2/bakey.c`): per-button counters filled in banjo's update. `bakey_pressed(BUTTON_x)` = newly
  pressed this frame, `bakey_held(BUTTON_x)`, `bakey_released(...)`. buttons: `enum button_e` in `include/enums.h`
  (`BUTTON_A`, `BUTTON_B`, `BUTTON_Z`, `BUTTON_L/R`, `BUTTON_C_*`, `BUTTON_D_*`, `BUTTON_START`).
- `bastick_*` (`src/core2/bastick.c`): `bastick_getX/Y()` (-1..1), `bastick_distance()`, `bastick_getAngle()`.
- raw: `controller_getJoystick(idx, f32[2])`, `controller_copyFaceButtons`, `controller_copySideButtons`
  (`src/core1/pfsmanager.c`).
- bk doesn't use the d-pad in normal play, which is why m0 uses d-pad down. steve's controls will need a proper
  plan (place/break/hotbar) later. `bakey_disableAll` exists if we need to eat banjo's input.

## damage / health

- health is item `ITEM_14_HEALTH` (honeycombs), max `ITEM_15_HEALTH_TOTAL` (starts at 5). air is `ITEM_17_AIR`.
- normal enemy hits: `baMarker_8028D7B8` (`src/core2/ba/marker.c`) → `item_adjustByDiffWithHud(ITEM_14_HEALTH, -n)`
  then a hurt state (`func_8028F428` / `func_8028F55C`). fall damage: `bs/splat.c`. flight hits: `bs/bFly.c`
  (`item_dec(ITEM_14_HEALTH)`).
- iframes: `baiFrame_*` (`src/core2/ba/iFrame.c`).
- **plan for hearts (m5):** hook `item_adjustByDiffWithHud` / `item_dec` for `ITEM_14_HEALTH` and translate to
  minecraft damage, or just watch the count change each tick. keep banjo's own death flow so the game handles it.

## level collision

- **collision provider registry** (`src/core2/code_999A0.c`): up to 8 providers, reset each map by `func_80320B84`.
  each provider is 3 queries + optional "which model was hit" getter, registered with `func_80320B44` (or
  `func_80320B24` without the getter). registered by: the map model (`mapModel.c`: `func_80309B48`,
  `func_80309DBC`, `func_80309EB0`), actor props (`code_7AF80.c`), and CC.
  - `func_80320B98(start, end, out_normal, flags)`: line segment. every provider shortens `end` to its hit, so
    the nearest hit wins. returns a `BKCollisionTri*` (flags = surface type) or NULL.
  - `func_80320C94(from, to, radius, out_normal, ?, flags)`: moving sphere.
  - `func_80320DB0(pos, radius, out_normal, flags)`: static sphere.
  - helpers: `func_80320930(pos, up, down, flags)` floor height probe, `func_803209C4(pos)`.
  - **this is bk's version of the sm64 "blocks as platforms" trick:** if we register a voxel provider here, banjo
    (and enemies, eggs, anything that queries collision) should collide with placed blocks for free. needs a fake
    `BKCollisionTri` to return (flags are read by callers, check what else is read). top candidate for m3.
- map-only line test: `func_80309B48(start, end, out_normal, flag_filter)` → `func_802E76B0` (`code_5FD90.c`).
- banjo's terrain collision: `func_80293F0C` / `func_80293668` (`src/core2/code_C4B0.c`). collider is a sphere:
  `D_8037C1F8[0]` = center height (80), `D_8037C1F8[1]` = radius (35), set by `func_80293D74`.
- floor info: `floor_getCurrentFloorYPosition()`, `func_80294480(normal)`, `func_802946F0()` = current floor tri.
- collision tri: `BKCollisionTri { s16 vtx[3]; s16 unk6; s32 flags; }` in `include/model.h`. vertices are s16.

## actors / objects

- `ActorInfo` (`include/prop.h`): marker id, actor id, model id, anims, `update_func`, `update2_func`,
  `draw_func(ActorMarker*, Gfx**, Mtx**, Vtx**)`, draw distance, shadow scale.
- spawn: `actor_new(s32 pos[3], yaw, ActorInfo*, flags)`, `actor_spawnWithYaw_f32(actor_id, f32 pos[3], yaw)`,
  `spawn_child_actor(actor_id, Actor**)` (`src/core2/code_9E370.c`).
- `Actor` has `position`, `velocity`, `state`, `marker` etc. `ActorMarker` has `drawFunc`, `collisionFunc`, id.
- recomp api (`include/bkrecomp_api.h`): `bkrecomp_extend_marker(_all)` adds per-marker data for mods,
  register from a `recomp_on_init` callback.

## drawing / display lists

- draw order inside `func_80334540`: sky → map opaque (`mapModel_opa_draw`) → props → `player_draw` → actors →
  ... → map translucent → particles.
- `player_draw(gfx, mtx, vtx)` → `baModel_draw` (skips if `!baModelIsVisible`). **hide banjo:** m0 clears the flag
  around `player_draw` and restores it after. `baModel_setVisible` / `baModel_isVisible` (syms ok). also
  `baModel_setPostDraw(fn)` exists as a hook point for drawing extra stuff at the player.
- matrix convention (`modelRender_draw`, `src/core2/modelRender.c`): modelview = translate(object − camera
  position) × rotation × scale; camera rotation is in the projection. so for our own geometry:
  `mlMtxIdent(); mlMtxTranslate(pos - cam); mlMtxApply(*mtx); gSPMatrix((*gfx)++, (*mtx)++, PUSH|LOAD|MODELVIEW)`,
  then `gSPPopMatrix` after. camera: `viewport_getPosition_vec3f`, `viewport_getRotation_vec3f`.
  objects further than 17000 units from the camera on any axis are culled.
- per-frame buffers are small (`src/core1/code_15B30.c`): 3700 gfx commands, 700 matrices, **430 vertices**.
  chunk meshes must live in our own buffers (`recomp_alloc`) and be called with `gSPDisplayList`.
- 2d text: `print_dialog(x, y, str)`, `print_bold_spaced(x, y, str)` queue strings for `printbuffer_draw`
  (32 slots/frame). **they store the pointer**, so use static buffers. screen is ~320x240.
- texture filter: `gDPSetTextureFilter(G_TF_POINT)` for crisp minecraft pixels (bk resets to bilerp after its print pass).

## units

- banjo collider: sphere r=35 centered 80 up. bk gravity −2700/s². minecraft gravity is ~0.08 blocks/tick² =
  32 blocks/s², so 2700 ≈ 32 × 84.
- **proposal: 1 block = 80 bk units** (steve 1.8 blocks = 144 units, close to banjo+kazooie height). to verify by eye in m2/m3.

## modding api bits

- `recomp_alloc/free`, `recomp_printf` (console, so launch the game from a terminal to see it).
- `recomp_get_mod_folder_path()` exists, but there's no file read api in `include/`. loading exporter data will
  probably need a native library (`native_libraries` in `mod.toml`) or baking data into the `.nrm`
  (`additional_files`). to research before m4. remember: exporter output can't be committed.

## open questions

- tick rate at high fps (m0 readout answers it). steve doesn't depend on the answer, he accumulates `time_getDelta()`.
- what callers read from a returned `BKCollisionTri` besides `flags` (needed for the voxel collision provider).
- how prop platforms (`code_C9F00.c`) carry banjo, so steve gets carried too.
- first person camera for steve, or keep bk's camera at first? bk camera code is `ncCamera*` / `cameraMode_update`.
- d-pad availability on the owner's controller mapping in banjo: recompiled.

## tree mining fix (untested)
- `func_803209EC()` after a line query: non-NULL = bk level model was hit, NULL = object model (cube prop) or a mod provider.
- trees are ModelProps in `cube->prop2Ptr`. `unk8_4` = active; bk checks it before drawing and before collision (code_A5BC0.c), so clearing it removes the whole tree.
- mining: level hit -> carve (grass/dirt/stone by depth). object hit -> only trees (from climb poles) count, mined without carving. each pole has trunk-height/80 logs; when the last one goes, the model near the pole is hidden and the pole stops being climbable.
- `cell_inside_ground` now casts *up* from the cell and skips objects (bk line tests are two-sided): an up-facing level floor above = buried. canopies no longer fool it.

## round: holes, hud align, transitions, talking (untested)
- talk-to actors (mole.c, lair code_5ED0, brentilda, code_91E10, code_4A6F0 mumbo) check `func_8028EFC8()` + `controller_copyFaceButtons` B == 1 (D_80281138[0].face_button) within ~250 units, movement group 0. steve fakes one frame of B.
- carved triangles: list[cmd] replaced by G_DL (0x06000000, pointer) to our list. restore = the VTX commands that last filled each cache slot, replayed oldest first.

## data exporter (2026-10-08)
- tools/mc_data.py: full-cube test = the model chain's first `elements` is one box 0..16 with all 6 faces (catches cube_all/cube_column/leaves/orientable). grass_block is special-cased (overlay).
- 26.x models can give textures as `{"sprite": ...}` objects.
- loot: alternatives -> first child not gated by silk touch / shears; table_bonus chances[0] = fortune 0 chance; set_count constant/uniform.
- tool tiers: tags incorrect_for_{wooden,stone,copper,iron,gold,diamond,netherite}_tool.
- fuel: Items.java `cookingFuel(ContextIntProviders.X)` + `context.register(X, cooking(..., ticks))`.
- food: Foods.java builders (+ stew(n) = nutrition n, 0.6), Items.java `.food(Foods.X[, Consumables.Y])`, consumeSeconds default 1.6.

- Face UV rotation: exporter writes `mc_state_rot` (face `rotation` carried through blockstate x/y, uvlock off) in the renderer's per-face texture frame; `turn_uv` applies it. Down faces use v=z (mirror of MC's 16-z), so down-face textures are mirrored.
- Bottles/player_isStable: bk's floor flag D_8037BF60 comes from banjo's own floor probe and stays 0 while Steve drives; `steve_floor_flag` (hook return of func_8028B71C) sets it from Steve's on_ground.
- Tree trunks are one log column: the cell holding the pole axis.
- Steve's B pulse must respect bakey's claim: talkers call func_8028EFC8 (bakey state[B]=2) every frame; if prev_state[B] is set, only the face button is pulsed. Otherwise banjo claw swipes (BS_CLAW, movement group != 0) and Bottles bails.
- Performance: terrain carving batches (terrain_carve_begin/end) so explosions rebuild patches + fills once; carved cells are a hash set. Fluids tick only when scheduled by a change next to them (fluids_on_change from world_set, like neighborChanged/onPlace), and their mesh is cached in world space (rebuilt on change). Redstone / tnt / fire rescan the grid only when world_change_count moves.
- Explosions also carve "surface" cells (terrain_cell_touches_ground: the ground passes through but the centre is above), which left floating triangles before.
- Ground cache: 16384 slots, probes bounded (32) with eviction; air cells check for ground just below before climbing 12 levels. Sections have a coordinate hash (find_section misses were full scans). Fluid meshes are per section (fluids.c FMesh), rebuilt only when that section's fluid changes, no global quad cap.
- Options (mod.toml config_options, src/options.c): starter_kit (new saves: stone tools, bread, torches, crafting table, only if bk's file has no jiggies/notes), toggle_sprint (ToggleKeyMapping), fov (30-110; the hand stays at 70 like minecraft).
- Timing: src/prof.h PROF_START/PROF_END print "[mc] slow: ..." over 2 ms.
