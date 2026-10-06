/*
 * planner.c — natural language (or model output) to build parameters.
 *
 * Two entry points:
 *
 *   mb_generate()            the local rule engine: keywords pick the template,
 *                            the palette and any explicit dimensions.
 *   mb_generate_with_plan()  the same tail, but fed by a validated model plan.
 *
 * Both funnel into finalize(), which is where determinism is guaranteed: the
 * voxels always come from the same six templates in structures.c.
 *
 * The arithmetic here is fixed down to the last rng() call and is pinned by the
 * golden tests. Note two deliberate details: keyword scoring uses UTF-16 code
 * units (so a CJK character counts as one unit, exactly as a UTF-16 host would
 * measure it), and an explicitly supplied dimension costs *no* random number —
 * a single extra rng() call would shift the output of every generated building
 * and break every recorded block count.
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
        STYLE_PALETTE("minecraft:white_concrete", "minecraft:gray_concrete",
                      "minecraft:smooth_stone", "minecraft:birch_planks",
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

static double scale_factor(MBScaleId s)
{
    switch (s) {
    case MB_SCALE_SMALL:  return 0.75;
    case MB_SCALE_LARGE:  return 1.38;
    case MB_SCALE_MEDIUM: return 1.0;
    case MB_SCALE_AUTO:
    default:              return 1.0;
    }
}

static const char* scale_label(MBScaleId s)
{
    switch (s) {
    case MB_SCALE_SMALL: return SCALE_LABEL_SMALL;
    case MB_SCALE_LARGE: return SCALE_LABEL_LARGE;
    default:             return SCALE_LABEL_MEDIUM;
    }
}

const char* mb_scale_label(MBScaleId s)
{
    return scale_label(s);
}

const char* mb_scale_label_auto(MBScaleId s)
{
    return s == MB_SCALE_AUTO ? "自动" : scale_label(s);
}

/* ========================================================================== */
/* UTF-16 text matching                                                       */
/* ========================================================================== */

static bool u16_isdigit(U16 c) { return c >= '0' && c <= '9'; }

static bool u16_isspace(U16 c)
{
    switch (c) {
    case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D:
    case 0x0020: case 0x00A0: case 0x1680: case 0x2028: case 0x2029:
    case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

/* index of `n` inside `h` starting at `from`, or -1 */
static long u16_index_of(const U16* h, size_t hn, const U16* n, size_t nn, size_t from)
{
    if (nn == 0) return (long)from;
    if (nn > hn) return -1;
    for (size_t i = from; i + nn <= hn; i++) {
        size_t j = 0;
        while (j < nn && h[i + j] == n[j]) j++;
        if (j == nn) return (long)i;
    }
    return -1;
}

static bool u16_contains(const U16* h, size_t hn, const char* needle, Arena* a)
{
    size_t nn = 0;
    U16* n = mb_u16_from_utf8(a, needle, &nn);
    return u16_index_of(h, hn, n, nn, 0) >= 0;
}

/* Matches /(\d{1,2})\s*[x×*]\s*(\d{1,2})/, returning the first match. */
static bool match_cross(const U16* t, size_t n, int* outA, int* outB)
{
    for (size_t i = 0; i + 3 <= n; i++) {
        if (!u16_isdigit(t[i])) continue;

        for (int aLen = 2; aLen >= 1; aLen--) {
            if (i + (size_t)aLen > n) continue;
            if (aLen == 2 && !u16_isdigit(t[i + 1])) continue;

            size_t p = i + (size_t)aLen;
            while (p < n && u16_isspace(t[p])) p++;
            if (p >= n) continue;
            if (t[p] != 'x' && t[p] != 0x00D7 && t[p] != '*') continue;
            p++;
            while (p < n && u16_isspace(t[p])) p++;
            if (p >= n || !u16_isdigit(t[p])) continue;

            int a = (aLen == 2) ? (t[i] - '0') * 10 + (t[i + 1] - '0') : (t[i] - '0');
            int b;
            if (p + 1 < n && u16_isdigit(t[p + 1])) {
                b = (t[p] - '0') * 10 + (t[p + 1] - '0');
            } else {
                b = t[p] - '0';
            }
            *outA = a;
            *outB = b;
            return true;
        }
    }
    return false;
}

/*
 * Matches /(?:alt1|alt2|...)\D{0,4}(\d{1,2})/.
 * Alternatives are tried in order; `\D{0,4}` is greedy and backtracks, and
 * `\d{1,2}` is greedy so two digits always win when available.
 */
static bool match_label_digits(Arena* a, const U16* t, size_t n, const char* const* alts,
                               int altCount, int* out)
{
    for (int k = 0; k < altCount; k++) {
        size_t nn = 0;
        U16* needle = mb_u16_from_utf8(a, alts[k], &nn);

        for (size_t i = 0; i + nn <= n; i++) {
            size_t j = 0;
            while (j < nn && t[i + j] == needle[j]) j++;
            if (j != nn) continue;

            /* \D{0,4} is greedy and backtracks, so try the widest gap first. */
            for (int g = 4; g >= 0; g--) {
                size_t p = i + nn + (size_t)g;
                if (p >= n) continue;
                bool ok = true;
                for (size_t q = i + nn; q < p; q++) {
                    if (u16_isdigit(t[q])) { ok = false; break; }
                }
                if (!ok) continue;
                if (!u16_isdigit(t[p])) continue;
                if (p + 1 < n && u16_isdigit(t[p + 1])) {
                    *out = (t[p] - '0') * 10 + (t[p + 1] - '0');
                } else {
                    *out = t[p] - '0';
                }
                return true;
            }
        }
    }
    return false;
}

/* Matches /(\d)\s*层/ */
static bool match_digit_before(Arena* a, const U16* t, size_t n, const char* suffix, int* out)
{
    size_t nn = 0;
    U16* s = mb_u16_from_utf8(a, suffix, &nn);

    for (size_t i = 0; i < n; i++) {
        if (!u16_isdigit(t[i])) continue;
        size_t p = i + 1;
        while (p < n && u16_isspace(t[p])) p++;
        if (p + nn > n) continue;
        size_t j = 0;
        while (j < nn && t[p + j] == s[j]) j++;
        if (j == nn) { *out = t[i] - '0'; return true; }
    }
    return false;
}

static int int_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ========================================================================== */
/* analyzePrompt                                                              */
/* ========================================================================== */

#define MB_MAX_FACTS 16

typedef struct {
    const MBStructure* structure;      /* NULL = unrecognised */
    const MBStyle*     style;
    bool               hasScale;
    MBScaleId          scale;

    const char* keywords[MB_MAX_FACTS];
    int         keywordCount;

    const char* modifiers[MB_MAX_FACTS];
    int         modifierCount;

    const char* detected[MB_MAX_FACTS * 2];
    int         detectedCount;

    int width, depth, height, floors;  /* -1 = not specified */
} PromptFacts;

static void facts_push(const char** list, int* count, int cap, const char* value)
{
    for (int i = 0; i < *count; i++) {
        if (list[i] == value || (list[i] && value && strcmp(list[i], value) == 0)) return;
    }
    if (*count < cap) list[(*count)++] = value;
}

static PromptFacts analyze_prompt(Arena* a, const char* prompt,
                                  const char* structureId, const char* styleId, MBScaleId scale)
{
    PromptFacts f;
    memset(&f, 0, sizeof(f));
    f.width = f.depth = f.height = f.floors = -1;

    /* text = prompt.toLowerCase() */
    size_t n = 0;
    U16* t = mb_u16_from_utf8(a, prompt, &n);
    for (size_t i = 0; i < n; i++) {
        if (t[i] >= 'A' && t[i] <= 'Z') t[i] = (U16)(t[i] - 'A' + 'a');
    }

    /* --- structure ------------------------------------------------------ */
    if (structureId && !mb_ieq(structureId, "auto")) {
        f.structure = structure_or_default(structureId);
    } else {
        const MBStructure* best = NULL;
        int bestScore = 0;
        for (int i = 0; i < MB_STRUCTURE_COUNT; i++) {
            int score = 0;
            const MBStructure* def = &MB_STRUCTURES[i];
            for (int k = 0; k < def->keywordCount; k++) {
                if (u16_contains(t, n, def->keywords[k], a)) {
                    score += (int)mb_u16_len(def->keywords[k]);
                }
            }
            /* Strictly greater: the first template wins a tie, like the web app. */
            if (score > 0 && score > bestScore) { best = def; bestScore = score; }
        }
        f.structure = best;
        if (best) {
            for (int k = 0; k < best->keywordCount; k++) {
                if (u16_contains(t, n, best->keywords[k], a)) {
                    facts_push(f.keywords, &f.keywordCount, MB_MAX_FACTS, best->keywords[k]);
                }
            }
        }
    }

    /* --- style ---------------------------------------------------------- */
    if (styleId && !mb_ieq(styleId, "auto")) {
        /* No fallback here — an unknown id yields NULL, exactly like the web app. */
        f.style = mb_style_by_id(styleId);
    } else {
        const MBStyle* best = NULL;
        int bestScore = 0;
        for (int i = 0; i < MB_STYLE_COUNT; i++) {
            int score = 0;
            const MBStyle* st = &MB_STYLES[i];
            for (int k = 0; k < st->keywordCount; k++) {
                if (u16_contains(t, n, st->keywords[k], a)) {
                    score += (int)mb_u16_len(st->keywords[k]);
                }
            }
            if (score > 0 && score > bestScore) { best = st; bestScore = score; }
        }
        f.style = best;
    }

    /* --- scale ---------------------------------------------------------- */
    if (scale != MB_SCALE_AUTO) {
        f.hasScale = true;
        f.scale = scale;
    } else {
        static const char* big[] = { "巨大", "宏伟", "超大", "大型", "城堡", "要塞", "大规模" };
        static const char* small[] = { "小", "迷你", "tiny", "small", "简易" };
        bool isBig = false, isSmall = false;
        for (int i = 0; i < 7; i++) if (u16_contains(t, n, big[i], a)) { isBig = true; break; }
        for (int i = 0; i < 5; i++) if (u16_contains(t, n, small[i], a)) { isSmall = true; break; }
        if (isBig)       { f.hasScale = true; f.scale = MB_SCALE_LARGE; }
        else if (isSmall) { f.hasScale = true; f.scale = MB_SCALE_SMALL; }
    }

    /* --- explicit dimensions -------------------------------------------- */
    {
        int a1 = 0, b1 = 0;
        if (match_cross(t, n, &a1, &b1)) {
            f.width = int_clamp(a1, 5, 48);
            f.depth = int_clamp(b1, 3, 48);
        }
    }
    {
        static const char* wAlts[] = { "宽", "宽度", "width" };
        int v = 0;
        if (match_label_digits(a, t, n, wAlts, 3, &v)) f.width = int_clamp(v, 5, 48);
    }
    {
        static const char* dAlts[] = { "深", "深度", "进深", "depth" };
        int v = 0;
        if (match_label_digits(a, t, n, dAlts, 4, &v)) f.depth = int_clamp(v, 3, 48);
    }
    {
        static const char* hAlts[] = { "高", "高度", "height" };
        int v = 0;
        if (match_label_digits(a, t, n, hAlts, 3, &v)) f.height = int_clamp(v, 3, 40);
    }
    {
        int v = 0;
        if (match_digit_before(a, t, n, "层", &v)) f.floors = int_clamp(v, 1, 4);
    }

    /* --- material mentions ---------------------------------------------- */
    for (int i = 0; i < MB_MATERIAL_COUNT; i++) {
        const MBMaterial* m = &MB_MATERIALS[i];
        const char* shortId = m->id + 10;   /* skip "minecraft:" */
        if (u16_contains(t, n, m->name, a) || u16_contains(t, n, shortId, a)) {
            facts_push(f.detected, &f.detectedCount, MB_MAX_FACTS * 2, m->name);
        }
    }

    /* --- modifiers ------------------------------------------------------- */
    {
        struct { const char* label; int kind; } rules[] = {
            { "两层结构", 1 }, { "三层结构", 2 }, { "平屋顶", 3 }, { "坡屋顶", 4 },
            { "大面积采光", 5 }, { "门廊/露台", 6 }, { "对称布局", 7 }, { "临水环境", 8 },
            { "加宽体量", 9 }, { "增加层高", 10 }, { "缩小体量", 11 },
        };
        for (int r = 0; r < 11; r++) {
            bool hit = false;
            switch (rules[r].kind) {
            case 1: {
                int v = -1;
                bool m = match_digit_before(a, t, n, "层", &v);
                hit = u16_contains(t, n, "二层", a) || u16_contains(t, n, "两层", a) ||
                      (m && v == 2);
                break;
            }
            case 2: {
                int v = -1;
                bool m = match_digit_before(a, t, n, "层", &v);
                hit = u16_contains(t, n, "三层", a) || (m && v == 3);
                break;
            }
            case 3: hit = u16_contains(t, n, "平顶", a); break;
            case 4: hit = u16_contains(t, n, "尖顶", a) || u16_contains(t, n, "坡顶", a) ||
                         u16_contains(t, n, "山墙", a); break;
            case 5: hit = u16_contains(t, n, "大窗", a) || u16_contains(t, n, "落地窗", a) ||
                         u16_contains(t, n, "玻璃", a); break;
            case 6: hit = u16_contains(t, n, "门廊", a) || u16_contains(t, n, "阳台", a) ||
                         u16_contains(t, n, "露台", a); break;
            case 7: hit = u16_contains(t, n, "对称", a); break;
            case 8: hit = u16_contains(t, n, "护城河", a) || u16_contains(t, n, "水", a); break;
            case 9: hit = u16_contains(t, n, "宽", a); break;
            case 10: hit = u16_contains(t, n, "高", a) || u16_contains(t, n, "宏伟", a) ||
                          u16_contains(t, n, "雄伟", a); break;
            case 11: hit = u16_contains(t, n, "小", a) || u16_contains(t, n, "迷你", a); break;
            default: break;
            }
            if (hit) {
                facts_push(f.modifiers, &f.modifierCount, MB_MAX_FACTS, rules[r].label);
            }
        }
    }

    return f;
}

/* ========================================================================== */
/* resolveOptions                                                             */
/* ========================================================================== */

typedef struct {
    const MBStructure* def;
    MBBuildOptions     options;
} ResolvedOptions;

/*
 * MBStyle.palette and MBBuildOptions.palette are separate anonymous structs, so
 * C will not assign one to the other — copy the slots across explicitly.
 */
static void palette_from_style(MBBuildOptions* o, const MBStyle* s)
{
    o->palette.wall   = s->palette.wall;
    o->palette.accent = s->palette.accent;
    o->palette.roof   = s->palette.roof;
    o->palette.floor  = s->palette.floor;
    o->palette.frame  = s->palette.frame;
    o->palette.glass  = s->palette.glass;
    o->palette.light  = s->palette.light;
    o->palette.nature = s->palette.nature;
}

static ResolvedOptions resolve_options(const PromptFacts* facts, uint32_t seed)
{
    const MBStructure* def = facts->structure ? facts->structure
                                              : structure_or_default(MB_DEFAULT_STRUCTURE_ID);
    const MBStyle* style = facts->style ? facts->style : style_or_default(MB_DEFAULT_STYLE_ID);
    double factor = facts->hasScale ? scale_factor(facts->scale) : 1.0;

    /* NOTE: the order of these three delta() calls is part of the contract.
     * JavaScript writes `facts.overrides.width ?? clamp(base + delta(2), ...)`,
     * and `??` short-circuits, so an explicit override never advances the
     * generator. The deltas therefore have to be pulled lazily, in
     * width -> depth -> height order, or every later delta shifts by one. */
    bool needW = facts->width  < 0;
    bool needD = facts->depth  < 0;
    bool needH = facts->height < 0;

    uint32_t rng = seed;
    int deltaW = needW ? mb_js_round((mb_rng_next(&rng) * 2.0 - 1.0) * 2.0) : 0;
    int deltaD = needD ? mb_js_round((mb_rng_next(&rng) * 2.0 - 1.0) * 2.0) : 0;
    int deltaH = needH ? mb_js_round((mb_rng_next(&rng) * 2.0 - 1.0) * 1.0) : 0;

    int baseWidth  = int_clamp(mb_js_round((double)def->defWidth  * factor), 5, 48);
    int baseDepth  = int_clamp(mb_js_round((double)def->defDepth  * factor), 3, 48);
    int baseHeight = int_clamp(mb_js_round((double)def->defHeight * factor), 3, 40);

    int width  = needW ? int_clamp(baseWidth  + deltaW, 5, 48) : facts->width;
    int depth  = needD ? int_clamp(baseDepth  + deltaD, 3, 48) : facts->depth;
    int height = needH ? int_clamp(baseHeight + deltaH, 3, 40) : facts->height;
    int floors = facts->floors >= 0 ? facts->floors : def->defFloors;

    ResolvedOptions out;
    out.def = def;
    out.options.width  = width;
    out.options.depth  = depth;
    out.options.height = height;
    out.options.floors = (strcmp(def->id, "modern-house") == 0) ? floors
                                                                 : (floors > 1 ? floors : 1);
    palette_from_style(&out.options, style);
    return out;
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
    const MBLlmTrace* trace;    /* NULL for the local engine */

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
/* local rule engine                                                          */
/* ========================================================================== */

MBBuildResult* mb_generate(Arena* a, const MBGenerateInput* in)
{
    const char* prompt = mb_trim_dup(a, in->prompt ? in->prompt : "");

    PromptFacts facts = analyze_prompt(a, prompt,
                                       in->structureId ? in->structureId : "auto",
                                       in->styleId ? in->styleId : "auto",
                                       in->scale);
    ResolvedOptions res = resolve_options(&facts, in->seed);
    const MBStyle* style = facts.style ? facts.style : style_or_default(MB_DEFAULT_STYLE_ID);

    /* notes: two always, two conditional. */
    const char** notes = (const char**)mb_arena_alloc(a, 4 * sizeof(char*));
    int noteCount = 0;
    notes[noteCount++] =
        "由本地规则引擎生成：提示词 -> 建筑类型 / 尺寸 / 材质风格 -> 参数化体素。";
    notes[noteCount++] =
        "同一提示词配合相同种子会得到完全一致的结果，种子改变则产生同风格变体。";
    if (!facts.structure) {
        notes[noteCount++] = "未识别到具体建筑类型，已回退到默认的现代别墅模板。";
    }
    if (facts.width >= 0 || facts.depth >= 0 || facts.height >= 0) {
        notes[noteCount++] = "检测到显式尺寸参数，已覆盖模板默认比例。";
    }

    FinalizeInput fi;
    memset(&fi, 0, sizeof(fi));
    fi.prompt = prompt;
    fi.seed = in->seed;
    fi.styleLabel = style->label;
    fi.scaleLabel = facts.hasScale ? scale_label(facts.scale) : SCALE_LABEL_MEDIUM;
    fi.isLlm = false;
    fi.keywords = facts.keywords;   fi.keywordCount = facts.keywordCount;
    fi.detected = facts.detected;   fi.detectedCount = facts.detectedCount;
    fi.modifiers = facts.modifiers; fi.modifierCount = facts.modifierCount;
    fi.notes = notes;               fi.noteCount = noteCount;

    return finalize(a, &fi, res.def, &res.options);
}

/* ========================================================================== */
/* model driven generation                                                    */
/* ========================================================================== */

MBBuildResult* mb_generate_with_plan(Arena* a, const char* rawPrompt, uint32_t seed,
                                     const MBLlmPlan* plan, const MBLlmTrace* trace,
                                     const char* structureId, const char* styleId,
                                     MBScaleId scale)
{
    const char* prompt = mb_trim_dup(a, rawPrompt ? rawPrompt : "");

    /* UI dropdowns win, then the model's choice, then the default. */
    const MBStructure* def = NULL;
    if (structureId && !mb_ieq(structureId, "auto")) def = mb_structure_by_id(structureId);
    if (!def && plan->structure) def = mb_structure_by_id(plan->structure);
    if (!def) def = structure_or_default(MB_DEFAULT_STRUCTURE_ID);

    const MBStyle* style = NULL;
    if (styleId && !mb_ieq(styleId, "auto")) style = mb_style_by_id(styleId);
    if (!style && plan->style) style = mb_style_by_id(plan->style);
    if (!style) style = style_or_default(MB_DEFAULT_STYLE_ID);

    /* The model may override individual palette slots on top of the style. */
    const char* wall   = plan->palette.wall   ? plan->palette.wall   : style->palette.wall;
    const char* accent = plan->palette.accent ? plan->palette.accent : style->palette.accent;
    const char* roof   = plan->palette.roof   ? plan->palette.roof   : style->palette.roof;
    const char* floor_ = plan->palette.floor  ? plan->palette.floor  : style->palette.floor;
    const char* frame  = plan->palette.frame  ? plan->palette.frame  : style->palette.frame;
    const char* glass  = plan->palette.glass  ? plan->palette.glass  : style->palette.glass;
    const char* light  = plan->palette.light  ? plan->palette.light  : style->palette.light;
    const char* nature = plan->palette.nature ? plan->palette.nature : style->palette.nature;

    MBScaleId scaleOverride = (scale != MB_SCALE_AUTO) ? scale : MB_SCALE_AUTO;
    double factor = scaleOverride != MB_SCALE_AUTO ? scale_factor(scaleOverride) : 1.0;

    /* Same jitter trick as the web app so "换一个变体" stays meaningful. */
    uint32_t rng = seed;
    int jw = mb_js_round((mb_rng_next(&rng) * 2.0 - 1.0) * 1.0);
    int jd = mb_js_round((mb_rng_next(&rng) * 2.0 - 1.0) * 1.0);
    int jh = mb_js_round((mb_rng_next(&rng) * 2.0 - 1.0) * 1.0);

    MBBuildOptions options;
    options.width  = int_clamp(mb_js_round((double)plan->width  * factor) + jw, 5, 48);
    options.depth  = int_clamp(mb_js_round((double)plan->depth  * factor) + jd, 3, 48);
    options.height = int_clamp(mb_js_round((double)plan->height * factor) + jh, 3, 40);
    options.floors = int_clamp(plan->floors, 1, 4);
    options.palette.wall = wall;
    options.palette.accent = accent;
    options.palette.roof = roof;
    options.palette.floor = floor_;
    options.palette.frame = frame;
    options.palette.glass = glass;
    options.palette.light = light;
    options.palette.nature = nature;

    /* detectedMaterials = unique non-null palette entries, in slot order. */
    const char* detected[8];
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
    if (scaleOverride != MB_SCALE_AUTO) {
        scaleLabel = scale_label(scaleOverride);
    } else if (plan->scale && strcmp(plan->scale, "small") == 0) {
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
