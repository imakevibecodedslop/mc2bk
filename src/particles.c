// TerrainParticle, ported: Particle(level, x, y, z, xa, ya, za) start motion,
// Particle.tick (gravity 0.04, friction 0.98, 0.7 on the ground), lifetime
// 4 / (rand * 0.9 + 0.1) ticks, quadSize 0.1 * (rand * 0.5 + 0.5) * 2 / 2,
// a random 4x4 texel corner of the block's particle texture, color 0.6
// (times the grass/foliage tint, which our textures already have baked in).

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "particles.h"
#include "mc_gbi.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

#define MAX_PARTICLES 160

typedef struct {
    int alive;
    f32 pos[3], o_pos[3]; // bk units
    f32 vel[3];           // blocks per tick
    int age, lifetime;
    f32 size;             // quadSize in blocks
    u16 tex;              // block texture index
    u8 uo, vo;            // which 4x4 texel bit of the texture
    int on_ground;
    u8 kind;              // 0 terrain, 1 smoke, 2 explosion, 3 poof
    u8 col;               // rCol = gCol = bCol (sprite particles)
} TParticle;

enum { PK_TERRAIN, PK_SMOKE, PK_EXPLOSION, PK_POOF };

// HugeExplosionSeedParticle: 6 explosion particles a tick for 8 ticks
#define MAX_SEEDS 4
static struct { int alive, age; f32 pos[3]; } seeds[MAX_SEEDS];

static TParticle parts[MAX_PARTICLES];
static u32 prng = 0x9E3779B9;

static f32 frand(void) {
    prng = prng * 1103515245u + 12345u;
    return (f32)((prng >> 8) & 0xFFFFFF) / 16777216.0f;
}

void particles_clear(void) {
    int i;
    for (i = 0; i < MAX_PARTICLES; i++) parts[i].alive = FALSE;
}

static TParticle *new_particle(void) {
    int i, oldest = 0;
    for (i = 0; i < MAX_PARTICLES; i++) {
        if (!parts[i].alive) return &parts[i];
        if (parts[i].age > parts[oldest].age) oldest = i;
    }
    return &parts[oldest];
}

// the block's particle texture (block models' "particle": grass uses dirt)
static u16 particle_tex(u16 block) {
    return world_block_particle_tex(block);
}

static void spawn(f32 x, f32 y, f32 z, f32 xa, f32 ya, f32 za, u16 block, f32 power, f32 scale) {
    TParticle *p = new_particle();
    f32 speed, dd;
    int k;

    p->alive = TRUE;
    p->pos[0] = x * MC_BLOCK;
    p->pos[1] = y * MC_BLOCK;
    p->pos[2] = z * MC_BLOCK;
    for (k = 0; k < 3; k++) p->o_pos[k] = p->pos[k];
    // Particle(level, x, y, z, xa, ya, za)
    p->vel[0] = xa + (frand() * 2.0f - 1.0f) * 0.4f;
    p->vel[1] = ya + (frand() * 2.0f - 1.0f) * 0.4f;
    p->vel[2] = za + (frand() * 2.0f - 1.0f) * 0.4f;
    speed = (frand() + frand() + 1.0f) * 0.15f;
    dd = __builtin_sqrtf(p->vel[0] * p->vel[0] + p->vel[1] * p->vel[1] + p->vel[2] * p->vel[2]);
    if (dd < 1e-5f) dd = 1e-5f;
    p->vel[0] = p->vel[0] / dd * speed * 0.4f;
    p->vel[1] = p->vel[1] / dd * speed * 0.4f + 0.1f;
    p->vel[2] = p->vel[2] / dd * speed * 0.4f;
    // setPower
    if (power != 1.0f) {
        p->vel[0] *= power;
        p->vel[1] = (p->vel[1] - 0.1f) * power + 0.1f;
        p->vel[2] *= power;
    }
    p->lifetime = (int)(4.0f / (frand() * 0.9f + 0.1f));
    p->age = 0;
    p->size = 0.1f * (frand() * 0.5f + 0.5f) * 2.0f / 2.0f * scale;
    p->tex = particle_tex(block);
    p->uo = (u8)(frand() * 3.0f);
    p->vo = (u8)(frand() * 3.0f);
    p->on_ground = FALSE;
    p->kind = PK_TERRAIN;
}

// a SingleQuadParticle with Particle's random start motion (xa = ya = za = 0)
static TParticle *spawn_sprite(const f32 pos_bk[3], u8 kind) {
    TParticle *p = new_particle();
    f32 speed, dd;
    int k;

    p->alive = TRUE;
    for (k = 0; k < 3; k++) p->pos[k] = p->o_pos[k] = pos_bk[k];
    p->vel[0] = (frand() * 2.0f - 1.0f) * 0.4f;
    p->vel[1] = (frand() * 2.0f - 1.0f) * 0.4f;
    p->vel[2] = (frand() * 2.0f - 1.0f) * 0.4f;
    speed = (frand() + frand() + 1.0f) * 0.15f;
    dd = __builtin_sqrtf(p->vel[0] * p->vel[0] + p->vel[1] * p->vel[1] + p->vel[2] * p->vel[2]);
    if (dd < 1e-5f) dd = 1e-5f;
    p->vel[0] = p->vel[0] / dd * speed * 0.4f;
    p->vel[1] = p->vel[1] / dd * speed * 0.4f + 0.1f;
    p->vel[2] = p->vel[2] / dd * speed * 0.4f;
    p->size = 0.1f * (frand() * 0.5f + 0.5f) * 2.0f;
    p->age = 0;
    p->on_ground = FALSE;
    p->kind = kind;
    return p;
}

// SmokeParticle (BaseAshSmokeParticle: dir 0.1, colour random * 0.3, life 8,
// gravity -0.1, friction 0.96, size * 0.75)
void particles_smoke(const f32 pos_bk[3]) {
    TParticle *p = spawn_sprite(pos_bk, PK_SMOKE);
    int k;
    for (k = 0; k < 3; k++) p->vel[k] *= 0.1f;
    p->col = (u8)(frand() * 0.3f * 255.0f);
    p->size *= 0.75f;
    p->lifetime = (int)(8.0f / (frand() * 0.8f + 0.2f));
    if (p->lifetime < 1) p->lifetime = 1;
}

// ExplodeParticle (ParticleTypes.POOF): SingleQuadParticle(level, x, y, z)
// starts still, then xa + random * 0.05, gravity -0.1, friction 0.9, colour
// 0.7..1, quadSize 0.1 * (r * r * 6 + 1), 16 / (r * 0.8 + 0.2) + 2 ticks
void particles_poof(const f32 pos_bk[3], const f32 vel[3]) {
    TParticle *p = spawn_sprite(pos_bk, PK_POOF);
    f32 r;
    int k;
    for (k = 0; k < 3; k++) p->vel[k] = vel[k] + (frand() * 2.0f - 1.0f) * 0.05f;
    p->col = (u8)((frand() * 0.3f + 0.7f) * 255.0f);
    r = frand();
    p->size = 0.1f * (r * frand() * 6.0f + 1.0f);
    p->lifetime = (int)(16.0f / (frand() * 0.8f + 0.2f)) + 2;
}

// HugeExplosionParticle: still, 6 + random(4) ticks, colour 0.4..1, full bright
static void explosion_particle(const f32 pos_bk[3], f32 size) {
    TParticle *p = spawn_sprite(pos_bk, PK_EXPLOSION);
    p->vel[0] = p->vel[1] = p->vel[2] = 0.0f;
    p->lifetime = 6 + (int)(frand() * 4.0f);
    p->col = (u8)((frand() * 0.6f + 0.4f) * 255.0f);
    p->size = 2.0f * (1.0f - size * 0.5f);
}

// ServerExplosion: EXPLOSION_EMITTER (power >= 2) -> HugeExplosionSeedParticle
void particles_explosion(const f32 pos_bk[3]) {
    int i;
    for (i = 0; i < MAX_SEEDS; i++) {
        if (!seeds[i].alive) {
            seeds[i].alive = TRUE;
            seeds[i].age = 0;
            seeds[i].pos[0] = pos_bk[0];
            seeds[i].pos[1] = pos_bk[1];
            seeds[i].pos[2] = pos_bk[2];
            return;
        }
    }
}

void particles_destroy_block(s32 cell[3], u16 block) {
    int xx, yy, zz;

    for (xx = 0; xx < 4; xx++) {
        for (yy = 0; yy < 4; yy++) {
            for (zz = 0; zz < 4; zz++) {
                f32 rx = (xx + 0.5f) / 4.0f, ry = (yy + 0.5f) / 4.0f, rz = (zz + 0.5f) / 4.0f;
                spawn(cell[0] + rx, cell[1] + ry, cell[2] + rz, rx - 0.5f, ry - 0.5f, rz - 0.5f, block, 1.0f, 1.0f);
            }
        }
    }
}

void particles_crack(s32 cell[3], s32 n[3], u16 block) {
    f32 x = cell[0] + frand() * 0.8f + 0.1f;
    f32 y = cell[1] + frand() * 0.8f + 0.1f;
    f32 z = cell[2] + frand() * 0.8f + 0.1f;

    if (n[1] < 0) y = cell[1] - 0.1f;
    if (n[1] > 0) y = cell[1] + 1.1f;
    if (n[2] < 0) z = cell[2] - 0.1f;
    if (n[2] > 0) z = cell[2] + 1.1f;
    if (n[0] < 0) x = cell[0] - 0.1f;
    if (n[0] > 0) x = cell[0] + 1.1f;
    spawn(x, y, z, 0.0f, 0.0f, 0.0f, block, 0.2f, 0.6f);
}

void particles_tick(void) {
    int i, k;

    for (i = 0; i < MAX_SEEDS; i++) {
        int n;
        if (!seeds[i].alive) continue;
        for (n = 0; n < 6; n++) {
            f32 q[3];
            for (k = 0; k < 3; k++) q[k] = seeds[i].pos[k] + (frand() - frand()) * 4.0f * MC_BLOCK;
            explosion_particle(q, (f32)seeds[i].age / 8.0f);
        }
        if (++seeds[i].age == 8) seeds[i].alive = FALSE;
    }

    for (i = 0; i < MAX_PARTICLES; i++) {
        TParticle *p = &parts[i];
        if (!p->alive) continue;
        for (k = 0; k < 3; k++) p->o_pos[k] = p->pos[k];
        if (p->age++ >= p->lifetime) {
            p->alive = FALSE;
            continue;
        }
        if (p->kind == PK_EXPLOSION) continue; // HugeExplosionParticle.tick: only ages
        if (p->kind == PK_POOF) {
            // Particle.tick, gravity -0.1, friction 0.9 (not collided: it rises)
            p->vel[1] -= 0.04f * -0.1f;
            for (k = 0; k < 3; k++) {
                p->pos[k] += p->vel[k] * MC_BLOCK;
                p->vel[k] *= 0.9f;
            }
            continue;
        }
        if (p->kind == PK_SMOKE) {
            // Particle.tick with gravity -0.1, friction 0.96, no ground test
            // (speedUpWhenYMotionIsBlocked: the ceiling isn't tested either)
            p->vel[1] -= 0.04f * -0.1f;
            for (k = 0; k < 3; k++) {
                p->pos[k] += p->vel[k] * MC_BLOCK;
                p->vel[k] *= 0.96f;
            }
            continue;
        }
        p->vel[1] -= 0.04f; // gravity 1.0
        // move: only the fall is collided (a ray down), like a tiny box
        p->pos[0] += p->vel[0] * MC_BLOCK;
        p->pos[2] += p->vel[2] * MC_BLOCK;
        if (p->vel[1] < 0.0f) {
            f32 a[3], b[3], hit[3], n[3];
            a[0] = b[0] = p->pos[0];
            a[2] = b[2] = p->pos[2];
            a[1] = p->pos[1] + 1.0f;
            b[1] = p->pos[1] + p->vel[1] * MC_BLOCK;
            if (steve_raycast(a, b, hit, n)) {
                p->pos[1] = hit[1];
                p->vel[1] = 0.0f;
                p->on_ground = TRUE;
            } else {
                p->pos[1] = b[1];
                p->on_ground = FALSE;
            }
        } else {
            p->pos[1] += p->vel[1] * MC_BLOCK;
            p->on_ground = FALSE;
        }
        p->vel[0] *= 0.98f;
        p->vel[1] *= 0.98f;
        p->vel[2] *= 0.98f;
        if (p->on_ground) {
            p->vel[0] *= 0.7f;
            p->vel[2] *= 0.7f;
        }
    }
}

#define PBUFS 3
static Vtx pvtx[PBUFS][MAX_PARTICLES][4];
static int pbuf;

// camera facing quads (SingleQuadParticle), camera relative coords
void particles_draw(Gfx **gfx, Mtx **mtx) {
#ifdef MC_HAVE_TEXTURES
    f32 cam[3], look[3], right[3], up[3], len, pt = steve_partial_tick();
    int i, any = FALSE, k;

    for (i = 0; i < MAX_PARTICLES; i++) if (parts[i].alive) any = TRUE;
    if (!any) return;

    viewport_getPosition_vec3f(cam);
    viewport_getLookVector(look);
    right[0] = -look[2];
    right[1] = 0.0f;
    right[2] = look[0];
    len = __builtin_sqrtf(right[0] * right[0] + right[2] * right[2]);
    if (len < 1e-4f) { right[0] = 1.0f; right[2] = 0.0f; len = 1.0f; }
    right[0] /= len;
    right[2] /= len;
    up[0] = right[1] * look[2] - right[2] * look[1];
    up[1] = right[2] * look[0] - right[0] * look[2];
    up[2] = right[0] * look[1] - right[1] * look[0];
    if (up[1] < 0.0f) for (k = 0; k < 3; k++) up[k] = -up[k];

    pbuf = (pbuf + 1) % PBUFS;
    world_set_texture_state(gfx);
    for (i = 0; i < MAX_PARTICLES; i++) {
        TParticle *p = &parts[i];
        Vtx *v = pvtx[pbuf][i];
        f32 c[3], s;
        int q;

        if (!p->alive) continue;
        s = p->size * MC_BLOCK;
        if (p->kind == PK_SMOKE) {
            // BaseAshSmokeParticle.getQuadSize: grows over the first 1/32 of its life
            f32 g = ((f32)p->age + pt) / p->lifetime * 32.0f;
            s *= g > 1.0f ? 1.0f : g;
        }
        for (k = 0; k < 3; k++) c[k] = p->o_pos[k] + (p->pos[k] - p->o_pos[k]) * pt - cam[k];
        for (q = 0; q < 4; q++) {
            f32 sx = (q == 1 || q == 2) ? 1.0f : -1.0f;
            f32 sy = (q >= 2) ? 1.0f : -1.0f;
            for (k = 0; k < 3; k++) v[q].v.ob[k] = (s16)(c[k] + right[k] * sx * s + up[k] * sy * s);
            v[q].v.flag = 0;
            // getU0/U1: (uo + 1) / 4 .. uo / 4 of the sprite = 4 texels
            if (p->kind == PK_TERRAIN) {
                v[q].v.tc[0] = (s16)(((s32)p->uo * 4 + ((sx > 0) ? 0 : 4)) << 5);
                v[q].v.tc[1] = (s16)(((s32)p->vo * 4 + ((sy > 0) ? 0 : 4)) << 5);
                v[q].v.cn[0] = v[q].v.cn[1] = v[q].v.cn[2] = 153; // rCol 0.6
            } else {
                v[q].v.tc[0] = (s16)((sx > 0 ? 16 : 0) << 5);
                v[q].v.tc[1] = (s16)((sy > 0 ? 0 : 16) << 5);
                v[q].v.cn[0] = v[q].v.cn[1] = v[q].v.cn[2] = p->col;
            }
            v[q].v.cn[3] = 0xFF;
        }
        mlMtxIdent();
        mlMtxApply(*mtx);
        gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_PARTICLE_START + i, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
        gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        {
            // setSpriteFromAge: sprites[age * (count - 1) / lifetime]
            const u16 *tex = &mc_block_tex[p->tex * 256];
            if (p->kind != PK_TERRAIN) {
                int first = p->kind == PK_SMOKE ? MC_PSPRITE_SMOKE_FIRST : p->kind == PK_POOF ? MC_PSPRITE_POOF_FIRST : MC_PSPRITE_EXPLOSION_FIRST;
                int count = p->kind == PK_SMOKE ? MC_PSPRITE_SMOKE_COUNT : p->kind == PK_POOF ? MC_PSPRITE_POOF_COUNT : MC_PSPRITE_EXPLOSION_COUNT;
                tex = &mc_particle_tex[(first + p->age * (count - 1) / p->lifetime) * 256];
            }
        gDPLoadTextureBlock((*gfx)++, tex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0,
            G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
        }
        gSPVertex((*gfx)++, v, 4, 0);
        gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
        gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
        gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
    }
    world_restore_texture_state(gfx);
#endif
}
