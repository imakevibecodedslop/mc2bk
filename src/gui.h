#ifndef __MC_GUI_H__
#define __MC_GUI_H__

#include <ultra64.h>
#include "survival.h"

// minecraft's gui space: gui pixels at the gui scale minecraft would pick for
// this window (Window.calculateScale, "auto"), origin top left like
// GuiGraphics. drawn with rt64's extended rectangles anchored to the screen
// center, so they keep square pixels in widescreen (plain n64 rectangles get
// stretched).

typedef struct {
    s32 scale;     // minecraft gui scale (4 at 1080p)
    s32 width;     // gui size in gui pixels
    s32 height;
    f32 unit;      // n64 framebuffer units per gui pixel
} GuiMetrics;

extern GuiMetrics gGui;

void gui_update_metrics(void);
void gui_begin(Gfx **gfx, Mtx **mtx);
void gui_end(Gfx **gfx);
void gui_restore_state(Gfx **gfx);
void gui_blit_nine_slice(Gfx **gfx, const u32 *tex, s32 tw, s32 th, s32 border, f32 x, f32 y, f32 w, f32 h);

// draw (sx, sy, sw, sh) of a 32 bit texture at gui (x, y), w x h gui pixels
void gui_blit(Gfx **gfx, const u32 *tex, s32 tex_w, s32 tex_h, s32 sx, s32 sy, s32 sw, s32 sh,
    f32 x, f32 y, f32 w, f32 h);
// solid color (0xAARRGGBB, like GuiGraphics.fill)
void gui_fill(Gfx **gfx, f32 x, f32 y, f32 w, f32 h, u32 argb);
void gui_fill_screen(Gfx **gfx, u32 argb);
void gui_line(Gfx **gfx, f32 x0, f32 y0, f32 x1, f32 y1, f32 width, u32 argb); // width in gui px // whole window, edge to edge
// Font.drawString. returns the width
s32 gui_text(Gfx **gfx, const char *s, f32 x, f32 y, u32 rgb, int shadow);
s32 gui_text_width(const char *s);
s32 gui_text_alpha(Gfx **gfx, const char *s, f32 x, f32 y, u32 rgb, int shadow, u8 alpha);
// GuiGraphics.renderItem + renderItemDecorations (count, durability bar)
void gui_item(Gfx **gfx, const ItemStack *st, f32 x, f32 y, int decorations);

// the chest item's 3d model in a 16x16 slot (the survival inventory tab icon)
void gui_chest_icon(Gfx **gfx, f32 x, f32 y);
void gui_shield_icon(Gfx **gfx, f32 x, f32 y); // the shield's model

#endif
