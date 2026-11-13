/* Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* CS623 Assignment 1 - Problem I: thread hierarchy + control (thread)
 * divergence tool. Derived from NVBit's instr_count example tool. */

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unordered_set>

/* every tool needs to include this once */
#include "nvbit_tool.h"

/* nvbit interface file */
#include "nvbit.h"

/* nvbit utility functions */
#include "utils/utils.h"

/* kernel id counter, maintained in system memory */
uint32_t kernel_id = 0;

/* per-kernel warp-instruction counter and accumulated "inactive lane"
 * count, both updated by the GPU while the kernel runs. Reset before
 * every kernel launch. */
__managed__ unsigned long long warp_instrs = 0;
__managed__ unsigned long long inactive_lanes = 0;

/* global control variables for this tool */
uint32_t instr_begin_interval = 0;
uint32_t instr_end_interval = UINT32_MAX;
uint32_t start_grid_num = 0;
uint32_t end_grid_num = UINT32_MAX;
int verbose = 0;
bool mangled = false;

/* a pthread mutex, used to prevent multiple kernels to run concurrently and
 * therefore to "corrupt" the counters */
pthread_mutex_t mutex;

void nvbit_at_init() {
    setenv("CUDA_MANAGED_FORCE_DEVICE_ALLOC", "1", 1);

    GET_VAR_INT(
        instr_begin_interval, "INSTR_BEGIN", 0,
        "Beginning of the instruction interval where to apply instrumentation");
    GET_VAR_INT(
        instr_end_interval, "INSTR_END", UINT32_MAX,
        "End of the instruction interval where to apply instrumentation");
    GET_VAR_INT(start_grid_num, "START_GRID_NUM", 0,
                "Beginning of the kernel launch interval where to apply "
                "instrumentation");
    GET_VAR_INT(
        end_grid_num, "END_GRID_NUM", UINT32_MAX,
        "End of the kernel launch interval where to apply instrumentation");
    GET_VAR_INT(mangled, "MANGLED_NAMES", 1,
                "Print kernel names mangled or not");
    GET_VAR_INT(verbose, "TOOL_VERBOSE", 0, "Enable verbosity inside the tool");

    std::string pad(100, '-');
    printf("%s\n", pad.c_str());
}

/* Set used to avoid re-instrumenting the same functions multiple times */
std::unordered_set<CUfunction> already_instrumented;

void instrument_function_if_needed(CUcontext ctx, CUfunction func) {
    std::vector<CUfunction> related_functions =
        nvbit_get_related_functions(ctx, func);
    related_functions.push_back(func);

    for (auto f : related_functions) {
        if (!already_instrumented.insert(f).second) {
            continue;
        }

        const std::vector<Instr *> &instrs = nvbit_get_instrs(ctx, f);

        if (verbose) {
            printf("inspecting %s - num instrs %ld\n",
                   nvbit_get_func_name(ctx, f), instrs.size());
        }

        for (auto i : instrs) {
            if (i->getIdx() >= instr_begin_interval &&
                i->getIdx() < instr_end_interval) {
                if (verbose == 1) {
                    i->print();
                } else if (verbose == 2) {
                    i->printDecoded();
                }

                /* Insert a call to "count_div" before every instruction so
                 * that we see the active-mask of every executed warp
                 * instruction, regardless of predicate (see inject_funcs.cu)
                 */
                nvbit_insert_call(i, "count_div", IPOINT_BEFORE);
                nvbit_add_call_arg_const_val64(i, (uint64_t)&warp_instrs);
                nvbit_add_call_arg_const_val64(i, (uint64_t)&inactive_lanes);
            }
        }
    }
}

void nvbit_at_cuda_event(CUcontext ctx, int is_exit, nvbit_api_cuda_t cbid,
                          const char *name, void *params, CUresult *pStatus) {
    if (cbid == API_CUDA_cuLaunch || cbid == API_CUDA_cuLaunchKernel_ptsz ||
        cbid == API_CUDA_cuLaunchGrid || cbid == API_CUDA_cuLaunchGridAsync ||
        cbid == API_CUDA_cuLaunchKernel ||
        cbid == API_CUDA_cuLaunchKernelEx ||
        cbid == API_CUDA_cuLaunchKernelEx_ptsz) {
        CUfunction func;
        if (cbid == API_CUDA_cuLaunchKernelEx_ptsz ||
            cbid == API_CUDA_cuLaunchKernelEx) {
            cuLaunchKernelEx_params *p = (cuLaunchKernelEx_params *)params;
            func = p->f;
        } else {
            cuLaunchKernel_params *p = (cuLaunchKernel_params *)params;
            func = p->f;
        }

        if (!is_exit) {
            pthread_mutex_lock(&mutex);
            instrument_function_if_needed(ctx, func);

            bool active_region = true;
            if (kernel_id < start_grid_num || kernel_id >= end_grid_num) {
                active_region = false;
            }
            nvbit_enable_instrumented(ctx, func, active_region);

            warp_instrs = 0;
            inactive_lanes = 0;
        } else {
            CUDA_SAFECALL(cudaDeviceSynchronize());

            int grid_x = 0, grid_y = 0, grid_z = 0;
            int block_x = 0, block_y = 0, block_z = 0;
            if (cbid == API_CUDA_cuLaunchKernel_ptsz ||
                cbid == API_CUDA_cuLaunchKernel) {
                cuLaunchKernel_params *p2 = (cuLaunchKernel_params *)params;
                grid_x = p2->gridDimX;
                grid_y = p2->gridDimY;
                grid_z = p2->gridDimZ;
                block_x = p2->blockDimX;
                block_y = p2->blockDimY;
                block_z = p2->blockDimZ;
            } else if (cbid == API_CUDA_cuLaunchKernelEx_ptsz ||
                       cbid == API_CUDA_cuLaunchKernelEx) {
                cuLaunchKernelEx_params *p2 = (cuLaunchKernelEx_params *)params;
                grid_x = p2->config->gridDimX;
                grid_y = p2->config->gridDimY;
                grid_z = p2->config->gridDimZ;
                block_x = p2->config->blockDimX;
                block_y = p2->config->blockDimY;
                block_z = p2->config->blockDimZ;
            }

            double divergence = 0.0;
            if (warp_instrs > 0) {
                divergence = (double)inactive_lanes / (32.0 * (double)warp_instrs);
            }

            printf(
                "\nkernel %d - %s\n"
                "  thread blocks (grid dim)  : %d x %d x %d\n"
                "  threads per block (block dim): %d x %d x %d\n"
                "  executed warp instructions: %llu\n"
                "  thread (control) divergence: %.6f\n",
                kernel_id++, nvbit_get_func_name(ctx, func, mangled), grid_x,
                grid_y, grid_z, block_x, block_y, block_z, warp_instrs,
                divergence);
            pthread_mutex_unlock(&mutex);
        }
    }
}

void nvbit_at_term() {
    std::string pad(100, '-');
    printf("%s\n", pad.c_str());
}
