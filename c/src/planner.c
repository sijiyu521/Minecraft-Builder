/*
 * planner.c — validated model intent to deterministic build parameters.
 *
 * There is a single generation path now:
 *
 *   mb_generate()  takes the MBLlmPlan produced by llm.c, picks the template,
 *                  style and palette, applies the same seeded jitter the model
 *                  hint describes, and calls finalize().
 *
 * finalize() is where determinism is guaranteed: the voxels always come from
 * the same six templates in structures.c, and the arithmetic is fixed down to
 * the last rng() call (so the same plan + same seed is reproducible). The old
 * keyword-based local rule engine is gone; a configured model is required.
 */
#include "mb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* styles                                                                     */
/* ========================================================================== */

/*
 * Palette resolution: every field has a default, the provided ones win.
 */
#define STYLE_PALETTE(wall, accent, roof, floor, frame, glass, light, nature) \
    { wall, accent, roof, floor, frame, glass, light, nature }

#define PALETTE_DEFAULTS(wall) \
    { wall, "minecraft:light_gray_concrete", "minecraft:smooth_stone", \
      "minecraft:birch_planks", "minecraft:stripped_oak_log", "minecraft:glass", \
      "minecraft:sea_lantern", "minecraft:oak_leaves" }

static const char* kw_style_modern[] = { "现代", "简约", "混凝土", "石英", "白", "极简",
                                         "modern", "villa" };
static const char* kw_style_medieval[] = { "中世纪", "城堡", "石砖", "石头", "古堡", "要塞",
                                           "防御", "medieval", "stone" };
static const char* kw_style_rustic[] = { "木屋", "乡村", "农舍", "木头", "森林", "乡村风",
                                         "rustic", "wood", "wooden" };
static const char* kw_style_desert[] = { "沙漠", "沙", "砂岩", "埃及", "desert", "sand" };
static const char* kw_style_fantasy[] = { "奇幻", "魔法", "精灵", "梦幻", "水晶",
                                          "fantasy", "magic", "elf" };
static const char* kw_style_industrial[] = { "工业", "工厂", "仓库", "机械", "灰", "黑",
                                             "industrial", "factory" };
static const char* kw_style_mossy[] = { "废墟", "苔", "古老", "遗迹", "破败",
                                        "mossy", "ruin", "ancient" };

#define NSTYLE(x) ((int)(sizeof(x) / sizeof((x)[0])))

static const MBStyle MB_STYLES_SRC[] = {
    {
        "modern", "现代简约", kw_style_modern, NSTYLE(kw_style_modern),
        STYLE_PALETTE("minecraft:white_concrete", "minecraft:light_gray_concrete",
                      "minecraft:gray_concrete", "minecraft:birch_planks",
                      "minecraft:light_gray_concrete", "minecraft:glass",
                      "minecraft:sea_lantern", "minecraft:oak_leaves"),
    },
    {
        "medieval", "中世纪石造", kw_style_medieval, NSTYLE(kw_style_medieval),
        STYLE_PALETTE("minecraft:stone_bricks", "minecraft:polished_andesite",
                      "minecraft:dark_oak_planks", "minecraft:spruce_planks",
                      "minecraft:spruce_log", "minecraft:glass_pane",
                      "minecraft:lantern", "minecraft:spruce_leaves"),
    },
    {
        "rustic", "乡村木质", kw_style_rustic, NSTYLE(kw_style_rustic),
        STYLE_PALETTE("minecraft:oak_planks", "minecraft:stripped_oak_log",
                      "minecraft:spruce_planks", "minecraft:oak_planks",
                      "minecraft:oak_log", "minecraft:glass_pane",
                      "minecraft:lantern", "minecraft:oak_leaves"),
    },
    {
        "desert", "沙漠砂岩", kw_style_desert, NSTYLE(kw_style_desert),
        STYLE_PALETTE("minecraft:sand", "minecraft:terracotta",
                      "minecraft:terracotta", "minecraft:sand",
                      "minecraft:oak_log", "minecraft:glass_pane",
                      "minecraft:lantern", "minecraft:hay_block"),
    },
    {
        "fantasy", "奇幻魔法", kw_style_fantasy, NSTYLE(kw_style_fantasy),
        STYLE_PALETTE("minecraft:quartz_block", "minecraft:deepslate_bricks",
                      "minecraft:dark_oak_planks", "minecraft:birch_planks",
                      "minecraft:dark_oak_planks", "minecraft:tinted_glass",
                      "minecraft:glowstone", "minecraft:spruce_leaves"),
    },
    {
        "industrial", "工业冷调", kw_style_industrial, NSTYLE(kw_style_industrial),
        STYLE_PALETTE("minecraft:gray_concrete", "minecraft:black_concrete",
                      "minecraft:polished_andesite", "minecraft:smooth_stone",
                      "minecraft:iron_bars", "minecraft:tinted_glass",
                      "minecraft:sea_lantern", "minecraft:oak_leaves"),
    },
    {
        "mossy", "废墟苔痕", kw_style_mossy, NSTYLE(kw_style_mossy),
        STYLE_PALETTE("minecraft:mossy_stone_bricks", "minecraft:cobblestone",
                      "minecraft:spruce_planks", "minecraft:cobblestone",
                      "minecraft:spruce_log", "minecraft:glass_pane",
                      "minecraft:lantern", "minecraft:oak_leaves"),
    },
};

const MBStyle MB_STYLES[] = {
    MB_STYLES_SRC[0], MB_STYLES_SRC[1], MB_STYLES_SRC[2], MB_STYLES_SRC[3],
    MB_STYLES_SRC[4], MB_STYLES_SRC[5], MB_STYLES_SRC[6],
};

const int   MB_STYLE_COUNT = NSTYLE(MB_STYLES);
const char* const MB_DEFAULT_STYLE_ID = "modern";

const MBStyle* mb_style_by_id(const char* id)
{
    if (!id) return NULL;
    for (int i = 0; i < MB_STYLE_COUNT; i++) {
        if (mb_ieq(MB_STYLES[i].id, id)) return &MB_STYLES[i];
    }
    return NULL;
}

static const MBStyle* style_or_default(const char* id)
{
    const MBStyle* s = mb_style_by_id(id);
    return s ? s : mb_style_by_id(MB_DEFAULT_STYLE_ID);
}

static const MBStructure* structure_or_default(const char* id)
{
    const MBStructure* s = mb_structure_by_id(id);
    return s ? s : mb_structure_by_id(MB_DEFAULT_STRUCTURE_ID);
}

static const char* SCALE_LABEL_SMALL  = "小型";
static const char* SCALE_LABEL_MEDIUM = "中型";
static const char* SCALE_LABEL_LARGE  = "大型";

static int int_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ========================================================================== */
/* stats + finalize                                                           */
/* ========================================================================== */

typedef struct {
    const char* id;
    int         count;
} Usage;

static int cmp_usage_desc(const void* pa, const void* pb)
{
    const Usage* a = (const Usage*)pa;
    const Usage* b = (const Usage*)pb;
    if (a->count != b->count) return a->count > b->count ? -1 : 1;
    return strcmp(a->id, b->id);
}

typedef struct {
    int      total;
    Usage*   byMaterial;
    int      count;
    const char** palette;
} Stats;

static Stats compute_stats(Arena* a, const MBVoxel* voxels, size_t n)
{
    Stats s;
    s.total = (int)n;
    s.byMaterial = (Usage*)mb_arena_alloc(a, (n ? n : 1) * sizeof(Usage));
    s.count = 0;

    for (size_t i = 0; i < n; i++) {
        const char* id = voxels[i].material;
        int found = -1;
        for (int k = 0; k < s.count; k++) {
            if (strcmp(s.byMaterial[k].id, id) == 0) { found = k; break; }
        }
        if (found >= 0) s.byMaterial[found].count++;
        else {
            s.byMaterial[s.count].id = id;
            s.byMaterial[s.count].count = 1;
            s.count++;
        }
    }

    qsort(s.byMaterial, (size_t)s.count, sizeof(Usage), cmp_usage_desc);

    s.palette = (const char**)mb_arena_alloc(a, (s.count ? s.count : 1) * sizeof(char*));
    for (int i = 0; i < s.count; i++) s.palette[i] = s.byMaterial[i].id;
    return s;
}

typedef struct {
    const char*  prompt;
    uint32_t     seed;
    const char*  styleLabel;
    const char*  scaleLabel;
    const char*  name;          /* NULL = derive from style + template */
    bool         isLlm;
    const MBLlmTrace* trace;    /* usually set; NULL is allowed for tests */

    const char** keywords;      int keywordCount;
    const char** detected;      int detectedCount;
    const char** modifiers;     int modifierCount;
    const char** notes;         int noteCount;
} FinalizeInput;

static MBBuildResult* finalize(Arena* a, const FinalizeInput* in,
                               const MBStructure* def, const MBBuildOptions* options)
{
    /* `seed || hashString(prompt)` — a zero seed means "derive from the prompt". */
    uint32_t bseed = in->seed ? in->seed : mb_hash_string(in->prompt);

    MBBuilder b;
    mb_builder_init(&b, a, bseed);
    def->build(&b, options);

    MBVoxel* voxels = NULL;
    size_t count = 0;
    MBSize size;
    mb_sketch_normalize(a, &b.sketch, &size, &voxels, &count);

    Stats stats = compute_stats(a, voxels, count);

    MBBuildResult* r = (MBBuildResult*)mb_arena_calloc(a, sizeof(MBBuildResult));
    r->size = size;
    r->blocks = voxels;
    r->blockCount = count;
    r->palette = stats.palette;
    r->paletteCount = stats.count;
    r->byMaterial = (MBMaterialUsage*)mb_arena_alloc(a, (stats.count ? stats.count : 1) *
                                                          sizeof(MBMaterialUsage));
    for (int i = 0; i < stats.count; i++) {
        r->byMaterial[i].id = stats.byMaterial[i].id;
        r->byMaterial[i].count = stats.byMaterial[i].count;
    }
    r->byMaterialCount = stats.count;
    r->total = stats.total;

    if (in->name) {
        r->name = in->name;
    } else {
        r->name = mb_arena_printf(a, "%s · %s", in->styleLabel, def->label);
    }

    r->summary = mb_arena_printf(
        a, "%s体量 %d×%d 占地 · 最高 %d 格 · 共 %d 个方块 · %d 种材质",
        in->scaleLabel, size.x, size.z, size.y, stats.total, stats.count);

    MBPlannerReport* rp = &r->report;
    rp->prompt = (in->prompt && in->prompt[0]) ? in->prompt : "（空提示词）";
    rp->structure = def->id;
    rp->structureLabel = def->label;
    rp->scale = in->scaleLabel;
    rp->seed = in->seed;
    rp->keywords = in->keywords;
    rp->keywordCount = in->keywordCount;
    rp->detectedMaterials = in->detected;
    rp->detectedMaterialCount = in->detectedCount;
    rp->modifiers = in->modifiers;
    rp->modifierCount = in->modifierCount;
    rp->notes = in->notes;
    rp->noteCount = in->noteCount;
    rp->isLlm = in->isLlm;
    if (in->trace) {
        rp->hasLlmUser = true;
        rp->llm = *in->trace;
    }
    return r;
}

/* ========================================================================== */
/* generation (model plan -> deterministic voxels)                            */
/* ========================================================================== */

static int color_luma(const char* material)
{
    uint32_t c = mb_material_color(material ? material : "");
    int r = (int)((c >> 16) & 0xFFu);
    int g = (int)((c >> 8) & 0xFFu);
    int b = (int)(c & 0xFFu);
    return (r * 299 + g * 587 + b * 114) / 1000;
}

static const char* contrast_slot(const char* candidate, const char* wall,
                                 const char* styleDefault, const char* harsh, int minGap)
{
    if (!candidate || !wall) return candidate;
    int gap = color_luma(candidate) - color_luma(wall);
    if (gap < 0) gap = -gap;
    if (gap >= minGap) return candidate;
    if (styleDefault) {
        int styleGap = color_luma(styleDefault) - color_luma(wall);
        if (styleGap < 0) styleGap = -styleGap;
        if (styleGap >= minGap) return styleDefault;
    }
    return harsh;
}

MBBuildResult* mb_generate(Arena* a, const char* rawPrompt, uint32_t seed,
                           const MBLlmPlan* plan, const MBLlmTrace* trace)
{
    const char* prompt = mb_trim_dup(a, rawPrompt ? rawPrompt : "");

    /* llm.c already whitelisted these ids, but unknown values still fall back
     * defensively here so a malformed plan can never crash the builder. */
    const MBStructure* def = structure_or_default(plan->structure);
    const MBStyle* style = style_or_default(plan->style);

    /* The model may override individual palette slots on top of the style. */
    const char* wall   = plan->palette.wall   ? plan->palette.wall   : style->palette.wall;
    const char* accent = plan->palette.accent ? plan->palette.accent : style->palette.accent;
    const char* roof   = plan->palette.roof   ? plan->palette.roof   : style->palette.roof;
    const char* floor_ = plan->palette.floor  ? plan->palette.floor  : style->palette.floor;
    const char* frame  = plan->palette.frame  ? plan->palette.frame  : style->palette.frame;
    const char* glass  = plan->palette.glass  ? plan->palette.glass  : style->palette.glass;
    const char* light  = plan->palette.light  ? plan->palette.light  : style->palette.light;
    const char* nature = plan->palette.nature ? plan->palette.nature : style->palette.nature;

    /* Keep the palette legible. Models sometimes paint wall, roof and trim in
     * near-identical whites/greys, producing an undifferentiated box; when the
     * luminance gap is too small, borrow the style's contrasting slot. */
    roof = contrast_slot(roof, wall, style->palette.roof,
                         "minecraft:dark_oak_planks", 55);
    frame = contrast_slot(frame, wall, style->palette.frame,
                          "minecraft:dark_oak_planks", 40);
    accent = contrast_slot(accent, wall, style->palette.accent,
                           "minecraft:gray_concrete", 35);

    /* Same jitter trick as the web app so "换一个变体" stays meaningful. */
    uint32_t rng = seed;
    int jw = mb_js_round((mb_rng_next(&rng) * 2.0 - 1.0) * 1.0);
    int jd = mb_js_round((mb_rng_next(&rng) * 2.0 - 1.0) * 1.0);
    int jh = mb_js_round((mb_rng_next(&rng) * 2.0 - 1.0) * 1.0);

    MBBuildOptions options;
    options.width  = int_clamp(plan->width + jw, 5, 48);
    options.depth  = int_clamp(plan->depth + jd, 3, 48);
    options.height = int_clamp(plan->height + jh, 3, 40);
    options.floors = int_clamp(plan->floors, 1, 4);
    options.palette.wall = wall;
    options.palette.accent = accent;
    options.palette.roof = roof;
    options.palette.floor = floor_;
    options.palette.frame = frame;
    options.palette.glass = glass;
    options.palette.light = light;
    options.palette.nature = nature;

    /* detectedMaterials = unique non-null palette entries, in slot order.
     * This array is linked into the report, so it has to live in the arena:
     * a stack array here used to dangle as soon as mb_generate() returned. */
    const char** detected = (const char**)mb_arena_alloc(a, 8 * sizeof(char*));
    int detectedCount = 0;
    const char* slots[8] = { wall, accent, roof, floor_, frame, glass, light, nature };
    for (int i = 0; i < 8; i++) {
        bool dup = false;
        for (int k = 0; k < detectedCount; k++) {
            if (strcmp(detected[k], slots[i]) == 0) { dup = true; break; }
        }
        if (!dup) detected[detectedCount++] = mb_material_name(slots[i]);
    }

    const char** notes = (const char**)mb_arena_alloc(a, (size_t)(2 + plan->noteCount) * sizeof(char*));
    int noteCount = 0;
    notes[noteCount++] =
        "由大模型规划 建筑意图，再由确定性体素构建器生成几何：模型不会直接输出方块坐标，因此结果始终可建造。";
    notes[noteCount++] = mb_arena_printf(a, "模型：%s · 耗时 %ld ms。",
                                         trace ? trace->model : "unknown",
                                         trace ? trace->latencyMs : 0L);
    for (int i = 0; i < plan->noteCount; i++) notes[noteCount++] = plan->notes[i];

    const char* scaleLabel;
    if (plan->scale && strcmp(plan->scale, "small") == 0) {
        scaleLabel = SCALE_LABEL_SMALL;
    } else if (plan->scale && strcmp(plan->scale, "large") == 0) {
        scaleLabel = SCALE_LABEL_LARGE;
    } else {
        scaleLabel = SCALE_LABEL_MEDIUM;
    }

    const char* name = NULL;
    if (plan->name && plan->name[0]) {
        name = mb_arena_printf(a, "%s · %s", plan->name, style->label);
    }

    FinalizeInput fi;
    memset(&fi, 0, sizeof(fi));
    fi.prompt = prompt;
    fi.seed = seed;
    fi.styleLabel = style->label;
    fi.scaleLabel = scaleLabel;
    fi.name = name;
    fi.isLlm = true;
    fi.trace = trace;
    fi.keywords = plan->keywords;   fi.keywordCount = plan->keywordCount;
    fi.detected = (const char**)detected; fi.detectedCount = detectedCount;
    fi.modifiers = plan->modifiers; fi.modifierCount = plan->modifierCount;
    fi.notes = notes;               fi.noteCount = noteCount;

    return finalize(a, &fi, def, &options);
}
