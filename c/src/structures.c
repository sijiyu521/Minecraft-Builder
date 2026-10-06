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

static void punch_windows(MBBuilder* b, const MBBuildOptions* o, int baseY, int wallHeight)
{
    int w = o->width, d = o->depth;
    if (wallHeight < 4 || w < 7 || d < 7) return;

    int sill = baseY + 1;
    int head = baseY + (wallHeight - 2 < 2 ? wallHeight - 2 : 2);
    if (head < sill) return;

    for (int x = 2; x <= w - 3; x += 3) {
        int x2 = x + 1 < w - 3 ? x + 1 : w - 3;
        mb_b_cut_opening(b, 'x', x, x2, sill, head, 0, 0);
        mb_b_glaze(b, 'x', x, x2, sill, head, 0, o->palette.glass, 0);
        mb_b_cut_opening(b, 'x', x, x2, sill, head, d - 1, 0);
        mb_b_glaze(b, 'x', x, x2, sill, head, d - 1, o->palette.glass, 0);
    }

    for (int z = 2; z <= d - 3; z += 3) {
        int z2 = z + 1 < d - 3 ? z + 1 : d - 3;
        mb_b_cut_opening(b, 'z', z, z2, sill, head, 0, 0);
        mb_b_glaze(b, 'z', z, z2, sill, head, 0, o->palette.glass, 0);
        mb_b_cut_opening(b, 'z', z, z2, sill, head, w - 1, 0);
        mb_b_glaze(b, 'z', z, z2, sill, head, w - 1, o->palette.glass, 0);
    }
}

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

/* ========================================================================== */
/* 1. modern house                                                            */
/* ========================================================================== */

static void build_modern_house(MBBuilder* b, const MBBuildOptions* o)
{
    int w = o->width, d = o->depth, h = o->height, floors = o->floors;

    /* Foundation with a one block lip all around. */
    mb_b_rect(b, (MBRect){ -1, -1, w, d }, 0, "minecraft:stone_bricks");
    mb_b_rect(b, (MBRect){ 0, 0, w - 1, d - 1 }, 0, o->palette.floor);

    for (int floor = 0; floor < floors; floor++) {
        int base = 1 + floor * (h + 1);
        mb_b_perimeter(b, 0, base, 0, w - 1, base + h - 1, d - 1, o->palette.wall);
        /* Strong horizontal band between floors. */
        mb_b_perimeter(b, 0, base, 0, w - 1, base, d - 1, o->palette.accent);

        punch_windows(b, o, base, h);

        /* Ceiling / next floor slab. */
        mb_b_rect(b, (MBRect){ 0, 0, w - 1, d - 1 }, base + h, o->palette.floor);

        /* Interior lighting. */
        mb_b_set(b, w / 2, base + h - 1, d / 2, o->palette.light);
        if (w > 11) {
            mb_b_set(b, 2, base + h - 1, 2, o->palette.light);
            mb_b_set(b, w - 3, base + h - 1, d - 3, o->palette.light);
        }
    }

    punch_door(b, o, 1);

    /* Vertical circulation: a ladder shaft in one corner. Drawing it last means
     * it punches through the slabs naturally. */
    int top = 1 + floors * (h + 1);
    for (int y = 1; y < top; y++) {
        mb_sketch_remove(&b->sketch, 1, y, 1);
        mb_b_set(b, 1, y, 1, "minecraft:ladder");
    }

    /* Flat roof with overhang, plus a dark fascia band. */
    mb_b_rect(b, (MBRect){ -1, -1, w, d }, top, o->palette.roof);
    mb_b_perimeter(b, -1, top, -1, w, top, d, o->palette.accent);
    roof_lights(b, o, top);

    /* Roof terrace railing on the top floor. */
    if (floors > 1 && w > 9) {
        mb_b_perimeter(b, -1, top + 1, -1, w, top + 1, d, "minecraft:glass_pane");
    }
}

/* ========================================================================== */
/* 2. log cabin                                                               */
/* ========================================================================== */

static void build_cabin(MBBuilder* b, const MBBuildOptions* o)
{
    int w = o->width, d = o->depth, h = o->height;

    mb_b_rect(b, (MBRect){ 0, 0, w - 1, d - 1 }, 0, "minecraft:cobblestone");
    mb_b_perimeter(b, 0, 1, 0, w - 1, h, d - 1, o->palette.frame);
    mb_b_perimeter(b, 0, 1, 0, w - 1, h, d - 1, o->palette.frame);

    /* Corner posts. */
    mb_b_box(b, 0, 1, 0, 0, h + 1, 0, o->palette.accent);
    mb_b_box(b, w - 1, 1, 0, w - 1, h + 1, 0, o->palette.accent);
    mb_b_box(b, 0, 1, d - 1, 0, h + 1, d - 1, o->palette.accent);
    mb_b_box(b, w - 1, 1, d - 1, w - 1, h + 1, d - 1, o->palette.accent);

    /* Windows: small squares instead of the wide modern glazing. */
    if (w > 8) {
        int xs[2] = { 2, w - 3 };
        for (int i = 0; i < 2; i++) {
            int x = xs[i];
            mb_b_cut_opening(b, 'x', x, x, 2, 3, d - 1, 0);
            mb_b_glaze(b, 'x', x, x, 2, 3, d - 1, o->palette.glass, 0);
            mb_b_cut_opening(b, 'x', x, x, 2, 3, 0, 0);
            mb_b_glaze(b, 'x', x, x, 2, 3, 0, o->palette.glass, 0);
        }
    }
    if (d > 8) {
        int zs[2] = { 2, d - 3 };
        for (int i = 0; i < 2; i++) {
            int z = zs[i];
            mb_b_cut_opening(b, 'z', z, z, 2, 3, 0, 0);
            mb_b_glaze(b, 'z', z, z, 2, 3, 0, o->palette.glass, 0);
            mb_b_cut_opening(b, 'z', z, z, 2, 3, w - 1, 0);
            mb_b_glaze(b, 'z', z, z, 2, 3, w - 1, o->palette.glass, 0);
        }
    }

    punch_door(b, o, 1);

    /* Gable roof with a one block overhang. */
    mb_b_gable_roof(b, -1, -1, w, d, h + 1, o->palette.roof, o->palette.frame);

    /* Floor inside. */
    mb_b_rect(b, (MBRect){ 1, 1, w - 2, d - 2 }, 1, o->palette.floor);

    /* Porch in front of the door. */
    int doorX = w / 2 - 1;
    if (doorX < 1) doorX = 1;
    mb_b_rect(b, (MBRect){ doorX - 2, d, doorX + 3, d + 1 }, 1, o->palette.floor);
    mb_b_box(b, doorX - 2, 2, d + 1, doorX - 2, 3, d + 1, o->palette.frame);
    mb_b_box(b, doorX + 3, 2, d + 1, doorX + 3, 3, d + 1, o->palette.frame);
    mb_b_rect(b, (MBRect){ doorX - 2, d, doorX + 3, d + 1 }, 4, o->palette.roof);

    /* Interior lights on the ground floor. */
    mb_b_set(b, w / 2, h, d / 2, o->palette.light);
    mb_b_set(b, doorX, 2, d - 2, o->palette.light);

    /* Chimney. */
    mb_b_box(b, w - 2, 1, 1, w - 2, h + 3, 1, "minecraft:bricks");
}

/* ========================================================================== */
/* 3. castle tower                                                            */
/* ========================================================================== */

static void build_castle_tower(MBBuilder* b, const MBBuildOptions* o)
{
    int w = o->width, d = o->depth, H = o->height;
    int smaller = w < d ? w : d;
    int radius = smaller / 2 - 1;
    if (radius < 2) radius = 2;
    double cx = 0.0, cz = 0.0;

    /* Solid stone base. */
    mb_b_cylinder(b, cx, cz, (double)radius, 0, 1, "minecraft:stone_bricks", 1, true, 0.0);
    mb_b_cylinder(b, cx, cz, (double)radius + 0.6, 0, 0, "minecraft:cobblestone", 1, true, 0.0);

    /* Shell. */
    mb_b_cylinder(b, cx, cz, (double)radius, 2, H, o->palette.wall, 1, false, 0.0);

    /* Floor ring every 5 blocks. */
    for (int y = 2 + 5; y <= H; y += 5) {
        mb_b_cylinder(b, cx, cz, (double)radius - 1.0, y, y, o->palette.floor, 1, true, 0.0);
        mb_b_set(b, 0, y + 1, 0, o->palette.light);
    }

    /* Arrow slits in the four cardinal directions. */
    {
        static const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (int k = 0; k < 4; k++) {
            for (int y = 4; y <= H - 3; y += 4) {
                for (int step = 0; step <= radius + 1; step++) {
                    int x = (int)cx + dirs[k][0] * step;
                    int z = (int)cz + dirs[k][1] * step;
                    if (!mb_sketch_has(&b->sketch, x, y, z)) continue;
                    mb_sketch_set(&b->sketch, x, y, z, o->palette.glass);
                    mb_sketch_set(&b->sketch, x, y + 1, z, o->palette.glass);
                }
            }
        }
    }

    /* Battlements. */
    mb_b_ring_merlons(b, cx, cz, (double)radius, H + 1, H + 2, o->palette.wall);
    mb_b_cylinder(b, cx, cz, (double)radius, H + 1, H + 1, o->palette.floor, 1, false, 0.0);
    mb_b_set(b, 0, H + 1, 0, o->palette.light);

    /* Entrance on the +Z side. */
    mb_b_cut_opening(b, 'z', -1, 1, 2, 4, (int)cz + radius, 0);
    mb_b_set(b, 0, 5, (int)cz + radius, o->palette.accent);

    /* Spiral ladder hint so the tower is traversable in game. */
    for (int y = 2; y <= H; y++) {
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
    int w = o->width, d = o->depth, h = o->height;
    int smaller = w < d ? w : d;
    int radius = smaller / 2 - 1;
    if (radius < 3) radius = 3;
    int deckY = h;

    /* Trunk. */
    mb_b_cylinder(b, 0, 0, 1, 0, deckY, "minecraft:spruce_log", 1, true, 0.0);
    mb_b_cylinder(b, 0, 0, 2, 0, 1, "minecraft:spruce_log", 1, true, 0.0);

    /* Platform. */
    mb_b_cylinder(b, 0, 0, (double)radius, deckY, deckY, o->palette.floor, 1, true, 0.0);

    /* Cabin shell on the platform. */
    int shell = radius - 1;
    if (shell < 2) shell = 2;
    mb_b_cylinder(b, 0, 0, (double)shell, deckY + 1, deckY + 3, o->palette.frame, 1, false, 0.0);

    /* Windows around the shell. */
    for (int i = 0; i < 8; i++) {
        double angle = ((double)i / 8.0) * 3.14159265358979323846 * 2.0;
        int x = mb_js_round(cos(angle) * (double)(radius - 1));
        int z = mb_js_round(sin(angle) * (double)(radius - 1));
        mb_sketch_set(&b->sketch, x, deckY + 2, z, o->palette.glass);
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
    }

    /* Canopy pushed out from under the cabin. */
    mb_b_ellipsoid(b, 0, deckY - 2, 0, (double)(radius + 2), 2, (double)(radius + 2),
                   o->palette.nature, 0.45, false);
    mb_b_ellipsoid(b, 0, deckY + 5, 0, (double)radius, 2, (double)radius,
                   o->palette.nature, 0.5, false);

    /* Ladder up the trunk. */
    for (int ly = 1; ly < deckY; ly++) {
        mb_sketch_set(&b->sketch, 1, ly, 0, "minecraft:ladder");
    }

    /* Railing, leaving one gap for the ladder. */
    mb_b_cylinder(b, 0, 0, (double)radius, deckY + 1, deckY + 1,
                  "minecraft:iron_bars", 0, false, 0.0);
    mb_sketch_remove(&b->sketch, 1, deckY + 1, 0);
}

/* ========================================================================== */
/* 5. stone bridge                                                            */
/* ========================================================================== */

static void build_bridge(MBBuilder* b, const MBBuildOptions* o)
{
    int w = o->width, d = o->depth;
    int span = w - 1;
    int deckY = (int)ceil((double)span / 4.0);
    if (deckY < 3) deckY = 3;

    /* Deck. */
    mb_b_rect(b, (MBRect){ 0, 0, span, d - 1 }, deckY, o->palette.floor);

    /* Arch profile: a parabola-ish curve under the deck. */
    for (int x = 0; x <= span; x++) {
        double t = (double)x / (double)span;
        int arc = mb_js_round(sin(3.14159265358979323846 * t) * (double)deckY);
        for (int y = deckY - 1; y >= deckY - 1 - arc + 1; y--) {
            if (y < 0) break;
            int base = deckY - 1 - arc;
            int diff = y - base;
            if (diff < 0) diff = -diff;
            bool fill = (diff <= 1) || (arc <= 1);
            if (!fill && y > 0) continue;
            mb_b_box(b, x, y, 0, x, y, d - 1, o->palette.wall);
        }
    }

    /* Piers down to the ground at both ends and the middle. */
    {
        int piers[3] = { 0, span / 2, span };
        for (int k = 0; k < 3; k++) {
            int x = piers[k];
            for (int px = x - 1; px <= x + 1; px++) {
                if (px < 0 || px > span) continue;
                for (int pz = 0; pz < d; pz++) {
                    for (int y = 0; y < deckY; y++) {
                        mb_sketch_set(&b->sketch, px, y, pz, "minecraft:stone_bricks");
                    }
                }
            }
        }
    }

    /* Railings. */
    {
        int zs[2] = { 0, d - 1 };
        for (int k = 0; k < 2; k++) {
            int z = zs[k];
            for (int x = 0; x <= span; x++) {
                if (x % 3 == 0) {
                    mb_b_set(b, x, deckY + 1, z, o->palette.frame);
                    mb_b_set(b, x, deckY + 2, z, o->palette.frame);
                }
                mb_b_set(b, x, deckY + 1, z,
                         (x % 3 == 0) ? o->palette.frame : "minecraft:iron_bars");
            }
            for (int x = 0; x <= span; x++) {
                mb_b_set(b, x, deckY + 2, z, "minecraft:iron_bars");
            }
        }
    }

    /* Lanterns on the piers. */
    {
        int xs[2] = { 1, span - 1 };
        int zs[2] = { 0, d - 1 };
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                mb_b_set(b, xs[i], deckY + 3, zs[j], o->palette.light);
            }
        }
    }

    /* Under-deck lighting for the archway. */
    {
        int midX = span / 2;
        mb_b_set(b, midX, deckY - 2, 0, o->palette.light);
        mb_b_set(b, midX, deckY - 2, d - 1, o->palette.light);
    }
}

/* ========================================================================== */
/* 6. castle wall                                                             */
/* ========================================================================== */

static void build_castle_wall(MBBuilder* b, const MBBuildOptions* o)
{
    int w = o->width, d = o->depth, h = o->height;
    int span = w - 1;
    int thickness = d < 3 ? d : 3;
    if (thickness < 1) thickness = 1;
    int zEnd = thickness - 1;

    /* Wall body with a battered (slightly thicker) footing. */
    mb_b_box(b, 0, 0, 0, span, h - 1, zEnd, o->palette.wall);
    mb_b_box(b, 0, 0, -1, span, 1, zEnd + 1, "minecraft:cobblestone");

    /* Walkway on top. */
    {
        int zTop = zEnd - 1;
        if (zTop < 1) zTop = 1;
        mb_b_rect(b, (MBRect){ 1, 1, span - 1, zTop }, h, o->palette.floor);
    }

    /* Outer battlements (both long faces). */
    mb_b_crenellation(b, 0, 0, span, zEnd, h + 1, o->palette.wall);

    /* Corner towers. */
    {
        int xs[2] = { 0, span };
        int r = d / 2 + 1;
        if (r < 2) r = 2;
        for (int k = 0; k < 2; k++) {
            int x = xs[k];
            mb_b_cylinder(b, (double)x, 1, (double)r, 0, h + 2, o->palette.wall, 1, false, 0.0);
            mb_b_cylinder(b, (double)x, 1, (double)r, 0, 1, "minecraft:stone_bricks", 1, true, 0.0);
            mb_b_cylinder(b, (double)x, 1, (double)r, h + 1, h + 2, o->palette.floor, 1, false, 0.0);
            mb_b_ring_merlons(b, (double)x, 1, (double)r, h + 3, h + 3, o->palette.wall);
            mb_b_set(b, x, h + 1, 1, o->palette.light);
        }
    }

    /* Wall-mounted torches along the walkway. */
    for (int x = 3; x < span - 2; x += 5) {
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
