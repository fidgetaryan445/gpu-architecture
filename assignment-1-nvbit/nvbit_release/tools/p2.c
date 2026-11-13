/* CS623 Assignment 1 - Problem II post-processing program.
 *
 * Reads the binary PC trace produced by tools/pc_trace (a flat file of
 * little-endian uint64_t PCs, one per instruction executed by an active
 * warp) and reports:
 *   - the number of unique instructions (unique PCs) accessed
 *   - the number of unique 32-byte instruction cache blocks accessed
 *     (a block starts at an address that is a multiple of 32, so the
 *     block of PC p is p & ~31ULL)
 *
 * Usage: ./p2 pc_trace.bin [pc_trace2.bin ...]
 * Each file is reported separately.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CACHE_BLOCK_SIZE 32ULL

/* Simple open-addressing hash set for uint64_t keys. */
typedef struct {
    uint64_t *slots;
    char *used;
    size_t capacity;
    size_t count;
} u64set_t;

static uint64_t hash64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static void set_init(u64set_t *s, size_t capacity_hint) {
    size_t cap = 16;
    while (cap < capacity_hint * 2) cap <<= 1;
    s->slots = (uint64_t *)calloc(cap, sizeof(uint64_t));
    s->used = (char *)calloc(cap, sizeof(char));
    s->capacity = cap;
    s->count = 0;
    if (!s->slots || !s->used) {
        fprintf(stderr, "p2: out of memory allocating hash set\n");
        exit(1);
    }
}

static void set_grow(u64set_t *s) {
    size_t old_cap = s->capacity;
    uint64_t *old_slots = s->slots;
    char *old_used = s->used;

    s->capacity = old_cap * 2;
    s->slots = (uint64_t *)calloc(s->capacity, sizeof(uint64_t));
    s->used = (char *)calloc(s->capacity, sizeof(char));
    if (!s->slots || !s->used) {
        fprintf(stderr, "p2: out of memory growing hash set\n");
        exit(1);
    }
    s->count = 0;

    size_t i;
    for (i = 0; i < old_cap; i++) {
        if (old_used[i]) {
            uint64_t key = old_slots[i];
            size_t mask = s->capacity - 1;
            size_t idx = hash64(key) & mask;
            while (s->used[idx]) idx = (idx + 1) & mask;
            s->used[idx] = 1;
            s->slots[idx] = key;
            s->count++;
        }
    }
    free(old_slots);
    free(old_used);
}

/* returns 1 if key was newly inserted, 0 if it was already present */
static int set_insert(u64set_t *s, uint64_t key) {
    if ((s->count + 1) * 4 >= s->capacity * 3) { /* load factor > 0.75 */
        set_grow(s);
    }
    size_t mask = s->capacity - 1;
    size_t idx = hash64(key) & mask;
    while (s->used[idx]) {
        if (s->slots[idx] == key) return 0;
        idx = (idx + 1) & mask;
    }
    s->used[idx] = 1;
    s->slots[idx] = key;
    s->count++;
    return 1;
}

static void set_free(u64set_t *s) {
    free(s->slots);
    free(s->used);
}

static void process_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "p2: could not open %s\n", path);
        return;
    }

    u64set_t instrs, blocks;
    set_init(&instrs, 1 << 16);
    set_init(&blocks, 1 << 16);

    uint64_t pc;
    uint64_t total = 0;
    while (fread(&pc, sizeof(uint64_t), 1, fp) == 1) {
        set_insert(&instrs, pc);
        set_insert(&blocks, pc & ~(CACHE_BLOCK_SIZE - 1));
        total++;
    }
    fclose(fp);

    printf("==== %s ====\n", path);
    printf("  executed warp instructions (trace records) : %llu\n",
           (unsigned long long)total);
    printf("  unique instructions accessed (unique PCs)   : %zu\n",
           instrs.count);
    printf("  unique instruction cache blocks (32B) accessed: %zu\n",
           blocks.count);

    set_free(&instrs);
    set_free(&blocks);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s pc_trace.bin [pc_trace2.bin ...]\n",
                argv[0]);
        return 1;
    }
    int i;
    for (i = 1; i < argc; i++) {
        process_file(argv[i]);
    }
    return 0;
}
