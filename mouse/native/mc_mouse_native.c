// native linux library for the "bk_minecraft_mouse" mod: minecraft-style
// mouse look, mouse buttons, scroll wheel and extra keyboard keys.
//
// banjo: recompiled doesn't give mods the mouse, and it turns sdl's relative
// mouse mode off on every input event. so this library:
//   - finds the game's own sdl2 at runtime (dlsym, no link-time dependency),
//   - adds an sdl event watch, which runs on the game's main (event) thread,
//   - adds up mouse motion there and re-centers the cursor itself,
//   - hides the cursor with a blank one while captured.
// esc toggles the capture (esc also opens/closes the recomp menu), losing
// window focus releases it.
//
// the mod calls these through the recomp calling convention: args in a0-a3
// (ctx->r4..r7), return value in v0 (ctx->r2). only ints cross the boundary,
// no pointers into game memory.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

// tells the recomp runtime which native api this library speaks
EXPORT uint32_t recomp_api_version = 1;

// start of N64Recomp's recomp_context (MIT, github.com/N64Recomp/N64Recomp,
// include/recomp.h): 32 64-bit gprs first. we only touch those.
typedef struct {
    uint64_t r[32];
} recomp_context_gprs;

static int32_t arg0(recomp_context_gprs *ctx) { return (int32_t)ctx->r[4]; }
static void ret_i32(recomp_context_gprs *ctx, int32_t v) { ctx->r[2] = (uint64_t)(int64_t)v; }

// --- the bits of sdl2's stable abi we need ---

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;

#define SDL_KEYDOWN 0x300
#define SDL_WINDOWEVENT 0x200
#define SDL_MOUSEMOTION 0x400
#define SDL_MOUSEBUTTONDOWN 0x401
#define SDL_MOUSEBUTTONUP 0x402
#define SDL_MOUSEWHEEL 0x403
#define SDL_WINDOWEVENT_FOCUS_GAINED 12
#define SDL_WINDOWEVENT_FOCUS_LOST 13
#define SDL_SCANCODE_ESCAPE 41
#define SDL_MOUSEWHEEL_FLIPPED 1

typedef struct { Uint32 type, timestamp, windowID; Uint8 event, p1, p2, p3; Sint32 data1, data2; } SDL_WindowEvent;
typedef struct { Sint32 scancode; Sint32 sym; Uint16 mod; Uint32 unused; } SDL_Keysym;
typedef struct { Uint32 type, timestamp, windowID; Uint8 state, repeat, p2, p3; SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef struct { Uint32 type, timestamp, windowID, which, state; Sint32 x, y, xrel, yrel; } SDL_MouseMotionEvent;
typedef struct { Uint32 type, timestamp, windowID, which; Uint8 button, state, clicks, p1; Sint32 x, y; } SDL_MouseButtonEvent;
typedef struct { Uint32 type, timestamp, windowID, which; Sint32 x, y; Uint32 direction; } SDL_MouseWheelEvent;
typedef union {
    Uint32 type;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    Uint8 padding[56];
} SDL_Event;

typedef int (*SDL_EventFilter)(void *userdata, SDL_Event *event);

static struct {
    void (*AddEventWatch)(SDL_EventFilter, void *);
    void *(*GetWindowFromID)(Uint32);
    void (*GetWindowSize)(void *, int *, int *);
    void (*WarpMouseInWindow)(void *, int, int);
    void *(*CreateCursor)(const Uint8 *, const Uint8 *, int, int, int, int);
    void (*SetCursor)(void *);
    void *(*GetDefaultCursor)(void);
    const Uint8 *(*GetKeyboardState)(int *);
    Uint32 (*GetTicks)(void);
    Uint32 (*GetMouseState)(int *, int *);
} sdl;

static int sdl_state = 0; // 0 = not tried, 1 = ok, -1 = not found

// --- relative mouse mode ---
// warping the cursor back to the middle doesn't work on wayland (the compositor
// doesn't allow it), so the cursor hit the window edge and the view flicked.
// sdl's relative mode locks the pointer properly, but the game turns it off on
// every input event. the game calls sdl through its GOT, so we point its
// SDL_SetRelativeMouseMode entry at a wrapper that keeps it on while captured.

static int (*real_set_relative)(int enabled) = NULL;
static int relative_patched = 0;
static int relative_on = 0;
static int captured_now(void);

static int wrap_set_relative(int enabled) {
    if (sdl_state == 1 && captured_now()) {
        enabled = 1;
    }
    relative_on = enabled;
    return real_set_relative(enabled);
}

static int patch_got_cb(struct dl_phdr_info *info, size_t size, void *data) {
    ElfW(Addr) base = info->dlpi_addr;
    ElfW(Dyn) *dyn = NULL;
    ElfW(Rela) *tables[2] = { NULL, NULL };
    size_t sizes[2] = { 0, 0 };
    ElfW(Sym) *symtab = NULL;
    const char *strtab = NULL;
    int i, t, *done = data;

    (void)size;
    // only the game executable itself (listed first, with an empty name)
    if (*done || info->dlpi_name == NULL || info->dlpi_name[0] != '\0') {
        *done = 1;
        return 1;
    }
    *done = 1;
    for (i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = (ElfW(Dyn) *)(base + info->dlpi_phdr[i].p_vaddr);
        }
    }
    if (dyn == NULL) {
        return 1;
    }
#define FIX(p) ((ElfW(Addr))(p) < base ? (ElfW(Addr))(p) + base : (ElfW(Addr))(p))
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
            case DT_JMPREL: tables[0] = (ElfW(Rela) *)FIX(dyn->d_un.d_ptr); break;
            case DT_PLTRELSZ: sizes[0] = dyn->d_un.d_val; break;
            case DT_RELA: tables[1] = (ElfW(Rela) *)FIX(dyn->d_un.d_ptr); break;
            case DT_RELASZ: sizes[1] = dyn->d_un.d_val; break;
            case DT_SYMTAB: symtab = (ElfW(Sym) *)FIX(dyn->d_un.d_ptr); break;
            case DT_STRTAB: strtab = (const char *)FIX(dyn->d_un.d_ptr); break;
        }
    }
#undef FIX
    if (symtab == NULL || strtab == NULL) {
        return 1;
    }
    for (t = 0; t < 2; t++) {
        size_t n = sizes[t] / sizeof(ElfW(Rela)), k;

        for (k = 0; tables[t] != NULL && k < n; k++) {
            ElfW(Rela) *r = &tables[t][k];
            unsigned long type = ELF64_R_TYPE(r->r_info);
            void **slot;
            uintptr_t page;
            long ps = sysconf(_SC_PAGESIZE);

            if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT) {
                continue;
            }
            if (strcmp(strtab + symtab[ELF64_R_SYM(r->r_info)].st_name, "SDL_SetRelativeMouseMode") != 0) {
                continue;
            }
            slot = (void **)(base + r->r_offset);
            page = (uintptr_t)slot & ~(uintptr_t)(ps - 1);
            // the GOT can be read-only after relocation (relro)
            if (mprotect((void *)page, ps, PROT_READ | PROT_WRITE) != 0) {
                continue;
            }
            *slot = (void *)wrap_set_relative;
            relative_patched = 1;
        }
    }
    return 1;
}

static void patch_relative_mode(void) {
    int done = 0;

    real_set_relative = dlsym(RTLD_DEFAULT, "SDL_SetRelativeMouseMode");
    if (real_set_relative == NULL) {
        return;
    }
    dl_iterate_phdr(patch_got_cb, &done);
    fprintf(stderr, "[mc mouse] relative mouse mode %s\n", relative_patched ? "hooked" : "not available, using warp");
}

// --- shared state (main thread writes some, game thread others) ---

static volatile int wanted = 0;           // game thread: first person active
static volatile int gui_mode = 0;         // a screen is open: free cursor, but count clicks
static volatile Uint32 wanted_ticks = 0;  // when the game last asked
static volatile int escaped = 0;          // esc pressed: cursor released
static volatile int focused = 1;
static volatile Uint32 window_id = 0;

static int acc_dx, acc_dy, acc_wheel;
static int clicks_left, clicks_right, clicks_middle;
static volatile int buttons_held; // bit 0 left, 1 right, 2 middle (minecraft mines while held)

static void *blank_cursor = NULL;
static int cursor_is_blank = 0;
static int warp_pending = 0, warp_x = 0, warp_y = 0;

#define ATOMIC_ADD(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define ATOMIC_TAKE(p) __atomic_exchange_n((p), 0, __ATOMIC_SEQ_CST)

static void *find_sym(void *lib, const char *name) {
    void *p = dlsym(RTLD_DEFAULT, name);
    if (p == NULL && lib != NULL) {
        p = dlsym(lib, name);
    }
    return p;
}

static int load_sdl(void) {
    void *lib;

    if (sdl_state != 0) {
        return sdl_state == 1;
    }
    // the game already has sdl2 loaded, look for it without loading a second copy
    lib = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
    sdl.AddEventWatch = find_sym(lib, "SDL_AddEventWatch");
    sdl.GetWindowFromID = find_sym(lib, "SDL_GetWindowFromID");
    sdl.GetWindowSize = find_sym(lib, "SDL_GetWindowSize");
    sdl.WarpMouseInWindow = find_sym(lib, "SDL_WarpMouseInWindow");
    sdl.CreateCursor = find_sym(lib, "SDL_CreateCursor");
    sdl.SetCursor = find_sym(lib, "SDL_SetCursor");
    sdl.GetDefaultCursor = find_sym(lib, "SDL_GetDefaultCursor");
    sdl.GetKeyboardState = find_sym(lib, "SDL_GetKeyboardState");
    sdl.GetTicks = find_sym(lib, "SDL_GetTicks");
    sdl.GetMouseState = find_sym(lib, "SDL_GetMouseState");

    if (!sdl.AddEventWatch || !sdl.GetWindowFromID || !sdl.GetWindowSize || !sdl.WarpMouseInWindow
        || !sdl.CreateCursor || !sdl.SetCursor || !sdl.GetDefaultCursor || !sdl.GetKeyboardState || !sdl.GetTicks
        || !sdl.GetMouseState) {
        fprintf(stderr, "[mc mouse] couldn't find sdl2 in the game process, mouse disabled\n");
        sdl_state = -1;
        return 0;
    }
    sdl_state = 1;
    return 1;
}

static int captured_now(void) {
    // if the game stops asking (paused, menu, loading), let go of the mouse
    Uint32 now = sdl.GetTicks();
    return wanted && !escaped && focused && window_id != 0 && (now - wanted_ticks) < 300;
}

static void update_cursor(int captured) {
    if (relative_patched && captured != relative_on) {
        relative_on = captured;
        real_set_relative(captured);
    }
    if (captured && !cursor_is_blank) {
        if (blank_cursor == NULL) {
            static const Uint8 zero[8] = { 0 };
            blank_cursor = sdl.CreateCursor(zero, zero, 8, 8, 0, 0);
        }
        if (blank_cursor != NULL) {
            sdl.SetCursor(blank_cursor);
            cursor_is_blank = 1;
        }
    } else if (!captured && cursor_is_blank) {
        sdl.SetCursor(sdl.GetDefaultCursor());
        cursor_is_blank = 0;
    }
}

// runs on the thread that pumps sdl events (the game's main thread)
static int event_watch(void *userdata, SDL_Event *e) {
    int captured;

    (void)userdata;
    switch (e->type) {
        case SDL_KEYDOWN:
            // only matters while the game wants the mouse, otherwise esc is just the menu key
            if (e->key.keysym.scancode == SDL_SCANCODE_ESCAPE && !e->key.repeat
                && wanted && (sdl.GetTicks() - wanted_ticks) < 300) {
                escaped = !escaped;
            }
            break;
        case SDL_WINDOWEVENT:
            window_id = e->window.windowID;
            if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                focused = 0;
            } else if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                focused = 1;
            }
            break;
        case SDL_MOUSEMOTION:
            window_id = e->motion.windowID;
            if (!captured_now()) {
                break;
            }
            if (warp_pending && e->motion.x == warp_x && e->motion.y == warp_y) {
                warp_pending = 0; // the motion our own re-centering caused
                break;
            }
            ATOMIC_ADD(&acc_dx, e->motion.xrel);
            ATOMIC_ADD(&acc_dy, e->motion.yrel);
            {
                void *win = sdl.GetWindowFromID(e->motion.windowID);
                int w = 0, h = 0;

                if (win != NULL && !relative_on) {
                    sdl.GetWindowSize(win, &w, &h);
                    // re-center once the cursor wanders away, so it never hits the edge
                    if (abs(e->motion.x - w / 2) > w / 4 || abs(e->motion.y - h / 2) > h / 4) {
                        warp_pending = 1;
                        warp_x = w / 2;
                        warp_y = h / 2;
                        sdl.WarpMouseInWindow(win, warp_x, warp_y);
                    }
                }
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e->button.button >= 1 && e->button.button <= 3) {
                static const int bit[4] = { 0, 1, 4, 2 };
                __atomic_and_fetch(&buttons_held, ~bit[e->button.button], __ATOMIC_SEQ_CST);
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if ((captured_now() || gui_mode) && e->button.button >= 1 && e->button.button <= 3) {
                static const int bit[4] = { 0, 1, 4, 2 };
                __atomic_or_fetch(&buttons_held, bit[e->button.button], __ATOMIC_SEQ_CST);
            }
            if (captured_now() || gui_mode) {
                if (e->button.button == 1) ATOMIC_ADD(&clicks_left, 1);
                else if (e->button.button == 3) ATOMIC_ADD(&clicks_right, 1);
                else if (e->button.button == 2) ATOMIC_ADD(&clicks_middle, 1);
            }
            break;
        case SDL_MOUSEWHEEL:
            if (captured_now()) {
                int y = e->wheel.y;
                if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    y = -y;
                }
                ATOMIC_ADD(&acc_wheel, y);
            } else if (gui_mode) {
                ATOMIC_ADD(&acc_wheel, e->wheel.y);
            }
            break;
    }

    captured = captured_now();
    update_cursor(captured);
    return 1;
}

// --- exports (game thread) ---

// a0 = 1 if the mod wants mouse look, 2 for a gui screen (free cursor, clicks
// still counted). returns bit 0 = sdl found, bit 1 = captured.
EXPORT void mcm_poll(uint8_t *rdram, recomp_context_gprs *ctx) {
    static int watch_added = 0;
    int want = arg0(ctx) == 1;

    gui_mode = arg0(ctx) == 2;

    (void)rdram;
    if (!load_sdl()) {
        ret_i32(ctx, 0);
        return;
    }
    if (!watch_added) {
        patch_relative_mode();
        sdl.AddEventWatch(event_watch, NULL);
        watch_added = 1;
        fprintf(stderr, "[mc mouse] ready. esc frees / locks the mouse.\n");
    }
    if (want && !wanted) {
        escaped = 0; // switching into first person grabs the mouse again
    }
    wanted = want;
    wanted_ticks = sdl.GetTicks();
    if (!want) {
        // drop anything collected while we didn't want it
        ATOMIC_TAKE(&acc_dx);
        ATOMIC_TAKE(&acc_dy);
    }
    ret_i32(ctx, 1 | (captured_now() ? 2 : 0));
}

EXPORT void mcm_take_dx(uint8_t *rdram, recomp_context_gprs *ctx) {
    (void)rdram;
    ret_i32(ctx, ATOMIC_TAKE(&acc_dx));
}

EXPORT void mcm_take_dy(uint8_t *rdram, recomp_context_gprs *ctx) {
    (void)rdram;
    ret_i32(ctx, ATOMIC_TAKE(&acc_dy));
}

// presses since last call: left | right << 8 | middle << 16 (each capped at 255)
EXPORT void mcm_take_clicks(uint8_t *rdram, recomp_context_gprs *ctx) {
    int l = ATOMIC_TAKE(&clicks_left), r = ATOMIC_TAKE(&clicks_right), m = ATOMIC_TAKE(&clicks_middle);

    (void)rdram;
    if (l > 255) l = 255;
    if (r > 255) r = 255;
    if (m > 255) m = 255;
    ret_i32(ctx, l | (r << 8) | (m << 16));
}

// buttons held right now: left | right << 1 | middle << 2 (0 while not captured)
EXPORT void mcm_buttons(uint8_t *rdram, recomp_context_gprs *ctx) {
    (void)rdram;
    if (sdl_state != 1 || (!captured_now() && !gui_mode)) {
        buttons_held = 0;
    }
    ret_i32(ctx, buttons_held);
}

// window size in pixels: w << 16 | h (0 if unknown)
EXPORT void mcm_window_size(uint8_t *rdram, recomp_context_gprs *ctx) {
    void *win;
    int w = 0, h = 0;

    (void)rdram;
    if (sdl_state == 1 && window_id != 0 && (win = sdl.GetWindowFromID(window_id)) != NULL) {
        sdl.GetWindowSize(win, &w, &h);
    }
    ret_i32(ctx, ((w & 0xFFFF) << 16) | (h & 0xFFFF));
}

// cursor position in window pixels: x << 16 | y
EXPORT void mcm_cursor(uint8_t *rdram, recomp_context_gprs *ctx) {
    int x = 0, y = 0;

    (void)rdram;
    if (sdl_state == 1) {
        sdl.GetMouseState(&x, &y);
    }
    ret_i32(ctx, ((x & 0xFFFF) << 16) | (y & 0xFFFF));
}

EXPORT void mcm_take_wheel(uint8_t *rdram, recomp_context_gprs *ctx) {
    (void)rdram;
    ret_i32(ctx, ATOMIC_TAKE(&acc_wheel));
}

// a0 = sdl scancode. returns 1 while held (only while the window has focus).
EXPORT void mcm_key(uint8_t *rdram, recomp_context_gprs *ctx) {
    int sc = arg0(ctx);
    int num = 0;
    const Uint8 *keys;

    (void)rdram;
    if (sdl_state != 1 || !focused || sc < 0) {
        ret_i32(ctx, 0);
        return;
    }
    keys = sdl.GetKeyboardState(&num);
    ret_i32(ctx, (keys != NULL && sc < num) ? keys[sc] : 0);
}

// --- small save files for the steve mod (one per bk save slot) ---
// bytes go across one at a time (only ints cross the boundary). files live
// next to banjo: recompiled's config: ~/.config/BanjoRecompiled/mc_steve_<slot>.bin

static uint8_t save_buf[65536];
static int save_len;
static uint8_t load_buf[65536];
static int load_len;

static void save_path(char *out, size_t n, int slot) {
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/.config/BanjoRecompiled/mc_steve_%d.bin", home ? home : ".", slot);
}

EXPORT void mcm_save_begin(uint8_t *rdram, recomp_context_gprs *ctx) {
    (void)rdram;
    save_len = 0;
    ret_i32(ctx, 0);
}

EXPORT void mcm_save_put(uint8_t *rdram, recomp_context_gprs *ctx) {
    (void)rdram;
    if (save_len < (int)sizeof(save_buf)) save_buf[save_len++] = (uint8_t)arg0(ctx);
    ret_i32(ctx, 0);
}

EXPORT void mcm_save_commit(uint8_t *rdram, recomp_context_gprs *ctx) {
    char path[512], tmp[520];
    FILE *f;
    int ok = 0;

    (void)rdram;
    save_path(path, sizeof(path), arg0(ctx));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f != NULL) {
        ok = fwrite(save_buf, 1, save_len, f) == (size_t)save_len;
        ok = (fclose(f) == 0) && ok;
        if (ok) ok = rename(tmp, path) == 0;
    }
    ret_i32(ctx, ok ? save_len : -1);
}

EXPORT void mcm_load_open(uint8_t *rdram, recomp_context_gprs *ctx) {
    char path[512];
    FILE *f;

    (void)rdram;
    save_path(path, sizeof(path), arg0(ctx));
    load_len = -1;
    f = fopen(path, "rb");
    if (f != NULL) {
        load_len = (int)fread(load_buf, 1, sizeof(load_buf), f);
        fclose(f);
    }
    ret_i32(ctx, load_len);
}

EXPORT void mcm_load_get(uint8_t *rdram, recomp_context_gprs *ctx) {
    int i = arg0(ctx);
    (void)rdram;
    ret_i32(ctx, (i >= 0 && i < load_len) ? load_buf[i] : -1);
}

EXPORT void mcm_save_delete(uint8_t *rdram, recomp_context_gprs *ctx) {
    char path[512];
    (void)rdram;
    save_path(path, sizeof(path), arg0(ctx));
    ret_i32(ctx, remove(path) == 0);
}

// --- sounds: the steve mod's minecraft sounds (its SoundEngine picks the
// file, volume, pitch and pan; this is the speaker). a small mixer on its own
// sdl audio device next to the game's. the files are 16 bit mono 44100 hz pcm
// that tools/mc_sounds.py made, in mc_sounds/<n>.pcm next to this library,
// loaded the first time each is played.

typedef struct {
    int freq;
    Uint16 format;
    Uint8 channels, silence;
    Uint16 samples, padding;
    Uint32 size;
    void (*callback)(void *userdata, Uint8 *stream, int len);
    void *userdata;
} SDL_AudioSpec;
#define SDL_INIT_AUDIO 0x10
#define AUDIO_S16SYS 0x8010
#define SND_RATE 44100
#define SND_VOICES 48
#define SND_FILES_MAX 4096

static struct {
    Uint32 (*OpenAudioDevice)(const char *, int, const SDL_AudioSpec *, SDL_AudioSpec *, int);
    void (*PauseAudioDevice)(Uint32, int);
    void (*LockAudioDevice)(Uint32);
    void (*UnlockAudioDevice)(Uint32);
    int (*InitSubSystem)(Uint32);
    Uint32 (*WasInit)(Uint32);
} sa;
static int snd_state = 0; // 0 not tried, 1 ok, -1 failed
static Uint32 snd_dev;
static int snd_out_rate = SND_RATE;
static char snd_dir[1024];

typedef struct {
    int16_t *data;
    uint32_t len; // samples
    int tried;
} SndFile;
static SndFile *snd_files;

typedef struct {
    const int16_t *data;
    uint32_t len;
    double pos, step;
    float gl, gr;
    int active;
} SndVoice;
static SndVoice voices[SND_VOICES];

static void snd_mix(void *userdata, Uint8 *stream, int len) {
    int16_t *out = (int16_t *)stream;
    int frames = len / 4, f, v;

    (void)userdata;
    for (f = 0; f < frames; f++) {
        float l = 0.0f, r = 0.0f;
        for (v = 0; v < SND_VOICES; v++) {
            SndVoice *vc = &voices[v];
            uint32_t i;
            float s, t;
            if (!vc->active) continue;
            i = (uint32_t)vc->pos;
            if (i + 1 >= vc->len) {
                vc->active = 0;
                continue;
            }
            t = (float)(vc->pos - i);
            s = vc->data[i] * (1.0f - t) + vc->data[i + 1] * t; // linear resampling
            l += s * vc->gl;
            r += s * vc->gr;
            vc->pos += vc->step;
        }
        if (l > 32767.0f) l = 32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r > 32767.0f) r = 32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        out[f * 2] = (int16_t)l;
        out[f * 2 + 1] = (int16_t)r;
    }
}

// the folder over the mods folder this library is in (~/.config/BanjoRecompiled)
static void config_dir(char *out, size_t cap) {
    Dl_info info;
    out[0] = 0;
    if (dladdr((void *)config_dir, &info) && info.dli_fname != NULL) {
        const char *slash = strrchr(info.dli_fname, '/');
        size_t n = slash ? (size_t)(slash - info.dli_fname) : 0;
        if (n >= cap) n = 0;
        memcpy(out, info.dli_fname, n);
        out[n] = 0;
        slash = strrchr(out, '/'); // (up from mods/)
        if (slash != NULL) out[slash - out] = 0;
    }
}

static int snd_init(void) {
    void *lib;
    SDL_AudioSpec want, have;
    Dl_info info;

    if (snd_state != 0) return snd_state == 1;
    snd_state = -1;
    lib = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
    sa.OpenAudioDevice = find_sym(lib, "SDL_OpenAudioDevice");
    sa.PauseAudioDevice = find_sym(lib, "SDL_PauseAudioDevice");
    sa.LockAudioDevice = find_sym(lib, "SDL_LockAudioDevice");
    sa.UnlockAudioDevice = find_sym(lib, "SDL_UnlockAudioDevice");
    sa.InitSubSystem = find_sym(lib, "SDL_InitSubSystem");
    sa.WasInit = find_sym(lib, "SDL_WasInit");
    if (!sa.OpenAudioDevice || !sa.PauseAudioDevice || !sa.LockAudioDevice || !sa.UnlockAudioDevice || !sa.InitSubSystem || !sa.WasInit) {
        fprintf(stderr, "[mc sound] no sdl2 audio in the game process: no minecraft sounds\n");
        return 0;
    }
    if (!(sa.WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && sa.InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[mc sound] couldn't start sdl audio: no minecraft sounds\n");
        return 0;
    }
    memset(&want, 0, sizeof(want));
    want.freq = SND_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = snd_mix;
    snd_dev = sa.OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (snd_dev == 0) {
        fprintf(stderr, "[mc sound] couldn't open an audio device: no minecraft sounds\n");
        return 0;
    }
    snd_out_rate = have.freq > 0 ? have.freq : SND_RATE;
    // mc_sounds/ in the game's config folder, one up from the mods folder
    // this library sits in (in mods/ the recomp would try it as a mod)
    config_dir(snd_dir, sizeof(snd_dir));
    (void)info;
    snd_files = calloc(SND_FILES_MAX, sizeof(SndFile));
    if (snd_files == NULL) return 0;
    sa.PauseAudioDevice(snd_dev, 0);
    fprintf(stderr, "[mc sound] mixer on (%d hz), sounds from %s/mc_sounds\n", snd_out_rate, snd_dir[0] ? snd_dir : ".");
    snd_state = 1;
    return 1;
}

static SndFile *snd_file(int n) {
    SndFile *f;
    char path[1200];
    FILE *fp;
    long size;

    if (n < 0 || n >= SND_FILES_MAX) return NULL;
    f = &snd_files[n];
    if (!f->tried) {
        f->tried = 1;
        snprintf(path, sizeof(path), "%s/mc_sounds/%d.pcm", snd_dir[0] ? snd_dir : ".", n);
        fp = fopen(path, "rb");
        if (fp != NULL) {
            fseek(fp, 0, SEEK_END);
            size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (size > 4) {
                f->data = malloc((size_t)size);
                if (f->data != NULL && fread(f->data, 1, (size_t)size, fp) == (size_t)size) {
                    f->len = (uint32_t)(size / 2);
                } else {
                    free(f->data);
                    f->data = NULL;
                }
            }
            fclose(fp);
        }
    }
    return f->data != NULL ? f : NULL;
}

// play(file, volume * 4096, pitch * 4096, pan * 4096 (-1 left .. 1 right)) -> voice or -1
EXPORT void mcm_snd_play(uint8_t *rdram, recomp_context_gprs *ctx) {
    int file = arg0(ctx);
    float vol = (int32_t)ctx->r[5] / 4096.0f, pitch = (int32_t)ctx->r[6] / 4096.0f, pan = (int32_t)ctx->r[7] / 4096.0f;
    SndFile *f;
    int v, slot = -1;
    float a;

    (void)rdram;
    if (!snd_init() || vol <= 0.0f || (f = snd_file(file)) == NULL) {
        ret_i32(ctx, -1);
        return;
    }
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    if (pitch < 0.1f) pitch = 0.1f;
    sa.LockAudioDevice(snd_dev);
    for (v = 0; v < SND_VOICES; v++) {
        if (!voices[v].active) { slot = v; break; }
    }
    if (slot < 0) {
        // all busy: take the one nearest its end
        double best = -1.0;
        for (v = 0; v < SND_VOICES; v++) {
            double left = voices[v].len - voices[v].pos;
            if (best < 0.0 || left < best) { best = left; slot = v; }
        }
    }
    a = (pan + 1.0f) * 0.78539816f; // equal power
    voices[slot].data = f->data;
    voices[slot].len = f->len;
    voices[slot].pos = 0.0;
    voices[slot].step = pitch * (double)SND_RATE / snd_out_rate;
    voices[slot].gl = vol * __builtin_cosf(a);
    voices[slot].gr = vol * __builtin_sinf(a);
    voices[slot].active = 1;
    sa.UnlockAudioDevice(snd_dev);
    ret_i32(ctx, slot);
}

// stop everything (SoundEngine.stopAll: map changes, pause)
EXPORT void mcm_snd_stop_all(uint8_t *rdram, recomp_context_gprs *ctx) {
    int v;
    (void)rdram;
    if (snd_state == 1) {
        sa.LockAudioDevice(snd_dev);
        for (v = 0; v < SND_VOICES; v++) voices[v].active = 0;
        sa.UnlockAudioDevice(snd_dev);
    }
    ret_i32(ctx, 0);
}

// --- custom skin: ~/.config/BanjoRecompiled/mc_skin.png (one up from mods/).
// decoded here (png: zlib + the five filters, 8 bit rgba / rgb / grey, any
// palette), then SkinTextureDownloader.processLegacySkin: 64 x 32 skins get
// their limbs copied into the 64 x 64 layout, the notch transparency hack,
// the base layer made opaque. the mod reads it a pixel at a time.

#include <zlib.h>
#include <sys/stat.h>

static uint32_t skin_px[64 * 64]; // 0xRRGGBBAA
static int skin_gen;              // > 0: a skin is loaded (bumps on every change)
static time_t skin_mtime;
static off_t skin_size;
static int skin_seen;

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

// -> 0xRRGGBBAA pixels (malloc'd), or NULL
static uint32_t *png_decode(const uint8_t *d, size_t n, int *w_out, int *h_out) {
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    size_t pos = 8, idat_len = 0;
    uint8_t *idat = NULL, *raw = NULL, pal[256][4];
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0, npal = 0, x, y, k;
    uint32_t *out = NULL;
    uLongf raw_len;
    int chans, bpp, stride;

    if (n < 8 || memcmp(d, sig, 8) != 0) return NULL;
    memset(pal, 255, sizeof(pal));
    while (pos + 12 <= n) {
        uint32_t len = be32(d + pos);
        const uint8_t *type = d + pos + 4, *body = d + pos + 8;
        if (pos + 12 + len > n) break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = (int)be32(body);
            h = (int)be32(body + 4);
            depth = body[8];
            ctype = body[9];
            interlace = body[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            npal = (int)(len / 3 > 256 ? 256 : len / 3);
            for (k = 0; k < npal; k++) { pal[k][0] = body[k * 3]; pal[k][1] = body[k * 3 + 1]; pal[k][2] = body[k * 3 + 2]; }
        } else if (!memcmp(type, "tRNS", 4) && ctype == 3) {
            for (k = 0; k < (int)len && k < 256; k++) pal[k][3] = body[k];
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *t = realloc(idat, idat_len + len);
            if (t == NULL) { free(idat); return NULL; }
            idat = t;
            memcpy(idat + idat_len, body, len);
            idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || interlace != 0 || idat == NULL) { free(idat); return NULL; }
    chans = ctype == 6 ? 4 : ctype == 2 ? 3 : ctype == 4 ? 2 : 1;
    if ((ctype != 3 && depth != 8) || (ctype == 3 && depth > 8)) { free(idat); return NULL; }
    bpp = (chans * depth + 7) / 8;
    stride = (w * chans * depth + 7) / 8;
    raw_len = (uLongf)(stride + 1) * h;
    raw = malloc(raw_len);
    out = malloc(sizeof(uint32_t) * w * h);
    if (raw == NULL || out == NULL || uncompress(raw, &raw_len, idat, idat_len) != Z_OK || raw_len < (uLongf)(stride + 1) * h) {
        free(idat); free(raw); free(out);
        return NULL;
    }
    free(idat);
    // unfilter in place (filter byte, then the scanline)
    for (y = 0; y < h; y++) {
        uint8_t *line = raw + y * (stride + 1) + 1, *prev = y ? raw + (y - 1) * (stride + 1) + 1 : NULL;
        int f = line[-1];
        for (x = 0; x < stride; x++) {
            int a = x >= bpp ? line[x - bpp] : 0, b = prev ? prev[x] : 0, c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            switch (f) {
                case 1: line[x] = (uint8_t)(line[x] + a); break;
                case 2: line[x] = (uint8_t)(line[x] + b); break;
                case 3: line[x] = (uint8_t)(line[x] + ((a + b) >> 1)); break;
                case 4: line[x] = (uint8_t)(line[x] + paeth(a, b, c)); break;
            }
        }
        for (x = 0; x < w; x++) {
            uint8_t r, g, bl, al = 255;
            if (ctype == 3) {
                int bits = x * depth, v = (line[bits >> 3] >> (8 - depth - (bits & 7))) & ((1 << depth) - 1);
                r = pal[v][0]; g = pal[v][1]; bl = pal[v][2]; al = pal[v][3];
            } else if (ctype == 6) {
                r = line[x * 4]; g = line[x * 4 + 1]; bl = line[x * 4 + 2]; al = line[x * 4 + 3];
            } else if (ctype == 2) {
                r = line[x * 3]; g = line[x * 3 + 1]; bl = line[x * 3 + 2];
            } else if (ctype == 4) {
                r = g = bl = line[x * 2]; al = line[x * 2 + 1];
            } else {
                r = g = bl = line[x];
            }
            out[y * w + x] = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)bl << 8) | al;
        }
    }
    free(raw);
    *w_out = w;
    *h_out = h;
    return out;
}

// NativeImage.copyRect(startX, startY, offsetX, offsetY, sizeX, sizeY, swapX, swapY)
static void skin_copy_rect(int sx, int sy, int ox, int oy, int w, int h, int swap_x, int swap_y) {
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int dx = swap_x ? w - 1 - x : x, dy = swap_y ? h - 1 - y : y;
            skin_px[(sy + oy + dy) * 64 + sx + ox + dx] = skin_px[(sy + y) * 64 + sx + x];
        }
    }
}

static void skin_set_no_alpha(int x0, int y0, int x1, int y1) {
    int x, y;
    for (x = x0; x < x1; x++) for (y = y0; y < y1; y++) skin_px[y * 64 + x] |= 0xFF;
}

static void skin_notch_hack(int x0, int y0, int x1, int y1) {
    int x, y;
    for (x = x0; x < x1; x++) for (y = y0; y < y1; y++) if ((skin_px[y * 64 + x] & 0xFF) < 128) return;
    for (x = x0; x < x1; x++) for (y = y0; y < y1; y++) skin_px[y * 64 + x] &= 0xFFFFFF00;
}

static int skin_load(const char *path) {
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long size;
    uint32_t *img;
    int w = 0, h = 0, legacy, y;

    if (fp == NULL) return 0;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024) { fclose(fp); return 0; }
    buf = malloc((size_t)size);
    if (buf == NULL || fread(buf, 1, (size_t)size, fp) != (size_t)size) { fclose(fp); free(buf); return 0; }
    fclose(fp);
    img = png_decode(buf, (size_t)size, &w, &h);
    free(buf);
    if (img == NULL) {
        fprintf(stderr, "[mc skin] %s isn't a png it can read (8 bit, not interlaced)\n", path);
        return 0;
    }
    // processLegacySkin: only 64 x 32 / 64 x 64
    if (w != 64 || (h != 32 && h != 64)) {
        fprintf(stderr, "[mc skin] discarding incorrectly sized (%dx%d) skin\n", w, h);
        free(img);
        return 0;
    }
    legacy = h == 32;
    memset(skin_px, 0, sizeof(skin_px));
    for (y = 0; y < h; y++) memcpy(&skin_px[y * 64], &img[y * 64], 64 * sizeof(uint32_t));
    free(img);
    if (legacy) {
        // (fillRect(0, 32, 64, 32, 0): already clear)
        skin_copy_rect(4, 16, 16, 32, 4, 4, 1, 0);
        skin_copy_rect(8, 16, 16, 32, 4, 4, 1, 0);
        skin_copy_rect(0, 20, 24, 32, 4, 12, 1, 0);
        skin_copy_rect(4, 20, 16, 32, 4, 12, 1, 0);
        skin_copy_rect(8, 20, 8, 32, 4, 12, 1, 0);
        skin_copy_rect(12, 20, 16, 32, 4, 12, 1, 0);
        skin_copy_rect(44, 16, -8, 32, 4, 4, 1, 0);
        skin_copy_rect(48, 16, -8, 32, 4, 4, 1, 0);
        skin_copy_rect(40, 20, 0, 32, 4, 12, 1, 0);
        skin_copy_rect(44, 20, -8, 32, 4, 12, 1, 0);
        skin_copy_rect(48, 20, -16, 32, 4, 12, 1, 0);
        skin_copy_rect(52, 20, -8, 32, 4, 12, 1, 0);
    }
    skin_set_no_alpha(0, 0, 32, 16);
    if (legacy) skin_notch_hack(32, 0, 64, 32);
    skin_set_no_alpha(0, 16, 64, 32);
    skin_set_no_alpha(16, 48, 48, 64);
    fprintf(stderr, "[mc skin] loaded %s (64x%d%s)\n", path, h, legacy ? ", legacy" : "");
    return 1;
}

// the skin's generation: > 0 a skin is loaded (changes when the file does),
// 0 none. cheap to call (one stat)
EXPORT void mcm_skin_poll(uint8_t *rdram, recomp_context_gprs *ctx) {
    char dir[1024], path[1100];
    struct stat st;
    (void)rdram;
    config_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/mc_skin.png", dir[0] ? dir : ".");
    if (stat(path, &st) != 0) {
        if (skin_gen > 0) skin_gen = -skin_gen - 1; // gone: back to steve (a new number)
        skin_seen = 0;
        ret_i32(ctx, skin_gen > 0 ? skin_gen : 0);
        return;
    }
    if (!skin_seen || st.st_mtime != skin_mtime || st.st_size != skin_size) {
        int g = (skin_gen < 0 ? -skin_gen : skin_gen) + 1;
        skin_seen = 1;
        skin_mtime = st.st_mtime;
        skin_size = st.st_size;
        skin_gen = skin_load(path) ? g : -g;
    }
    ret_i32(ctx, skin_gen > 0 ? skin_gen : 0);
}

// pixel i (y * 64 + x) of the loaded skin: 0xRRGGBBAA
EXPORT void mcm_skin_px(uint8_t *rdram, recomp_context_gprs *ctx) {
    int i = arg0(ctx);
    (void)rdram;
    ret_i32(ctx, (i >= 0 && i < 64 * 64) ? (int32_t)skin_px[i] : 0);
}
