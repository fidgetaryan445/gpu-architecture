// PART A: computes memory divergence per memory reference.
//
// Reads a trace produced by the mem_div NVBit tool. Each line is one
// warp-level memory reference:
//   <block_id> <addr1_hex> <addr2_hex> ... <addrN_hex>
// where N is the number of active threads that executed that reference.
//
// For each line:
//   divergence = (# unique 128B data cache blocks among the N addresses)
//                / N
// The program accumulates this over all lines and reports the average
// (sum of per-reference divergence / number of references).
//
// Usage: ./divergence mem_trace.txt

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>

static const uint64_t CACHE_BLOCK_SIZE = 128;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <mem_trace_file>\n", argv[0]);
        return 1;
    }

    std::ifstream in(argv[1]);
    if (!in.is_open()) {
        std::fprintf(stderr, "Could not open trace file: %s\n", argv[1]);
        return 1;
    }

    double sum_divergence = 0.0;
    uint64_t num_refs = 0;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);

        uint64_t block_id;
        ls >> block_id;  // first token on the line; not needed for Part A

        std::unordered_set<uint64_t> unique_blocks;
        int active_threads = 0;

        std::string addr_tok;
        while (ls >> addr_tok) {
            uint64_t addr = std::strtoull(addr_tok.c_str(), nullptr, 16);
            uint64_t block_addr = addr / CACHE_BLOCK_SIZE;  // remove block offset
            unique_blocks.insert(block_addr);
            active_threads++;
        }

        if (active_threads > 0) {
            double divergence =
                (double)unique_blocks.size() / (double)active_threads;
            sum_divergence += divergence;
            num_refs++;
        }
    }

    double avg_divergence = (num_refs > 0) ? (sum_divergence / (double)num_refs) : 0.0;

    std::printf("Trace file                     : %s\n", argv[1]);
    std::printf("Total warp-level memory refs    : %lu\n",
                (unsigned long)num_refs);
    std::printf("Memory divergence per reference : %.6f\n", avg_divergence);

    return 0;
}
