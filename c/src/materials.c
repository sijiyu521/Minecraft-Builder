/*
 * materials.c — the block catalogue.
 *
 * This table is the single source of truth for three separate things:
 *   1. display names / colours used by the terminal renderer and exporters,
 *   2. the whitelist that rejects hallucinated block names coming from a model,
 *   3. the catalogue injected into the model prompt.
 *
 * Order matters and must not be shuffled: the local rule engine reports detected
 * materials in table order, and the golden tests assert those orders.
 */
#include "mb.h"

#include <stdlib.h>
#include <string.h>

#define M(shrt, nm, col, cat) \
    { "minecraft:" shrt, nm, col, cat, 0 }
#define ML(shrt, nm, col, cat, lt) \
    { "minecraft:" shrt, nm, col, cat, lt }

const MBMaterial MB_MATERIALS[] = {
    /* --- stone family ---------------------------------------------------- */
    M("stone",                "石头",         0x7d7d7d, MB_CAT_STONE),
    M("cobblestone",          "圆石",         0x757575, MB_CAT_STONE),
    M("stone_bricks",         "石砖",         0x7b7b7b, MB_CAT_STONE),
    M("mossy_stone_bricks",   "苔石砖",       0x6f7d63, MB_CAT_STONE),
    M("deepslate_bricks",     "深板岩砖",     0x4d4d52, MB_CAT_STONE),
    M("polished_andesite",    "磨制安山岩",   0x85878b, MB_CAT_STONE),
    M("smooth_stone",         "平滑石头",     0x9c9c9c, MB_CAT_STONE),
    M("sand",                 "沙子",         0xdbd1a0, MB_CAT_STONE),

    /* --- wood family ----------------------------------------------------- */
    M("oak_log",              "橡木原木",     0x6b532c, MB_CAT_WOOD),
    M("spruce_log",           "云杉原木",     0x4a3520, MB_CAT_WOOD),
    M("stripped_oak_log",     "去皮橡木",     0xb18b52, MB_CAT_WOOD),
    M("oak_planks",           "橡木木板",     0xb58a4f, MB_CAT_WOOD),
    M("spruce_planks",        "云杉木板",     0x6b4f2f, MB_CAT_WOOD),
    M("dark_oak_planks",      "深色橡木木板", 0x3f2d17, MB_CAT_WOOD),
    M("birch_planks",         "白桦木板",     0xd7c99a, MB_CAT_WOOD),
    M("bookshelf",            "书架",         0x957b53, MB_CAT_WOOD),

    /* --- masonry --------------------------------------------------------- */
    M("bricks",               "砖块",         0x96594a, MB_CAT_STONE),
    M("quartz_block",         "石英块",       0xecebe4, MB_CAT_STONE),
    M("smooth_quartz",        "平滑石英",     0xeae8e0, MB_CAT_STONE),
    M("white_concrete",       "白色混凝土",   0xcfd5d6, MB_CAT_STONE),
    M("light_gray_concrete",  "淡灰色混凝土", 0x7d7d73, MB_CAT_STONE),
    M("gray_concrete",        "灰色混凝土",   0x36393d, MB_CAT_STONE),
    M("black_concrete",       "黑色混凝土",   0x0b0b0d, MB_CAT_STONE),

    /* --- glass ----------------------------------------------------------- */
    M("glass",                "玻璃",         0xa8d8e8, MB_CAT_GLASS),
    M("glass_pane",           "玻璃板",       0xb6dfec, MB_CAT_GLASS),
    M("tinted_glass",         "遮光玻璃",     0x3a3a45, MB_CAT_GLASS),

    /* --- nature ---------------------------------------------------------- */
    M("oak_leaves",           "橡树树叶",     0x4a7a2e, MB_CAT_NATURE),
    M("spruce_leaves",        "云杉树叶",     0x2f5a34, MB_CAT_NATURE),
    M("grass_block",          "草方块",       0x6aa84f, MB_CAT_NATURE),
    M("dirt",                 "泥土",         0x79553a, MB_CAT_NATURE),
    M("hay_block",            "干草块",       0xb9a23f, MB_CAT_NATURE),
    M("water",                "水",           0x3a6ea5, MB_CAT_NATURE),

    /* --- wool / decoration ------------------------------------------------ */
    M("white_wool",           "白色羊毛",     0xe9ecec, MB_CAT_WOOL),
    M("red_wool",             "红色羊毛",     0xa02722, MB_CAT_WOOL),
    M("blue_wool",            "蓝色羊毛",     0x35399d, MB_CAT_WOOL),
    M("terracotta",           "陶瓦",         0x985e43, MB_CAT_DECO),
    M("iron_bars",            "铁栏杆",       0x6e6e6e, MB_CAT_DECO),
    M("ladder",               "梯子",         0x8a6a3a, MB_CAT_DECO),
    M("crafting_table",       "工作台",       0x7a4f2a, MB_CAT_DECO),

    /* --- lighting --------------------------------------------------------- */
    ML("glowstone",           "萤石",         0xf9d67a, MB_CAT_LIGHT, 15),
    ML("sea_lantern",         "海晶灯",       0xcfe6e0, MB_CAT_LIGHT, 15),
    ML("lantern",             "灯笼",         0xf5b95e, MB_CAT_LIGHT, 15),
};

const int MB_MATERIAL_COUNT = (int)(sizeof(MB_MATERIALS) / sizeof(MB_MATERIALS[0]));

static const char* short_id(const char* id)
{
    if (!id) return "";
    if (strncmp(id, "minecraft:", 10) == 0) return id + 10;
    return id;
}

const MBMaterial* mb_material(const char* idOrShort)
{
    static const MBMaterial unknown = { "", "", 0xff00ff, MB_CAT_DECO, 0 };
    static MBMaterial fallback;
    const char* key = short_id(idOrShort);

    for (int i = 0; i < MB_MATERIAL_COUNT; i++) {
        if (strcmp(MB_MATERIALS[i].id + 10, key) == 0) return &MB_MATERIALS[i];
    }
    /* Mirror the web app: keep the requested id so exports stay traceable. */
    fallback = unknown;
    fallback.id = idOrShort ? idOrShort : "";
    fallback.name = idOrShort ? idOrShort : "";
    return &fallback;
}

bool mb_material_known(const char* idOrShort)
{
    return mb_material(idOrShort)->color != 0xff00ff;
}

const char* mb_material_name(const char* id)
{
    return mb_material(id)->name;
}

uint32_t mb_material_color(const char* id)
{
    return mb_material(id)->color;
}
