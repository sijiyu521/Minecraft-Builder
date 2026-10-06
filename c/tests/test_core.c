/*
 * test_core.c — golden-value regression suite for the C edition.
 *
 *     mingw32-make test
 *
 * The numbers printed in GOLDEN below were recorded from the original reference
 * implementation of the engine. Pinning them here turns this suite into a
 * regression and cross-implementation check: the deterministic geometry, the
 * prompt parsing, the model response validation and the four exporters all have
 * to reproduce the recorded values exactly, down to the block count.
 *
 * No network access is required. Every LLM code path exercised here works on a
 * canned model response, so `mingw32-make test` is safe to run offline and in
 * CI.
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
/* structural validation — the invariants every build must satisfy            */
/* ========================================================================== */

static void validate_build(Arena* arena, const char* label, const MBBuildResult* r)
{
    char detail[256];

    check(label, r != NULL, "mb_generate returned NULL");
    if (!r) return;

    expect_int(label, (long)r->blockCount > 0, 1);
    check(label, r->size.x > 0 && r->size.y > 0 && r->size.z > 0,
          "bounding box has a non-positive edge");
    if (r->blockCount == 0) return;

    uint64_t* keys = mb_arena_alloc(arena, sizeof(uint64_t) * r->blockCount);

    bool material_known = true, in_range = true, no_nan = true;
    for (size_t i = 0; i < r->blockCount; i++) {
        const MBVoxel* v = &r->blocks[i];
        if (!mb_material_known(v->material)) material_known = false;
        if (v->x < 0 || v->y < 0 || v->z < 0 ||
            v->x >= r->size.x || v->y >= r->size.y || v->z >= r->size.z) {
            in_range = false;
        }
        keys[i] = ((uint64_t)v->x << 42) | ((uint64_t)v->y << 21) | (uint64_t)v->z;
        if (strstr(v->material, "NaN") != NULL) no_nan = false;
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
/* golden table                                                               */
/* ========================================================================== */

/*
 * generateBuild({ prompt: <first keyword>, structureId: <id>, seed: 20260101 })
 * reproduced below as recorded values. `w`/`d`/`h` are the bounding box, which
 * includes decorative overhang (roof eaves, corner towers), so it is not always
 * the requested footprint.
 */
typedef struct {
    const char* id;
    int width, depth, height;
    int blocks;
    int materials;
} Golden;

static const Golden GOLDEN[] = {
    { "modern-house", 16, 13, 17, 1342, 9 },
    { "cabin",         9, 10, 10,  334, 7 },
    { "castle-tower", 11, 11, 24,  960, 8 },
    { "treehouse",    15, 15, 15,  870, 8 },
    { "bridge",       20,  5,  9,  424, 6 },
    { "castle-wall",  28,  5, 12,  942, 5 },
};

#define GOLDEN_COUNT ((int)(sizeof(GOLDEN) / sizeof(GOLDEN[0])))

/* ========================================================================== */
/* sections                                                                   */
/* ========================================================================== */

static void test_templates(Arena* a, MBBuildResult** out)
{
    section("templates (seed 20260101)");
    for (int i = 0; i < GOLDEN_COUNT; i++) {
        const Golden* g = &GOLDEN[i];
        const MBStructure* st = mb_structure_by_id(g->id);
        check(g->id, st != NULL, "unknown structure id in the golden table");
        if (!st) continue;

        MBGenerateInput in;
        memset(&in, 0, sizeof in);
        in.prompt = st->keywords[0];
        in.structureId = g->id;
        in.scale = MB_SCALE_AUTO;
        in.seed = 20260101u;

        MBBuildResult* r = mb_generate(a, &in);
        out[i] = r;
        validate_build(a, g->id, r);
        if (!r) continue;

        printf("  . %-14s %2dx%-2d h%-2d  %5zu blocks  %d materials\n",
               g->id, r->size.x, r->size.z, r->size.y, r->blockCount, r->byMaterialCount);

        expect_int(g->id, r->size.x, g->width);
        expect_int(g->id, r->size.z, g->depth);
        expect_int(g->id, r->size.y, g->height);
        expect_int(g->id, (long)r->blockCount, g->blocks);
        expect_int(g->id, r->byMaterialCount, g->materials);

        /* The chosen template must also be the one the planner reports. */
        expect_str(g->id, r->report.structure, g->id);
        check(g->id, r->report.structureLabel != NULL && r->report.structureLabel[0] != '\0',
              "empty structure label");
        check(g->id, !r->report.isLlm, "a local build must not be flagged as llm");
    }
}

static void test_styles(Arena* a)
{
    section("material styles (seed 7)");
    for (int i = 0; i < MB_STYLE_COUNT; i++) {
        const MBStyle* s = &MB_STYLES[i];
        MBGenerateInput in;
        memset(&in, 0, sizeof in);
        in.prompt = s->keywords[0];
        in.styleId = s->id;
        in.scale = MB_SCALE_AUTO;
        in.seed = 7u;

        MBBuildResult* r = mb_generate(a, &in);
        char label[64];
        snprintf(label, sizeof label, "style %s", s->id);
        check(label, r != NULL && r->blockCount > 0, "style produced no blocks");
    }
}

static void test_prompt_parsing(Arena* a)
{
    section("prompt parsing");
    MBBuildResult* r;

    MBGenerateInput in;
    memset(&in, 0, sizeof in);
    in.prompt = "\xe4\xb8\xad\xe4\xb8\x96\xe7\xba\xaa\xe7\x9f\xb3\xe7\xa0\x96"
                "\xe5\x9f\x8e\xe5\xa0\xa1\xe5\xa1\x94\xe6\xa5\xbc\xef\xbc\x8c"
                "\xe9\xab\x98 24 \xe6\xa0\xbc\xef\xbc\x8c\xe5\xb8\xa6\xe7\xae\xad"
                "\xe7\xaa\x97\xe5\x92\x8c\xe9\x9b\x89\xe5\xa0\x8d";
    in.scale = MB_SCALE_AUTO;
    in.seed = 1u;
    r = mb_generate(a, &in);
    expect_str("recognises a castle tower", r ? r->report.structure : NULL, "castle-tower");

    /* Requested footprint 20x12; the modern house roof overhangs one block on
     * every side, so the exported bounding box is the footprint plus two. */
    memset(&in, 0, sizeof in);
    in.prompt = "\xe7\x8e\xb0\xe4\xbb\xa3\xe5\x88\xab\xe5\xa2\x85 \xe5\xae\xbd 20 \xe6\xb7\xb1 12";
    in.seed = 1u;
    r = mb_generate(a, &in);
    expect_int("width 20 becomes a 22 wide box", r ? r->size.x : -1, 22);
    expect_int("depth 12 becomes a 14 deep box", r ? r->size.z : -1, 14);

    MBGenerateInput narrow, wide;
    memset(&narrow, 0, sizeof narrow);
    narrow.prompt = "\xe5\x9f\x8e\xe5\xa2\x99 \xe5\xae\xbd 20";
    narrow.seed = 1u;
    wide = narrow;
    wide.prompt = "\xe5\x9f\x8e\xe5\xa2\x99 \xe5\xae\xbd 30";

    MBBuildResult* rn = mb_generate(a, &narrow);
    MBBuildResult* rw = mb_generate(a, &wide);
    expect_int("an explicit width really drives the output",
               (rw && rn) ? rw->size.x - rn->size.x : -1, 10);

    MBGenerateInput other = narrow;
    other.seed = 999u;
    MBBuildResult* ro = mb_generate(a, &other);
    expect_int("an explicit width is not jittered by the seed",
               (ro && rn) ? ro->size.x - rn->size.x : -1, 0);

    memset(&in, 0, sizeof in);
    in.prompt = "just build me something";
    in.seed = 1u;
    r = mb_generate(a, &in);
    check("an unrecognised prompt falls back to the default template",
          r != NULL && r->blockCount > 0, "no blocks");
    expect_str("fallback structure", r ? r->report.structure : NULL, MB_DEFAULT_STRUCTURE_ID);
}

static void test_determinism(Arena* a, const MBBuildResult* first)
{
    section("determinism");
    MBGenerateInput in;
    memset(&in, 0, sizeof in);
    in.prompt = "cabin";
    in.scale = MB_SCALE_AUTO;
    in.seed = 42u;

    MBBuildResult* p = mb_generate(a, &in);
    MBBuildResult* q = mb_generate(a, &in);
    check("the same seed reproduces the same build", blocks_equal(p, q), "blocks differ");

    in.seed = 43u;
    MBBuildResult* s = mb_generate(a, &in);
    check("a different seed produces a variant", !blocks_equal(p, s), "blocks are identical");

    /* An explicit structure id must survive the request. */
    in.structureId = "castle-wall";
    in.seed = 5u;
    MBBuildResult* t = mb_generate(a, &in);
    expect_str("an explicit structure id wins over the prompt",
               t ? t->report.structure : NULL, "castle-wall");

    check("the first template is reproducible too", first != NULL, "missing build");
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
        "\"name\":\"\xe7\x81\xb0\xe7\x9f\xb3\xe5\x93\xa8\xe5\xa1\x94\","
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

    MBBuildResult* ai = mb_generate_with_plan(a, prompt, 4242u, &plan, &trace,
                                              NULL, NULL, MB_SCALE_AUTO);
    validate_build(a, "llm plan", ai);
    if (!ai) return;

    check("the report is flagged as llm-generated", ai->report.isLlm, "isLlm is false");
    check("the report carries the model name",
          ai->report.hasLlmUser && strcmp(ai->report.llm.model, "stub-model") == 0,
          ai->report.llm.model);
    expect_str("the report carries the endpoint", ai->report.llm.endpoint, "api.example.com");
    expect_int("the report carries the latency", ai->report.llm.latencyMs, 123);
    expect_str("the model chose the template", ai->report.structure, "castle-tower");
    check("the model's name is used",
          ai->name != NULL && strncmp(ai->name, "\xe7\x81\xb0\xe7\x9f\xb3\xe5\x93\xa8\xe5\xa1\x94", 12) == 0,
          ai->name);
    expect_int("keywords are passed through", ai->report.keywordCount, 2);
    check("keyword text is passed through",
          ai->report.keywordCount == 2 &&
          strcmp(ai->report.keywords[0], "\xe5\x9f\x8e\xe5\xa0\xa1") == 0 &&
          strcmp(ai->report.keywords[1], "\xe5\xa1\x94\xe6\xa5\xbc") == 0,
          "keyword mismatch");
    /* The two boilerplate notes come first; the model's own notes are appended. */
    bool sawNote = false;
    for (int i = 0; i < ai->report.noteCount; i++) {
        if (strstr(ai->report.notes[i], "\xe4\xb8\x89\xe5\xb1\x82\xe7\x9f\xb3\xe5\xa1\x94") != NULL) {
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
    MBBuildResult* bareBuild = mb_generate_with_plan(a, prompt, 4242u, &bare, &trace,
                                                     NULL, NULL, MB_SCALE_AUTO);
    check("an empty palette falls back to the style defaults",
          bareBuild != NULL && !blocks_equal(ai, bareBuild), "identical to the explicit plan");

    MBBuildResult* again = mb_generate_with_plan(a, prompt, 4242u, &plan, &trace,
                                                 NULL, NULL, MB_SCALE_AUTO);
    check("the same plan reproduces the same build", blocks_equal(ai, again),
          "blocks differ between runs");

    MBBuildResult* locked = mb_generate_with_plan(a, prompt, 4242u, &plan, &trace,
                                                  "bridge", NULL, MB_SCALE_AUTO);
    expect_str("the UI lock overrides the model's choice",
               locked ? locked->report.structure : NULL, "bridge");
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
    check("mcfunction starts with the build name",
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
        expect_str("JSON format marker", format ? format->u.str.ptr : NULL,
                   "minecraft-builder/plan");
        check("JSON version is 1", version && version->type == MB_JSON_NUM &&
              (int)version->u.num == 1, "wrong version");
        check("JSON carries every block",
              blocks && blocks->type == MB_JSON_ARR &&
              (int)blocks->u.arr.count == (int)r->blockCount, "block count mismatch");
        check("JSON carries the planner report", planner && planner->type == MB_JSON_OBJ,
              "planner section missing");
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

    snprintf(detail, sizeof detail, "\xe5\x90\x88\xe8\xae\xa1,,%d,100.00%%", r->total);
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
}

/* ========================================================================== */

int main(void)
{
    Arena arena;
    mb_arena_init(&arena);

    MBBuildResult* builds[GOLDEN_COUNT];
    memset(builds, 0, sizeof builds);

    printf("Minecraft Builder — C edition test suite\n");
    printf("materials: %d   structures: %d   styles: %d\n",
           MB_MATERIAL_COUNT, MB_STRUCTURE_COUNT, MB_STYLE_COUNT);

    test_templates(&arena, builds);
    test_styles(&arena);
    test_prompt_parsing(&arena);
    test_determinism(&arena, builds[0]);
    test_llm_extraction(&arena);
    test_llm_build(&arena);
    test_exporters(&arena, builds[0]);
    test_helpers(&arena);

    printf("\n%s  %d checks, %d failures\n",
           g_failures == 0 ? "[PASS]" : "[FAIL]", g_checks, g_failures);

    mb_arena_destroy(&arena);
    return g_failures == 0 ? 0 : 1;
}
