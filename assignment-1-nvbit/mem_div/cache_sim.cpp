// PART B: models an array of L1 caches (one per SM), each 128KB, 32-way
// associative, 128B blocks, LRU replacement -- tags only.
//
// 32 sets per cache: NUM_SETS = CAPACITY / (BLOCK_SIZE * ASSOC)
//                             = 131072 / (128 * 32) = 32
//
// For a block address:
//   set_index = block_addr mod NUM_SETS   (== block_addr & (NUM_SETS-1))
//   tag       = block_addr / NUM_SETS     (== block_addr >> log2(NUM_SETS))
//
// Thread blocks are scheduled round robin: block k goes to SM (k mod n).
// Reads the same trace format as divergence.cpp:
//   <block_id> <addr1_hex> <addr2_hex> ... <addrN_hex>
// Each address on a line is one thread's single cache access, routed to
// the SM owning that line's block_id, processed strictly in trace order.
//
// Runs the whole simulation once per SM count in {1,2,4,8,16,32,64},
// re-reading the trace file fresh for each count (keeps memory usage low
// regardless of trace size).
//
// Usage: ./cache_sim mem_trace.txt

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <list>
#include <sstream>
#include <string>
#include <vector>

static const uint64_t BLOCK_SIZE = 128;              // bytes
static const uint64_t CAPACITY   = 128 * 1024;         // bytes, per L1 cache
static const uint64_t ASSOC      = 32;                 // ways
static const uint64_t NUM_SETS   = CAPACITY / (BLOCK_SIZE * ASSOC);  // = 32

struct L1Cache {
    // one LRU list of tags per set; front = MRU, back = LRU
    std::vector<std::list<uint64_t>> sets;
    L1Cache() : sets(NUM_SETS) {}

    bool access(uint64_t block_addr) {
        uint64_t set_idx = block_addr % NUM_SETS;  // == block_addr & (NUM_SETS-1)
        uint64_t tag = block_addr / NUM_SETS;       // == block_addr >> log2(NUM_SETS)

        std::list<uint64_t>& way_list = sets[set_idx];
        for (auto it = way_list.begin(); it != way_list.end(); ++it) {
            if (*it == tag) {
                // hit: move to MRU position
                way_list.erase(it);
                way_list.push_front(tag);
                return true;
            }
        }
        // miss: insert as MRU, evict LRU if the set is now over capacity
        way_list.push_front(tag);
        if (way_list.size() > ASSOC) {
            way_list.pop_back();
        }
        return false;
    }
};

static void run_config(const char* trace_path, int n, uint64_t& hits,
                       uint64_t& misses) {
    std::vector<L1Cache> caches(n);

    std::ifstream in(trace_path);
    if (!in.is_open()) {
        std::fprintf(stderr, "Could not open trace file: %s\n", trace_path);
        std::exit(1);
    }

    hits = 0;
    misses = 0;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);

        uint64_t block_id;
        ls >> block_id;

        int sm = (int)(block_id % (uint64_t)n);
        L1Cache& cache = caches[sm];

        std::string addr_tok;
        while (ls >> addr_tok) {
            uint64_t addr = std::strtoull(addr_tok.c_str(), nullptr, 16);
            uint64_t block_addr = addr / BLOCK_SIZE;
            if (cache.access(block_addr)) {
                hits++;
            } else {
                misses++;
            }
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <mem_trace_file>\n", argv[0]);
        return 1;
    }
    const char* trace_path = argv[1];

    const int sm_counts[] = {1, 2, 4, 8, 16, 32, 64};

    std::printf("Trace file: %s\n", trace_path);
    std::printf("%-8s %-16s %-16s %-12s\n", "#SMs", "Hits", "Misses",
                "Hit rate");

    for (int n : sm_counts) {
        uint64_t hits = 0, misses = 0;
        run_config(trace_path, n, hits, misses);

        uint64_t total = hits + misses;
        double hit_rate = (total > 0) ? (100.0 * (double)hits / (double)total) : 0.0;

        std::printf("%-8d %-16lu %-16lu %-11.2f%%\n", n,
                    (unsigned long)hits, (unsigned long)misses, hit_rate);
    }

    return 0;
}
