// minecraft gui drawing, see gui.h

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "bk_externs.h"
#include "mc_gbi.h"
#include "gui.h"
#include "items.h"
#include "mouse.h"
#include "world.h"
#include "shield.h"
#include "pose.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

GuiMetrics gGui;

static u32 white_tex[16] __attribute__((aligned(8))) = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
};

// Window.calculateScale(0, false)
void gui_update_metrics(void) {
    s32 w = gMouse.window_w, h = gMouse.window_h, s = 1;

    if (w <= 0 || h <= 0) {
        w = 1920; // no mouse mod: assume 1080p
        h = 1080;
    }
    while (s < w && s < h && w / (s + 1) >= 320 && h / (s + 1) >= 240) {
        s++;
    }
    gGui.scale = s;
    gGui.width = (w + s - 1) / s;
    gGui.height = (h + s - 1) / s;
    // the n64 framebuffer is 240 units tall over the whole window height
    gGui.unit = (f32)s * (f32)gFramebufferHeight / (f32)h;
}

// the gui is drawn as textured triangles, not n64 rectangles: rectangles can
// only be placed in quarter n64 pixels and rt64 draws them at n64 resolution,
// which can't hit minecraft's exact pixel grid (wobbly lines, broken letters).
// triangles render at full resolution with float precision. the projection
// is an orthographic camera over the whole window in gui pixels, tagged
// "stretch" so rt64 doesn't aspect-correct it for widescreen.

#define GUI_MAX_VTX 4096
#define GUI_BUFFERS 3
static Vtx gui_vtx[GUI_BUFFERS][GUI_MAX_VTX];
static int gui_buf, gui_count;

// bk's counters (notes, jiggies...) move themselves to the screen edges with
// rt64's viewport/rect align, and that sticks around for whatever is drawn
// next, which shifted the hotbar sideways. (bk's own viewport is left alone:
// it isn't the same in every map.)
static Vp gui_vp;

void gui_begin(Gfx **gfx, Mtx **mtx) {
    gui_buf = (gui_buf + 1) % GUI_BUFFERS;
    gui_count = 0;

    gDPPipeSync((*gfx)++);
    gEXSetViewportAlign((*gfx)++, G_EX_ORIGIN_NONE, 0, 0);
    gEXSetRectAlign((*gfx)++, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 0, 0);
    // bk's note/jiggy counters leave a small viewport of their own (the
    // spinning 3d note) active. our own full-framebuffer one, the same one
    // bk's viewport_pushFramebufferExtendsToVpStack makes, explicitly not
    // aligned to any screen edge. bk's state comes back in gui_end.
    gui_vp.vp.vscale[0] = gui_vp.vp.vtrans[0] = (gFramebufferWidth / 2) << 2;
    gui_vp.vp.vscale[1] = gui_vp.vp.vtrans[1] = (gFramebufferHeight / 2) << 2;
    gui_vp.vp.vscale[2] = gui_vp.vp.vtrans[2] = 0x1FF;
    gui_vp.vp.vscale[3] = gui_vp.vp.vtrans[3] = 0;
    gEXPushViewport((*gfx)++);
    gEXViewport((*gfx)++, G_EX_ORIGIN_NONE, &gui_vp);
    // bk's scissor box is the original 4:3 screen: open it to the whole window
    gEXSetScissor((*gfx)++, G_SC_NON_INTERLACE, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, gFramebufferWidth, gFramebufferHeight);

    // projection: (0,0) top left .. (width, height) bottom right in gui pixels.
    // rt64 maps a projection onto the 4:3 area in the middle of the window
    // (window height * 4/3 wide), so the x range covers just that much of the
    // gui: half width * (4:3 / window aspect) around the center. "stretch"
    // keeps rt64 from changing it further. works at any window size/aspect.
    // vertices are in quarter gui pixels, so the modelview scales by 1/4.
    {
        f32 win_w = (gMouse.window_w > 0) ? (f32)gMouse.window_w : 1920.0f;
        f32 win_h = (gMouse.window_h > 0) ? (f32)gMouse.window_h : 1080.0f;
        f32 k = (4.0f / 3.0f) / (win_w / win_h);
        f32 half = gGui.width * 0.5f;

        guOrtho(*mtx, half - half * k, half + half * k, (f32)gGui.height, 0.0f, -100.0f, 100.0f, 1.0f);
    }
    gEXMatrixGroup((*gfx)++, MC_ID_GUI_PROJECTION, G_EX_INTERPOLATE_SIMPLE, G_EX_NOPUSH, G_MTX_PROJECTION,
        G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
        G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_STRETCH,
        G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP);
    gSPMatrix((*gfx)++, OS_PHYSICAL_TO_K0((*mtx)++), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    guScale(*mtx, 0.25f, 0.25f, 1.0f);
    gEXMatrixGroupSkipAll((*gfx)++, MC_ID_GUI_MODELVIEW, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    gSPMatrix((*gfx)++, OS_PHYSICAL_TO_K0((*mtx)++), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

    gui_restore_state(gfx);
}

// the rdp state gui drawing expects (after something else changed it)
void gui_restore_state(Gfx **gfx) {
    gDPPipeSync((*gfx)++);
    gDPSetCycleType((*gfx)++, G_CYC_1CYCLE);
    gSPClearGeometryMode((*gfx)++, G_ZBUFFER | G_LIGHTING | G_CULL_BOTH | G_FOG | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
    gSPSetGeometryMode((*gfx)++, G_SHADE | G_SHADING_SMOOTH);
    gSPTexture((*gfx)++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gDPSetTexturePersp((*gfx)++, G_TP_PERSP);
    gDPSetTextureLOD((*gfx)++, G_TL_TILE);
    gDPSetTextureLUT((*gfx)++, G_TT_NONE);
    gDPSetTextureDetail((*gfx)++, G_TD_CLAMP);
    gDPSetTextureConvert((*gfx)++, G_TC_FILT);
    gDPSetTextureFilter((*gfx)++, G_TF_POINT);
    gDPSetCombineKey((*gfx)++, G_CK_NONE);
    gDPSetAlphaCompare((*gfx)++, G_AC_NONE);
    gDPSetRenderMode((*gfx)++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetPrimColor((*gfx)++, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF);
    gDPSetCombineMode((*gfx)++, G_CC_MODULATERGBA_PRIM, G_CC_MODULATERGBA_PRIM);
}

void gui_end(Gfx **gfx) {
    gDPPipeSync((*gfx)++);
    gEXPopViewport((*gfx)++);
    gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
    gDPSetPrimColor((*gfx)++, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF);
    gSPTexture((*gfx)++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetTextureFilter((*gfx)++, G_TF_BILERP);
    gSPSetGeometryMode((*gfx)++, G_ZBUFFER);
    gDPSetScissor((*gfx)++, G_SC_NON_INTERLACE, 0, 0, gFramebufferWidth, gFramebufferHeight);
}

// one textured quad: screen rect (x0,y0)-(x1,y1) in gui px, texels (s0,t0)-(s1,t1)
static void gui_quad(Gfx **gfx, f32 x0, f32 y0, f32 x1, f32 y1, s32 s0, s32 t0, s32 s1, s32 t1) {
    Vtx *v;
    int i;

    if (gui_count + 4 > GUI_MAX_VTX) {
        return;
    }
    v = &gui_vtx[gui_buf][gui_count];
    gui_count += 4;
    for (i = 0; i < 4; i++) {
        f32 x = (i == 1 || i == 2) ? x1 : x0;
        f32 y = (i >= 2) ? y1 : y0;
        v[i].v.ob[0] = (s16)(x * 4.0f + (x >= 0 ? 0.5f : -0.5f));
        v[i].v.ob[1] = (s16)(y * 4.0f + (y >= 0 ? 0.5f : -0.5f));
        v[i].v.ob[2] = 0;
        v[i].v.flag = 0;
        v[i].v.tc[0] = (s16)(((i == 1 || i == 2) ? s1 : s0) << 5);
        v[i].v.tc[1] = (s16)(((i >= 2) ? t1 : t0) << 5);
        v[i].v.cn[0] = v[i].v.cn[1] = v[i].v.cn[2] = v[i].v.cn[3] = 0xFF;
    }
    gSPVertex((*gfx)++, v, 4, 0);
    gSP2Triangles((*gfx)++, 0, 2, 1, 0, 0, 3, 2, 0);
}

void gui_blit(Gfx **gfx, const u32 *tex, s32 tex_w, s32 tex_h, s32 sx, s32 sy, s32 sw, s32 sh,
    f32 x, f32 y, f32 w, f32 h) {
    // tmem fits 1024 32-bit texels per load: go in column strips
    s32 strip = 1024 / sh;
    f32 xs = w / (f32)sw; // gui px per texel
    s32 col;

    if (strip > sw) strip = sw;
    if (strip > 4) strip &= ~3;

    for (col = 0; col < sw; col += strip) {
        s32 cw = (sw - col < strip) ? (sw - col) : strip;
        s32 uls = sx + col;

        gDPLoadTextureTile((*gfx)++, tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, tex_w, tex_h,
            uls, sy, uls + cw - 1, sy + sh - 1, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
            G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        gui_quad(gfx, x + col * xs, y, x + (col + cw) * xs, y + h, uls, sy, uls + cw, sy + sh);
    }
}

// GuiGraphics.blitSprite with "nine_slice" scaling: corners stay 1:1, the
// edges and the middle stretch
void gui_blit_nine_slice(Gfx **gfx, const u32 *tex, s32 tw, s32 th, s32 border, f32 x, f32 y, f32 w, f32 h) {
    s32 sxs[3] = { 0, border, tw - border }, sws[3] = { border, tw - 2 * border, border };
    s32 sys[3] = { 0, border, th - border }, shs[3] = { border, th - 2 * border, border };
    f32 dxs[3] = { x, x + border, x + w - border }, dws[3] = { border, w - 2 * border, border };
    f32 dys[3] = { y, y + border, y + h - border }, dhs[3] = { border, h - 2 * border, border };
    int i, j;

    for (j = 0; j < 3; j++) {
        for (i = 0; i < 3; i++) {
            if (dws[i] > 0 && dhs[j] > 0) {
                gui_blit(gfx, tex, tw, th, sxs[i], sys[j], sws[i], shs[j], dxs[i], dys[j], dws[i], dhs[j]);
            }
        }
    }
}

// a solid color over the whole window, edge to edge (screen dimming). gui
// triangles are clipped to the middle 4:3 area, so this uses rt64's fill
// rectangle anchored to the left and right window edges.
void gui_fill_screen(Gfx **gfx, u32 argb) {
    gDPPipeSync((*gfx)++);
    gDPSetPrimColor((*gfx)++, 0, 0, (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, argb >> 24);
    gDPSetCombineMode((*gfx)++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gEXFillRectangle((*gfx)++, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, gFramebufferWidth, gFramebufferHeight);
    gDPPipeSync((*gfx)++);
    gDPSetPrimColor((*gfx)++, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF);
    gDPSetCombineMode((*gfx)++, G_CC_MODULATERGBA_PRIM, G_CC_MODULATERGBA_PRIM);
}

void gui_fill(Gfx **gfx, f32 x, f32 y, f32 w, f32 h, u32 argb) {
    gDPPipeSync((*gfx)++);
    gDPSetPrimColor((*gfx)++, 0, 0, (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, argb >> 24);
    gui_blit(gfx, white_tex, 4, 4, 0, 0, 4, 4, x, y, w, h);
    gDPPipeSync((*gfx)++);
    gDPSetPrimColor((*gfx)++, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF);
}

// a solid line (x0,y0)-(x1,y1), width gui px wide: the quad around it
void gui_line(Gfx **gfx, f32 x0, f32 y0, f32 x1, f32 y1, f32 width, u32 argb) {
    f32 dx = x1 - x0, dy = y1 - y0, len = __builtin_sqrtf(dx * dx + dy * dy), nx, ny;
    f32 px[4], py[4];
    Vtx *v;
    int i;

    if (len < 0.001f || gui_count + 4 > GUI_MAX_VTX) {
        return;
    }
    nx = -dy / len * width * 0.5f;
    ny = dx / len * width * 0.5f;
    px[0] = x0 + nx, py[0] = y0 + ny;
    px[1] = x1 + nx, py[1] = y1 + ny;
    px[2] = x1 - nx, py[2] = y1 - ny;
    px[3] = x0 - nx, py[3] = y0 - ny;
    gDPPipeSync((*gfx)++);
    gDPSetPrimColor((*gfx)++, 0, 0, (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, argb >> 24);
    gDPLoadTextureTile((*gfx)++, white_tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 4, 4, 0, 0, 3, 3, 0,
        G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    v = &gui_vtx[gui_buf][gui_count];
    gui_count += 4;
    for (i = 0; i < 4; i++) {
        v[i].v.ob[0] = (s16)(px[i] * 4.0f + (px[i] >= 0 ? 0.5f : -0.5f));
        v[i].v.ob[1] = (s16)(py[i] * 4.0f + (py[i] >= 0 ? 0.5f : -0.5f));
        v[i].v.ob[2] = 0;
        v[i].v.flag = 0;
        v[i].v.tc[0] = v[i].v.tc[1] = 2 << 5;
        v[i].v.cn[0] = v[i].v.cn[1] = v[i].v.cn[2] = v[i].v.cn[3] = 0xFF;
    }
    gSPVertex((*gfx)++, v, 4, 0);
    gSP2Triangles((*gfx)++, 0, 2, 1, 0, 0, 3, 2, 0); // no culling in the gui pass
    gDPPipeSync((*gfx)++);
    gDPSetPrimColor((*gfx)++, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF);
}

s32 gui_text_width(const char *s) {
#ifdef MC_HAVE_TEXTURES
    s32 w = 0;
    for (; *s; s++) {
        w += mc_font_width[(u8)*s] + 1;
    }
    return w;
#else
    return 0;
#endif
}

static u8 text_alpha = 0xFF;

static void text_pass(Gfx **gfx, const char *s, f32 x, f32 y, u32 rgb) {
#ifdef MC_HAVE_TEXTURES
    gDPPipeSync((*gfx)++);
    gDPSetPrimColor((*gfx)++, 0, 0, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, text_alpha);
    for (; *s; s++) {
        u8 c = (u8)*s;
        if (c != ' ') {
            gui_blit(gfx, mc_font, 128, 128, (c % 16) * 8, (c / 16) * 8, 8, 8, x, y, 8, 8);
        }
        x += mc_font_width[c] + 1;
    }
#endif
}

// GuiGraphics.text with a fading alpha (the held item name)
s32 gui_text_alpha(Gfx **gfx, const char *s, f32 x, f32 y, u32 rgb, int shadow, u8 alpha) {
    s32 w;
    text_alpha = alpha;
    w = gui_text(gfx, s, x, y, rgb, shadow);
    text_alpha = 0xFF;
    return w;
}

s32 gui_text(Gfx **gfx, const char *s, f32 x, f32 y, u32 rgb, int shadow) {
    if (shadow) {
        // Font: shadow color = color * 0.25, one pixel down and right
        text_pass(gfx, s, x + 1, y + 1, (rgb & 0xFCFCFC) >> 2);
    }
    text_pass(gfx, s, x, y, rgb);
    gDPPipeSync((*gfx)++);
    gDPSetPrimColor((*gfx)++, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF);
    return gui_text_width(s);
}

// a block in a slot, drawn as a 3d cube like ItemRenderer does: the block
// model's "gui" display (rotation 30 225 0, scale 0.625) in a 16x16 slot,
// with the gui "side" light (top full, left face 0.8, right face 0.6)
static void gui_block_icon(Gfx **gfx, BlockId b, f32 x, f32 y) {
    // faces: origin corner, u axis, v axis on the unit cube (v runs down the texture)
    static const s8 faces[6][3][3] = {
        { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },   // +y
        { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 0, -1 } },  // -y
        { { 0, 1, 1 }, { 1, 0, 0 }, { 0, -1, 0 } },  // +z
        { { 1, 1, 0 }, { -1, 0, 0 }, { 0, -1, 0 } }, // -z
        { { 1, 1, 1 }, { 0, 0, -1 }, { 0, -1, 0 } }, // +x
        { { 0, 1, 0 }, { 0, 0, 1 }, { 0, -1, 0 } },  // -x
    };
    static const s8 normals[6][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 } };
    // rotY(225) then rotX(30)
    const f32 cy = -0.70710678f, sy = -0.70710678f, cx = 0.8660254f, sx = 0.5f;
    int f, i;

    for (f = 0; f < 6; f++) {
        f32 p[4][3], sxy[4][2], n[3], shade_f;
        Vtx *v;
        u8 shade;

        // is the face looking at the screen
        n[0] = normals[f][0] * cy + normals[f][2] * sy;
        n[2] = -normals[f][0] * sy + normals[f][2] * cy;
        n[1] = normals[f][1];
        if (n[1] * sx + n[2] * cx <= 0.0f) {
            continue;
        }
        for (i = 0; i < 3; i++) {
            p[0][i] = faces[f][0][i];
            p[1][i] = faces[f][0][i] + faces[f][1][i];
            p[2][i] = faces[f][0][i] + faces[f][1][i] + faces[f][2][i];
            p[3][i] = faces[f][0][i] + faces[f][2][i];
        }
        for (i = 0; i < 4; i++) {
            f32 vx = (p[i][0] - 0.5f) * 0.625f, vy = (p[i][1] - 0.5f) * 0.625f, vz = (p[i][2] - 0.5f) * 0.625f;
            f32 rx = vx * cy + vz * sy, rz = -vx * sy + vz * cy, ry = vy;
            f32 ry2 = ry * cx - rz * sx;
            sxy[i][0] = x + 8.0f + rx * 16.0f;
            sxy[i][1] = y + 8.0f - ry2 * 16.0f;
        }
        if (f == 0) {
            shade_f = 1.0f;
        } else {
            f32 mid = (sxy[0][0] + sxy[2][0]) * 0.5f;
            shade_f = (mid < x + 8.0f) ? 0.8f : 0.6f;
        }
        shade = (u8)(shade_f * 255.0f);
        if (gui_count + 4 > GUI_MAX_VTX) {
            return;
        }
        v = &gui_vtx[gui_buf][gui_count];
        gui_count += 4;
        for (i = 0; i < 4; i++) {
            v[i].v.ob[0] = (s16)(sxy[i][0] * 4.0f + 0.5f);
            v[i].v.ob[1] = (s16)(sxy[i][1] * 4.0f + 0.5f);
            v[i].v.ob[2] = 0;
            v[i].v.flag = 0;
            v[i].v.tc[0] = (s16)(((i == 1 || i == 2) ? 16 : 0) << 5);
            v[i].v.tc[1] = (s16)(((i >= 2) ? 16 : 0) << 5);
            v[i].v.cn[0] = v[i].v.cn[1] = v[i].v.cn[2] = shade;
            v[i].v.cn[3] = 0xFF;
        }
        gDPLoadTextureBlock((*gfx)++, &mc_block_tex[mc_block(b)->tex[f] * 256], G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0,
            G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
        gSPVertex((*gfx)++, v, 4, 0);
        gSP2Triangles((*gfx)++, 0, 2, 1, 0, 0, 3, 2, 0);
    }
}

#ifdef MC_HAVE_MODELS
// a model block's 3d item model (stairs, slabs, fences, buttons...) through
// its display.gui (ItemTransform: rotationXYZ, scale) in a 16x16 slot, the
// quads facing the screen drawn farthest first (no depth buffer here), lit
// like the cube (top 1, left 0.8, right 0.6)
static void gui_model_icon(Gfx **gfx, BlockId b, f32 x, f32 y) {
    const McModelState *im = &mc_block_item_models[b];
    const McItemGui *g = &mc_block_item_gui[b];
    f32 cxr = ml_cos_deg(g->rot[0]), sxr = ml_sin_deg(g->rot[0]);
    f32 cyr = ml_cos_deg(g->rot[1]), syr = ml_sin_deg(g->rot[1]);
    f32 czr = ml_cos_deg(g->rot[2]), szr = ml_sin_deg(g->rot[2]);
    f32 pts[48][4][3], depth[48];
    u8 shade[48];
    int order[48], n = 0, q, c, i, j;

    for (q = 0; q < im->count && q < 48; q++) {
        const McQuad *mq = &mc_model_quads[im->first + q];
        f32 e1[3], e2[3], nz, ny, nx, dz = 0.0f;
        for (c = 0; c < 4; c++) {
            f32 vx = (mq->v[c][0] / 256.0f - 0.5f) * g->scale, vy = (mq->v[c][1] / 256.0f - 0.5f) * g->scale;
            f32 vz = (mq->v[c][2] / 256.0f - 0.5f) * g->scale, t;
            // Rz, then Ry, then Rx (rotationXYZ: Rx * Ry * Rz)
            t = vx * czr - vy * szr; vy = vx * szr + vy * czr; vx = t;
            t = vx * cyr + vz * syr; vz = -vx * syr + vz * cyr; vx = t;
            t = vy * cxr - vz * sxr; vz = vy * sxr + vz * cxr; vy = t;
            pts[n][c][0] = vx;
            pts[n][c][1] = vy;
            pts[n][c][2] = vz;
            dz += vz * 0.25f;
        }
        for (i = 0; i < 3; i++) {
            e1[i] = pts[n][1][i] - pts[n][0][i];
            e2[i] = pts[n][2][i] - pts[n][0][i];
        }
        nx = e1[1] * e2[2] - e1[2] * e2[1];
        ny = e1[2] * e2[0] - e1[0] * e2[2];
        nz = e1[0] * e2[1] - e1[1] * e2[0];
        if (nz <= 0.0f) continue; // facing away
        {
            f32 l = __builtin_sqrtf(nx * nx + ny * ny + nz * nz);
            if (l > 0.0f && ny / l > 0.7f) shade[n] = 255;
            else shade[n] = (nx < 0.0f) ? 204 : 153;
        }
        depth[n] = dz;
        order[n] = q;
        n++;
    }
    // farthest (smallest z) first
    {
        int idx[48], k;
        for (i = 0; i < n; i++) idx[i] = i;
        for (i = 1; i < n; i++) {
            for (k = i; k > 0 && depth[idx[k - 1]] > depth[idx[k]]; k--) {
                int t = idx[k]; idx[k] = idx[k - 1]; idx[k - 1] = t;
            }
        }
        for (k = 0; k < n; k++) {
            const McQuad *mq = &mc_model_quads[im->first + order[idx[k]]];
            int e = idx[k];
            Vtx *v;
            if (gui_count + 4 > GUI_MAX_VTX) return;
            v = &gui_vtx[gui_buf][gui_count];
            gui_count += 4;
            for (c = 0; c < 4; c++) {
                v[c].v.ob[0] = (s16)((x + 8.0f + pts[e][c][0] * 16.0f) * 4.0f + 0.5f);
                v[c].v.ob[1] = (s16)((y + 8.0f - pts[e][c][1] * 16.0f) * 4.0f + 0.5f);
                v[c].v.ob[2] = 0;
                v[c].v.flag = 0;
                v[c].v.tc[0] = mq->uv[c][0];
                v[c].v.tc[1] = mq->uv[c][1];
                v[c].v.cn[0] = v[c].v.cn[1] = v[c].v.cn[2] = shade[e];
                v[c].v.cn[3] = 0xFF;
            }
            gDPLoadTextureBlock((*gfx)++, &mc_block_tex[mq->tex * 256], G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0,
                G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
            gSPVertex((*gfx)++, v, 4, 0);
            gSP2Triangles((*gfx)++, 0, 2, 1, 0, 0, 3, 2, 0);
        }
    }
}
#endif
// the chest item: its special model is ChestModel (createSingleBodyLayer:
// bottom, lid, lock), each box's faces UV mapped like ModelPart.Cube /
// Polygon, through the item/template_chest "gui" display (rotation 30 45 0,
// scale 0.625). texture entity/chest/normal.png (64x64).
void gui_chest_icon(Gfx **gfx, f32 x, f32 y) {
    // texOffs u, v; box min x, y, z; size w, h, d; part offset x, y, z
    static const s8 boxes[3][11] = {
        { 0, 19, 1, 0, 1, 14, 10, 14, 0, 0, 0 },   // bottom
        { 0, 0, 1, 0, 0, 14, 5, 14, 0, 9, 1 },     // lid (closed)
        { 0, 0, 7, -2, 14, 2, 4, 1, 0, 9, 1 },     // lock
    };
    // rotY(45) then rotX(30)
    const f32 cy = 0.70710678f, sy = 0.70710678f, cx = 0.8660254f, sx = 0.5f;
    int b, f, i;

    for (b = 0; b < 3; b++) {
        const s8 *bx = boxes[b];
        f32 x0 = bx[2] + bx[8], y0 = bx[3] + bx[9], z0 = bx[4] + bx[10];
        f32 x1 = x0 + bx[5], y1 = y0 + bx[6], z1 = z0 + bx[7];
        f32 w = bx[5], h = bx[6], d = bx[7];
        f32 u0 = bx[0], u1 = u0 + d, u2 = u0 + d + w, u22 = u0 + d + w + w, u3 = u0 + d + w + d, u4 = u3 + w;
        f32 v0 = bx[1], v1 = v0 + d, v2 = v0 + d + h;
        // t0..t3 at minZ, l0..l3 at maxZ (ModelPart.Cube)
        f32 c[8][3] = {
            { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 },
            { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 },
        };
        // DOWN, UP, WEST, NORTH, EAST, SOUTH: corners and (u0, v0, u1, v1)
        const u8 quads[6][4] = { { 5, 4, 0, 1 }, { 2, 3, 7, 6 }, { 0, 4, 7, 3 }, { 1, 0, 3, 2 }, { 5, 1, 2, 6 }, { 4, 5, 6, 7 } };
        const f32 uvs[6][4] = {
            { u1, v0, u2, v1 }, { u2, v1, u22, v0 }, { u0, v1, u1, v2 },
            { u1, v1, u2, v2 }, { u2, v1, u3, v2 }, { u3, v1, u4, v2 },
        };
        static const s8 normals[6][3] = { { 0, -1, 0 }, { 0, 1, 0 }, { -1, 0, 0 }, { 0, 0, -1 }, { 1, 0, 0 }, { 0, 0, 1 } };

        for (f = 0; f < 6; f++) {
            f32 nx = normals[f][0], ny = normals[f][1], nz = normals[f][2];
            f32 rnz = -nx * sy + nz * cy;
            f32 sxy[4][2], tex[4][2], umin, umax, vmin, vmax, shade_f;
            Vtx *v;
            u8 shade;

            if (ny * sx + rnz * cx <= 0.0f) continue; // facing away
            // Polygon: vertex 0 (u1, v0), 1 (u0, v0), 2 (u0, v1), 3 (u1, v1)
            tex[0][0] = uvs[f][2]; tex[0][1] = uvs[f][1];
            tex[1][0] = uvs[f][0]; tex[1][1] = uvs[f][1];
            tex[2][0] = uvs[f][0]; tex[2][1] = uvs[f][3];
            tex[3][0] = uvs[f][2]; tex[3][1] = uvs[f][3];
            for (i = 0; i < 4; i++) {
                const f32 *p = c[quads[f][i]];
                f32 vx = (p[0] / 16.0f - 0.5f) * 0.625f, vy = (p[1] / 16.0f - 0.5f) * 0.625f, vz = (p[2] / 16.0f - 0.5f) * 0.625f;
                f32 rx = vx * cy + vz * sy, rz = -vx * sy + vz * cy;
                f32 ry2 = vy * cx - rz * sx;
                sxy[i][0] = x + 8.0f + rx * 16.0f;
                sxy[i][1] = y + 8.0f - ry2 * 16.0f;
            }
            if (f == 1) {
                shade_f = 1.0f;
            } else {
                f32 rnx = nx * cy + nz * sy;
                shade_f = (rnx < 0.0f) ? 0.8f : 0.6f;
            }
            shade = (u8)(shade_f * 255.0f);
            umin = umax = tex[0][0];
            vmin = vmax = tex[0][1];
            for (i = 1; i < 4; i++) {
                if (tex[i][0] < umin) umin = tex[i][0];
                if (tex[i][0] > umax) umax = tex[i][0];
                if (tex[i][1] < vmin) vmin = tex[i][1];
                if (tex[i][1] > vmax) vmax = tex[i][1];
            }
            if (gui_count + 4 > GUI_MAX_VTX || umax <= umin || vmax <= vmin) continue;
            v = &gui_vtx[gui_buf][gui_count];
            gui_count += 4;
            for (i = 0; i < 4; i++) {
                v[i].v.ob[0] = (s16)(sxy[i][0] * 4.0f + 0.5f);
                v[i].v.ob[1] = (s16)(sxy[i][1] * 4.0f + 0.5f);
                v[i].v.ob[2] = 0;
                v[i].v.flag = 0;
                v[i].v.tc[0] = (s16)(tex[i][0] * 32.0f);
                v[i].v.tc[1] = (s16)(tex[i][1] * 32.0f);
                v[i].v.cn[0] = v[i].v.cn[1] = v[i].v.cn[2] = shade;
                v[i].v.cn[3] = 0xFF;
            }
            // only this face's part of the 64x64 texture (tmem holds 1024 texels)
            gDPLoadTextureTile((*gfx)++, mc_chest_tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 64, 64,
                (s32)umin, (s32)vmin, (s32)umax - 1, (s32)vmax - 1, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
                G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            gSPVertex((*gfx)++, v, 4, 0);
            gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
        }
    }
}

// the shield in a slot: its model (shield.c) through the "gui" display
// transform, an orthographic view: item space x right, y up, 16 gui px per
// unit, centered on the slot. the handle first (it's behind the plate)
void gui_shield_icon(Gfx **gfx, f32 x, f32 y) {
#ifdef MC_HAVE_TEXTURES
    Pose p;
    int bi, f, i;

    pose_identity(&p);
    shield_display(&p, SHIELD_GUI, FALSE);
    for (bi = 1; bi >= 0; bi--) {
        const ShieldBox *b = &shield_boxes[bi];
        f32 bm[3] = { b->min[0] + b->size[0] / 2, b->min[1] + b->size[1] / 2, b->min[2] + b->size[2] / 2 }, mid[3];
        pose_apply(&p, bm, mid);
        for (f = 0; f < 6; f++) {
            f32 c4[4][3], tex[4][2], pts[4][3], qc[3] = { 0, 0, 0 }, umin, umax, vmin, vmax;
            Vtx *v;
            u8 shade;

            shield_box_face(b, f, c4, tex);
            for (i = 0; i < 4; i++) {
                int k;
                pose_apply(&p, c4[i], pts[i]);
                for (k = 0; k < 3; k++) qc[k] += pts[i][k] / 4.0f;
            }
            if (qc[2] - mid[2] <= 0.0f) continue; // facing away from the screen
            umin = umax = tex[0][0];
            vmin = vmax = tex[0][1];
            for (i = 1; i < 4; i++) {
                if (tex[i][0] < umin) umin = tex[i][0];
                if (tex[i][0] > umax) umax = tex[i][0];
                if (tex[i][1] < vmin) vmin = tex[i][1];
                if (tex[i][1] > vmax) vmax = tex[i][1];
            }
            if (gui_count + 4 > GUI_MAX_VTX || umax <= umin || vmax <= vmin) continue;
            shade = shield_face_shade(f);
            v = &gui_vtx[gui_buf][gui_count];
            gui_count += 4;
            for (i = 0; i < 4; i++) {
                f32 sx = x + 8.0f + pts[i][0] * 16.0f, sy = y + 8.0f - pts[i][1] * 16.0f;
                v[i].v.ob[0] = (s16)(sx * 4.0f + 0.5f);
                v[i].v.ob[1] = (s16)(sy * 4.0f + 0.5f);
                v[i].v.ob[2] = 0;
                v[i].v.flag = 0;
                v[i].v.tc[0] = (s16)(tex[i][0] * 32.0f);
                v[i].v.tc[1] = (s16)(tex[i][1] * 32.0f);
                v[i].v.cn[0] = v[i].v.cn[1] = v[i].v.cn[2] = shade;
                v[i].v.cn[3] = 0xFF;
            }
            gDPLoadTextureTile((*gfx)++, mc_shield_tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 64, 64,
                (s32)umin, (s32)vmin, (s32)umax - 1, (s32)vmax - 1, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
                G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            gSPVertex((*gfx)++, v, 4, 0);
            gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
        }
    }
#endif
}

void gui_item(Gfx **gfx, const ItemStack *st, f32 x, f32 y, int decorations) {
    if (st->count == 0 || st->id == 0) {
        return;
    }
#ifdef MC_HAVE_TEXTURES
    if (st->id == ITEM_SHIELD) {
        gui_shield_icon(gfx, x, y); // ShieldModel through item/shield's gui transform
    } else if (st->id == BLOCK_CHEST) {
        gui_chest_icon(gfx, x, y); // the chest item's special model (ChestModel)
    } else if (item_is_block(st->id)) {
#ifdef MC_HAVE_MODELS
        if (mc_block_item_models[st->id].count > 0) gui_model_icon(gfx, st->id, x, y);
        else
#endif
        gui_block_icon(gfx, st->id, x, y);
    } else if (item_sprite(st->id) != NULL) {
        gui_blit(gfx, item_sprite(st->id), 16, 16, 0, 0, 16, 16, x, y, 16, 16);
    }
#endif
    if (!decorations) {
        return;
    }
    // durability bar (ItemStack.isBarVisible / getBarWidth / getBarColor)
    if (item_max_damage(st->id) > 0 && st->damage > 0) {
        s32 max = item_max_damage(st->id);
        f32 frac = (f32)(max - st->damage) / (f32)max;
        s32 bw = (s32)(13.0f - (f32)st->damage * 13.0f / (f32)max + 0.5f);
        // Mth.hsvToRgb(f / 3, 1, 1): red -> yellow -> green
        f32 hue = frac / 3.0f * 6.0f;
        u32 r, g;
        if (hue < 1.0f) { r = 255; g = (u32)(hue * 255.0f); }
        else { r = (u32)((2.0f - hue) * 255.0f); g = 255; }
        gui_fill(gfx, x + 2, y + 13, 13, 2, 0xFF000000);
        gui_fill(gfx, x + 2, y + 13, bw, 1, 0xFF000000 | (r << 16) | (g << 8));
    }
    if (st->count > 1) {
        static char buf[16][4];
        static int bi;
        char *b = buf[bi = (bi + 1) % 16];
        int n = st->count;

        if (n >= 10) { b[0] = '0' + n / 10; b[1] = '0' + n % 10; b[2] = 0; }
        else { b[0] = '0' + n; b[1] = 0; }
        gui_text(gfx, b, x + 19 - 2 - gui_text_width(b), y + 6 + 3, 0xFFFFFF, TRUE);
    }
}
