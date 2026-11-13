/* CS623 Assignment 1 - Problem III Part B.
 *
 * Models an array of per-SM L1 data caches (tags only) fed by the
 * memory address trace produced by the stripped-down tools/mem_trace
 * tool, for number-of-SMs in {1,2,4,8,16,32,64}.
 *
 * Cache parameters (fixed, per SM):
 *   capacity     = 128 KB
 *   associativity= 32
 *   block size   = 128 B
 *   => blocks per cache = 131072 / 128 = 1024
 *   => number of sets   = 1024 / 32   = 32
 *   replacement  = LRU
 *
 * Addressing (64-bit addresses):
 *   block_addr = addr >> 7                    (strip 7-bit block offset)
 *   set_index  = block_addr mod num_sets       (== block_addr & (num_sets-1)
 *                since num_sets is a power of two)
 *   tag        = block_addr / num_sets         (the remaining high-order
 *                bits of the block address; the assignment states
 *                set_index in two equivalent forms - mod and bitwise AND,
 *                which coincide for a power-of-two num_sets - so the tag
 *                is whatever is left of the block address once the set
 *                index bits are removed)
 *
 * Scheduling: thread block (CTA) k is scheduled on SM (k mod n) for an
 * n-SM configuration. Each thread's memory access is a single cache
 * access, processed in the order recorded in the trace file; within one
 * warp-level trace record, active lanes are processed in increasing
 * lane order.
 *
 * Usage: ./p3b mem_trace.bin [mem_trace2.bin ...]
 * Reports total L1 hits/misses aggregated over all SMs, for each SM
 * count in {1,2,4,8,16,32,64}.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int cta_id;
    unsigned active_mask;
    uint64_t addrs[32];
} mem_access_t;

#define CACHE_CAPACITY_BYTES (128 * 1024)
#define ASSOCIATIVITY 32
#define BLOCK_SIZE 128
#define NUM_SETS ((CACHE_CAPACITY_BYTES / BLOCK_SIZE) / ASSOCIATIVITY) /* 32 */

/* one set: `ASSOCIATIVITY` tags plus their LRU recency stamps */
typedef struct {
    uint64_t tag[ASSOCIATIVITY];
    uint64_t recency[ASSOCIATIVITY];
    int valid[ASSOCIATIVITY];
} cache_set_t;

/* one L1 cache (one per SM): NUM_SETS sets */
typedef struct {
    cache_set_t sets[NUM_SETS];
} l1_cache_t;

static uint64_t g_clock = 0;

/* returns 1 on hit, 0 on miss; always updates LRU state */
static int cache_access(l1_cache_t *cache, uint64_t block_addr) {
    unsigned set_idx = (unsigned)(block_addr & (NUM_SETS - 1));
    uint64_t tag = block_addr / NUM_SETS;
    cache_set_t *set = &cache->sets[set_idx];

    g_clock++;

    int way;
    for (way = 0; way < ASSOCIATIVITY; way++) {
        if (set->valid[way] && set->tag[way] == tag) {
            set->recency[way] = g_clock;
            return 1; /* hit */
        }
    }

    /* miss: find an empty way, else evict the LRU way */
    int victim = -1;
    for (way = 0; way < ASSOCIATIVITY; way++) {
        if (!set->valid[way]) {
            victim = way;
            break;
        }
    }
    if (victim == -1) {
        uint64_t oldest = UINT64_MAX;
        for (way = 0; way < ASSOCIATIVITY; way++) {
            if (set->recency[way] < oldest) {
                oldest = set->recency[way];
                victim = way;
            }
        }
    }
    set->valid[victim] = 1;
    set->tag[victim] = tag;
    set->recency[victim] = g_clock;
    return 0; /* miss */
}

/* Loads the whole trace file into memory once (trace files here are a
 * few tens of MB at most; keeping it resident lets us re-run the same
 * trace for every SM-count configuration without re-parsing from disk
 * seven times). */
static mem_access_t *load_trace(const char *path, size_t *out_count) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "p3b: could not open %s\n", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    size_t count = (size_t)sz / sizeof(mem_access_t);
    mem_access_t *buf = (mem_access_t *)malloc(count * sizeof(mem_access_t));
    if (!buf) {
        fprintf(stderr, "p3b: out of memory reading %s\n", path);
        fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, sizeof(mem_access_t), count, fp);
    fclose(fp);
    if (got != count) {
        fprintf(stderr, "p3b: short read on %s\n", path);
    }
    *out_count = count;
    return buf;
}

static void run_config(mem_access_t *trace, size_t count, int num_sms) {
    l1_cache_t *caches = (l1_cache_t *)calloc(num_sms, sizeof(l1_cache_t));
    if (!caches) {
        fprintf(stderr, "p3b: out of memory allocating %d caches\n", num_sms);
        exit(1);
    }

    g_clock = 0;
    unsigned long long hits = 0, misses = 0;

    for (size_t i = 0; i < count; i++) {
        mem_access_t *ma = &trace[i];
        int sm_id = ((ma->cta_id % num_sms) + num_sms) % num_sms;
        l1_cache_t *cache = &caches[sm_id];

        for (int lane = 0; lane < 32; lane++) {
            if (!((ma->active_mask >> lane) & 1u)) continue;
            uint64_t block_addr = ma->addrs[lane] >> 7; /* / 128 */
            if (cache_access(cache, block_addr)) {
                hits++;
            } else {
                misses++;
            }
        }
    }

    printf("  SMs = %3d : hits = %12llu, misses = %12llu, hit rate = %.4f%%\n",
           num_sms, hits, misses,
           100.0 * (double)hits / (double)(hits + misses));

    free(caches);
}

static void process_file(const char *path) {
    size_t count = 0;
    mem_access_t *trace = load_trace(path, &count);
    if (!trace) return;

    printf("==== %s (%zu warp-level memory references) ====\n", path, count);

    int sm_counts[] = {1, 2, 4, 8, 16, 32, 64};
    for (size_t i = 0; i < sizeof(sm_counts) / sizeof(sm_counts[0]); i++) {
        run_config(trace, count, sm_counts[i]);
    }

    free(trace);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s mem_trace.bin [mem_trace2.bin ...]\n",
                argv[0]);
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        process_file(argv[i]);
    }
    return 0;
}
