#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
#include <chrono>

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

#ifndef VERSION
#define VERSION 2
#endif

#if VERSION == 3
#include <thrust/device_vector.h>
#include <thrust/transform.h>
#include <thrust/copy.h>
#include <thrust/count.h>
#include <thrust/iterator/counting_iterator.h>
#endif

typedef unsigned long long ull;

#if VERSION == 3
#define SLICE (1ull << 28)
#else
#define MAX_MATCHES (1 << 22)
#endif

__constant__ double c[10][10], d[10], e[10], start[10], step[10];
__constant__ int num_values[10];

double grid_start[10], grid_step[10];
int grid_count[10];
std::vector<ull> matches;

void load_inputs() {
  double disp[120], grid[30];
  double c_vals[10][10], d_vals[10], e_vals[10];

  FILE *f = fopen("disp.txt", "r");
  for (int i = 0; i < 120; i++) fscanf(f, "%lf", &disp[i]);
  fclose(f);
  f = fopen("grid.txt", "r");
  for (int i = 0; i < 30; i++) fscanf(f, "%lf", &grid[i]);
  fclose(f);

  for (int i = 0; i < 10; i++) {
    for (int j = 0; j < 10; j++) c_vals[i][j] = disp[i * 12 + j];
    d_vals[i] = disp[i * 12 + 10];
    e_vals[i] = 0.3 * disp[i * 12 + 11];
    grid_start[i] = grid[3 * i];
    grid_step[i] = grid[3 * i + 2];
    grid_count[i] = (int)floor((grid[3 * i + 1] - grid[3 * i]) / grid[3 * i + 2]);
  }

  cudaCheckError(cudaMemcpyToSymbol(c, c_vals, sizeof c_vals));
  cudaCheckError(cudaMemcpyToSymbol(d, d_vals, sizeof d_vals));
  cudaCheckError(cudaMemcpyToSymbol(e, e_vals, sizeof e_vals));
  cudaCheckError(cudaMemcpyToSymbol(start, grid_start, sizeof grid_start));
  cudaCheckError(cudaMemcpyToSymbol(step, grid_step, sizeof grid_step));
  cudaCheckError(cudaMemcpyToSymbol(num_values, grid_count, sizeof grid_count));
}

void write_results(const char *name) {
  std::sort(matches.begin(), matches.end());
  FILE *f = fopen(name, "w");
  for (size_t m = 0; m < matches.size(); m++) {
    ull rest = matches[m];
    int value_index[10];
    for (int j = 9; j >= 0; j--) {
      value_index[j] = rest % grid_count[j];
      rest /= grid_count[j];
    }
    for (int j = 0; j < 10; j++)
      fprintf(f, "%lf%c", grid_start[j] + value_index[j] * grid_step[j],
              j < 9 ? '\t' : '\n');
  }
  fclose(f);
}

#if VERSION == 1

__global__ void search_kernel(int index0, int index1, ull *match_ids,
                              unsigned int *match_count) {
  int tid = blockIdx.x * blockDim.x + threadIdx.x;
  if (tid >= num_values[2] * num_values[3] * num_values[4] * num_values[5])
    return;

  int value_index[10];
  value_index[0] = index0;
  value_index[1] = index1;
  int rest = tid;
  for (int j = 5; j >= 2; j--) {
    value_index[j] = rest % num_values[j];
    rest /= num_values[j];
  }

  for (value_index[6] = 0; value_index[6] < num_values[6]; value_index[6]++)
    for (value_index[7] = 0; value_index[7] < num_values[7]; value_index[7]++)
      for (value_index[8] = 0; value_index[8] < num_values[8]; value_index[8]++)
        for (value_index[9] = 0; value_index[9] < num_values[9]; value_index[9]++) {
          double value[10];
          for (int j = 0; j < 10; j++)
            value[j] = start[j] + value_index[j] * step[j];

          bool ok = true;
          for (int i = 0; i < 10; i++) {
            double sum = 0;
            for (int j = 0; j < 10; j++) sum += c[i][j] * value[j];
            if (fabs(sum - d[i]) > e[i]) ok = false;
          }

          if (ok) {
            ull point_id = 0;
            for (int j = 0; j < 10; j++)
              point_id = point_id * num_values[j] + value_index[j];
            unsigned int slot = atomicAdd(match_count, 1u);
            if (slot < MAX_MATCHES) match_ids[slot] = point_id;
          }
        }
}

int main() {
  load_inputs();

  ull *match_ids;
  unsigned int *match_count;
  cudaCheckError(cudaMalloc(&match_ids, MAX_MATCHES * sizeof(ull)));
  cudaCheckError(cudaMalloc(&match_count, sizeof(unsigned int)));

  int threads = grid_count[2] * grid_count[3] * grid_count[4] * grid_count[5];
  int blocks = (threads + 127) / 128;

  auto t0 = std::chrono::high_resolution_clock::now();
  for (int index0 = 0; index0 < grid_count[0]; index0++)
    for (int index1 = 0; index1 < grid_count[1]; index1++) {
      cudaCheckError(cudaMemset(match_count, 0, sizeof(unsigned int)));
      search_kernel<<<blocks, 128>>>(index0, index1, match_ids, match_count);
      cudaCheckError(cudaGetLastError());

      unsigned int num_matches;
      cudaCheckError(cudaMemcpy(&num_matches, match_count, sizeof num_matches,
                 cudaMemcpyDeviceToHost));
      if (num_matches > MAX_MATCHES) {
        fprintf(stderr, "match buffer overflow\n");
        return 1;
      }
      size_t old = matches.size();
      matches.resize(old + num_matches);
      cudaCheckError(cudaMemcpy(matches.data() + old, match_ids, num_matches * sizeof(ull),
                 cudaMemcpyDeviceToHost));
    }
  auto t1 = std::chrono::high_resolution_clock::now();

  write_results("results-v1.txt");
  printf("result pnts: %ld\n", (long)matches.size());
  printf("Total time = %f\n", std::chrono::duration<double>(t1 - t0).count());

  cudaFree(match_ids);
  cudaFree(match_count);
  return 0;
}

#elif VERSION == 2

__device__ void add_term(const double *from, double *to, int j, double value) {
  for (int i = 0; i < 10; i++) to[i] = from[i] + c[i][j] * value;
}

__global__ void search_kernel(int index0, ull *match_ids,
                              unsigned int *match_count) {
  int tid = blockIdx.x * blockDim.x + threadIdx.x;
  if (tid >= num_values[1] * num_values[2] * num_values[3] * num_values[4] *
                 num_values[5])
    return;

  int value_index[10];
  value_index[0] = index0;
  int rest = tid;
  for (int j = 5; j >= 1; j--) {
    value_index[j] = rest % num_values[j];
    rest /= num_values[j];
  }

  double partial[10][10];
  for (int i = 0; i < 10; i++) partial[0][i] = -d[i];
  for (int j = 0; j < 6; j++)
    add_term(partial[j], partial[j + 1], j,
             start[j] + value_index[j] * step[j]);

  for (value_index[6] = 0; value_index[6] < num_values[6]; value_index[6]++) {
    add_term(partial[6], partial[7], 6, start[6] + value_index[6] * step[6]);
    for (value_index[7] = 0; value_index[7] < num_values[7]; value_index[7]++) {
      add_term(partial[7], partial[8], 7, start[7] + value_index[7] * step[7]);
      for (value_index[8] = 0; value_index[8] < num_values[8]; value_index[8]++) {
        add_term(partial[8], partial[9], 8,
                 start[8] + value_index[8] * step[8]);
        for (value_index[9] = 0; value_index[9] < num_values[9]; value_index[9]++) {
          double value = start[9] + value_index[9] * step[9];

          bool ok = true;
          for (int i = 0; i < 10 && ok; i++)
            ok = fabs(partial[9][i] + c[i][9] * value) <= e[i];

          if (ok) {
            ull point_id = 0;
            for (int j = 0; j < 10; j++)
              point_id = point_id * num_values[j] + value_index[j];
            unsigned int slot = atomicAdd(match_count, 1u);
            if (slot < MAX_MATCHES) match_ids[slot] = point_id;
          }
        }
      }
    }
  }
}

int main() {
  load_inputs();

  ull *match_ids;
  unsigned int *match_count;
  cudaCheckError(cudaMalloc(&match_ids, MAX_MATCHES * sizeof(ull)));
  cudaCheckError(cudaMalloc(&match_count, sizeof(unsigned int)));

  int threads = grid_count[1] * grid_count[2] * grid_count[3] * grid_count[4] *
                grid_count[5];
  int blocks = (threads + 127) / 128;

  auto t0 = std::chrono::high_resolution_clock::now();
  for (int index0 = 0; index0 < grid_count[0]; index0++) {
    cudaCheckError(cudaMemset(match_count, 0, sizeof(unsigned int)));
    search_kernel<<<blocks, 128>>>(index0, match_ids, match_count);
    cudaCheckError(cudaGetLastError());

    unsigned int num_matches;
    cudaCheckError(cudaMemcpy(&num_matches, match_count, sizeof num_matches,
               cudaMemcpyDeviceToHost));
    if (num_matches > MAX_MATCHES) {
      fprintf(stderr, "match buffer overflow\n");
      return 1;
    }
    size_t old = matches.size();
    matches.resize(old + num_matches);
    cudaCheckError(cudaMemcpy(matches.data() + old, match_ids, num_matches * sizeof(ull),
               cudaMemcpyDeviceToHost));
  }
  auto t1 = std::chrono::high_resolution_clock::now();

  write_results("results-v2.txt");
  printf("result pnts: %ld\n", (long)matches.size());
  printf("Total time = %f\n", std::chrono::duration<double>(t1 - t0).count());

  cudaFree(match_ids);
  cudaFree(match_count);
  return 0;
}

#else

struct IsMatch {
  __device__ char operator()(ull point_id) const {
    int value_index[10];
    for (int j = 9; j >= 0; j--) {
      value_index[j] = point_id % num_values[j];
      point_id /= num_values[j];
    }

    double value[10];
    for (int j = 0; j < 10; j++)
      value[j] = start[j] + value_index[j] * step[j];

    for (int i = 0; i < 10; i++) {
      double sum = 0;
      for (int j = 0; j < 10; j++) sum += c[i][j] * value[j];
      if (fabs(sum - d[i]) > e[i]) return 0;
    }
    return 1;
  }
};

struct NonZero {
  __host__ __device__ bool operator()(char flag) const { return flag != 0; }
};

int main() {
  load_inputs();

  ull total = 1;
  for (int j = 0; j < 10; j++) total *= grid_count[j];

  thrust::device_vector<char> flags(SLICE);

  auto t0 = std::chrono::high_resolution_clock::now();
  for (ull base = 0; base < total; base += SLICE) {
    ull len = std::min(SLICE, total - base);
    thrust::counting_iterator<ull> first(base);

    thrust::transform(first, first + len, flags.begin(), IsMatch());

    size_t num_matches =
        thrust::count_if(flags.begin(), flags.begin() + len, NonZero());
    if (num_matches == 0) continue;

    thrust::device_vector<ull> match_ids(num_matches);
    thrust::copy_if(first, first + len, flags.begin(), match_ids.begin(),
                    NonZero());

    size_t old = matches.size();
    matches.resize(old + num_matches);
    thrust::copy(match_ids.begin(), match_ids.end(), matches.begin() + old);
  }
  auto t1 = std::chrono::high_resolution_clock::now();

  write_results("results-v3.txt");
  printf("result pnts: %ld\n", (long)matches.size());
  printf("Total time = %f\n", std::chrono::duration<double>(t1 - t0).count());
  return 0;
}

#endif
