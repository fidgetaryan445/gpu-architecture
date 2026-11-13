#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <set>

#define cudaCheckError(ans)                                                    \
  {                                                                            \
    gpuAssert((ans), __FILE__, __LINE__);                                      \
  }
inline void gpuAssert(cudaError_t code, const char *file, int line,
                      bool abort = true) {
  if (code != cudaSuccess) {
    fprintf(stderr, "GPUassert: %s %s %d\n", cudaGetErrorString(code), file,
            line);
    if (abort)
      exit(code);
  }
}

#define K 3
#define BLOCK 128

uint32_t N;
size_t num_words;
unsigned int *filter;
std::vector<uint8_t> model;
std::set<uint32_t> truth;
size_t bad_deletes = 0;

__host__ __device__ uint32_t rotate_left(uint32_t y, int r) {
  return (y << r) | (y >> (32 - r));
}

__host__ __device__ uint32_t scramble(uint32_t y, int a, int b, uint32_t mul) {
  y ^= rotate_left(y, a) ^ rotate_left(y, b);
  y *= mul;
  y ^= y >> 15;
  return y;
}

__host__ __device__ uint32_t hash1(uint32_t key, uint32_t n) { return scramble(key * 0x9E3779B1u + 0x7F4A7C15u, 13, 24, 0x2C1B3C6Du) % n; }
__host__ __device__ uint32_t hash2(uint32_t key, uint32_t n) { return scramble(key * 0x85EBCA6Bu + 0x165667B1u, 7, 19, 0x297A2D39u) % n; }
__host__ __device__ uint32_t hash3(uint32_t key, uint32_t n) { return scramble(key * 0xC2B2AE35u + 0x27D4EB2Fu, 11, 22, 0x7FEB352Du) % n; }
__host__ __device__ uint32_t hash4(uint32_t key, uint32_t n) { return scramble(key * 0x27D4EB2Fu + 0x9E3779B9u, 5, 17, 0x846CA68Bu) % n; }

__host__ __device__ void positions(uint32_t key, uint32_t N, uint32_t *pos) {
#ifdef BLOCKED
  uint32_t block = hash4(key, N / BLOCK);
  pos[0] = block * BLOCK + hash1(key, BLOCK);
  pos[1] = block * BLOCK + hash2(key, BLOCK);
  pos[2] = block * BLOCK + hash3(key, BLOCK);
#else
  pos[0] = hash1(key, N);
  pos[1] = hash2(key, N);
  pos[2] = hash3(key, N);
#endif
}

__device__ void counter_add(unsigned int *words, uint32_t pos) {
  atomicAdd(&words[pos >> 2], 1u << ((pos & 3) * 8));
}

__device__ bool counter_sub(unsigned int *words, uint32_t pos) {
  unsigned int shift = (pos & 3) * 8;
  unsigned int old = words[pos >> 2];
  while (((old >> shift) & 0xFF) != 0) {
    unsigned int seen = atomicCAS(&words[pos >> 2], old, old - (1u << shift));
    if (seen == old) return true;
    old = seen;
  }
  return false;
}

__device__ unsigned int counter_get(const unsigned int *words, uint32_t pos) {
  return (words[pos >> 2] >> ((pos & 3) * 8)) & 0xFF;
}

__global__ void insert_kernel(unsigned int *words, const uint32_t *keys, int n,
                              uint32_t N) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  uint32_t pos[K];
  positions(keys[i], N, pos);
  for (int j = 0; j < K; j++) counter_add(words, pos[j]);
}

__global__ void search_kernel(const unsigned int *words, const uint32_t *keys,
                              int n, uint32_t N, uint8_t *found) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  uint32_t pos[K];
  positions(keys[i], N, pos);
  uint8_t present = 1;
  for (int j = 0; j < K; j++)
    if (counter_get(words, pos[j]) == 0) present = 0;
  found[i] = present;
}

__global__ void delete_kernel(unsigned int *words, const uint32_t *keys,
                              const uint8_t *present, int n, uint32_t N) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n || !present[i]) return;
  uint32_t pos[K];
  positions(keys[i], N, pos);
  for (int j = 0; j < K; j++) counter_sub(words, pos[j]);
}

std::vector<uint32_t> read_trace(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long bytes = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint32_t> keys(bytes / 4);
  if (fread(keys.data(), 4, keys.size(), f) != keys.size()) exit(1);
  fclose(f);
  return keys;
}

std::vector<uint32_t> unique_keys(std::vector<uint32_t> keys) {
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

void insert_host(const std::vector<uint32_t> keys) {
  int n = keys.size();
  if (n == 0) return;
  uint32_t *dev_keys;
  cudaCheckError(cudaMalloc(&dev_keys, n * sizeof(uint32_t)));
  cudaCheckError(cudaMemcpy(dev_keys, keys.data(), n * sizeof(uint32_t), cudaMemcpyHostToDevice));
  insert_kernel<<<(n + 255) / 256, 256>>>(filter, dev_keys, n, N);
  cudaCheckError(cudaGetLastError());
  cudaCheckError(cudaDeviceSynchronize());
  cudaFree(dev_keys);
}

void delete_host(const std::vector<uint32_t> keys) {
  int n = keys.size();
  if (n == 0) return;
  uint32_t *dev_keys;
  uint8_t *dev_present;
  cudaCheckError(cudaMalloc(&dev_keys, n * sizeof(uint32_t)));
  cudaCheckError(cudaMalloc(&dev_present, n));
  cudaCheckError(cudaMemcpy(dev_keys, keys.data(), n * sizeof(uint32_t), cudaMemcpyHostToDevice));
  search_kernel<<<(n + 255) / 256, 256>>>(filter, dev_keys, n, N, dev_present);
  cudaCheckError(cudaGetLastError());
  delete_kernel<<<(n + 255) / 256, 256>>>(filter, dev_keys, dev_present, n, N);
  cudaCheckError(cudaGetLastError());
  cudaCheckError(cudaDeviceSynchronize());
  cudaFree(dev_keys);
  cudaFree(dev_present);
}

std::vector<uint8_t> search_host(const std::vector<uint32_t> keys) {
  int n = keys.size();
  std::vector<uint8_t> found(n);
  if (n == 0) return found;
  uint32_t *dev_keys;
  uint8_t *dev_found;
  cudaCheckError(cudaMalloc(&dev_keys, n * sizeof(uint32_t)));
  cudaCheckError(cudaMalloc(&dev_found, n));
  cudaCheckError(cudaMemcpy(dev_keys, keys.data(), n * sizeof(uint32_t), cudaMemcpyHostToDevice));
  search_kernel<<<(n + 255) / 256, 256>>>(filter, dev_keys, n, N, dev_found);
  cudaCheckError(cudaGetLastError());
  cudaCheckError(cudaMemcpy(found.data(), dev_found, n, cudaMemcpyDeviceToHost));
  cudaFree(dev_keys);
  cudaFree(dev_found);
  return found;
}

void model_insert(const std::vector<uint32_t> keys) {
  for (size_t i = 0; i < keys.size(); i++) {
    uint32_t pos[K];
    positions(keys[i], N, pos);
    for (int j = 0; j < K; j++) model[pos[j]]++;
    truth.insert(keys[i]);
  }
}

void model_delete(const std::vector<uint32_t> keys) {
  std::vector<uint8_t> present(keys.size());
  for (size_t i = 0; i < keys.size(); i++) {
    uint32_t pos[K];
    positions(keys[i], N, pos);
    present[i] = 1;
    for (int j = 0; j < K; j++)
      if (model[pos[j]] == 0) present[i] = 0;
  }
  for (size_t i = 0; i < keys.size(); i++) {
    if (!present[i]) continue;
    if (truth.count(keys[i]) == 0) bad_deletes++;
    uint32_t pos[K];
    positions(keys[i], N, pos);
    for (int j = 0; j < K; j++)
      if (model[pos[j]] > 0) model[pos[j]]--;
  }
  for (size_t i = 0; i < keys.size(); i++) truth.erase(keys[i]);
}

void check_result(const char *stage) {
  std::vector<uint8_t> gpu(num_words * 4);
  cudaCheckError(cudaMemcpy(gpu.data(), filter, gpu.size(), cudaMemcpyDeviceToHost));
  size_t differ = 0;
  for (size_t p = 0; p < N; p++)
    if (gpu[p] != model[p]) differ++;
  printf("%s: %zu counters differ from the host model\n", stage, differ);
}

void report_search(const char *stage, const std::vector<uint32_t> keys,
                   const std::vector<uint8_t> found) {
  size_t present = 0, false_pos = 0, false_neg = 0;
  for (size_t i = 0; i < keys.size(); i++) {
    bool real = truth.count(keys[i]) > 0;
    if (found[i]) present++;
    if (found[i] && !real) false_pos++;
    if (!found[i] && real) false_neg++;
  }
  printf("%s: %zu searches, %zu present, %zu false negatives, %zu false positives, "
         "false positive rate %.6f\n",
         stage, keys.size(), present, false_neg, false_pos,
         keys.empty() ? 0.0 : (double)false_pos / keys.size());
}

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: %s N <insert-trace> <search-trace> <delete-trace>\n", argv[0]);
    return 1;
  }
  N = strtoul(argv[1], NULL, 10);
  if (N == 0) {
    fprintf(stderr, "N must be at least 1\n");
    return 1;
  }
#ifdef BLOCKED
  N = (N + BLOCK - 1) / BLOCK * BLOCK;
#endif
  num_words = (N + 3) / 4;
  cudaCheckError(cudaMalloc(&filter, num_words * 4));
  cudaCheckError(cudaMemset(filter, 0, num_words * 4));
  model.assign(N, 0);

  std::vector<uint32_t> insert_keys = unique_keys(read_trace(argv[2]));
  std::vector<uint32_t> search_keys = unique_keys(read_trace(argv[3]));
  std::vector<uint32_t> delete_keys = unique_keys(read_trace(argv[4]));

  insert_host(insert_keys);
  model_insert(insert_keys);
  check_result("after insert");
  report_search("search after insert", search_keys, search_host(search_keys));

  delete_host(delete_keys);
  model_delete(delete_keys);
  check_result("after delete");
  printf("deleted keys that were never inserted but looked present: %zu\n", bad_deletes);
  report_search("search after delete", search_keys, search_host(search_keys));

  cudaFree(filter);
  return 0;
}
