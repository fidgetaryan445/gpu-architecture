#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <numeric>

uint32_t seed = 1;

uint32_t key_of(uint32_t index) {
  uint32_t x = index + seed * 0x9E3779B9u;
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

void write_trace(const char *name, const std::vector<uint32_t> keys) {
  FILE *f = fopen(name, "wb");
  if (f == NULL) exit(1);
  if (!keys.empty()) fwrite(keys.data(), 4, keys.size(), f);
  fclose(f);
  printf("%s: %zu keys\n", name, keys.size());
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s n [dup_pct] [ghost_pct] [seed]\n", argv[0]);
    return 1;
  }
  uint32_t n = strtoul(argv[1], NULL, 10);
  uint32_t dup_pct = argc > 2 ? atoi(argv[2]) : 10;
  uint32_t ghost_pct = argc > 3 ? atoi(argv[3]) : 0;
  seed = argc > 4 ? strtoul(argv[4], NULL, 10) : 1;
  if (dup_pct > 100) dup_pct = 100;
  if (ghost_pct > 100) ghost_pct = 100;
  uint32_t period = dup_pct > 0 ? 100 / dup_pct : 0;

  std::vector<uint32_t> insert_keys, delete_keys, search_keys(n);

  for (uint32_t i = 0; i < n; i++) {
    insert_keys.push_back(key_of(i));
    if (period > 0 && i % period == period - 1)
      insert_keys.push_back(key_of((i * 7 + 3) % (i + 1)));
  }

  for (uint32_t i = 0; i < n; i += 3) {
    delete_keys.push_back(key_of(i));
    if (ghost_pct > 0 && (i / 3) % 100 < ghost_pct)
      delete_keys.push_back(key_of(n + i));
  }

  uint32_t stride = n * 0.618 + 1;
  while (std::gcd(stride, n) != 1) stride++;
  for (uint32_t t = 0; t < n; t++) {
    uint32_t group = t / 3;
    uint32_t key;
    if (t % 3 == 0) key = key_of((3 * group + 1) % n);
    else if (t % 3 == 1) key = key_of((3 * group) % n);
    else key = key_of(n + group);
    search_keys[(uint64_t)t * stride % n] = key;
  }

  if (insert_keys.size() > 10000000)
    fprintf(stderr, "warning: insert trace has more than 10^7 keys\n");

  char name[64];
  snprintf(name, sizeof name, "insert_%u.bin", n);
  write_trace(name, insert_keys);
  snprintf(name, sizeof name, "search_%u.bin", n);
  write_trace(name, search_keys);
  snprintf(name, sizeof name, "delete_%u.bin", n);
  write_trace(name, delete_keys);
  return 0;
}
