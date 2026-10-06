/*
 * voxel.c — sparse voxel storage and the geometric toolkit.
 *
 * This is the deterministic half of the program. Nothing here consults a
 * language model: given a template id, a palette and four integers it always
 * produces the exact same set of blocks. That property is what makes the
 * "a hallucinating model can never emit an unbuildable structure" claim true.
 *
 * Coordinates during construction are signed and may be negative (roof eaves
 * overhang to -1, the treehouse is centred on the origin). normalize() shifts
 * everything so the minimum corner is (0,0,0).
 */
#include "mb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* open addressing hash map keyed by packed coordinates                       */
/* ========================================================================== */

#define MB_COORD_BIAS 2048        /* supports -2048 .. +2047 while building */
#define MB_COORD_BITS 21
#define MB_DEFAULT_CAP 1024u

static uint64_t pack_key(int x, int y, int z)
{
    uint64_t mask = (1ull << MB_COORD_BITS) - 1;
    uint64_t ux = (uint64_t)(x + MB_COORD_BIAS) & mask;
    uint64_t uy = (uint64_t)(y + MB_COORD_BIAS) & mask;
    uint64_t uz = (uint64_t)(z + MB_COORD_BIAS) & mask;
    return (ux << (MB_COORD_BITS * 2)) | (uy << MB_COORD_BITS) | uz;
}

static void unpack_key(uint64_t k, int* x, int* y, int* z)
{
    uint64_t mask = (1ull << MB_COORD_BITS) - 1;
    *x = (int)((k >> (MB_COORD_BITS * 2)) & mask) - MB_COORD_BIAS;
    *y = (int)((k >> MB_COORD_BITS) & mask) - MB_COORD_BIAS;
    *z = (int)(k & mask) - MB_COORD_BIAS;
}

static size_t key_hash(uint64_t k)
{
    /* splitmix64 finaliser — good avalanche keeps linear probing short. */
    k ^= k >> 30;
    k *= 0xbf58476d1ce4e5b9ull;
    k ^= k >> 27;
    k *= 0x94d049bb133111ebull;
    k ^= k >> 31;
    return (size_t)k;
}

void mb_sketch_init(MBSketch* s, Arena* a)
{
    s->cap = MB_DEFAULT_CAP;
    s->count = 0;
    s->arena = a;
    s->keys = (uint64_t*)mb_arena_calloc(a, s->cap * sizeof(uint64_t));
    s->vals = (const char**)mb_arena_calloc(a, s->cap * sizeof(char*));
}

static void sketch_rehash(MBSketch* s, size_t newCap)
{
    uint64_t* oldKeys = s->keys;
    const char** oldVals = s->vals;
    size_t oldCap = s->cap;

    s->keys = (uint64_t*)mb_arena_calloc(s->arena, newCap * sizeof(uint64_t));
    s->vals = (const char**)mb_arena_calloc(s->arena, newCap * sizeof(char*));
    s->cap = newCap;

    size_t mask = newCap - 1;
    for (size_t i = 0; i < oldCap; i++) {
        if (!oldKeys[i]) continue;
        size_t slot = key_hash(oldKeys[i]) & mask;
        while (s->keys[slot]) slot = (slot + 1) & mask;
        s->keys[slot] = oldKeys[i];
        s->vals[slot] = oldVals[i];
    }
}

const char* mb_sketch_get(const MBSketch* s, int x, int y, int z)
{
    if (!s->cap) return NULL;
    uint64_t k = pack_key(x, y, z);
    size_t mask = s->cap - 1;
    size_t slot = key_hash(k) & mask;
    for (;;) {
        uint64_t entry = s->keys[slot];
        if (!entry) return NULL;
        if (entry == k) return s->vals[slot];
        slot = (slot + 1) & mask;
    }
}

bool mb_sketch_has(const MBSketch* s, int x, int y, int z)
{
    return mb_sketch_get(s, x, y, z) != NULL;
}

size_t mb_sketch_count(const MBSketch* s)
{
    return s->count;
}

void mb_sketch_set(MBSketch* s, int x, int y, int z, const char* material)
{
    uint64_t k = pack_key(x, y, z);
    size_t mask = s->cap - 1;
    size_t slot = key_hash(k) & mask;

    for (;;) {
        uint64_t entry = s->keys[slot];
        if (!entry) {
            s->keys[slot] = k;
            s->vals[slot] = material;
            s->count++;
            /* Keep the load factor under 70%. */
            if (s->count * 10 >= s->cap * 7) sketch_rehash(s, s->cap * 2);
            return;
        }
        if (entry == k) {
            s->vals[slot] = material;
            return;
        }
        slot = (slot + 1) & mask;
    }
}

void mb_sketch_remove(MBSketch* s, int x, int y, int z)
{
    uint64_t k = pack_key(x, y, z);
    size_t mask = s->cap - 1;
    size_t slot = key_hash(k) & mask;

    for (;;) {
        uint64_t entry = s->keys[slot];
        if (!entry) return;               /* not present */
        if (entry == k) break;
        slot = (slot + 1) & mask;
    }

    s->keys[slot] = 0;
    s->vals[slot] = NULL;
    s->count--;

    /* Close the hole by re-inserting the displaced cluster. */
    size_t i = (slot + 1) & mask;
    while (s->keys[i]) {
        uint64_t kk = s->keys[i];
        const char* vv = s->vals[i];
        s->keys[i] = 0;
        s->vals[i] = NULL;
        s->count--;

        size_t target = key_hash(kk) & mask;
        while (s->keys[target]) target = (target + 1) & mask;
        s->keys[target] = kk;
        s->vals[target] = vv;
        s->count++;

        i = (i + 1) & mask;
    }
}

/* ========================================================================== */
/* normalize                                                                  */
/* ========================================================================== */

static int cmp_voxel(const void* pa, const void* pb)
{
    const MBVoxel* a = (const MBVoxel*)pa;
    const MBVoxel* b = (const MBVoxel*)pb;
    if (a->y != b->y) return a->y < b->y ? -1 : 1;
    if (a->z != b->z) return a->z < b->z ? -1 : 1;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    return 0;
}

void mb_sketch_normalize(Arena* a, const MBSketch* s, MBSize* size, MBVoxel** voxels, size_t* count)
{
    if (s->count == 0) {
        size->x = size->y = size->z = 0;
        *voxels = NULL;
        *count = 0;
        return;
    }

    int minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    bool first = true;

    MBVoxel* out = (MBVoxel*)mb_arena_alloc(a, s->count * sizeof(MBVoxel));
    size_t n = 0;

    for (size_t i = 0; i < s->cap; i++) {
        if (!s->keys[i]) continue;
        int x, y, z;
        unpack_key(s->keys[i], &x, &y, &z);
        if (first) {
            minX = maxX = x;
            minY = maxY = y;
            minZ = maxZ = z;
            first = false;
        } else {
            if (x < minX) minX = x;
            if (y < minY) minY = y;
            if (z < minZ) minZ = z;
            if (x > maxX) maxX = x;
            if (y > maxY) maxY = y;
            if (z > maxZ) maxZ = z;
        }
        out[n].x = x;
        out[n].y = y;
        out[n].z = z;
        out[n].material = s->vals[i];
        n++;
    }

    for (size_t i = 0; i < n; i++) {
        out[i].x -= minX;
        out[i].y -= minY;
        out[i].z -= minZ;
    }

    qsort(out, n, sizeof(MBVoxel), cmp_voxel);

    size->x = maxX - minX + 1;
    size->y = maxY - minY + 1;
    size->z = maxZ - minZ + 1;
    *voxels = out;
    *count = n;
}

/* ========================================================================== */
/* builder                                                                    */
/* ========================================================================== */

void mb_builder_init(MBBuilder* b, Arena* a, uint32_t seed)
{
    b->arena = a;
    b->rng = seed;
    mb_sketch_init(&b->sketch, a);
}

void mb_b_set(MBBuilder* b, int x, int y, int z, const char* material)
{
    mb_sketch_set(&b->sketch, x, y, z, material);
}

void mb_b_box(MBBuilder* b, int x1, int y1, int z1, int x2, int y2, int z2, const char* material)
{
    int ax = x1 < x2 ? x1 : x2, bx = x1 < x2 ? x2 : x1;
    int ay = y1 < y2 ? y1 : y2, by = y1 < y2 ? y2 : y1;
    int az = z1 < z2 ? z1 : z2, bz = z1 < z2 ? z2 : z1;

    for (int x = ax; x <= bx; x++)
        for (int y = ay; y <= by; y++)
            for (int z = az; z <= bz; z++)
                mb_sketch_set(&b->sketch, x, y, z, material);
}

void mb_b_rect(MBBuilder* b, MBRect r, int y, const char* material)
{
    mb_b_box(b, r.x1, y, r.z1, r.x2, y, r.z2, material);
}

void mb_b_perimeter(MBBuilder* b, int x1, int y1, int z1, int x2, int y2, int z2,
                    const char* material)
{
    int ax = x1 < x2 ? x1 : x2, bx = x1 < x2 ? x2 : x1;
    int ay = y1 < y2 ? y1 : y2, by = y1 < y2 ? y2 : y1;
    int az = z1 < z2 ? z1 : z2, bz = z1 < z2 ? z2 : z1;

    for (int y = ay; y <= by; y++) {
        for (int x = ax; x <= bx; x++) {
            mb_sketch_set(&b->sketch, x, y, az, material);
            mb_sketch_set(&b->sketch, x, y, bz, material);
        }
        for (int z = az + 1; z < bz; z++) {
            mb_sketch_set(&b->sketch, ax, y, z, material);
            mb_sketch_set(&b->sketch, bx, y, z, material);
        }
    }
}

void mb_b_cylinder(MBBuilder* b, double cx, double cz, double radius, int y1, int y2,
                   const char* material, int thickness, bool solid, double jitter)
{
    int ay = y1 < y2 ? y1 : y2, by = y1 < y2 ? y2 : y1;
    double outer = radius + 0.5;
    double inner = radius - (double)thickness + 0.5;

    /* NOTE: the rng() consumption order below (x, then z, then y) is part of the
     * deterministic contract — changing it changes every jittered building. */
    for (int x = (int)floor(cx - radius - 1.0); x <= (int)ceil(cx + radius + 1.0); x++) {
        for (int z = (int)floor(cz - radius - 1.0); z <= (int)ceil(cz + radius + 1.0); z++) {
            double dx = (double)x - cx;
            double dz = (double)z - cz;
            double dist = sqrt(dx * dx + dz * dz);
            if (dist > outer || dist < 0.5) continue;
            if (!solid && dist < inner) continue;
            for (int y = ay; y <= by; y++) {
                if (jitter > 0.0 && mb_rng_next(&b->rng) < jitter) continue;
                mb_sketch_set(&b->sketch, x, y, z, material);
            }
        }
    }
}

void mb_b_ellipsoid(MBBuilder* b, double cx, double cy, double cz, double rx, double ry, double rz,
                    const char* material, double ragged, bool hollow)
{
    for (int x = (int)floor(cx - rx); x <= (int)ceil(cx + rx); x++) {
        for (int y = (int)floor(cy - ry); y <= (int)ceil(cy + ry); y++) {
            for (int z = (int)floor(cz - rz); z <= (int)ceil(cz + rz); z++) {
                double dx = ((double)x - cx) / (rx + 0.5);
                double dy = ((double)y - cy) / (ry + 0.5);
                double dz = ((double)z - cz) / (rz + 0.5);
                double d = dx * dx + dy * dy + dz * dz;
                if (d > 1.0) continue;
                if (hollow && d < 0.35) continue;
                if (ragged > 0.0 && d > 0.55 && mb_rng_next(&b->rng) < ragged) continue;
                if (mb_sketch_has(&b->sketch, x, y, z)) continue;
                mb_sketch_set(&b->sketch, x, y, z, material);
            }
        }
    }
}

void mb_b_gable_roof(MBBuilder* b, int x1, int z1, int x2, int z2, int baseY,
                     const char* roofMaterial, const char* gableMaterial)
{
    int ax = x1 < x2 ? x1 : x2, bx = x1 < x2 ? x2 : x1;
    int az = z1 < z2 ? z1 : z2, bz = z1 < z2 ? z2 : z1;
    int depth = bz - az;
    int half = depth / 2 + 1;

    for (int i = 0; i < half; i++) {
        int y = baseY + i;
        int za = az + i;
        int zb = bz - i;
        if (za > zb) break;
        for (int x = ax; x <= bx; x++) {
            mb_sketch_set(&b->sketch, x, y, za, roofMaterial);
            mb_sketch_set(&b->sketch, x, y, zb, roofMaterial);
        }
        for (int z = az + i; z <= bz - i; z++) {
            mb_sketch_set(&b->sketch, ax, y - 1, z, gableMaterial);
            mb_sketch_set(&b->sketch, bx, y - 1, z, gableMaterial);
        }
    }
}

void mb_b_crenellation(MBBuilder* b, int x1, int z1, int x2, int z2, int y, const char* material)
{
    int ax = x1 < x2 ? x1 : x2, bx = x1 < x2 ? x2 : x1;
    int az = z1 < z2 ? z1 : z2, bz = z1 < z2 ? z2 : z1;

    for (int x = ax; x <= bx; x++) {
        for (int z = az; z <= bz; z++) {
            bool onRim = (x == ax || x == bx || z == az || z == bz);
            if (!onRim) continue;
            if ((x + z) % 2 != 0) continue;
            mb_sketch_set(&b->sketch, x, y, z, material);
        }
    }
}

void mb_b_ring_merlons(MBBuilder* b, double cx, double cz, double radius, int y1, int y2,
                       const char* material)
{
    double outer = radius + 0.5;
    double inner = radius - 1.0 + 0.5;
    int i = 0;

    for (int x = (int)floor(cx - radius - 1.0); x <= (int)ceil(cx + radius + 1.0); x++) {
        for (int z = (int)floor(cz - radius - 1.0); z <= (int)ceil(cz + radius + 1.0); z++) {
            double dx = (double)x - cx;
            double dz = (double)z - cz;
            double dist = sqrt(dx * dx + dz * dz);
            if (dist > outer || dist < inner) continue;
            i++;
            if (i % 2 != 0) continue;
            for (int y = y1; y <= y2; y++) {
                mb_sketch_set(&b->sketch, x, y, z, material);
            }
        }
    }
}

void mb_b_cut_opening(MBBuilder* b, char axis, int start, int end, int y1, int y2,
                      int plane, int axisOffset)
{
    for (int i = start; i <= end; i++) {
        for (int y = y1; y <= y2; y++) {
            if (axis == 'x') mb_sketch_remove(&b->sketch, i, y, plane + axisOffset);
            else             mb_sketch_remove(&b->sketch, plane + axisOffset, y, i);
        }
    }
}

void mb_b_glaze(MBBuilder* b, char axis, int start, int end, int y1, int y2,
                int plane, const char* material, int axisOffset)
{
    for (int i = start; i <= end; i++) {
        for (int y = y1; y <= y2; y++) {
            if (axis == 'x') mb_sketch_set(&b->sketch, i, y, plane + axisOffset, material);
            else             mb_sketch_set(&b->sketch, plane + axisOffset, y, i, material);
        }
    }
}
