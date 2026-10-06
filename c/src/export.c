/*
 * export.c — turn a finished build into files a player can actually use.
 *
 *   .mcfunction  a datapack function, one setblock per block
 *   .json        a neutral, diffable plan (re-importable, no NBT gymnastics)
 *   .csv         a material shopping list spreadsheets understand
 *   .md          a human readable report, handy as a README next to the rest
 *
 * The JSON is written by hand rather than through a generic serialiser so the
 * key order and the two-space indentation are stable and diffable — the exact
 * byte layout is asserted by the golden tests.
 */
#include "mb.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ========================================================================== */
/* timestamps                                                                 */
/* ========================================================================== */

/* new Date().toISOString() — always UTC, always millisecond precision. */
static char* iso_now(Arena* a)
{
    time_t t = time(NULL);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    return mb_arena_printf(a, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                           g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                           g.tm_hour, g.tm_min, g.tm_sec);
}

/* new Date().toISOString().replace('T',' ').slice(0,19) */
static char* stamp_now(Arena* a)
{
    time_t t = time(NULL);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
             g.tm_hour, g.tm_min, g.tm_sec);
    return mb_arena_strdup(a, buf);
}

static const char* stripped(const char* id)
{
    return (strncmp(id, "minecraft:", 10) == 0) ? id + 10 : id;
}

static void json_str(SBuf* b, const char* s)
{
    mb_json_escape(b, s ? s : "");
}

/* Join a string array with a separator, or "无" when empty. */
static void join_or_none(SBuf* b, const char** items, int count)
{
    if (!items || count <= 0) {
        mb_sb_puts(b, "无");
        return;
    }
    for (int i = 0; i < count; i++) {
        if (i) mb_sb_puts(b, "、");
        mb_sb_puts(b, items[i]);
    }
}

/* ========================================================================== */
/* .mcfunction                                                                */
/* ========================================================================== */

char* mb_export_mcfunction(Arena* a, const MBBuildResult* r, const char* namespaceName)
{
    const char* ns = (namespaceName && namespaceName[0]) ? namespaceName : "builder";

    SBuf b;
    mb_sb_init(&b, a);
    mb_sb_printf(&b, "# %s\n", r->name);
    mb_sb_printf(&b, "# %s\n", r->summary);
    mb_sb_printf(&b, "# 由 Minecraft Builder (AI 建筑工作台) 生成于 %s\n", stamp_now(a));
    mb_sb_printf(&b, "# 尺寸: %d x %d x %d\n", r->size.x, r->size.y, r->size.z);
    mb_sb_printf(&b, "# 用法: 站在目标原点后执行 /function %s:build\n", ns);
    mb_sb_puts(&b, "# 提示: 生成的命令以 ~ ~ ~ 为相对原点。\n\n");

    for (size_t i = 0; i < r->blockCount; i++) {
        const MBVoxel* v = &r->blocks[i];
        mb_sb_printf(&b, "setblock ~%d ~%d ~%d minecraft:%s\n",
                     v->x, v->y, v->z, stripped(v->material));
    }

    mb_sb_printf(&b, "\n# 共 %llu 个方块", (unsigned long long)r->blockCount);
    return mb_sb_done(&b);
}

/* ========================================================================== */
/* .json                                                                      */
/* ========================================================================== */

static void json_string_array(SBuf* b, const char* indent, const char* key,
                              const char** items, int count)
{
    mb_sb_printf(b, "%s\"%s\": [", indent, key);
    for (int i = 0; i < count; i++) {
        if (i) mb_sb_puts(b, ", ");
        json_str(b, items[i]);
    }
    mb_sb_puts(b, "]");
}

char* mb_export_json(Arena* a, const MBBuildResult* r)
{
    SBuf b;
    mb_sb_init(&b, a);
    const MBPlannerReport* p = &r->report;

    mb_sb_puts(&b, "{\n");
    mb_sb_puts(&b, "  \"format\": \"minecraft-builder/plan\",\n");
    mb_sb_puts(&b, "  \"version\": 1,\n");
    mb_sb_printf(&b, "  \"generatedAt\": \"%s\",\n", iso_now(a));
    mb_sb_puts(&b, "  \"name\": ");
    json_str(&b, r->name);
    mb_sb_puts(&b, ",\n  \"summary\": ");
    json_str(&b, r->summary);

    mb_sb_puts(&b, ",\n  \"size\": {\n");
    mb_sb_printf(&b, "    \"x\": %d,\n    \"y\": %d,\n    \"z\": %d\n",
                 r->size.x, r->size.y, r->size.z);
    mb_sb_puts(&b, "  },\n");

    mb_sb_puts(&b, "  \"palette\": [");
    for (int i = 0; i < r->paletteCount; i++) {
        const MBMaterial* m = mb_material(r->palette[i]);
        mb_sb_puts(&b, i ? ",\n" : "\n");
        mb_sb_puts(&b, "    {\n      \"id\": ");
        json_str(&b, r->palette[i]);
        mb_sb_puts(&b, ",\n      \"name\": ");
        json_str(&b, m->name);
        mb_sb_printf(&b, ",\n      \"color\": \"#%06x\"\n    }", m->color & 0xFFFFFFu);
    }
    mb_sb_puts(&b, r->paletteCount ? "\n  ],\n" : "],\n");

    mb_sb_printf(&b, "  \"stats\": {\n    \"total\": %d,\n    \"byMaterial\": [",
                 r->total);
    for (int i = 0; i < r->byMaterialCount; i++) {
        mb_sb_puts(&b, i ? ",\n" : "\n");
        mb_sb_puts(&b, "      {\n        \"id\": ");
        json_str(&b, r->byMaterial[i].id);
        mb_sb_printf(&b, ",\n        \"count\": %d\n      }", r->byMaterial[i].count);
    }
    mb_sb_puts(&b, r->byMaterialCount ? "\n    ]\n  },\n" : "]\n  },\n");

    /* --- planner report -------------------------------------------------- */
    mb_sb_puts(&b, "  \"planner\": {\n");
    mb_sb_puts(&b, "    \"prompt\": ");
    json_str(&b, p->prompt);
    mb_sb_printf(&b, ",\n    \"structure\": \"%s\",\n    \"structureLabel\": ",
                 p->structure);
    json_str(&b, p->structureLabel);
    mb_sb_printf(&b, ",\n    \"scale\": \"%s\",\n    \"seed\": %u,\n",
                 p->scale, (unsigned)p->seed);
    json_string_array(&b, "    ", "keywords", p->keywords, p->keywordCount);
    mb_sb_puts(&b, ",\n");
    json_string_array(&b, "    ", "detectedMaterials", p->detectedMaterials,
                      p->detectedMaterialCount);
    mb_sb_puts(&b, ",\n");
    json_string_array(&b, "    ", "modifiers", p->modifiers, p->modifierCount);
    mb_sb_puts(&b, ",\n");
    json_string_array(&b, "    ", "notes", p->notes, p->noteCount);
    mb_sb_printf(&b, ",\n    \"engine\": \"%s\"", p->isLlm ? "llm" : "local-rule-engine");

    if (p->hasLlmUser) {
        mb_sb_puts(&b, ",\n    \"llm\": {\n      \"model\": ");
        json_str(&b, p->llm.model);
        mb_sb_puts(&b, ",\n      \"endpoint\": ");
        json_str(&b, p->llm.endpoint);
        mb_sb_printf(&b, ",\n      \"latencyMs\": %ld", p->llm.latencyMs);
        mb_sb_puts(&b, ",\n      \"reasoning\": [");
        for (int i = 0; i < p->llm.reasoningCount; i++) {
            if (i) mb_sb_puts(&b, ", ");
            json_str(&b, p->llm.reasoning[i]);
        }
        /* The web app puts the planner notes here so the trace is self
           contained; keep that behaviour. */
        if (p->llm.reasoningCount == 0) {
            for (int i = 0; i < p->noteCount; i++) {
                if (i) mb_sb_puts(&b, ", ");
                json_str(&b, p->notes[i]);
            }
        }
        mb_sb_puts(&b, "]\n    }");
    }
    mb_sb_puts(&b, "\n  },\n");

    /* --- blocks ---------------------------------------------------------- */
    mb_sb_puts(&b, "  \"blocks\": [");
    for (size_t i = 0; i < r->blockCount; i++) {
        const MBVoxel* v = &r->blocks[i];
        mb_sb_puts(&b, i ? ",\n    {\n" : "\n    {\n");
        mb_sb_printf(&b, "      \"x\": %d,\n      \"y\": %d,\n      \"z\": %d,\n",
                     v->x, v->y, v->z);
        mb_sb_puts(&b, "      \"material\": ");
        json_str(&b, v->material);
        mb_sb_puts(&b, "\n    }");
    }
    mb_sb_puts(&b, r->blockCount ? "\n  ]\n}\n" : "]\n}\n");

    return mb_sb_done(&b);
}

/* ========================================================================== */
/* .csv                                                                       */
/* ========================================================================== */

char* mb_export_csv(Arena* a, const MBBuildResult* r)
{
    SBuf b;
    mb_sb_init(&b, a);
    mb_sb_puts(&b, "方块ID,中文名,数量,占总量比例\n");

    for (int i = 0; i < r->byMaterialCount; i++) {
        const MBMaterialUsage* u = &r->byMaterial[i];
        double ratio = r->total == 0 ? 0.0 : ((double)u->count / (double)r->total) * 100.0;
        mb_sb_printf(&b, "%s,%s,%d,%.2f%%\n",
                     u->id, mb_material(u->id)->name, u->count, ratio);
    }

    mb_sb_printf(&b, "合计,,%d,100.00%%", r->total);
    return mb_sb_done(&b);
}

/* ========================================================================== */
/* .md                                                                        */
/* ========================================================================== */

char* mb_export_markdown(Arena* a, const MBBuildResult* r)
{
    SBuf b;
    mb_sb_init(&b, a);
    const MBPlannerReport* p = &r->report;

    mb_sb_printf(&b, "# %s\n\n%s\n\n", r->name, r->summary);

    mb_sb_puts(&b, "## 提示词解析\n\n");
    mb_sb_printf(&b, "- 原始提示词: %s\n", p->prompt);
    mb_sb_printf(&b, "- 建筑类型: %s (`%s`)\n", p->structureLabel, p->structure);
    mb_sb_printf(&b, "- 体量: %s\n", p->scale);
    mb_sb_printf(&b, "- 种子: %u\n", (unsigned)p->seed);
    mb_sb_puts(&b, "- 识别关键词: ");
    join_or_none(&b, p->keywords, p->keywordCount);
    mb_sb_puts(&b, "\n- 识别材质: ");
    join_or_none(&b, p->detectedMaterials, p->detectedMaterialCount);
    mb_sb_puts(&b, "\n- 修饰语: ");
    join_or_none(&b, p->modifiers, p->modifierCount);
    mb_sb_puts(&b, "\n\n");

    mb_sb_puts(&b, "## 尺寸\n\n");
    mb_sb_printf(&b, "- 宽 (X): %d\n", r->size.x);
    mb_sb_printf(&b, "- 高 (Y): %d\n", r->size.y);
    mb_sb_printf(&b, "- 深 (Z): %d\n", r->size.z);
    mb_sb_printf(&b, "- 方块总数: %d\n\n", r->total);

    mb_sb_puts(&b, "## 材料清单\n\n");
    mb_sb_puts(&b, "| 方块 | 中文名 | 数量 |\n| --- | --- | --- |\n");
    for (int i = 0; i < r->byMaterialCount; i++) {
        const MBMaterialUsage* u = &r->byMaterial[i];
        mb_sb_printf(&b, "| `%s` | %s | %d |\n", u->id, mb_material(u->id)->name, u->count);
    }
    mb_sb_putc(&b, '\n');

    return mb_sb_done(&b);
}

/* ========================================================================== */
/* slugify                                                                    */
/* ========================================================================== */

/*
 * Everything outside [a-z0-9] collapses into a single '-'. A Chinese name
 * therefore slugifies to nothing, which is why there is a time based fallback.
 */
char* mb_slugify(Arena* a, const char* input)
{
    SBuf b;
    mb_sb_init(&b, a);

    const char* s = input ? input : "";
    bool pendingDash = false;
    for (; *s; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (pendingDash && b.len > 0) mb_sb_putc(&b, '-');
            pendingDash = false;
            mb_sb_putc(&b, c);
        } else {
            pendingDash = true;
        }
    }

    if (b.len == 0) {
        /* Fallback mirrors `build-${Date.now()}`: epoch milliseconds. */
        time_t t = time(NULL);
        mb_sb_printf(&b, "build-%lld", (long long)t * 1000LL);
    }
    return mb_sb_done(&b);
}
