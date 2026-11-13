#include <chrono>
#include <cstdlib>
#include <iostream>
#include <numeric>

#define THRESHOLD (std::numeric_limits<float>::epsilon())

using std::cout;
using std::endl;

using std::chrono::duration_cast;
using HR = std::chrono::high_resolution_clock;
using HRTimer = HR::time_point;
using std::chrono::microseconds;
using std::chrono::milliseconds;

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

// TODO: Evaluate with three sizes, 64, 128, and 256.
const uint32_t Input_Width = (1 << 8);
const uint32_t Input_Height = (1 << 8);
const uint32_t Input_Num_Elems = (Input_Width * Input_Width * Input_Width);
const uint32_t Input_Size_Bytes = Input_Num_Elems * sizeof(float);

const uint32_t Filter_Width = 5;
const uint32_t Filter_Num_Elems = (Filter_Width * Filter_Width * Filter_Width);
const uint32_t Filter_Size_Bytes = Filter_Num_Elems * sizeof(float);

// TODO: Implement a single-threaded CPU version
void seq_cpu_conv(const float *h_input, const float *h_filter,
                  float *h_output) {
  const size_t N = Input_Width, K = Filter_Width;
  for (size_t z = 0; z < N; z++)
    for (size_t y = 0; y < N; y++)
      for (size_t x = 0; x < N; x++) {
        float sum = 0;
        for (size_t fz = 0; fz < K; fz++)
          for (size_t fy = 0; fy < K; fy++)
            for (size_t fx = 0; fx < K; fx++) {
              size_t zz = z + fz - 2, yy = y + fy - 2, xx = x + fx - 2;
              if (zz >= N || yy >= N || xx >= N) continue;
              sum += h_filter[(fz * K + fy) * K + fx] *
                     h_input[(zz * N + yy) * N + xx];
            }
        h_output[(z * N + y) * N + x] = sum;
      }
}

// TODO: Implement a naive version using loads and stores to global memory. Edit
// the function definition as required.
__global__ void basic_kernel(const float *input, const float *filter,
                             float *output) {
  const size_t N = Input_Width, K = Filter_Width;
  size_t x = blockIdx.x * blockDim.x + threadIdx.x;
  size_t y = blockIdx.y * blockDim.y + threadIdx.y;
  size_t z = blockIdx.z * blockDim.z + threadIdx.z;
  if (x >= N || y >= N || z >= N) return;

  float sum = 0;
  for (size_t fz = 0; fz < K; fz++)
    for (size_t fy = 0; fy < K; fy++)
      for (size_t fx = 0; fx < K; fx++) {
        size_t zz = z + fz - 2, yy = y + fy - 2, xx = x + fx - 2;
        if (zz >= N || yy >= N || xx >= N) continue;
        sum += filter[(fz * K + fy) * K + fx] * input[(zz * N + yy) * N + xx];
      }
  output[(z * N + y) * N + x] = sum;
}

__constant__ float c_filter[Filter_Num_Elems];

// TODO: Use constant memory for accessing the filter and shared memory for
// storing the input array including halo cells.
__global__ void opt_kernel(const float *input, float *output) {
  const size_t N = Input_Width, K = Filter_Width;
  __shared__ float tile[12][12][12];

  size_t tid = (threadIdx.z * 8 + threadIdx.y) * 8 + threadIdx.x;
  size_t x0 = blockIdx.x * 8, y0 = blockIdx.y * 8, z0 = blockIdx.z * 8;

  for (size_t i = tid; i < 12 * 12 * 12; i += 512) {
    size_t tx = i % 12, ty = (i / 12) % 12, tz = i / 144;
    size_t gx = x0 + tx - 2, gy = y0 + ty - 2, gz = z0 + tz - 2;
    float v = 0;
    if (gx < N && gy < N && gz < N) v = input[(gz * N + gy) * N + gx];
    tile[tz][ty][tx] = v;
  }
  __syncthreads();

  float sum = 0;
  for (int fz = 0; fz < K; fz++)
    for (int fy = 0; fy < K; fy++) {
      int row = (fz * K + fy) * K;
      sum += c_filter[row] * tile[threadIdx.z + fz][threadIdx.y + fy][threadIdx.x]
           + c_filter[row + 1] * tile[threadIdx.z + fz][threadIdx.y + fy][threadIdx.x + 1]
           + c_filter[row + 2] * tile[threadIdx.z + fz][threadIdx.y + fy][threadIdx.x + 2]
           + c_filter[row + 3] * tile[threadIdx.z + fz][threadIdx.y + fy][threadIdx.x + 3]
           + c_filter[row + 4] * tile[threadIdx.z + fz][threadIdx.y + fy][threadIdx.x + 4];
    }

  output[((z0 + threadIdx.z) * N + (y0 + threadIdx.y)) * N + (x0 + threadIdx.x)] = sum;
}

void check_result(const float *w_ref, const float *w_opt, const uint32_t N) {
  double maxdiff = 0.0;
  int numdiffs = 0;

  for (uint64_t i = 0; i < (N * N * N); i++) {
    double this_diff = w_ref[i] - w_opt[i];
    if (std::fabs(this_diff) > THRESHOLD) {
      numdiffs++;
      if (this_diff > maxdiff) {
        maxdiff = this_diff;
      }
    }
  }

  if (numdiffs > 0) {
    cout << numdiffs << " Diffs found over THRESHOLD " << THRESHOLD
         << "; Max Diff = " << maxdiff << endl;
  } else {
    cout << "No differences found between base and test versions\n";
  }
}

void print3D(const float *A, const uint32_t N) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      for (int k = 0; k < N; ++k) {
        cout << A[i * N * N + j * N + k] << "\t";
      }
      cout << "n";
    }
    cout << "n";
  }
}

int main() {
  auto *h_input = new float[Input_Num_Elems];
  std::fill_n(h_input, Input_Num_Elems, 1.0F);

  const float h_filter3D[Filter_Width][Filter_Width][Filter_Width] = {
      {2.0F, 3.0F, 4.0F, 3.0F, 2.0F},
      {4.0F, 5.0F, 6.0F, 5.0F, 4.0F},
      {3.0F, 4.0F, 5.0F, 4.0F, 3.0F}};
  auto *h_filter = new float[Filter_Num_Elems];
  memcpy(h_filter, h_filter3D, Filter_Size_Bytes);

  auto *h_output_cpu = new float[Input_Num_Elems];
  HRTimer start = HR::now();
  seq_cpu_conv(h_input, h_filter, h_output_cpu);
  HRTimer end = HR::now();
  auto duration = duration_cast<milliseconds>(end - start).count();
  cout << "Serial version: " << duration << " ms\n";

  float *d_input = nullptr;
  float *d_output = nullptr;
  float *d_filter = nullptr;
  auto *h_output_gpu = new float[Input_Num_Elems];
  cudaCheckError(cudaMalloc(&d_input, Input_Size_Bytes));
  cudaCheckError(cudaMalloc(&d_output, Input_Size_Bytes));
  cudaCheckError(cudaMalloc(&d_filter, Filter_Size_Bytes));

  cudaEvent_t ev_start, ev_stop;
  cudaCheckError(cudaEventCreate(&ev_start));
  cudaCheckError(cudaEventCreate(&ev_stop));
  dim3 block(8, 8, 8);
  dim3 grid(Input_Width / 8, Input_Width / 8, Input_Width / 8);

  float kernel_time = 0.0f;
  double total_time = 0.0;

  // TODO: Invoke kernel3D_basic()
  // TODO: Adapt check_result()
  for (int run = -1; run < 10; run++) {
    HRTimer t0 = HR::now();
    cudaCheckError(cudaMemcpy(d_input, h_input, Input_Size_Bytes, cudaMemcpyHostToDevice));
    cudaCheckError(cudaMemcpy(d_filter, h_filter, Filter_Size_Bytes, cudaMemcpyHostToDevice));
    cudaCheckError(cudaEventRecord(ev_start));
    basic_kernel<<<grid, block>>>(d_input, d_filter, d_output);
    cudaCheckError(cudaEventRecord(ev_stop));
    cudaCheckError(cudaGetLastError());
    cudaCheckError(cudaMemcpy(h_output_gpu, d_output, Input_Size_Bytes, cudaMemcpyDeviceToHost));
    HRTimer t1 = HR::now();
    if (run < 0) continue;
    float ms;
    cudaCheckError(cudaEventElapsedTime(&ms, ev_start, ev_stop));
    kernel_time += ms;
    total_time += duration_cast<microseconds>(t1 - t0).count() / 1000.0;
  }
  kernel_time /= 10;
  check_result(h_output_cpu, h_output_gpu, Input_Width);

  cout << "Kernel3D_basic time (ms): " << kernel_time << "\n";
  cout << "Kernel3D_basic end-to-end time (ms): " << total_time / 10 << "\n";

  kernel_time = 0.0f;
  total_time = 0.0;

  // TODO: Invoke kernel3D_opt()
  // TODO: Adapt check_result()
  for (int run = -1; run < 10; run++) {
    HRTimer t0 = HR::now();
    cudaCheckError(cudaMemcpy(d_input, h_input, Input_Size_Bytes, cudaMemcpyHostToDevice));
    cudaCheckError(cudaMemcpyToSymbol(c_filter, h_filter, Filter_Size_Bytes));
    cudaCheckError(cudaEventRecord(ev_start));
    opt_kernel<<<grid, block>>>(d_input, d_output);
    cudaCheckError(cudaEventRecord(ev_stop));
    cudaCheckError(cudaGetLastError());
    cudaCheckError(cudaMemcpy(h_output_gpu, d_output, Input_Size_Bytes, cudaMemcpyDeviceToHost));
    HRTimer t1 = HR::now();
    if (run < 0) continue;
    float ms;
    cudaCheckError(cudaEventElapsedTime(&ms, ev_start, ev_stop));
    kernel_time += ms;
    total_time += duration_cast<microseconds>(t1 - t0).count() / 1000.0;
  }
  kernel_time /= 10;
  check_result(h_output_cpu, h_output_gpu, Input_Width);
  cout << "Kernel3D_opt time (ms): " << kernel_time << "\n";
  cout << "Kernel3D_opt end-to-end time (ms): " << total_time / 10 << "\n";

  // TODO: Free memory
  cudaCheckError(cudaEventDestroy(ev_start));
  cudaCheckError(cudaEventDestroy(ev_stop));
  cudaCheckError(cudaFree(d_input));
  cudaCheckError(cudaFree(d_output));
  cudaCheckError(cudaFree(d_filter));
  delete[] h_input;
  delete[] h_filter;
  delete[] h_output_cpu;
  delete[] h_output_gpu;

  return EXIT_SUCCESS;
}
