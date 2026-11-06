/*
 * structures.c — the six building templates.
 *
 * A template is a pure function of (width, depth, height, floors, palette).
 * That is the whole reason a language model can be trusted in the loop: the
 * model only ever picks an *id* from this table, and this table cannot produce
 * an unbuildable result.
 *
 * The arithmetic below is frozen and locked by the golden tests, including the
 * things that look odd — the iron-bar railing of the treehouse is declared with
 * zero thickness and therefore never places a block, because no integer (dx,dz)
 * lands exactly on `radius + 0.5`. Such quirks are the behaviour the recorded
 * block counts were taken from, so they must not be "cleaned up".
 */
#include "mb.h"

#include <math.h>
#include <string.h>

/* ========================================================================== */
/* shared helpers                                                             */
/* ========================================================================== */

static void punch_door(MBBuilder* b, const MBBuildOptions* o, int baseY)
{
    int w = o->width, d = o->depth;
    int doorX = w / 2 - 1;
    if (doorX < 1) doorX = 1;

    mb_b_cut_opening(b, 'x', doorX, doorX + 1, baseY, baseY + 2, d - 1, 0);
    /* Lintel light so the entrance reads as an entrance in the preview. */
    mb_b_set(b, doorX, baseY + 3, d - 1, o->palette.light);
    mb_b_set(b, doorX + 1, baseY + 3, d - 1, o->palette.accent);
}

static void roof_lights(MBBuilder* b, const MBBuildOptions* o, int y)
{
    int w = o->width, d = o->depth;
    int cx = w / 2, cz = d / 2;
    mb_b_set(b, cx, y, cz, o->palette.light);
    if (w > 9 && d > 9) {
        mb_b_set(b, 2, y, 2, o->palette.light);
        mb_b_set(b, w - 3, y, 2, o->palette.light);
        mb_b_set(b, 2, y, d - 3, o->palette.light);
        mb_b_set(b, w - 3, y, d - 3, o->palette.light);
    }
}

/* A glazed band with structural mullions, used on every floor of the modern
 * house. The opening is cut out of the wall, filled with glass, then frame
 * columns are stamped on top every other block so the facade reads as windows
 * instead of a glass ribbon. `skipA`/`skipB` leave room for a door. */
static void window_band(MBBuilder* b, const MBBuildOptions* o,
                        char axis, int plane, int from, int to,
                        int y1, int y2, int skipA, int skipB)
{
    if (to - from < 2 || y2 < y1) return;
    mb_b_cut_opening(b, axis, from, to, y1, y2, plane, 0);
    mb_b_glaze(b, axis, from, to, y1, y2, plane, o->palette.glass, 0);

    for (int i = from; i <= to; i++) {
        int px = axis == 'x' ? i : plane;
        int pz = axis == 'x' ? plane : i;
        bool mullion = ((i - from) % 3 == 0) && i != skipA && i != skipB;
        if (mullion) {
            for (int y = y1; y <= y2; y++) mb_b_set(b, px, y, pz, o->palette.frame);
        }
        mb_b_set(b, px, y1 - 1, pz, o->palette.accent);
        mb_b_set(b, px, y2 + 1, pz, o->palette.frame);
    }
}

/* ========================================================================== */
/* 1. modern house                                                            */
/* ========================================================================== */

static void build_modern_house(MBBuilder* b, const MBBuildOptions* o)
{
    int w = o->width, d = o->depth;
    int floors = o->floors > 0 ? o->floors : 1;
    if (floors > 4) floors = 4;

    /* The plan's height is the *total* height of the building, not the height
     * of one storey. Split it into equal storeys 3-6 blocks tall so a 13-block
     * plan with two floors becomes a two-storey house instead of a 27-block
     * tower with pancake slabs. */
    int story = (o->height - 2) / floors;
    if (story < 4) story = 4;
    if (story > 6) story = 6;
    int top = 1 + floors * story;   /* first y above the top storey's ceiling */

    /* Foundation with a one block lip all around. */
    mb_b_rect(b, (MBRect){ -1, -1, w, d }, 0, "minecraft:stone_bricks");
    mb_b_rect(b, (MBRect){ 0, 0, w - 1, d - 1 }, 0, o->palette.floor);

    for (int floor = 0; floor < floors; floor++) {
        int base = 1 + floor * story;

        mb_b_perimeter(b, 0, base, 0, w - 1, base + story - 1, d - 1, o->palette.wall);
        /* Strong horizontal band at every floor line, plus corner columns. */
        mb_b_perimeter(b, 0, base, 0, w - 1, base, d - 1, o->palette.accent);
        mb_b_box(b, 0, base, 0, 0, base + story - 1, 0, o->palette.frame);
        mb_b_box(b, w - 1, base, 0, w - 1, base + story - 1, 0, o->palette.frame);
        mb_b_box(b, 0, base, d - 1, 0, base + story - 1, d - 1, o->palette.frame);
        mb_b_box(b, w - 1, base, d - 1, w - 1, base + story - 1, d - 1, o->palette.frame);

        int wy1 = base + 1;
        int wy2 = base + story - 2;
        if (wy2 < wy1) wy2 = wy1;
        if (w >= 9) {
            window_band(b, o, 'x', d - 1, 2, w - 3, wy1, wy2, -99, -99);
            window_band(b, o, 'x', 0, 2, w - 3, wy1, wy2, -99, -99);
        }
        if (d >= 8) {
            window_band(b, o, 'z', 0, 2, d - 3, wy1, wy2, -99, -99);
            window_band(b, o, 'z', w - 1, 2, d - 3, wy1, wy2, -99, -99);
        }

        /* Ceiling / next floor slab. */
        mb_b_rect(b, (MBRect){ 0, 0, w - 1, d - 1 }, base + story, o->palette.floor);
        /* Interior lighting. */
        mb_b_set(b, w / 2, base + story - 1, d / 2, o->palette.light);
    }

    punch_door(b, o, 1);
    /* Entrance: two steps down to the garden, a slim cantilevered canopy above
     * the door (no posts to clutter the facade) and two planters. */
    {
        int doorX = w / 2 - 1;
        if (doorX < 1) doorX = 1;
        int canopyY = 1 + story;
        if (canopyY < 5) canopyY = 5;
        mb_b_rect(b, (MBRect){ doorX - 1, d, doorX + 2, d + 1 }, 0, o->palette.floor);
        mb_b_rect(b, (MBRect){ doorX - 2, d, doorX + 3, d + 1 }, canopyY, o->palette.roof);
        mb_b_rect(b, (MBRect){ doorX - 2, d + 1, doorX + 3, d + 1 }, canopyY,
                  o->palette.accent);
        mb_b_set(b, doorX - 2, 1, d, o->palette.nature);
        mb_b_set(b, doorX + 3, 1, d, o->palette.nature);
        mb_b_set(b, doorX, canopyY - 1, d, o->palette.light);
    }

    /* Side garage: a lower wing breaks up the box massing and is the first
     * thing that reads as "villa" instead of "apartment block". */
    {
        int gx1 = w, gx2 = w + 4;
        int gz1 = 1, gz2 = d - 1;
        if (gz2 - gz1 >= 4) {
            mb_b_rect(b, (MBRect){ gx1, gz1, gx2, gz2 }, 0, o->palette.floor);
            mb_b_perimeter(b, gx1, 1, gz1, gx2, story, gz2, o->palette.wall);
            mb_b_perimeter(b, gx1, story, gz1, gx2, story, gz2, o->palette.accent);
            mb_b_rect(b, (MBRect){ gx1, gz1, gx2, gz2 }, story + 1, o->palette.roof);
            mb_b_perimeter(b, gx1, story + 1, gz1, gx2, story + 1, gz2, o->palette.accent);
            /* Garage door on the same front facade as the entrance. */
            mb_b_box(b, gx1 + 1, 1, gz2, gx2 - 1, 3, gz2, o->palette.roof);
            mb_b_box(b, gx1 + 1, 4, gz2, gx2 - 1, 4, gz2, o->palette.frame);
            mb_b_box(b, gx1, 1, gz1, gx1, story, gz1, o->palette.frame);
            /* Driveway. */
            mb_b_rect(b, (MBRect){ gx1 + 1, gz2 + 1, gx2 - 1, d + 1 }, 0,
                      "minecraft:smooth_stone");
            /* Connecting door into the house. */
            mb_b_cut_opening(b, 'z', 2, 3, 1, 2, w - 1, 0);
        }
    }

    /* Roof terrace: flat roof with overhang, accent fascia and a parapet. */
    mb_b_rect(b, (MBRect){ -1, -1, w, d }, top, o->palette.roof);
    mb_b_perimeter(b, -1, top, -1, w, top, d, o->palette.accent);
    roof_lights(b, o, top);
    mb_b_perimeter(b, 0, top + 1, 0, w - 1, top + 1, d - 1, o->palette.wall);
    if (w > 9) {
        mb_b_perimeter(b, 0, top + 2, 0, w - 1, top + 2, d - 1, "minecraft:glass_pane");
    }
    mb_b_set(b, 2, top + 1, 2, o->palette.nature);
    mb_b_set(b, w - 3, top + 1, 2, o->palette.nature);
    mb_b_set(b, 2, top + 1, d - 3, o->palette.nature);
    mb_b_set(b, w - 3, top + 1, d - 3, o->palette.nature);

    /* Pergola over one half of the roof terrace. */
    if (w > 10 && d > 9 && floors > 1) {
        int px[4] = { 3, w / 2, 3, w / 2 };
        int pz[4] = { 3, 3, d - 4, d - 4 };
        for (int i = 0; i < 4; i++) {
            mb_b_box(b, px[i], top + 1, pz[i], px[i], top + 2, pz[i], o->palette.frame);
        }
        mb_b_box(b, 3, top + 3, 3, w / 2, top + 3, 3, o->palette.frame);
        mb_b_box(b, 3, top + 3, d - 4, w / 2, top + 3, d - 4, o->palette.frame);
        mb_b_box(b, 3, top + 3, 3, 3, top + 3, d - 4, o->palette.frame);
        mb_b_box(b, w / 2, top + 3, 3, w / 2, top + 3, d - 4, o->palette.frame);
        mb_b_set(b, 3, top + 4, 3, o->palette.nature);
        mb_b_set(b, w / 2, top + 4, d - 4, o->palette.nature);
    }

    /* Vertical circulation: a ladder shaft in one corner, drawn last so it
     * punches through the floor slabs. */
    for (int y = 1; y < top; y++) {
        mb_sketch_remove(&b->sketch, 1, y, 1);
        mb_b_set(b, 1, y, 1, "minecraft:ladder");
    }
}

/* A framed two-block-wide window, used by the cabin and the treehouse. */
static void small_window(MBBuilder* b, const MBBuildOptions* o,
                         char axis, int plane, int at, int y1, int y2)
{
    mb_b_cut_opening(b, axis, at, at + 1, y1, y2, plane, 0);
    mb_b_glaze(b, axis, at, at + 1, y1, y2, plane, o->palette.glass, 0);
    for (int i = at - 1; i <= at + 2; i++) {
        int px = axis == 'x' ? i : plane;
        int pz = axis == 'x' ? plane : i;
        mb_b_set(b, px, y1 - 1, pz, o->palette.accent);
        mb_b_set(b, px, y2 + 1, pz, o->palette.accent);
    }
    for (int y = y1; y <= y2; y++) {
        mb_b_set(b, axis == 'x' ? at - 1 : plane, y, axis == 'x' ? plane : at - 1,
                 o->palette.accent);
        mb_b_set(b, axis == 'x' ? at + 2 : plane, y, axis == 'x' ? plane : at + 2,
                 o->palette.accent);
    }
}

/* ========================================================================== */
/* 2. log cabin                                                               */
/* ========================================================================== */

static void build_cabin(MBBuilder* b, const MBBuildOptions* o)
{
    int w = o->width, d = o->depth;

    /* Tall plans should not turn the cabin into a grain silo: the wall height
     * is capped and the rest of the plan height becomes roof volume. */
    int wall = o->height - 2;
    if (wall < 4) wall = 4;
    if (wall > 7) wall = 7;
    int wallTop = 1 + wall;
    int roofHalf = (d + 1) / 2 + 1;
    int roofTop = wallTop + roofHalf;

    /* Stone footing and plank floor. */
    mb_b_rect(b, (MBRect){ -1, -1, w, d }, 0, "minecraft:cobblestone");
    mb_b_rect(b, (MBRect){ 0, 0, w - 1, d - 1 }, 0, o->palette.floor);
    mb_b_rect(b, (MBRect){ 1, 1, w - 2, d - 2 }, 1, o->palette.floor);

    /* Log walls with a darker base band and corner posts. */
    mb_b_perimeter(b, 0, 1, 0, w - 1, wallTop, d - 1, o->palette.frame);
    mb_b_perimeter(b, 0, 1, 0, w - 1, 1, d - 1, o->palette.accent);
    mb_b_perimeter(b, 0, wallTop, 0, w - 1, wallTop, d - 1, o->palette.accent);
    mb_b_box(b, 0, 1, 0, 0, wallTop + 1, 0, o->palette.accent);
    mb_b_box(b, w - 1, 1, 0, w - 1, wallTop + 1, 0, o->palette.accent);
    mb_b_box(b, 0, 1, d - 1, 0, wallTop + 1, d - 1, o->palette.accent);
    mb_b_box(b, w - 1, 1, d - 1, w - 1, wallTop + 1, d - 1, o->palette.accent);

    /* Windows: two per long wall (when there is room), one per short wall. */
    int doorX = w / 2 - 1;
    if (doorX < 1) doorX = 1;
    int wy = 2;
    int wy2 = wallTop - 1;
    if (wy2 < wy) wy2 = wy;
    if (w >= 9) {
        small_window(b, o, 'x', d - 1, 2, wy, wy2);
        small_window(b, o, 'x', d - 1, w - 4, wy, wy2);
        small_window(b, o, 'x', 0, 2, wy, wy2);
        small_window(b, o, 'x', 0, w - 4, wy, wy2);
    }
    if (d >= 8) {
        small_window(b, o, 'z', 0, 2, wy, wy2);
        small_window(b, o, 'z', w - 1, 2, wy, wy2);
    }

    punch_door(b, o, 1);

    /* Porch: plank deck, two posts, a little roof and lanterns. */
    mb_b_rect(b, (MBRect){ doorX - 2, d, doorX + 3, d + 1 }, 1, o->palette.floor);
    mb_b_box(b, doorX - 2, 1, d + 1, doorX - 2, 4, d + 1, o->palette.frame);
    mb_b_box(b, doorX + 3, 1, d + 1, doorX + 3, 4, d + 1, o->palette.frame);
    mb_b_rect(b, (MBRect){ doorX - 3, d, doorX + 4, d + 2 }, 5, o->palette.roof);
    mb_b_set(b, doorX - 3, 2, d + 1, o->palette.light);
    mb_b_set(b, doorX + 4, 2, d + 1, o->palette.light);
    mb_b_set(b, doorX - 2, 1, d + 1, o->palette.nature);
    mb_b_set(b, doorX + 3, 1, d + 1, o->palette.nature);

    /* Gable roof: flush gable ends, one block of eaves front and back. */
    mb_b_gable_roof(b, 0, -1, w - 1, d, wallTop + 1, o->palette.roof, o->palette.frame);

    /* Chimney on the side, capped with stone. */
    mb_b_box(b, w - 2, 1, 1, w - 2, roofTop + 2, 1, "minecraft:bricks");
    mb_b_box(b, w - 2, roofTop + 3, 1, w - 2, roofTop + 3, 1, "minecraft:stone_bricks");

    /* Interior light. */
    mb_b_set(b, w / 2, wallTop - 1, d / 2, o->palette.light);
    mb_b_set(b, doorX, 2, d - 2, o->palette.light);
}

/* ========================================================================== */
/* 3. castle tower                                                            */
/* ========================================================================== */

static void build_castle_tower(MBBuilder* b, const MBBuildOptions* o)
{
    int w = o->width, d = o->depth;
    int H = o->height;
    if (H < 10) H = 10;
    if (H > 30) H = 30;

    int smaller = w < d ? w : d;
    int radius = smaller / 2 - 1;
    if (radius < 2) radius = 2;
    if (radius > 6) radius = 6;
    double cx = 0.0, cz = 0.0;

    /* Battered base: the footing is wider than the shell and the first two
     * courses step in, so the tower does not read as a plain cylinder. */
    mb_b_cylinder(b, cx, cz, (double)radius + 1.4, 0, 1, "minecraft:cobblestone", 1, true, 0.0);
    mb_b_cylinder(b, cx, cz, (double)radius + 0.7, 2, 2, o->palette.frame, 1, false, 0.0);
    mb_b_cylinder(b, cx, cz, (double)radius, 3, H, o->palette.wall, 1, false, 0.0);

    /* A string course every six blocks adds scale and hides the plain shell. */
    for (int y = 6; y <= H - 3; y += 6) {
        mb_b_cylinder(b, cx, cz, (double)radius, y, y, o->palette.accent, 1, false, 0.0);
    }

    /* Floor rings with lights. */
    for (int y = 5; y <= H - 2; y += 5) {
        mb_b_cylinder(b, cx, cz, (double)radius - 1.0, y, y, o->palette.floor, 1, true, 0.0);
        mb_b_set(b, 0, y + 1, 0, o->palette.light);
    }

    /* Mid-height lookout gallery so a tall shaft has a readable silhouette. */
    if (H >= 14) {
        int gy = H / 2;
        mb_b_cylinder(b, cx, cz, (double)radius + 0.8, gy, gy, o->palette.floor, 1, false, 0.0);
        mb_b_cylinder(b, cx, cz, (double)radius + 0.8, gy + 1, gy + 1,
                      "minecraft:iron_bars", 0, false, 0.0);
        for (int k = 0; k < 8; k++) {
            double angle = ((double)k / 8.0) * 3.14159265358979323846 * 2.0;
            int px = mb_js_round(cx + cos(angle) * (double)(radius + 0.8));
            int pz = mb_js_round(cz + sin(angle) * (double)(radius + 0.8));
            mb_b_set(b, px, gy - 1, pz, o->palette.frame);
        }
    }

    /* Arrow slits in the four cardinal directions, framed in the accent. */
    {
        static const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (int k = 0; k < 4; k++) {
            for (int y = 5; y <= H - 4; y += 5) {
                for (int step = 0; step <= radius + 1; step++) {
                    int x = (int)cx + dirs[k][0] * step;
                    int z = (int)cz + dirs[k][1] * step;
                    if (!mb_sketch_has(&b->sketch, x, y, z)) continue;
                    mb_sketch_set(&b->sketch, x, y, z, o->palette.glass);
                    mb_sketch_set(&b->sketch, x, y + 1, z, o->palette.glass);
                    mb_sketch_set(&b->sketch, x, y - 1, z, o->palette.accent);
                    mb_sketch_set(&b->sketch, x, y + 2, z, o->palette.accent);
                }
            }
        }
    }

    /* Machicolation ring: corbels under the top walkway. */
    mb_b_cylinder(b, cx, cz, (double)radius + 0.9, H - 1, H - 1, o->palette.frame, 1, false, 0.0);
    for (int k = 0; k < 8; k++) {
        double angle = ((double)k / 8.0) * 3.14159265358979323846 * 2.0;
        int px = mb_js_round(cx + cos(angle) * (double)(radius + 1));
        int pz = mb_js_round(cz + sin(angle) * (double)(radius + 1));
        mb_b_set(b, px, H - 2, pz, o->palette.frame);
    }

    /* A wider crown gallery, then battlements and a beacon flag. */
    mb_b_cylinder(b, cx, cz, (double)radius + 1, H - 3, H - 1, o->palette.wall, 1, false, 0.0);
    mb_b_cylinder(b, cx, cz, (double)radius + 1, H - 3, H - 3, o->palette.accent, 1, false, 0.0);
    mb_b_ring_merlons(b, cx, cz, (double)radius + 1, H + 1, H + 2, o->palette.wall);
    mb_b_cylinder(b, cx, cz, (double)radius + 1, H + 1, H + 1, o->palette.floor, 1, false, 0.0);
    mb_b_set(b, 0, H + 2, 0, o->palette.light);
    mb_b_box(b, 0, H + 2, 0, 0, H + 5, 0, o->palette.frame);
    mb_b_box(b, 1, H + 4, 0, 2, H + 5, 0, "minecraft:red_wool");

    /* Entrance: arched opening with stone steps and torches. */
    {
        int zf = (int)cz + radius;
        mb_b_cut_opening(b, 'z', -1, 1, 3, 5, zf, 0);
        mb_b_set(b, -1, 6, zf, o->palette.accent);
        mb_b_set(b, 0, 6, zf, o->palette.accent);
        mb_b_set(b, 1, 6, zf, o->palette.accent);
        mb_b_set(b, -2, 4, zf, o->palette.light);
        mb_b_set(b, 2, 4, zf, o->palette.light);
        mb_b_box(b, -2, 0, zf + 1, 2, 2, zf + 2, "minecraft:stone_bricks");
        mb_b_box(b, -1, 2, zf + 1, 1, 2, zf + 2, "minecraft:cobblestone");
    }

    /* Spiral ladder so the tower is traversable in game. */
    for (int y = 3; y <= H; y++) {
        double angle = ((double)y / (double)H) * 3.14159265358979323846 * 2.0;
        int lx = mb_js_round(cx + cos(angle) * (double)(radius - 1));
        int lz = mb_js_round(cz + sin(angle) * (double)(radius - 1));
        if (!mb_sketch_has(&b->sketch, lx, y, lz)) continue;
        mb_sketch_set(&b->sketch, lx, y, lz, "minecraft:ladder");
    }
}

/* ========================================================================== */
/* 4. treehouse                                                               */
/* ========================================================================== */

static void build_treehouse(MBBuilder* b, const MBBuildOptions* o)
{
    int w = o->width, d = o->depth;
    int smaller = w < d ? w : d;
    int radius = smaller / 2 - 1;
    if (radius < 3) radius = 3;
    if (radius > 7) radius = 7;

    int deckY = o->height - 5;
    if (deckY < 4) deckY = 4;
    if (deckY > 8) deckY = 8;

    /* Trunk with roots. */
    mb_b_cylinder(b, 0, 0, 1.4, 0, deckY + 3, "minecraft:spruce_log", 1, true, 0.0);
    {
        static const int roots[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (int k = 0; k < 4; k++) {
            int rx = roots[k][0], rz = roots[k][1];
            mb_b_box(b, rx, 0, rz, rx * 3, 1, rz * 3, "minecraft:spruce_log");
        }
    }

    /* Platform with a braced underside. */
    mb_b_cylinder(b, 0, 0, (double)radius, deckY, deckY, o->palette.floor, 1, true, 0.0);
    for (int k = 0; k < 4; k++) {
        static const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        int px = dirs[k][0] * (radius - 1);
        int pz = dirs[k][1] * (radius - 1);
        mb_b_box(b, px, deckY - 2, pz, px, deckY - 1, pz, o->palette.frame);
    }

    /* Railing around the deck, with a gap on the +Z side for the ladder. */
    mb_b_cylinder(b, 0, 0, (double)radius, deckY + 1, deckY + 1,
                  "minecraft:iron_bars", 0, false, 0.0);
    mb_sketch_remove(&b->sketch, 1, deckY + 1, 0);

    /* Hut shell with windows on all four sides and a door facing +Z. */
    int shell = radius - 1;
    if (shell < 2) shell = 2;
    mb_b_cylinder(b, 0, 0, (double)shell, deckY + 1, deckY + 3, o->palette.wall, 1, false, 0.0);
    {
        static const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (int k = 0; k < 4; k++) {
            int px = dirs[k][0] * shell;
            int pz = dirs[k][1] * shell;
            mb_b_set(b, px, deckY + 2, pz, o->palette.glass);
            mb_b_set(b, dirs[k][0] * (shell + 1), deckY + 2, dirs[k][1] * (shell + 1),
                     o->palette.accent);
        }
        mb_b_cut_opening(b, 'z', 0, 1, deckY + 1, deckY + 2, shell, 0);
        mb_b_set(b, 0, deckY + 3, shell, o->palette.accent);
        mb_b_set(b, 1, deckY + 3, shell, o->palette.accent);
    }
    mb_b_set(b, 0, deckY + 3, 0, o->palette.light);

    /* Conical roof. */
    {
        int r = radius - 1;
        if (r < 2) r = 2;
        int y = deckY + 4;
        while (r >= 1) {
            mb_b_cylinder(b, 0, 0, (double)r, y, y, o->palette.roof, 1, true, 0.0);
            r -= 1;
            y += 1;
        }
        mb_b_set(b, 0, y, 0, o->palette.light);
    }

    /* Canopy: a fringe under the deck and a crown well above the roof, so the
     * hut itself stays visible instead of being swallowed by leaves. */
    mb_b_ellipsoid(b, 0, deckY - 2, 0, (double)(radius + 2), 1,
                   (double)(radius + 2), o->palette.nature, 0.55, false);
    {
        int roofTop = deckY + 3 + radius;
        mb_b_ellipsoid(b, 0, roofTop + 1, 0, (double)(radius + 1), 2,
                       (double)(radius + 1), o->palette.nature, 0.55, false);
        mb_b_ellipsoid(b, 0, roofTop + 3, 0, (double)radius, 1,
                       (double)radius, o->palette.nature, 0.65, false);
    }

    /* Ladder up the trunk and hanging lanterns under the deck. */
    for (int ly = 1; ly < deckY; ly++) {
        mb_sketch_set(&b->sketch, 1, ly, 0, "minecraft:ladder");
    }
    mb_b_set(b, 0, deckY - 1, radius, o->palette.light);
    mb_b_set(b, radius, deckY - 1, 0, o->palette.light);
    mb_b_set(b, 0, deckY - 1, -radius, o->palette.light);
    mb_b_set(b, -radius, deckY - 1, 0, o->palette.light);
}

/* ========================================================================== */
/* 5. stone bridge                                                            */
/* ========================================================================== */

static void build_bridge(MBBuilder* b, const MBBuildOptions* o)
{
    int w = o->width, d = o->depth;
    int span = w - 1;
    if (span < 6) span = 6;

    int deckY = o->height - 2;
    if (deckY < 3) deckY = 3;
    if (deckY > 10) deckY = 10;

    /* Spandrels: solid above the arch curve, open underneath. The arch springs
     * from both abutments and peaks two blocks below the deck. */
    for (int x = 0; x <= span; x++) {
        double t = (double)x / (double)span;
        int openTop = mb_js_round((double)(deckY - 1) * sin(3.14159265358979323846 * t));
        if (openTop > deckY - 2) openTop = deckY - 2;
        if (openTop > 0) {
            for (int y = openTop; y < deckY; y++) {
                mb_b_box(b, x, y, 0, x, y, d - 1, o->palette.wall);
            }
            /* Voussoir course along the arch. */
            mb_b_box(b, x, openTop - 1, 0, x, openTop - 1, d - 1, o->palette.accent);
        }
    }

    /* Abutments and wing walls. */
    mb_b_box(b, -1, 0, 0, 0, deckY - 1, d - 1, "minecraft:stone_bricks");
    mb_b_box(b, span, 0, 0, span + 1, deckY - 1, d - 1, "minecraft:stone_bricks");

    /* Deck with one block of overhang on each side. */
    mb_b_rect(b, (MBRect){ -1, -1, span + 1, d }, deckY, o->palette.floor);
    mb_b_rect(b, (MBRect){ -1, -1, span + 1, d }, deckY - 1, o->palette.accent);

    /* Railings on both sides: stone posts every three blocks, iron bars in
     * between, and lanterns only at the ends and the middle. */
    for (int side = 0; side < 2; side++) {
        int z = side == 0 ? -1 : d;
        for (int x = -1; x <= span + 1; x++) {
            if (x % 3 == 0 || x == -1 || x == span + 1) {
                mb_b_set(b, x, deckY + 1, z, o->palette.frame);
                mb_b_set(b, x, deckY + 2, z, o->palette.frame);
                mb_b_set(b, x, deckY + 3, z, o->palette.accent);
                if (x == -1 || x == span + 1 || x == span / 2) {
                    mb_b_set(b, x, deckY + 4, z, o->palette.light);
                }
            } else {
                mb_b_set(b, x, deckY + 1, z, "minecraft:iron_bars");
                mb_b_set(b, x, deckY + 2, z, "minecraft:iron_bars");
            }
        }
    }

    /* Keystone lights under the deck ends. */
    mb_b_set(b, span / 2, deckY - 2, 0, o->palette.light);
    mb_b_set(b, span / 2, deckY - 2, d - 1, o->palette.light);
}

/* ========================================================================== */
/* 6. castle wall                                                             */
/* ========================================================================== */

static void build_castle_wall(MBBuilder* b, const MBBuildOptions* o)
{
    int w = o->width, d = o->depth;
    int span = w - 1;
    int h = o->height;
    if (h < 6) h = 6;
    if (h > 16) h = 16;

    int thickness = d;
    if (thickness < 2) thickness = 2;
    if (thickness > 5) thickness = 5;
    int zEnd = thickness - 1;

    /* Wall body, battered footing and a string course at walkway level. */
    mb_b_box(b, 0, 0, 0, span, h - 1, zEnd, o->palette.wall);
    mb_b_box(b, -1, 0, -1, span + 1, 1, zEnd + 1, "minecraft:cobblestone");
    mb_b_box(b, 0, h - 1, 0, span, h - 1, zEnd, o->palette.accent);

    /* Walkway between the two crenellated faces. */
    mb_b_box(b, 0, h, 1, span, h, zEnd - 1, o->palette.floor);
    mb_b_crenellation(b, 0, 0, span, zEnd, h + 1, o->palette.wall);

    /* Arrow slits on the outer face. */
    for (int x = 3; x <= span - 3; x += 5) {
        mb_b_set(b, x, h - 4, 0, o->palette.glass);
        mb_b_set(b, x, h - 3, 0, o->palette.glass);
        mb_b_set(b, x, h - 5, 0, o->palette.accent);
        mb_b_set(b, x, h - 2, 0, o->palette.accent);
    }

    /* Gatehouse: arched gateway flanked by two towers, now with a proper
     * bridge room above the gate. */
    {
        int gx = span / 2;
        int gh = h - 2 < 5 ? h - 2 : 5;   /* gate height */

        for (int x = gx - 1; x <= gx + 1; x++) {
            mb_b_cut_opening(b, 'z', 0, zEnd, 0, gh, x, 0);
        }
        mb_b_box(b, gx - 1, gh + 1, 0, gx + 1, gh + 2, zEnd, o->palette.accent);
        /* Open portcullis bars above the passage. */
        for (int x = gx - 1; x <= gx + 1; x++) {
            mb_b_set(b, x, gh, 0, "minecraft:iron_bars");
            mb_b_set(b, x, gh, zEnd, "minecraft:iron_bars");
        }
        /* Stepped approach outside the gate. */
        mb_b_box(b, gx - 2, 0, zEnd + 1, gx + 2, 1, zEnd + 2, "minecraft:stone_bricks");

        /* Flanking towers sit clear of the gate opening. */
        for (int side = 0; side < 2; side++) {
            int tx = side == 0 ? gx - 3 : gx + 3;
            int tz1 = -1, tz2 = zEnd + 1;
            mb_b_box(b, tx - 1, 0, tz1, tx + 1, h + 3, tz2, o->palette.wall);
            mb_b_box(b, tx - 1, 0, tz1, tx + 1, 1, tz2, "minecraft:cobblestone");
            mb_b_box(b, tx - 1, h + 1, tz1, tx + 1, h + 1, tz2, o->palette.accent);
            mb_b_crenellation(b, tx - 1, tz1, tx + 1, tz2, h + 4, o->palette.wall);
            mb_b_set(b, tx, h + 1, (tz1 + tz2) / 2, o->palette.light);
            mb_b_set(b, tx, gh, zEnd, o->palette.light);
        }

        /* Bridge room over the gate. */
        mb_b_box(b, gx - 3, h + 1, -1, gx + 3, h + 2, zEnd + 1, o->palette.wall);
        mb_b_crenellation(b, gx - 3, -1, gx + 3, zEnd + 1, h + 3, o->palette.wall);
        mb_b_box(b, gx - 1, h + 2, zEnd + 1, gx + 1, h + 2, zEnd + 1, o->palette.accent);
    }

    /* Corner towers with machicolation and battlements. */
    {
        int xs[2] = { 0, span };
        int r = thickness / 2 + 2;
        if (r < 3) r = 3;
        for (int k = 0; k < 2; k++) {
            int x = xs[k];
            mb_b_cylinder(b, (double)x, 1, (double)r, 0, h + 4, o->palette.wall, 1, false, 0.0);
            mb_b_cylinder(b, (double)x, 1, (double)r, 0, 1, "minecraft:cobblestone", 1, true, 0.0);
            mb_b_cylinder(b, (double)x, 1, (double)r + 0.7, h + 2, h + 2,
                          o->palette.accent, 1, false, 0.0);
            mb_b_cylinder(b, (double)x, 1, (double)r, h + 3, h + 3, o->palette.floor, 1, false, 0.0);
            mb_b_ring_merlons(b, (double)x, 1, (double)r, h + 5, h + 5, o->palette.wall);
            mb_b_set(b, x, h + 3, 1, o->palette.light);
        }
    }

    /* Walkway torches. */
    for (int x = 4; x < span - 2; x += 7) {
        mb_b_set(b, x, h + 1, zEnd, o->palette.light);
    }
}

/* ========================================================================== */
/* table                                                                      */
/* ========================================================================== */

static const char* kw_modern_house[] = { "现代", "别墅", "平顶", "简约", "modern",
                                         "villa", "house", "房子", "住宅", "公寓" };
static const char* kw_cabin[] = { "小屋", "木屋", "乡村", "农舍", "cabin",
                                  "cottage", "农夫", "乡村风" };
static const char* kw_castle_tower[] = { "塔楼", "城堡", "塔", "要塞", "哨塔",
                                         "tower", "castle", "fort", "碉堡" };
static const char* kw_treehouse[] = { "树屋", "森林", "丛林", "精灵", "treehouse",
                                      "tree", "树", "原始" };
static const char* kw_bridge[] = { "桥", "拱桥", "跨越", "bridge", "跨海", "栈桥" };
static const char* kw_castle_wall[] = { "城墙", "围墙", "防御", "防务", "wall",
                                        "栅栏", "围栏" };

#define NELEM(x) ((int)(sizeof(x) / sizeof((x)[0])))

const MBStructure MB_STRUCTURES[] = {
    {
        "modern-house", "现代别墅", "平顶、大落地窗、混凝土与石英的简洁体块",
        kw_modern_house, NELEM(kw_modern_house), 15, 11, 5, 2, build_modern_house,
    },
    {
        "cabin", "原木小屋", "原木墙、山墙屋顶、带门廊的小木屋",
        kw_cabin, NELEM(kw_cabin), 11, 9, 4, 1, build_cabin,
    },
    {
        "castle-tower", "城堡塔楼", "圆形石砖塔、分层箭窗、顶部雉堞",
        kw_castle_tower, NELEM(kw_castle_tower), 11, 11, 20, 0, build_castle_tower,
    },
    {
        "treehouse", "树屋", "树干支撑、圆形平台与环绕树叶",
        kw_treehouse, NELEM(kw_treehouse), 13, 13, 6, 1, build_treehouse,
    },
    {
        "bridge", "石桥", "跨水拱桥，桥面护栏与桥墩",
        kw_bridge, NELEM(kw_bridge), 21, 5, 0, 0, build_bridge,
    },
    {
        "castle-wall", "城墙", "带走道与雉堞的直墙，两端配角楼",
        kw_castle_wall, NELEM(kw_castle_wall), 25, 3, 7, 0, build_castle_wall,
    },
};

const int MB_STRUCTURE_COUNT = NELEM(MB_STRUCTURES);

const char* const MB_DEFAULT_STRUCTURE_ID = "modern-house";

const MBStructure* mb_structure_by_id(const char* id)
{
    if (!id) return NULL;
    for (int i = 0; i < MB_STRUCTURE_COUNT; i++) {
        if (strcmp(MB_STRUCTURES[i].id, id) == 0) return &MB_STRUCTURES[i];
    }
    return NULL;
}
