/*
 * materials.c — the block catalogue.
 *
 * This table is the single source of truth for four separate things:
 *   1. display names / one-line descriptions / colours used by the UI,
 *      the renderer and the exporters,
 *   2. the whitelist that rejects hallucinated block names coming from a model,
 *   3. the catalogue injected into the model prompt,
 *   4. the block/PNG filename the optional vanilla texture loader looks for.
 *
 * Order matters and must not be shuffled: the model prompt, the exporters and
 * the tests all read the catalogue in this order.
 */
#include "mb.h"

#include <stdlib.h>
#include <string.h>

#define M(shrt, nm, ds, col, cat) \
    { "minecraft:" shrt, nm, ds, col, cat, 0 }
#define ML(shrt, nm, ds, col, cat, lt) \
    { "minecraft:" shrt, nm, ds, col, cat, lt }

const MBMaterial MB_MATERIALS[] = {
    /* --- stone family ---------------------------------------------------- */
    M("stone",                "石头",         "灰色基础石材，适合地基、粗砌墙体和体块垫层。", 0x7d7d7d, MB_CAT_STONE),
    M("cobblestone",          "圆石",         "粗糙圆石，做地基、护坡和乡村/废墟墙面的粗粝质感。", 0x757575, MB_CAT_STONE),
    M("stone_bricks",         "石砖",         "规整石砖，城堡墙体和柱子的主材，轮廓清晰。", 0x7b7b7b, MB_CAT_STONE),
    M("mossy_stone_bricks",   "苔石砖",       "带苔藓的石砖，做废墟、古堡和潮湿墙面。", 0x6f7d63, MB_CAT_STONE),
    M("deepslate_bricks",     "深板岩砖",     "深色冷调砖，现代/工业风的暗部与边框。", 0x4d4d52, MB_CAT_STONE),
    M("polished_andesite",    "磨制安山岩",   "中性灰磨光石，做台阶、地面和现代外墙。", 0x85878b, MB_CAT_STONE),
    M("smooth_stone",         "平滑石头",     "干净浅灰，适合现代墙面、台面和屋顶压顶。", 0x9c9c9c, MB_CAT_STONE),
    M("sand",                 "沙子",         "暖黄细沙，沙漠风墙面、地面和海岸线。", 0xdbd1a0, MB_CAT_STONE),

    /* --- wood family ----------------------------------------------------- */
    M("oak_log",              "橡木原木",     "暖棕木纹，木屋承重柱、边框和原木墙。", 0x6b532c, MB_CAT_WOOD),
    M("spruce_log",           "云杉原木",     "深棕原木，森林小屋、桁架和深色结构。", 0x4a3520, MB_CAT_WOOD),
    M("stripped_oak_log",     "去皮橡木",     "浅棕光滑木，做室内梁柱、家具和对比线条。", 0xb18b52, MB_CAT_WOOD),
    M("oak_planks",           "橡木木板",     "经典浅色木板，木屋墙、地板和阳台。", 0xb58a4f, MB_CAT_WOOD),
    M("spruce_planks",        "云杉木板",     "深棕木板，屋顶、墙板和乡村配色。", 0x6b4f2f, MB_CAT_WOOD),
    M("dark_oak_planks",      "深色橡木木板", "近黑棕木，做对比墙、门和深色结构。", 0x3f2d17, MB_CAT_WOOD),
    M("birch_planks",         "白桦木板",     "浅米色木板，北欧/现代室内与外墙点缀。", 0xd7c99a, MB_CAT_WOOD),
    M("bookshelf",            "书架",         "带书的木质方块，做图书馆、书房和墙面装饰。", 0x957b53, MB_CAT_WOOD),

    /* --- masonry --------------------------------------------------------- */
    M("bricks",               "砖块",         "红砖，烟囱、壁炉和英伦/工业风墙面。", 0x96594a, MB_CAT_STONE),
    M("quartz_block",         "石英块",       "明亮乳白，做现代立面、台面和装饰线条。", 0xecebe4, MB_CAT_STONE),
    M("smooth_quartz",        "平滑石英",     "无纹理亮白，现代极简墙面与台面。", 0xeae8e0, MB_CAT_STONE),
    M("white_concrete",       "白色混凝土",   "纯白无纹理，现代建筑的主墙。", 0xcfd5d6, MB_CAT_STONE),
    M("light_gray_concrete",  "淡灰色混凝土", "浅灰无纹理，次墙、分缝和中性底。", 0x7d7d73, MB_CAT_STONE),
    M("gray_concrete",        "灰色混凝土",   "中灰无纹理，现代/工业的对比墙面与屋顶。", 0x36393d, MB_CAT_STONE),
    M("black_concrete",       "黑色混凝土",   "近黑无纹理，做阴影带、边框和强烈对比。", 0x0b0b0d, MB_CAT_STONE),

    /* --- glass ----------------------------------------------------------- */
    M("glass",                "玻璃",         "全透明方块，落地窗、幕墙和采光顶。", 0xa8d8e8, MB_CAT_GLASS),
    M("glass_pane",           "玻璃板",       "薄玻璃片，窗格、栏杆和温室隔断。", 0xb6dfec, MB_CAT_GLASS),
    M("tinted_glass",         "遮光玻璃",     "深色半透明，做冷调幕墙和隐私窗。", 0x3a3a45, MB_CAT_GLASS),

    /* --- nature ---------------------------------------------------------- */
    M("oak_leaves",           "橡树树叶",     "暖绿树叶，庭院绿化、树冠和遮荫。", 0x4a7a2e, MB_CAT_NATURE),
    M("spruce_leaves",        "云杉树叶",     "深绿针叶，森林感和树屋树冠。", 0x2f5a34, MB_CAT_NATURE),
    M("grass_block",          "草方块",       "草皮方块，花园地面和建筑周边绿化。", 0x6aa84f, MB_CAT_NATURE),
    M("dirt",                 "泥土",         "土色方块，地基回填、花园和自然过渡。", 0x79553a, MB_CAT_NATURE),
    M("hay_block",            "干草块",       "金黄干草，做农场、装饰和暖色点缀。", 0xb9a23f, MB_CAT_NATURE),
    M("water",                "水",           "半透明蓝，桥下水面、护城河和景观池。", 0x3a6ea5, MB_CAT_NATURE),

    /* --- wool / decoration ------------------------------------------------ */
    M("white_wool",           "白色羊毛",     "柔软白块，做织物、旗帜和柔光装饰。", 0xe9ecec, MB_CAT_WOOL),
    M("red_wool",             "红色羊毛",     "饱和红，做旗帜、遮阳篷和强调色。", 0xa02722, MB_CAT_WOOL),
    M("blue_wool",            "蓝色羊毛",     "饱和蓝，做旗帜、织物和冷色点缀。", 0x35399d, MB_CAT_WOOL),
    M("terracotta",           "陶瓦",         "暖橙陶土，沙漠/地中海风墙面和屋顶。", 0x985e43, MB_CAT_DECO),
    M("iron_bars",            "铁栏杆",       "细铁栏，阳台护栏、窗栅和地牢感细节。", 0x6e6e6e, MB_CAT_DECO),
    M("ladder",               "梯子",         "可攀爬梯子，楼层之间和塔楼内的垂直交通。", 0x8a6a3a, MB_CAT_DECO),
    M("crafting_table",       "工作台",       "木工台，守卫室、工坊和乡村室内细节。", 0x7a4f2a, MB_CAT_DECO),

    /* --- lighting --------------------------------------------------------- */
    ML("glowstone",           "萤石",         "高亮发光，屋顶灯、室内主光源。", 0xf9d67a, MB_CAT_LIGHT, 15),
    ML("sea_lantern",         "海晶灯",       "冷白发光，现代室内、水景和露台照明。", 0xcfe6e0, MB_CAT_LIGHT, 15),
    ML("lantern",             "灯笼",         "暖黄小灯，门廊、栏杆和街巷照明。", 0xf5b95e, MB_CAT_LIGHT, 15),
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
    static const MBMaterial unknown = { "", "", "", 0xff00ff, MB_CAT_DECO, 0 };
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

const char* mb_material_desc(const char* id)
{
    return mb_material(id)->desc;
}

const char* mb_material_category_name(MBMaterialCategory category)
{
    switch (category) {
    case MB_CAT_STONE:  return "石材";
    case MB_CAT_WOOD:   return "木材";
    case MB_CAT_GLASS:  return "玻璃";
    case MB_CAT_DECO:   return "装饰";
    case MB_CAT_NATURE: return "自然";
    case MB_CAT_LIGHT:  return "光源";
    case MB_CAT_WOOL:   return "羊毛";
    default:            return "其它";
    }
}

uint32_t mb_material_color(const char* id)
{
    return mb_material(id)->color;
}
