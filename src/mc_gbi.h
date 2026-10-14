// rt64 transform tagging for the mod's own drawing.
//
// banjo: recompiled runs the game at 30 updates a second and lets rt64 draw
// the frames in between by interpolating matrices. it can only do that for
// matrices tagged with a stable id (a "matrix group"), the same id every
// frame, so it can pair this frame's transform with last frame's. untagged
// geometry only moves 30 times a second while everything else is smooth,
// which looks like lag. the recomp tags all of bk's own stuff (see
// BanjoRecomp patches/*_transform_tagging.c); these are the same macros.
//
// our cpu-built meshes (steve, the hand, items) change their vertices, not a
// matrix, so they use the "verts" variant: rt64 also interpolates vertex
// positions between frames when the vertex count stays the same.

#ifndef __MC_GBI_H__
#define __MC_GBI_H__

#include "rt64_extended_gbi.h"

#define gEXMatrixGroupSkipAll(cmd, id, push, proj, edit) \
    gEXMatrixGroup(cmd, id, G_EX_INTERPOLATE_SIMPLE, push, proj, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_ORDER_LINEAR, edit, G_EX_ASPECT_AUTO, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP)

#define gEXMatrixGroupSimpleNormal(cmd, id, push, proj, edit) \
    gEXMatrixGroup(cmd, id, G_EX_INTERPOLATE_SIMPLE, push, proj, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR, edit, G_EX_ASPECT_AUTO, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO)

#define gEXMatrixGroupSimpleVerts(cmd, id, push, proj, edit) \
    gEXMatrixGroup(cmd, id, G_EX_INTERPOLATE_SIMPLE, push, proj, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_ORDER_LINEAR, edit, G_EX_ASPECT_AUTO, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_AUTO)

// rt64 fill rectangle with edge anchoring (from the recomp's patches.h)
#ifndef gEXFillRectangle
#define gEXFillRectangle(cmd, lorigin, rorigin, ulx, uly, lrx, lry) \
    G_EX_COMMAND2(cmd, \
        PARAM(RT64_EXTENDED_OPCODE, 8, 24) | PARAM(G_EX_FILLRECT_V1, 24, 0), \
        PARAM(lorigin, 12, 0) | PARAM(rorigin, 12, 12), \
        \
        PARAM((ulx) * 4, 16, 16) | PARAM((uly) * 4, 16, 0), \
        PARAM((lrx) * 4, 16, 16) | PARAM((lry) * 4, 16, 0) \
    )
#endif

// our ids, in a range the recomp doesn't use (see its transform_ids.h)
#define MC_ID_SECTION_START   0x40000000 // + section index
#define MC_ID_STEVE_START     0x40100000 // + part
#define MC_ID_ITEM_START      0x40200000 // + item slot
#define MC_ID_CRACK           0x40200100
#define MC_ID_HAND_PROJECTION 0x40300000
#define MC_ID_HAND            0x40300001
#define MC_ID_HUD_PROJECTION  0x40300010
#define MC_ID_HUD_ICON_START  0x40300020 // + slot
#define MC_ID_GUI_PROJECTION  0x40300030
#define MC_ID_GUI_MODELVIEW   0x40300031
#define MC_ID_TERRAIN         0x40400000 // + 0..1
#define MC_ID_OUTLINE         0x40400010
#define MC_ID_PARTICLE_START  0x40500000 // + particle
#define MC_ID_ARROW_START     0x40600000 // + arrow
#define MC_ID_XP_ORB_START    0x40700000 // + orb id
#define MC_ID_TNT_START       0x40800000 // + primed tnt
#define MC_ID_FLUIDS          0x40900000
#define MC_ID_CHEST_START     0x40A00000 // + drawn chest
#define MC_ID_BOAT_START      0x40B00000 // + boat
#define MC_ID_ARMOR_START     0x40C00000 // + armor box

#endif
