/*
 * Minecraft Builder — C edition
 * -----------------------------
 * Shared declarations for the whole application.
 *
 * The interesting property of this code base is the split of responsibilities:
 *
 *   prompt ──> planner ──> intent (template id, palette, size, floors)
 *                              │
 *                              └──> deterministic voxel builder ──> voxels
 *
 * A language model is allowed to drive the *planner* half only. It never emits
 * a coordinate, so the worst case a hallucinating model can produce is a
 * different — but still buildable — template choice. Everything it returns is
 * validated against the real catalog in src/planner.c before it is used.
 *
 * The code is strict C11 with no third party dependencies. Networking is the
 * only platform specific part and is isolated behind mb_http.h.
 */
#ifndef MB_H
#define MB_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ========================================================================== */
/* arena — bump allocator                                                     */
/* ========================================================================== */
/*
 * Every transient structure in this program (JSON trees, plans, prompts, build
 * results, render buffers) lives in an arena. That removes essentially all
 * manual free() calls while still making lifetimes explicit: "this lives until
 * the arena is reset".
 */

typedef struct ArenaBlock ArenaBlock;

typedef struct {
    ArenaBlock* head;
} Arena;

void  mb_arena_init(Arena* a);
void  mb_arena_reset(Arena* a);
void  mb_arena_destroy(Arena* a);
void* mb_arena_alloc(Arena* a, size_t n);
void* mb_arena_calloc(Arena* a, size_t n);
char* mb_arena_strdup(Arena* a, const char* s);
char* mb_arena_strndup(Arena* a, const char* s, size_t n);
char* mb_arena_printf(Arena* a, const char* fmt, ...);

/* ========================================================================== */
/* string buffer                                                              */
/* ========================================================================== */

typedef struct {
    char*   data;
    size_t  len;
    size_t  cap;
    Arena*  arena;
} SBuf;

void mb_sb_init(SBuf* b, Arena* a);
void mb_sb_putc(SBuf* b, char c);
void mb_sb_write(SBuf* b, const char* s, size_t n);
void mb_sb_puts(SBuf* b, const char* s);
void mb_sb_printf(SBuf* b, const char* fmt, ...);
/* Detach as a NUL terminated arena string. */
char* mb_sb_done(SBuf* b);

/* ========================================================================== */
/* UTF-8 <-> UTF-16                                                           */
/* ========================================================================== */
/*
 * The web build of this project derives its random seed from the prompt with
 * `h ^= prompt.charCodeAt(i)` — that is UTF-16 code units, not bytes. To keep
 * both implementations producing byte-identical buildings we do the same, and
 * all keyword matching runs over UTF-16 code units as well. That also makes the
 * length-based scoring of keywords match `String.prototype.length`.
 */

typedef uint16_t U16;

U16*   mb_u16_from_utf8(Arena* a, const char* s, size_t* outLen);
size_t mb_u16_len(const char* utf8);
size_t mb_u16_len_buf(const U16* s);
char*  mb_utf8_from_u16(Arena* a, const U16* s, size_t len);
bool   mb_u16_is_space(U16 c);   /* JavaScript \s, as close as it matters */

/* ========================================================================== */
/* misc helpers                                                               */
/* ========================================================================== */

uint32_t mb_hash_string(const char* utf8);  /* FNV-1a over UTF-16 code units */
void     mb_rng_seed(uint32_t* state, uint32_t seed);
double   mb_rng_next(uint32_t* state);      /* mulberry32, identical to the web app */
int      mb_rand_int(uint32_t* state, int min, int max);
int      mb_js_round(double v);             /* Math.round: halves go up, not away from zero */
int      mb_clamp_int(int v, int min, int max);
char*    mb_trim_dup(Arena* a, const char* s);
bool     mb_ieq(const char* a, const char* b);
char*    mb_lower_dup(Arena* a, const char* s);

/* ASCII case insensitive helpers — several call sites mimic /.../i regexes. */
bool        mb_contains_ci(const char* haystack, const char* needle);
const char* mb_strstr_ci(const char* haystack, const char* needle);
int         mb_strncmp_ci(const char* a, const char* b, size_t n);
/* Monotonic milliseconds, used only for latency reporting. */
double      mb_now_ms(void);

/* ========================================================================== */
/* JSON                                                                       */
/* ========================================================================== */

typedef enum {
    MB_JSON_NULL,
    MB_JSON_BOOL,
    MB_JSON_NUM,
    MB_JSON_STR,
    MB_JSON_ARR,
    MB_JSON_OBJ
} MBJsonType;

typedef struct MBJson MBJson;

struct MBJson {
    MBJsonType type;
    union {
        bool     boolean;
        double   num;
        struct { char* ptr; size_t len; } str;              /* decoded UTF-8, NUL terminated */
        struct { MBJson** items; size_t count; } arr;
        struct { char** keys; MBJson** vals; size_t count; } obj;
    } u;
};

/* Returns NULL on a malformed document. */
MBJson* mb_json_parse(Arena* a, const char* text, size_t len);
MBJson* mb_json_get(const MBJson* obj, const char* key);
MBJson* mb_json_at(const MBJson* arr, size_t index);
const char* mb_json_type_name(MBJsonType t);
/* JavaScript-ish String(x) for the validation layer. */
char*       mb_json_to_js_string(Arena* a, const MBJson* v);
void        mb_json_escape(SBuf* b, const char* s);
/* Serialise a value (indented when pretty). */
char*       mb_json_stringify(Arena* a, const MBJson* v, bool pretty);

/* ========================================================================== */
/* materials                                                                  */
/* ========================================================================== */

typedef enum {
    MB_CAT_STONE,
    MB_CAT_WOOD,
    MB_CAT_GLASS,
    MB_CAT_DECO,
    MB_CAT_NATURE,
    MB_CAT_LIGHT,
    MB_CAT_WOOL
} MBMaterialCategory;

typedef struct {
    const char*        id;      /* full id, e.g. minecraft:stone_bricks */
    const char*        name;    /* Chinese display name */
    uint32_t           color;   /* 0xRRGGBB approximation of the in-game texture */
    MBMaterialCategory category;
    int                light;   /* 0 = does not emit light */
} MBMaterial;

extern const MBMaterial MB_MATERIALS[];
extern const int        MB_MATERIAL_COUNT;

/* Never returns NULL — unknown ids yield a magenta placeholder, like the web app. */
const MBMaterial* mb_material(const char* idOrShort);
bool              mb_material_known(const char* idOrShort);
const char*       mb_material_name(const char* id);
uint32_t          mb_material_color(const char* id);

/* ========================================================================== */
/* geometry primitives                                                        */
/* ========================================================================== */

typedef struct {
    uint64_t*    keys;    /* 0 = empty slot, packed x/y/z otherwise */
    const char** vals;    /* full block id, e.g. minecraft:stone_bricks */
    size_t       cap;     /* always a power of two */
    size_t       count;
    Arena*       arena;
} MBSketch;

typedef struct MBBuilder {
    MBSketch sketch;
    uint32_t rng;
    Arena*   arena;
} MBBuilder;

typedef struct {
    int x1, z1, x2, z2;
} MBRect;

typedef struct {
    int          width, depth, height, floors;
    struct {
        const char* wall;
        const char* accent;
        const char* roof;
        const char* floor;
        const char* frame;
        const char* glass;
        const char* light;
        const char* nature;
    } palette;
} MBBuildOptions;

typedef struct {
    int x, y, z;
    const char* material;
} MBVoxel;

typedef struct { int x, y, z; } MBSize;

void mb_builder_init(MBBuilder* b, Arena* a, uint32_t seed);

void mb_sketch_init(MBSketch* s, Arena* a);
void mb_sketch_set(MBSketch* s, int x, int y, int z, const char* material);
void mb_sketch_remove(MBSketch* s, int x, int y, int z);
bool mb_sketch_has(const MBSketch* s, int x, int y, int z);
const char* mb_sketch_get(const MBSketch* s, int x, int y, int z);
size_t mb_sketch_count(const MBSketch* s);
/* Shift to a (0,0,0) origin, sort by y/z/x and report the bounding box. */
void mb_sketch_normalize(Arena* a, const MBSketch* s, MBSize* size, MBVoxel** voxels, size_t* count);

void mb_b_set(MBBuilder* b, int x, int y, int z, const char* material);
void mb_b_box(MBBuilder* b, int x1, int y1, int z1, int x2, int y2, int z2, const char* material);
void mb_b_rect(MBBuilder* b, MBRect r, int y, const char* material);
void mb_b_perimeter(MBBuilder* b, int x1, int y1, int z1, int x2, int y2, int z2, const char* material);
void mb_b_cylinder(MBBuilder* b, double cx, double cz, double radius, int y1, int y2,
                   const char* material, int thickness, bool solid, double jitter);
void mb_b_ellipsoid(MBBuilder* b, double cx, double cy, double cz, double rx, double ry, double rz,
                    const char* material, double ragged, bool hollow);
void mb_b_gable_roof(MBBuilder* b, int x1, int z1, int x2, int z2, int baseY,
                     const char* roofMaterial, const char* gableMaterial);
void mb_b_crenellation(MBBuilder* b, int x1, int z1, int x2, int z2, int y, const char* material);
void mb_b_ring_merlons(MBBuilder* b, double cx, double cz, double radius, int y1, int y2,
                       const char* material);
void mb_b_cut_opening(MBBuilder* b, char axis, int start, int end, int y1, int y2,
                      int plane, int axisOffset);
void mb_b_glaze(MBBuilder* b, char axis, int start, int end, int y1, int y2,
                int plane, const char* material, int axisOffset);

/* ========================================================================== */
/* structures + styles                                                        */
/* ========================================================================== */

typedef struct {
    const char* id;
    const char* label;
    const char* hint;
    const char* const* keywords;
    int         keywordCount;
    int         defWidth, defDepth, defHeight, defFloors;
    void      (*build)(MBBuilder* b, const MBBuildOptions* o);
} MBStructure;

typedef struct {
    const char* id;
    const char* label;
    const char* const* keywords;
    int         keywordCount;
    struct {
        const char* wall;
        const char* accent;
        const char* roof;
        const char* floor;
        const char* frame;
        const char* glass;
        const char* light;
        const char* nature;
    } palette;
} MBStyle;

extern const MBStructure MB_STRUCTURES[];
extern const int         MB_STRUCTURE_COUNT;
extern const MBStyle     MB_STYLES[];
extern const int         MB_STYLE_COUNT;
extern const char* const MB_DEFAULT_STRUCTURE_ID;
extern const char* const MB_DEFAULT_STYLE_ID;

const MBStructure* mb_structure_by_id(const char* id);
const MBStyle*     mb_style_by_id(const char* id);

/* ========================================================================== */
/* build result                                                               */
/* ========================================================================== */

typedef struct { const char* id; int count; } MBMaterialUsage;

typedef struct {
    const char* model;
    const char* endpoint;
    long        latencyMs;
    const char** reasoning;
    int         reasoningCount;
} MBLlmTrace;

typedef struct {
    const char*  prompt;
    const char*  structure;
    const char*  structureLabel;
    const char*  scale;
    uint32_t     seed;
    const char** keywords;
    int          keywordCount;
    const char** detectedMaterials;
    int          detectedMaterialCount;
    const char** modifiers;
    int          modifierCount;
    const char** notes;
    int          noteCount;
    bool         isLlm;      /* false = local rule engine */
    bool         hasLlmUser; /* true when MBLlmTrace below is meaningful */
    MBLlmTrace   llm;
} MBPlannerReport;

typedef struct {
    const char*    name;
    const char*    summary;
    MBSize         size;
    MBVoxel*       blocks;
    size_t         blockCount;      /* size_t: a 48x40x48 building can be large */
    const char**   palette;
    int            paletteCount;
    MBMaterialUsage* byMaterial;
    int            byMaterialCount;
    int            total;
    MBPlannerReport report;
} MBBuildResult;

/* ========================================================================== */
/* planner                                                                    */
/* ========================================================================== */

typedef enum {
    MB_SCALE_AUTO,
    MB_SCALE_SMALL,
    MB_SCALE_MEDIUM,
    MB_SCALE_LARGE
} MBScaleId;

typedef struct {
    const char* prompt;
    const char* structureId;   /* NULL or "auto" means infer */
    const char* styleId;
    MBScaleId   scale;
    uint32_t    seed;
} MBGenerateInput;

typedef struct {
    const char* structure;
    const char* style;
    const char* scale;              /* "small" | "medium" | "large" */
    int         width, depth, height, floors;
    struct {
        const char* wall;
        const char* accent;
        const char* roof;
        const char* floor;
        const char* frame;
        const char* glass;
        const char* light;
        const char* nature;
    } palette;                      /* any slot may be NULL */
    const char*  name;
    const char** keywords;   int keywordCount;
    const char** modifiers;  int modifierCount;
    const char** notes;      int noteCount;
} MBLlmPlan;

MBBuildResult* mb_generate(Arena* a, const MBGenerateInput* in);

MBBuildResult* mb_generate_with_plan(Arena* a, const char* prompt, uint32_t seed,
                                     const MBLlmPlan* plan, const MBLlmTrace* trace,
                                     const char* structureId, const char* styleId,
                                     MBScaleId scale);

/* Catalog injected into the model prompt — the choices it may pick from. */
typedef struct {
    const char*  id;
    const char*  label;
    const char*  hint;
} MBStructureChoice;

typedef struct {
    const char* id;
    const char* label;
} MBStyleChoice;

const char* mb_scale_label(MBScaleId s);
const char* mb_scale_label_auto(MBScaleId s);

/* ========================================================================== */
/* LLM client                                                                 */
/* ========================================================================== */

typedef struct {
    char   baseUrl[256];
    char   apiKey[256];
    char   model[128];
    double temperature;
    bool   jsonMode;
} MBLlmConfig;

typedef struct {
    const char* id;
    const char* label;
    const char* baseUrl;
    const char* model;
    const char* hint;
    bool        keyOptional;
} MBLlmPreset;

extern const MBLlmPreset MB_LLM_PRESETS[];
extern const int          MB_LLM_PRESET_COUNT;

typedef enum {
    MB_LLM_ERR_NONE = 0,
    MB_LLM_ERR_NETWORK,
    MB_LLM_ERR_AUTH,
    MB_LLM_ERR_RATE_LIMIT,
    MB_LLM_ERR_BAD_RESPONSE,
    MB_LLM_ERR_ABORTED,
    MB_LLM_ERR_UNKNOWN
} MBLlmErrorKind;

typedef struct {
    MBLlmErrorKind kind;
    char           message[1024];
} MBLlmError;

void mb_llm_error_set(MBLlmError* e, MBLlmErrorKind kind, const char* fmt, ...);

typedef struct {
    MBLlmPlan   plan;
    char        model[128];
    char        endpoint[256];
    long        latencyMs;
    const char* raw;
} MBLlmResult;

typedef struct {
    MBLlmResult result;
    bool        ok;
    MBLlmError  error;
} MBLlmPlanOutcome;

MBLlmPlanOutcome mb_llm_plan(Arena* a, const char* prompt, const MBLlmConfig* cfg,
                             const char* lockStructure, const char* lockStyle);
MBLlmPlanOutcome mb_llm_ping(Arena* a, const MBLlmConfig* cfg);
const char*      mb_llm_error_kind_name(MBLlmErrorKind k);

/* Exposed for the test suite. */
MBJson*    mb_extract_json_object(Arena* a, const char* text, MBLlmError* err);
MBLlmPlan  mb_normalize_plan(Arena* a, const MBJson* raw, MBLlmError* err);
void       mb_llm_default_config(MBLlmConfig* cfg);
/* Replaces the key with *** before surfacing an error to the user. */
char*      mb_redact(Arena* a, const char* text, const char* apiKey);

/* ========================================================================== */
/* HTTP — the only platform specific module                                   */
/* ========================================================================== */

typedef struct {
    const char* body;         /* UTF-8 request body, may be NULL */
    size_t      bodyLen;
    const char* authorization;/* value for the Authorization header, or NULL */
    const char* contentType;  /* default: application/json */
} MBHttpRequest;

typedef struct {
    long        status;
    char*       body;         /* UTF-8, arena owned */
    size_t      bodyLen;
    MBLlmError* error;        /* filled in when the transport itself failed */
} MBHttpResponse;

bool mb_http_do(Arena* a, const char* method, const char* url,
                const MBHttpRequest* req, MBHttpResponse* out);
/* "https://api.deepseek.com/v1" -> "api.deepseek.com" */
char* mb_url_host(Arena* a, const char* url);
/* Join a base URL and a path, collapsing duplicate slashes. */
char* mb_url_join(Arena* a, const char* baseUrl, const char* path);

/* ========================================================================== */
/* exporters                                                                  */
/* ========================================================================== */

char* mb_export_mcfunction(Arena* a, const MBBuildResult* r, const char* namespaceName);
char* mb_export_json(Arena* a, const MBBuildResult* r);
char* mb_export_csv(Arena* a, const MBBuildResult* r);
char* mb_export_markdown(Arena* a, const MBBuildResult* r);
char* mb_slugify(Arena* a, const char* input);

/* ========================================================================== */
/* terminal                                                                   */
/* ========================================================================== */

void mb_term_init(void);
void mb_term_restore(void);
void mb_term_size(int* cols, int* rows);
void mb_term_write(const char* s);
/* Blocking read of one key. Returns an MB_KEY_* value. */
int  mb_term_read_key(void);
/* Temporarily leaves raw mode, reads a line, returns to raw mode. */
char* mb_term_input_line(Arena* a, const char* label);

enum {
    MB_KEY_NONE = 0,
    MB_KEY_ESC,
    MB_KEY_ENTER,
    MB_KEY_TAB,
    MB_KEY_BACKSPACE,
    MB_KEY_UP,
    MB_KEY_DOWN,
    MB_KEY_LEFT,
    MB_KEY_RIGHT,
    MB_KEY_PAGEUP,
    MB_KEY_PAGEDOWN,
    MB_KEY_HOME,
    MB_KEY_END,
    MB_KEY_CHAR = 1000,      /* value - MB_KEY_CHAR is an ASCII byte */
    MB_KEY_CTRL_C = 2000,
    MB_KEY_RESIZE = 2001
};

/* ========================================================================== */
/* renderer                                                                   */
/* ========================================================================== */

typedef struct {
    double yaw;               /* radians, rotates around the Y axis */
    double pitch;             /* radians, 0 = side view, 1.2 = looking down */
    double zoom;              /* pixels per block */
    bool   showGrid;
    bool   showAxes;
} MBView;

void mb_view_default(MBView* v);
/* True colour isometric render, double height pixels, UTF-8 half blocks. */
char* mb_render_iso(Arena* a, const MBBuildResult* r, const MBView* v, int cols, int rows);
/* Top-down floor plan of one layer. */
char* mb_render_slice(Arena* a, const MBBuildResult* r, int layer, int maxCols, int maxRows);
/* Legend shared by both views. */
char* mb_render_legend(Arena* a, const MBBuildResult* r);

/* ========================================================================== */
/* 3D preview — software rasteriser                                           */
/* ========================================================================== */
/*
 * gfx3d.c is a small z-buffered rasteriser that knows nothing about terminals or
 * windows: it writes into a plain pixel buffer and that is all. Keeping it free
 * of platform code means it can be driven headlessly — `mb.exe --shot out.bmp`
 * renders one frame offscreen, which is how the preview is regression tested and
 * how the screenshots in the README are produced.
 *
 * Geometry, materials and shading all come from the same catalog the terminal
 * build uses, so a block looks the same colour in both views.
 */

typedef struct {
    int       width, height;
    /* 0x00RRGGBB. On a little endian machine this is byte order B,G,R,X, which
       is exactly what a 32bpp BI_RGB DIB section wants — the buffer can be
       handed straight to StretchDIBits without a conversion pass. */
    uint32_t* color;
    float*    depth;      /* 1/z; 0 means "nothing drawn here yet" */
} MBFramebuffer;

void mb_fb_init(MBFramebuffer* fb, Arena* a, int width, int height);
/* Vertical gradient, `top` colour first. Also resets the depth buffer. */
void mb_fb_clear(MBFramebuffer* fb, uint32_t top, uint32_t bottom);

typedef struct {
    double x, y, z;       /* world position, in blocks */
    double yaw;           /* radians around +Y; 0 looks towards +Z */
    double pitch;         /* radians; negative looks down */
    double fov;           /* vertical field of view, radians */
} MBCamera;

typedef struct {
    bool   showGrid;
    bool   showAxes;
    bool   autoOrbit;     /* the front end advances yaw; the renderer never does */
    double gridExtent;    /* half width of the ground grid, in blocks */
} MBSceneOptions;

void mb_scene_default(MBSceneOptions* o);
/* A pleasant three-quarter view, framed on the building. */
void mb_camera_default(MBCamera* c, const MBBuildResult* r);
/* Keep the orientation, move the camera so the whole box fits the frame. */
void mb_camera_frame(MBCamera* c, const MBBuildResult* r);
/* Orthonormal basis; movement keys in the front end are expressed in it. */
void mb_camera_basis(const MBCamera* c, double* right, double* up, double* fwd);

void mb_render_scene(Arena* a, MBFramebuffer* fb, const MBBuildResult* r,
                     const MBCamera* cam, const MBSceneOptions* opt);

/* BMP output: encode into a caller supplied buffer, or straight to a file.
 * A 24bpp bottom-up BMP — the format every image viewer accepts. */
size_t mb_bmp_size(const MBFramebuffer* fb);
void   mb_bmp_encode(const MBFramebuffer* fb, unsigned char* out);
bool   mb_bmp_write(const MBFramebuffer* fb, const char* path);

/* -------------------------------------------------------------------------- */
/* the windowed front end (preview.c)                                         */
/* -------------------------------------------------------------------------- */
/* Opens a modal Win32 window and pumps messages until it is closed. Returns
 * false when no window could be created. On non-Windows builds this is a stub
 * that reports failure, so callers need no platform #ifdef of their own. */
bool mb_preview_open(const MBBuildResult* r, const char* title, bool hideConsole);

#endif /* MB_H */
