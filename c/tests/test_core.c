/*
 * test_core.c - offline regression suite for the C edition.
 *
 *     mingw32-make test
 *
 * The local rule engine is gone. Generation now starts from a validated model
 * intent (MBLlmPlan) and goes through the single entry point
 *
 *     mb_generate(arena, prompt, seed, plan, trace)
 *
 * so the tests below drive that exact path with hand-built plans and with
 * canned model JSON. No network access and no API key are required: the LLM
 * HTTP client is never called, only its pure validation helpers.
 *
 * The pinned fixture table at the bottom of the fixture section records the
 * block counts the deterministic builders produce for the default plans. The
 * plan-driven invariant section covers every structure id against the first,
 * middle and last style in the catalog. The old prompt-parsing and local-rule
 * golden tables are intentionally absent: they tested a code path that no
 * longer exists and their numbers belong to different geometry.
 *
 * Output is deliberately ASCII-only: PowerShell mangles UTF-8 on a piped
 * console, and a readable PASS/FAIL line is worth more than pretty labels.
 */

#include "mb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* tiny test harness                                                          */
/* ========================================================================== */

static int g_checks;
static int g_failures;

static void check(const char* label, bool ok, const char* detail)
{
    g_checks += 1;
    if (ok) return;
    g_failures += 1;
    if (detail && *detail) printf("  [FAIL] %-54s %s\n", label, detail);
    else                   printf("  [FAIL] %s\n", label);
}

static void expect_int(const char* label, long got, long want)
{
    char detail[96];
    snprintf(detail, sizeof detail, "got %ld, want %ld", got, want);
    check(label, got == want, detail);
}

static void expect_str(const char* label, const char* got, const char* want)
{
    char detail[512];
    snprintf(detail, sizeof detail, "got \"%s\", want \"%s\"",
             got ? got : "(null)", want ? want : "(null)");
    check(label, got != NULL && want != NULL && strcmp(got, want) == 0, detail);
}

static void section(const char* name)
{
    printf("\n> %s\n", name);
}

static int count_prefix_lines(const char* text, const char* prefix)
{
    size_t plen = strlen(prefix);
    int n = 0;
    const char* p = text;
    while (*p) {
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= plen && strncmp(p, prefix, plen) == 0) n += 1;
        if (!eol) break;
        p = eol + 1;
    }
    return n;
}

static int count_lines(const char* text)
{
    int n = 0;
    for (const char* p = text; *p; p++) if (*p == '\n') n += 1;
    return n;
}

/* Row count that does not care whether the text ends with a newline. */
static int count_rows(const char* text)
{
    size_t len = strlen(text);
    if (len == 0) return 0;
    int n = count_lines(text);
    if (text[len - 1] != '\n') n += 1;
    return n;
}

/* Two builds are identical when every voxel matches, field by field. Comparing
 * raw struct bytes would be wrong: MBVoxel has padding. */
static bool blocks_equal(const MBBuildResult* a, const MBBuildResult* b)
{
    if (!a || !b) return false;
    if (a->blockCount != b->blockCount) return false;
    for (size_t i = 0; i < a->blockCount; i++) {
        const MBVoxel* x = &a->blocks[i];
        const MBVoxel* y = &b->blocks[i];
        if (x->x != y->x || x->y != y->y || x->z != y->z) return false;
        if (strcmp(x->material, y->material) != 0) return false;
    }
    return true;
}

static bool build_uses_material(const MBBuildResult* r, const char* material)
{
    if (!r || !r->blocks) return false;
    for (size_t i = 0; i < r->blockCount; i++) {
        if (strcmp(r->blocks[i].material, material) == 0) return true;
    }
    return false;
}

static int cmp_u64(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* ========================================================================== */
/* plan helpers                                                               */
/* ========================================================================== */

/*
 * The old suite built MBBuildOptions directly. Every test now goes through the
 * real public entry point instead: a zeroed MBLlmPlan is the model's "empty
 * intent" and the seed supplies the same +/-1 footprint jitter the app uses.
 */
static MBLlmPlan plan_make(const char* structure, const char* style,
                           int width, int depth, int height, int floors,
                           const char* name)
{
    MBLlmPlan p;
    memset(&p, 0, sizeof p);
    p.structure = structure;
    p.style = style;
    p.scale = "medium";
    p.width = width;
    p.depth = depth;
    p.height = height;
    p.floors = floors;
    p.name = name;
    return p;
}

static MBLlmPlan plan_from_structure(const MBStructure* st, const char* style,
                                     const char* name)
{
    return plan_make(st->id, style, st->defWidth, st->defDepth,
                     st->defHeight, st->defFloors, name);
}

/* ========================================================================== */
/* structural validation - the invariants every build must satisfy            */
/* ========================================================================== */

static void validate_build(Arena* arena, const char* label, const MBBuildResult* r)
{
    char detail[256];

    check(label, r != NULL, "mb_generate returned NULL");
    if (!r) return;

    check(label, r->report.isLlm, "report.isLlm is false");
    check(label, r->name != NULL && r->name[0] != '\0', "empty build name");
    check(label, r->summary != NULL && r->summary[0] != '\0', "empty build summary");

    expect_int(label, (long)r->blockCount > 0, 1);
    check(label, r->size.x > 0 && r->size.y > 0 && r->size.z > 0,
          "bounding box has a non-positive edge");
    check(label, r->blocks != NULL, "block array is NULL");
    if (r->blockCount == 0 || !r->blocks) return;

    uint64_t* keys = mb_arena_alloc(arena, sizeof(uint64_t) * r->blockCount);

    bool material_known = true, in_range = true, no_nan = true;
    for (size_t i = 0; i < r->blockCount; i++) {
        const MBVoxel* v = &r->blocks[i];
        if (!v->material || !mb_material_known(v->material)) material_known = false;
        if (v->x < 0 || v->y < 0 || v->z < 0 ||
            v->x >= r->size.x || v->y >= r->size.y || v->z >= r->size.z) {
            in_range = false;
        }
        keys[i] = ((uint64_t)v->x << 42) | ((uint64_t)v->y << 21) | (uint64_t)v->z;
        if (v->material && strstr(v->material, "NaN") != NULL) no_nan = false;
    }
    check(label, material_known, "a voxel uses an unknown material id");
    check(label, in_range, "a voxel sits outside the reported bounding box");
    check(label, no_nan, "a material id is NaN");

    qsort(keys, r->blockCount, sizeof(uint64_t), cmp_u64);
    bool duplicate = false;
    for (size_t i = 1; i < r->blockCount; i++) {
        if (keys[i] == keys[i - 1]) { duplicate = true; break; }
    }
    check(label, !duplicate, "two voxels share a coordinate");

    int summed = 0;
    bool palette_known = true;
    for (int i = 0; i < r->byMaterialCount; i++) {
        summed += r->byMaterial[i].count;
        if (!mb_material_known(r->byMaterial[i].id)) palette_known = false;
    }
    check(label, summed == r->total, "byMaterial counts do not add up to total");
    expect_int(label, r->total, (long)r->blockCount);
    expect_int(label, r->paletteCount, r->byMaterialCount);
    check(label, palette_known, "the material palette holds an unknown id");

    for (int i = 0; i < r->paletteCount; i++) {
        if (!mb_material_known(r->palette[i])) {
            snprintf(detail, sizeof detail, "unknown palette entry \"%s\"", r->palette[i]);
            check(label, false, detail);
            break;
        }
    }
}

/* ========================================================================== */
/* pinned plan fixtures                                                       */
/* ========================================================================== */

/*
 * These are the default structure plans, generated through mb_generate() with
 * seed 20260101 (the same seed the old golden section used). The builders are
 * deterministic, so the bounding box, block count and material count are
 * stable and worth pinning: any accidental geometry change shows up here.
 *
 *   plan defaults for each structure, style cycled for palette coverage.
 */
typedef struct {
    const char* structure;
    const char* style;
    int width, depth, height, floors;
    uint32_t seed;
    int sizeX, sizeZ, sizeY;
    int blocks;
    int materials;
} PlanFixture;

static const PlanFixture FIXTURES[] = {
    { "modern-house", "modern",     15, 11,  5, 2, 20260101u, 21, 14, 14, 1287, 11 },
    { "cabin",        "rustic",     11,  9,  4, 1, 20260101u, 13, 13, 15,  613, 10 },
    { "castle-tower", "medieval",   11, 11, 20, 0, 20260101u, 11, 12, 27, 1253, 9 },
    { "treehouse",    "fantasy",    13, 13,  6, 1, 20260101u, 15, 15, 17,  956, 9 },
    { "bridge",       "industrial", 21,  5,  0, 0, 20260101u, 23,  7,  8,  646, 6 },
    { "castle-wall",  "mossy",      25,  3,  7, 0, 20260101u, 33,  9, 14, 1383, 7 },
};

#define FIXTURE_COUNT ((int)(sizeof(FIXTURES) / sizeof(FIXTURES[0])))

/* ========================================================================== */
/* sections                                                                   */
/* ========================================================================== */

static void test_plan_invariants(Arena* a)
{
    section("plan-driven invariants (all structures x first/middle/last style)");

    int styleIdx[3];
    styleIdx[0] = 0;
    styleIdx[1] = MB_STYLE_COUNT / 2;
    styleIdx[2] = MB_STYLE_COUNT - 1;

    static const char* invariantKeywords[] = { "alpha", "beta" };
    const char* mediumLabel = "\xe4\xb8\xad\xe5\x9e\x8b"; /* medium */

    for (int i = 0; i < MB_STRUCTURE_COUNT; i++) {
        const MBStructure* st = &MB_STRUCTURES[i];
        for (int k = 0; k < 3; k++) {
            const MBStyle* style = &MB_STYLES[styleIdx[k]];
            MBLlmPlan plan = plan_from_structure(st, style->id, "invariant");
            plan.keywords = invariantKeywords;
            plan.keywordCount = 2;
            plan.scale = "medium";

            char label[96];
            snprintf(label, sizeof label, "%s/%s", st->id, style->id);

            MBBuildResult* r = mb_generate(a, "invariant build", 777u, &plan, NULL);
            validate_build(a, label, r);
            if (!r) continue;

            expect_str(label, r->report.structure, st->id);
            expect_str(label, r->report.structureLabel, st->label);
            expect_str(label, r->report.scale, mediumLabel);
            expect_int(label, r->report.keywordCount, 2);
            check(label, r->report.keywords != NULL &&
                         strcmp(r->report.keywords[0], "alpha") == 0 &&
                         strcmp(r->report.keywords[1], "beta") == 0,
                  "plan keywords did not reach the report");
            check(label, !r->report.hasLlmUser, "a NULL trace must not be flagged as a user trace");
        }
    }
}

static void test_fixtures(Arena* a)
{
    section("plan fixtures (seed 20260101, pinned counts)");

    for (int i = 0; i < FIXTURE_COUNT; i++) {
        const PlanFixture* f = &FIXTURES[i];
        const MBStructure* st = mb_structure_by_id(f->structure);
        check(f->structure, st != NULL, "unknown structure id in the fixture table");
        if (!st) continue;

        MBLlmPlan plan = plan_make(f->structure, f->style, f->width, f->depth,
                                   f->height, f->floors, NULL);
        plan.scale = "medium";

        MBBuildResult* r = mb_generate(a, "fixture build", f->seed, &plan, NULL);
        validate_build(a, f->structure, r);
        if (!r) continue;

        printf("  . %-14s %2dx%-2d h%-2d  %5zu blocks  %d materials\n",
               f->structure, r->size.x, r->size.z, r->size.y,
               r->blockCount, r->byMaterialCount);

        expect_str(f->structure, r->report.structure, f->structure);
        expect_int(f->structure, r->size.x, f->sizeX);
        expect_int(f->structure, r->size.z, f->sizeZ);
        expect_int(f->structure, r->size.y, f->sizeY);
        expect_int(f->structure, (long)r->blockCount, f->blocks);
        expect_int(f->structure, r->byMaterialCount, f->materials);
    }
}

static void test_plan_semantics(Arena* a)
{
    section("plan semantics: fallbacks, palette, scale, prompt");

    /* Unknown ids fall back instead of crashing; the default style must win. */
    MBLlmPlan unknown = plan_make("no-such-structure", "no-such-style",
                                  15, 11, 5, 2, NULL);
    MBBuildResult* fb = mb_generate(a, "fallback", 123u, &unknown, NULL);
    validate_build(a, "unknown ids", fb);
    if (fb) {
        expect_str("unknown structure falls back to the default",
                   fb->report.structure, MB_DEFAULT_STRUCTURE_ID);
        check("unknown style falls back to the default palette",
              build_uses_material(fb, "minecraft:white_concrete"),
              "the modern wall material is missing");
        check("a NULL trace leaves hasLlmUser false", !fb->report.hasLlmUser, "");
    }

    /* Per-slot palette overrides win over the style defaults. */
    MBLlmPlan palette = plan_make("modern-house", "modern", 15, 11, 5, 2,
                                  "Palette Plan");
    palette.palette.wall = "minecraft:deepslate_bricks";
    palette.palette.light = "minecraft:glowstone";
    MBBuildResult* pb = mb_generate(a, "palette plan", 99u, &palette, NULL);
    validate_build(a, "palette override", pb);
    if (pb) {
        check("a plan palette override replaces the wall",
              build_uses_material(pb, "minecraft:deepslate_bricks"),
              "deepslate_bricks missing");
        check("a plan palette override replaces the light",
              build_uses_material(pb, "minecraft:glowstone"),
              "glowstone missing");
        check("the report carries the model name",
              pb->name != NULL && strstr(pb->name, "Palette Plan") != NULL,
              pb->name ? pb->name : "(null)");
    }

    /* Scale is a label passed through the report; unknown values mean medium. */
    {
        static const char* scales[4] = { "small", "medium", "large", "HUGE" };
        static const char* labels[4] = {
            "\xe5\xb0\x8f\xe5\x9e\x8b",   /* small */
            "\xe4\xb8\xad\xe5\x9e\x8b",   /* medium */
            "\xe5\xa4\xa7\xe5\x9e\x8b",   /* large */
            "\xe4\xb8\xad\xe5\x9e\x8b"    /* medium (fallback) */
        };
        for (int i = 0; i < 4; i++) {
            MBLlmPlan p = plan_make("modern-house", "modern", 15, 11, 5, 2, NULL);
            p.scale = scales[i];
            char label[64];
            snprintf(label, sizeof label, "scale %s", scales[i]);
            MBBuildResult* r = mb_generate(a, "scale build", 5u, &p, NULL);
            validate_build(a, label, r);
            if (!r) continue;
            expect_str(label, r->report.scale, labels[i]);
            check(label, r->summary != NULL && strstr(r->summary, labels[i]) != NULL,
                  "the scale label is missing from the summary");
        }
    }

    /* The prompt is trimmed before it reaches the report. */
    {
        MBLlmPlan p = plan_make("modern-house", "modern", 15, 11, 5, 2, "Prompt Plan");
        p.scale = "large";
        MBBuildResult* r = mb_generate(a, "  a modern house  ", 99u, &p, NULL);
        validate_build(a, "prompt trim", r);
        if (r) {
            expect_str("the prompt is trimmed", r->report.prompt, "a modern house");
            expect_int("the report records the seed", (long)r->report.seed, 99);
            expect_str("the scale label is large",
                       r->report.scale, "\xe5\xa4\xa7\xe5\x9e\x8b");
            check("the summary mentions the scale",
                  r->summary != NULL &&
                  strstr(r->summary, "\xe5\xa4\xa7\xe5\x9e\x8b") != NULL,
                  "large scale missing from the summary");
        }
    }
}

static void test_plan_determinism(Arena* a)
{
    section("determinism and seed jitter");

    MBLlmPlan plan = plan_make("modern-house", "modern", 15, 11, 5, 2,
                               "Deterministic House");
    plan.scale = "medium";

    MBBuildResult* p = mb_generate(a, "determinism", 42u, &plan, NULL);
    MBBuildResult* q = mb_generate(a, "determinism", 42u, &plan, NULL);
    validate_build(a, "same seed", p);
    validate_build(a, "same seed", q);
    check("the same plan and seed reproduce the same build", blocks_equal(p, q),
          "blocks differ");

    MBBuildResult* s = mb_generate(a, "determinism", 43u, &plan, NULL);
    validate_build(a, "different seed", s);
    check("a different seed produces a variant", !blocks_equal(p, s),
          "blocks are identical");
}

static void test_llm_extraction(Arena* a)
{
    section("model response extraction and validation");

    MBLlmError err;
    memset(&err, 0, sizeof err);

    const char* fenced =
        "\xe5\xa5\xbd\xe7\x9a\x84\xef\xbc\x8c\xe8\xbf\x99\xe6\x98\xaf\xe8\xa7\x84"
        "\xe5\x88\x92\xe7\xbb\x93\xe6\x9e\x9c\xef\xbc\x9a\n"
        "```json\n"
        "{\"structure\":\"castle-tower\",\"style\":\"medieval\",\"scale\":\"large\","
        "\"width\":14,\"depth\":14,\"height\":30,\"floors\":3,"
        "\"palette\":{\"wall\":\"stone_bricks\",\"roof\":\"minecraft:deepslate_bricks\"},"
        "\"name\":\"\xe9\xbb\x91\xe7\x9f\xb3\xe5\xa1\x94\"}\n"
        "```\n"
        "\xe9\x9c\x80\xe8\xa6\x81\xe6\x88\x91\xe5\x86\x8d\xe8\xb0\x83\xe6\x95\xb4\xe5\x90\x97\xef\xbc\x9f";

    MBJson* raw = mb_extract_json_object(a, fenced, &err);
    check("JSON is found inside a fenced code block", raw != NULL, err.message);

    MBLlmPlan plan;
    memset(&plan, 0, sizeof plan);
    if (raw) plan = mb_normalize_plan(a, raw, &err);
    expect_str("structure survives normalisation", plan.structure, "castle-tower");
    expect_str("style survives normalisation", plan.style, "medieval");
    expect_str("scale survives normalisation", plan.scale, "large");
    expect_int("height survives normalisation", plan.height, 30);
    expect_int("floors survive normalisation", plan.floors, 3);
    expect_str("a bare block name gains the minecraft: prefix",
               plan.palette.wall, "minecraft:stone_bricks");
    expect_str("a prefixed block name is kept",
               plan.palette.roof, "minecraft:deepslate_bricks");
    expect_str("the model's own name is kept", plan.name, "\xe9\xbb\x91\xe7\x9f\xb3\xe5\xa1\x94");

    check("prose without an object yields NULL",
          mb_extract_json_object(a, "sorry, I cannot help with that", &err) == NULL, "");
    check("an unbalanced object yields NULL",
          mb_extract_json_object(a, "{\"a\":1", &err) == NULL, "");
    check("a trailing comma is repaired",
          mb_extract_json_object(a, "{\"a\":1,}", &err) != NULL, "");
    check("braces inside a string do not confuse the scanner",
          mb_extract_json_object(a, "{\"name\":\"a}b\"}", &err) != NULL, "");

    /* Everything below is deliberately wrong; the validator has to tame it. */
    const char* junk =
        "{\"structure\":\"nope\",\"style\":\"nope\",\"scale\":\"HUGE\","
        "\"width\":999,\"depth\":-20,\"height\":500,\"floors\":9,"
        "\"palette\":{\"wall\":\"not_a_real_block\",\"roof\":42,\"glass\":\"glass\"},"
        "\"name\":\"\xe8\xb6\x85\xe9\x95\xbf\xe5\x90\x8d\xe5\xad\x97\xe8\xb6\x85"
        "\xe9\x95\xbf\xe5\x90\x8d\xe5\xad\x97\xe8\xb6\x85\xe9\x95\xbf\xe5\x90\x8d"
        "\xe5\xad\x97\","
        "\"keywords\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\"],"
        "\"modifiers\":null,\"notes\":\"not-an-array\"}";

    MBJson* junkJson = mb_json_parse(a, junk, strlen(junk));
    check("the junk payload parses as JSON", junkJson != NULL, "parser rejected the literal");
    MBLlmPlan tamed;
    memset(&tamed, 0, sizeof tamed);
    if (junkJson) tamed = mb_normalize_plan(a, junkJson, &err);
    expect_str("an unknown structure falls back to the first template",
               tamed.structure, MB_STRUCTURES[0].id);
    expect_str("an unknown style falls back to the first style",
               tamed.style, MB_STYLES[0].id);
    expect_str("an unknown scale falls back to medium", tamed.scale, "medium");
    expect_int("width is clamped to 48", tamed.width, 48);
    expect_int("depth is clamped to 3", tamed.depth, 3);
    expect_int("height is clamped to 40", tamed.height, 40);
    expect_int("floors are clamped to 4", tamed.floors, 4);
    check("an unknown block name is dropped", tamed.palette.wall == NULL,
          tamed.palette.wall ? tamed.palette.wall : "");
    check("a non-string block name is dropped", tamed.palette.roof == NULL,
          tamed.palette.roof ? tamed.palette.roof : "");
    expect_str("a valid block name is kept", tamed.palette.glass, "minecraft:glass");
    check("the name is truncated to 12 UTF-16 units", mb_u16_len(tamed.name) <= 12, "too long");
    expect_int("keywords are capped at 8", tamed.keywordCount, 8);
    expect_int("a non-array modifiers field becomes empty", tamed.modifierCount, 0);
    expect_int("a non-array notes field becomes empty", tamed.noteCount, 0);

    MBJson* emptyObj = mb_json_parse(a, "{}", 2);
    check("an empty object parses", emptyObj != NULL, "parser rejected {}");
    MBLlmPlan defaults;
    memset(&defaults, 0, sizeof defaults);
    if (emptyObj) defaults = mb_normalize_plan(a, emptyObj, &err);
    expect_int("a missing width defaults to 15", defaults.width, 15);
    expect_int("a missing depth defaults to 11", defaults.depth, 11);
    expect_int("a missing height defaults to 7", defaults.height, 7);
    expect_int("missing floors default to 1", defaults.floors, 1);
    expect_str("a missing structure defaults to the first template",
               defaults.structure, MB_STRUCTURES[0].id);
}

static void test_llm_build(Arena* a)
{
    section("model plan to building");

    MBLlmError err;
    memset(&err, 0, sizeof err);

    const char* json =
        "{\"structure\":\"castle-tower\",\"style\":\"medieval\",\"scale\":\"large\","
        "\"width\":16,\"depth\":16,\"height\":28,\"floors\":3,"
        "\"palette\":{\"wall\":\"stone_bricks\",\"accent\":\"white_concrete\"},"
        "\"name\":\"\xe7\x81\xb0\xe7\x9f\xb3\xe5\xa1\x94\","
        "\"keywords\":[\"\xe5\x9f\x8e\xe5\xa0\xa1\",\"\xe5\xa1\x94\xe6\xa5\xbc\"],"
        "\"modifiers\":[\"\xe5\xa4\x9a\xe5\xb1\x82\xe7\xbb\x93\xe6\x9e\x84\"],"
        "\"notes\":[\"\xe4\xb8\x89\xe5\xb1\x82\xe7\x9f\xb3\xe5\xa1\x94\xef\xbc\x8c"
        "\xe5\xb8\xa6\xe9\x9b\x89\xe5\xa0\x8d\xe3\x80\x82\"]}";

    MBJson* raw = mb_json_parse(a, json, strlen(json));
    MBLlmPlan plan;
    memset(&plan, 0, sizeof plan);
    if (raw) plan = mb_normalize_plan(a, raw, &err);
    check("the model plan normalises", raw != NULL && plan.structure != NULL, err.message);

    MBLlmTrace trace;
    memset(&trace, 0, sizeof trace);
    trace.model = "stub-model";
    trace.endpoint = "api.example.com";
    trace.latencyMs = 123;

    const char* prompt = "\xe7\xbb\x99\xe6\x88\x91\xe9\x80\xa0\xe4\xb8\x80\xe5\xba\xa7"
                         "\xe7\x81\xb0\xe8\x89\xb2\xe7\x9f\xb3\xe5\xa1\x94";

    MBBuildResult* ai = mb_generate(a, prompt, 4242u, &plan, &trace);
    validate_build(a, "llm plan", ai);
    if (!ai) return;

    check("the report carries the model name",
          ai->report.hasLlmUser && strcmp(ai->report.llm.model, "stub-model") == 0,
          ai->report.llm.model);
    expect_str("the report carries the endpoint", ai->report.llm.endpoint, "api.example.com");
    expect_int("the report carries the latency", ai->report.llm.latencyMs, 123);
    expect_str("the model chose the template", ai->report.structure, "castle-tower");
    expect_str("the model chose the scale", ai->report.scale, "\xe5\xa4\xa7\xe5\x9e\x8b");
    check("the model's name is used",
          ai->name != NULL &&
          strstr(ai->name, "\xe7\x81\xb0\xe7\x9f\xb3\xe5\xa1\x94") != NULL,
          ai->name ? ai->name : "(null)");
    expect_int("keywords are passed through", ai->report.keywordCount, 2);
    check("keyword text is passed through",
          ai->report.keywordCount == 2 &&
          strcmp(ai->report.keywords[0], "\xe5\x9f\x8e\xe5\xa0\xa1") == 0 &&
          strcmp(ai->report.keywords[1], "\xe5\xa1\x94\xe6\xa5\xbc") == 0,
          "keyword mismatch");
    /* The two boilerplate notes come first; the model's own note is appended. */
    expect_int("the report keeps the plan notes plus boilerplate",
               ai->report.noteCount, plan.noteCount + 2);
    bool sawNote = false;
    for (int i = 0; i < ai->report.noteCount; i++) {
        if (ai->report.notes[i] &&
            strstr(ai->report.notes[i], "\xe4\xb8\x89\xe5\xb1\x82\xe7\x9f\xb3\xe5\xa1\x94") != NULL) {
            sawNote = true;
        }
    }
    check("notes reach the report", sawNote, "note missing");
    check("the model's wall material is used",
          build_uses_material(ai, "minecraft:stone_bricks"), "stone_bricks absent");
    check("the model's accent material is used",
          build_uses_material(ai, "minecraft:white_concrete"), "white_concrete absent");

    /* Drop the palette: the style defaults must kick in and change the result. */
    MBLlmPlan bare = plan;
    bare.palette.wall = NULL;
    bare.palette.accent = NULL;
    bare.palette.roof = NULL;
    bare.palette.floor = NULL;
    bare.palette.frame = NULL;
    bare.palette.glass = NULL;
    bare.palette.light = NULL;
    bare.palette.nature = NULL;
    MBBuildResult* bareBuild = mb_generate(a, prompt, 4242u, &bare, &trace);
    check("an empty palette falls back to the style defaults",
          bareBuild != NULL && !blocks_equal(ai, bareBuild),
          "identical to the explicit plan");

    MBBuildResult* again = mb_generate(a, prompt, 4242u, &plan, &trace);
    check("the same plan reproduces the same build", blocks_equal(ai, again),
          "blocks differ between runs");
}

static void test_exporters(Arena* a, const MBBuildResult* r)
{
    section("exporters");

    check("a build is available for export", r != NULL, "missing build");
    if (!r) return;

    char detail[128];

    char* mc = mb_export_mcfunction(a, r, "builder");
    check("mcfunction exporter returned text", mc != NULL, "NULL result");
    if (!mc) return;

    snprintf(detail, sizeof detail, "counted %d",
             count_prefix_lines(mc, "setblock "));
    check("mcfunction emits one setblock per block",
          count_prefix_lines(mc, "setblock ") == (int)r->blockCount, detail);
    check("mcfunction has no NaN coordinate", strstr(mc, "NaN") == NULL, "");
    check("mcfunction starts with a comment and the build name",
          strncmp(mc, "# ", 2) == 0 && strstr(mc, r->name) != NULL, "missing header");
    snprintf(detail, sizeof detail, "# \xe5\x85\xb1 %zu \xe4\xb8\xaa\xe6\x96\xb9\xe5\x9d\x97",
             r->blockCount);
    check("mcfunction ends with the block tally", strstr(mc, detail) != NULL, detail);

    char* json = mb_export_json(a, r);
    check("json exporter returned text", json != NULL, "NULL result");
    MBJson* back = json ? mb_json_parse(a, json, strlen(json)) : NULL;
    check("the exported JSON is valid JSON", back != NULL, "re-parse failed");
    if (back) {
        MBJson* format = mb_json_get(back, "format");
        MBJson* version = mb_json_get(back, "version");
        MBJson* blocks = mb_json_get(back, "blocks");
        MBJson* planner = mb_json_get(back, "planner");
        MBJson* engine = planner ? mb_json_get(planner, "engine") : NULL;
        expect_str("JSON format marker", format ? format->u.str.ptr : NULL,
                   "minecraft-builder/plan");
        check("JSON version is 1", version && version->type == MB_JSON_NUM &&
              (int)version->u.num == 1, "wrong version");
        check("JSON carries every block",
              blocks && blocks->type == MB_JSON_ARR &&
              (int)blocks->u.arr.count == (int)r->blockCount, "block count mismatch");
        check("JSON carries the planner report", planner && planner->type == MB_JSON_OBJ,
              "planner section missing");
        expect_str("JSON reports the llm engine", engine ? engine->u.str.ptr : NULL, "llm");
    }

    char* csv = mb_export_csv(a, r);
    check("csv exporter returned text", csv != NULL, "NULL result");
    expect_int("CSV has a header, one row per material and a total",
               csv ? count_rows(csv) : -1, r->byMaterialCount + 2);

    char* md = mb_export_markdown(a, r);
    check("markdown exporter returned text", md != NULL, "NULL result");
    if (!md) return;

    check("markdown has a material section",
          strstr(md, "## \xe6\x9d\x90\xe6\x96\x99\xe6\xb8\x85\xe5\x8d\x95") != NULL,
          "missing section heading");
    check("markdown has a material table",
          strstr(md, "| \xe6\x96\xb9\xe5\x9d\x97 |") != NULL, "missing table header");
    check("markdown names the build", strstr(md, r->name) != NULL, "missing name");

    snprintf(detail, sizeof detail, "\xe5\x90\x88\xe8\xae\xa1,,,%d,100.00%%,",
             r->total);
    check("CSV ends with the 100% total row", csv && strstr(csv, detail) != NULL, detail);

    expect_str("slugify lowercases and hyphenates", mb_slugify(a, "Modern Villa 15x11"),
               "modern-villa-15x11");
    check("slugify falls back for a non-ASCII title",
          strncmp(mb_slugify(a, "\xe7\x8e\xb0\xe4\xbb\xa3\xe5\x88\xab\xe5\xa2\x85"),
                  "build-", 6) == 0,
          "no fallback prefix");
}

static void test_helpers(Arena* a)
{
    section("helpers, urls and presets");

    /* Math.round rounds halves towards +Infinity. */
    expect_int("js_round(2.5)", mb_js_round(2.5), 3);
    expect_int("js_round(-2.5)", mb_js_round(-2.5), -2);
    expect_int("js_round(0.5)", mb_js_round(0.5), 1);
    expect_int("js_round(-0.5)", mb_js_round(-0.5), 0);
    expect_int("js_round(-2.6)", mb_js_round(-2.6), -3);
    {
        volatile double zero = 0.0;
        expect_int("js_round(NaN)", mb_js_round(zero / zero), 0);
    }

    expect_int("FNV-1a of the empty string", (long)mb_hash_string(""),
               (long)(uint32_t)2166136261u);
    expect_int("FNV-1a of \"a\"", (long)mb_hash_string("a"),
               (long)(uint32_t)3826002220u); /* 0xe40c292c */

    /* The seed shown for the application's default prompt. It pins down that
     * the hash walks UTF-16 code units, not bytes. */
    const char* defPrompt =
        "\xe5\xb8\xae\xe6\x88\x91\xe5\xbb\xba\xe4\xb8\x80\xe5\xba\xa7\xe7\x8e\xb0"
        "\xe4\xbb\xa3\xe9\xa3\x8e\xe6\xa0\xbc\xe7\x9a\x84\xe4\xb8\xa4\xe5\xb1\x82"
        "\xe5\x88\xab\xe5\xa2\x85\xef\xbc\x8c\xe7\x99\xbd\xe5\xa2\x99\xe5\xa4\xa7"
        "\xe8\x90\xbd\xe5\x9c\xb0\xe7\xaa\x97\xef\xbc\x8c\xe5\xb0\xba\xe5\xaf\xb8 15x11";
    expect_int("FNV-1a over UTF-16 units", (long)mb_hash_string(defPrompt),
               (long)(uint32_t)2810436605u);
    expect_int("UTF-16 length of a mixed string",
               (long)mb_u16_len("\x61\xe4\xb8\xad\x62"), 3);
    expect_int("terminal width counts CJK cells",
               mb_display_width("A\xe4\xb8\xad" "B"), 4);
    expect_int("terminal width ignores ANSI colours",
               mb_display_width("\x1b[38;2;1;2;3m\xe4\xb8\xad\x1b[0m"), 2);
    {
        size_t n = 0;
        U16* u = mb_u16_from_utf8(a, "A\xe4\xb8\xad" "B", &n);
        expect_int("UTF-8 to UTF-16 code unit count", (long)n, 3);
        expect_str("UTF-16 back to UTF-8 round trips",
                   mb_utf8_from_u16(a, u, n), "A\xe4\xb8\xad" "B");
    }
    {
        SBuf clipped;
        mb_sb_init(&clipped, a);
        int width = mb_sb_puts_clip(&clipped, "ab\xe4\xb8\xad" "cd", 7, 3);
        expect_int("clipping never emits half a CJK glyph", width, 2);
        expect_str("clipping stops before an oversized CJK glyph",
                   mb_sb_done(&clipped), "ab");
    }

    expect_int("clamp above the range", mb_clamp_int(99, 0, 10), 10);
    expect_int("clamp below the range", mb_clamp_int(-5, 0, 10), 0);
    expect_str("trim removes surrounding whitespace", mb_trim_dup(a, "  hello \t\n"), "hello");
    check("case insensitive compare", mb_ieq("DeEpSeEk", "deepseek"), "not equal");
    check("case insensitive contains", mb_contains_ci("DeepSeek Chat", "seek"),
          "substring not found");

    expect_str("url_join trims a trailing slash",
               mb_url_join(a, "https://api.deepseek.com/v1/", "chat/completions"),
               "https://api.deepseek.com/v1/chat/completions");
    expect_str("url_join adds a leading slash",
               mb_url_join(a, "https://api.openai.com/v1", "chat/completions"),
               "https://api.openai.com/v1/chat/completions");
    expect_str("url_host extracts the host",
               mb_url_host(a, "https://api.deepseek.com/v1/chat/completions"),
               "api.deepseek.com");

    MBLlmConfig cfg;
    mb_llm_default_config(&cfg);
    check("the default config points at deepseek",
          strstr(cfg.baseUrl, "deepseek") != NULL, cfg.baseUrl);
    check("the default config has a model", cfg.model[0] != '\0', "empty model");
    check("the default config asks for json mode", cfg.jsonMode, "json mode off");
    check("the default temperature is sane",
          cfg.temperature > 0.0 && cfg.temperature <= 2.0, "out of range");

    expect_int("seven provider presets", MB_LLM_PRESET_COUNT, 7);
    bool presets_complete = true;
    for (int i = 0; i < MB_LLM_PRESET_COUNT; i++) {
        const MBLlmPreset* p = &MB_LLM_PRESETS[i];
        if (!p->id || !*p->id || !p->label || !*p->label) presets_complete = false;
        /* "custom" is the only preset allowed to have an empty endpoint. */
        if (strcmp(p->id, "custom") != 0 && (!p->baseUrl || !*p->baseUrl)) presets_complete = false;
        if (strcmp(p->id, "custom") != 0 && (!p->model || !*p->model)) presets_complete = false;
    }
    check("every preset is fully described", presets_complete, "an incomplete preset was found");

    check("redaction removes the api key",
          strstr(mb_redact(a, "failed with key sk-abcdefghijklmn in the header",
                           "sk-abcdefghijklmn"), "sk-abcdefghijklmn") == NULL,
          "the key leaked");
    check("redaction keeps the rest of the message",
          strstr(mb_redact(a, "failed with key sk-abcdefghijklmn", "sk-abcdefghijklmn"),
                 "failed with key") != NULL,
          "the message was destroyed");

    check("a known material is recognised", mb_material_known("stone_bricks"), "");
    check("the minecraft: prefix is accepted", mb_material_known("minecraft:glass"), "");
    check("an unknown material is rejected", !mb_material_known("unobtainium"), "");
    expect_str("an unknown material falls back to magenta",
               mb_material_color("unobtainium") == 0xFF00FFu ? "#ff00ff" : "#000000",
               "#ff00ff");
    check("materials carry a display name",
          mb_material_name("minecraft:stone_bricks")[0] != '\0', "empty name");

    /* Every catalogue entry must have a usable one-line description and a
     * category label: the UI tooltip, the exports and the README table all
     * depend on them. */
    {
        bool all_described = true;
        bool all_categorised = true;
        for (int i = 0; i < MB_MATERIAL_COUNT; i++) {
            const MBMaterial* m = &MB_MATERIALS[i];
            if (!m->desc || !m->desc[0]) all_described = false;
            if (!mb_material_category_name(m->category)[0]) all_categorised = false;
        }
        check("every material has a description", all_described, "empty desc");
        check("every category has a label", all_categorised, "empty category label");
    }
    check("an unknown material falls back to an empty description",
          mb_material_desc("unobtainium")[0] == '\0', "unexpected desc");

    /* Without an extracted vanilla pack every lookup must fail gracefully. */
    {
        unsigned char rgb[16 * 16 * 3];
        check("texture lookup without a pack fails cleanly",
              !mb_textures_load("__no_such_block__", rgb), "unexpected texture");
    }
}

/* ========================================================================== */

/*
 * The settings store is the only module that touches the user profile, so it is
 * tested through the explicit-path variants. That keeps `mingw32-make test`
 * hermetic: no real %APPDATA%\MinecraftBuilder\settings.ini is ever touched.
 */
static void test_settings_store(void)
{
    section("persisted model settings");

    char defaultPath[512];
    check("the default settings path resolves",
          mb_config_path(defaultPath, sizeof(defaultPath))
              && strstr(defaultPath, "settings.ini") != NULL,
          "mb_config_path returned nothing usable");

    const char* path = "build/test-settings.ini";
    remove(path);

    MBLlmConfig cfg;
    mb_llm_default_config(&cfg);
    snprintf(cfg.baseUrl, sizeof(cfg.baseUrl), "%s", "https://example.test/v1");
    snprintf(cfg.model, sizeof(cfg.model), "%s", "model-x");
    snprintf(cfg.apiKey, sizeof(cfg.apiKey), "%s", "sk-test=with=equals");
    cfg.temperature = 0.45;
    cfg.jsonMode = false;

    check("settings save reports success", mb_config_save_file(path, &cfg), "save failed");

    MBLlmConfig loaded;
    mb_llm_default_config(&loaded);
    loaded.temperature = 1.0;
    loaded.jsonMode = true;
    check("settings load reports success", mb_config_load_file(path, &loaded), "load failed");
    expect_str("the base URL survives a round trip", loaded.baseUrl, "https://example.test/v1");
    expect_str("the model id survives a round trip", loaded.model, "model-x");
    expect_str("the API key survives a round trip", loaded.apiKey, "sk-test=with=equals");
    check("temperature survives a round trip",
          loaded.temperature > 0.449 && loaded.temperature < 0.451, "wrong temperature");
    check("json mode survives a round trip", !loaded.jsonMode, "json mode flipped");

    check("a missing file does not fabricate settings",
          !mb_config_load_file("build/does-not-exist.ini", &loaded),
          "expected a missing file to fail");

    FILE* f = fopen(path, "wb");
    check("the settings file can be rewritten by hand", f != NULL, "fopen failed");
    if (f) {
        fputs("# comment\r\n"
              "\r\n"
              "base_url=  https://trimmed.test/v1  \r\n"
              "temperature=9\r\n"
              "json_mode=on\r\n"
              "oops-no-equals\r\n"
              "unknown_key=ignored\r\n"
              "model = qwen2.5:7b\r\n", f);
        fclose(f);

        MBLlmConfig loose;
        memset(&loose, 0, sizeof loose);
        check("a hand-edited file still loads", mb_config_load_file(path, &loose), "load failed");
        expect_str("whitespace around values is trimmed", loose.baseUrl,
                   "https://trimmed.test/v1");
        expect_str("whitespace around keys is trimmed", loose.model, "qwen2.5:7b");
        check("out-of-range temperature is clamped", loose.temperature == 2.0,
              "temperature was not clamped");
        check("textual json_mode values are accepted", loose.jsonMode, "on was not recognised");
    }

    remove(path);
}

/* ========================================================================== */

/*
 * The 3D preview is headless by design: the rasteriser never touches a window
 * and writes into a plain pixel buffer, so the software half of it can be
 * checked the same way as everything else. What matters here is that a frame
 * really is produced, that it is reproducible, and that the BMP the command
 * line writes is a structurally valid file.
 */
static void test_preview(Arena* a, const MBBuildResult* r)
{
    section("3d preview and bmp output");

    check("a build is available for preview", r != NULL, "missing build");
    if (!r) return;

    MBCamera cam;
    mb_camera_default(&cam, r);
    check("the default fov is sane", cam.fov > 0.10 && cam.fov < 3.0, "fov out of range");
    check("the default pitch looks down a little",
          cam.pitch < 0.0 && cam.pitch > -1.45, "pitch out of range");
    {
        double cx = r->size.x * 0.5, cy = r->size.y * 0.5, cz = r->size.z * 0.5;
        double dx = cam.x - cx, dy = cam.y - cy, dz = cam.z - cz;
        double d2 = dx * dx + dy * dy + dz * dz;
        double need = (double)r->size.x;
        if ((double)r->size.y > need) need = (double)r->size.y;
        if ((double)r->size.z > need) need = (double)r->size.z;
        check("the camera sits outside the model", d2 > need * need,
              "the camera ended up inside the bounding box");
    }

    MBSceneOptions opt;
    mb_scene_default(&opt);
    check("the grid is on by default", opt.showGrid, "grid off");

    enum { W = 240, H = 160 };
    MBFramebuffer fb;
    mb_fb_init(&fb, a, W, H);
    mb_render_scene(a, &fb, r, &cam, &opt);

    /* Collect coarse colour buckets; a frame with sky, ground and a textured
     * building has far more than a handful. */
    static unsigned char seen[4096];
    memset(seen, 0, sizeof seen);
    size_t distinct = 0;
    long drawn = 0;
    for (int i = 0; i < W * H; i++) {
        uint32_t c = fb.color[i];
        size_t key = (size_t)(((c >> 16) & 0xFFu) * 31u + ((c >> 8) & 0xFFu) * 7u
                              + (c & 0xFFu)) & 4095u;
        if (!seen[key]) { seen[key] = 1; distinct++; }
        if (fb.depth[i] > 0.0) drawn++;
    }
    check("the frame contains many distinct colours", distinct > 64,
          "the frame looks flat");
    check("most of the frame is covered by geometry",
          drawn > (long)(W * H) / 4, "almost nothing was drawn");

    /* Rendering the same scene twice must give the same picture. */
    MBFramebuffer again;
    mb_fb_init(&again, a, W, H);
    mb_render_scene(a, &again, r, &cam, &opt);
    check("rendering is reproducible",
          memcmp(fb.color, again.color, (size_t)W * H * sizeof(uint32_t)) == 0,
          "two renders of the same camera differ");

    /* The BMP writer has to produce a 24bpp file with a 4-byte row stride. */
    size_t stride = ((size_t)W * 3u + 3u) & ~(size_t)3u;
    expect_int("bmp size includes a padded row stride",
               (long)mb_bmp_size(&fb), (long)(54u + stride * (size_t)H));

    size_t bytes = mb_bmp_size(&fb);
    unsigned char* out = (unsigned char*)mb_arena_alloc(a, bytes);
    mb_bmp_encode(&fb, out);
    expect_int("bmp starts with the 'B' magic", out[0], 'B');
    expect_int("bmp continues with the 'M' magic", out[1], 'M');
    expect_int("bmp header size is 54", (int)(out[10] | (out[11] << 8)), 54);
    expect_int("bmp width", (int)(out[18] | (out[19] << 8)), W);
    expect_int("bmp height", (int)(out[22] | (out[23] << 8)), H);
    expect_int("bmp is 24bpp", (int)(out[28] | (out[29] << 8)), 24);
    check("bmp rows are padded to four bytes",
          stride % 4u == 0u && stride >= (size_t)W * 3u, "bad row stride");
}

/* ========================================================================== */

int main(void)
{
    Arena arena;
    mb_arena_init(&arena);

    printf("Minecraft Builder - C edition test suite\n");
    printf("materials: %d   structures: %d   styles: %d\n",
           MB_MATERIAL_COUNT, MB_STRUCTURE_COUNT, MB_STYLE_COUNT);

    /* One shared, plan-generated fixture for the exporter and preview tests. */
    MBLlmPlan fixturePlan = plan_make("modern-house", "modern", 15, 11, 5, 2,
                                      "Export Fixture");
    MBBuildResult* fixture = mb_generate(&arena, "export fixture", 20260101u,
                                         &fixturePlan, NULL);
    validate_build(&arena, "export fixture", fixture);

    test_plan_invariants(&arena);
    test_fixtures(&arena);
    test_plan_semantics(&arena);
    test_plan_determinism(&arena);
    test_llm_extraction(&arena);
    test_llm_build(&arena);
    test_exporters(&arena, fixture);
    test_helpers(&arena);
    test_settings_store();
    test_preview(&arena, fixture);

    printf("\n%s  %d checks, %d failures\n",
           g_failures == 0 ? "[PASS]" : "[FAIL]", g_checks, g_failures);

    mb_arena_destroy(&arena);
    return g_failures == 0 ? 0 : 1;
}
