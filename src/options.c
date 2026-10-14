// the mod's options (mod.toml [[manifest.config_options]]), read through
// recomp's config api and cached for a second at a time
#include "modding.h"
#include "recompconfig.h"
#include "options.h"

static u32 frames;
static int cached_kit = 1, cached_toggle = 0, cached_keep = 1, cached_ore_mc = 1;
static f32 cached_fov = 70.0f, cached_snd = 1.0f;
static int cached_slim;

static void refresh(void) {
    if (frames++ % 30 != 0) return;
    cached_kit = recomp_get_config_u32("starter_kit") != 0;
    cached_toggle = recomp_get_config_u32("toggle_sprint") != 0;
    cached_keep = recomp_get_config_u32("keep_terrain") != 0;
    cached_ore_mc = recomp_get_config_u32("ore_heights") == 0; // 0 = Minecraft
    cached_fov = (f32)recomp_get_config_double("fov");
    if (cached_fov < 30.0f || cached_fov > 110.0f) cached_fov = 70.0f;
    cached_snd = (f32)recomp_get_config_double("mc_sound_volume") / 100.0f;
    cached_slim = recomp_get_config_u32("skin_model") == 1; // 1 = Slim
    if (cached_snd < 0.0f || cached_snd > 1.0f) cached_snd = 1.0f;
}

int option_starter_kit(void) { refresh(); return cached_kit; }
int option_toggle_sprint(void) { refresh(); return cached_toggle; }
int option_keep_terrain(void) { refresh(); return cached_keep; }
int option_ore_depth_minecraft(void) { refresh(); return cached_ore_mc; }
f32 option_fov(void) { refresh(); return cached_fov; }
f32 option_sound_volume(void) { refresh(); return cached_snd; }
int option_skin_slim(void) { refresh(); return cached_slim; }
