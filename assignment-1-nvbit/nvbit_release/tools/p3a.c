/* CS623 Assignment 1 - Problem III Part A.
 *
 * Computes the "memory divergence per memory reference" from a memory
 * address trace produced by the stripped-down tools/mem_trace tool
 * (mem_access_t records: cta_id, active_mask, addrs[32]).
 *
 * For a single warp-level memory reference, the memory divergence is
 *     (number of unique 128B data cache blocks touched by the active
 *      threads) / (number of active threads for that reference)
 * A data cache block is 128 bytes, starting at addresses that are
 * multiples of 128, i.e. block_addr = addr >> 7 (addr / 128). Each
 * address in the trace is already the starting address of the access,
 * and (per the assignment) an access never straddles a block boundary.
 *
 * We accumulate this quantity over every warp-level reference in the
 * trace and divide by the number of references to get the overall
 * memory divergence per memory reference.
 *
 * Usage: ./p3a mem_trace.bin [mem_trace2.bin ...]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int cta_id;
    unsigned active_mask;
    uint64_t addrs[32];
} mem_access_t;

#define DATA_BLOCK_SIZE 128ULL

static int popcount32(unsigned x) {
    int c = 0;
    while (x) {
        x &= x - 1;
        c++;
    }
    return c;
}

static void process_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "p3a: could not open %s\n", path);
        return;
    }

    mem_access_t ma;
    uint64_t num_refs = 0;
    double divergence_sum = 0.0;

    /* scratch space for detecting unique blocks among <=32 addresses */
    uint64_t blocks[32];

    while (fread(&ma, sizeof(mem_access_t), 1, fp) == 1) {
        int active_threads = popcount32(ma.active_mask);
        if (active_threads == 0) continue; /* should not happen */

        int nblocks = 0;
        for (int lane = 0; lane < 32; lane++) {
            if (!((ma.active_mask >> lane) & 1u)) continue;
            uint64_t blk = ma.addrs[lane] >> 7; /* / 128 */

            int found = 0;
            for (int k = 0; k < nblocks; k++) {
                if (blocks[k] == blk) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                blocks[nblocks++] = blk;
            }
        }

        double ref_divergence = (double)nblocks / (double)active_threads;
        divergence_sum += ref_divergence;
        num_refs++;
    }
    fclose(fp);

    double result = (num_refs > 0) ? divergence_sum / (double)num_refs : 0.0;

    printf("==== %s ====\n", path);
    printf("  warp-level memory references : %llu\n",
           (unsigned long long)num_refs);
    printf("  memory divergence per memory reference: %.6f\n", result);
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
