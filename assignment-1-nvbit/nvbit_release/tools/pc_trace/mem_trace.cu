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

/* CS623 Assignment 1 - Problem II: PC trace tool. Derived from NVBit's
 * mem_trace example tool, but instead of instrumenting only memory
 * reference operands, it instruments EVERY static instruction and
 * records the PC of every instruction executed by an active warp
 * (predicates ignored, per the assignment statement). The resulting
 * trace is written as a flat binary file of little-endian uint64_t PCs
 * to PC_TRACE_FILE (default "pc_trace.bin"), consumed by ../p2.c. */

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

/* every tool needs to include this once */
#include "nvbit_tool.h"

/* nvbit interface file */
#include "nvbit.h"

/* for channel */
#include "utils/channel.hpp"

/* contains definition of the pc_access_t structure */
#include "common.h"

#define CHANNEL_SIZE (1l << 20)

struct CTXstate {
    int id;
    ChannelDev *channel_dev;
    ChannelHost channel_host;
    volatile bool recv_thread_done = false;
};

pthread_mutex_t mutex;
pthread_mutex_t cuda_event_mutex;

std::unordered_map<CUcontext, CTXstate *> ctx_state_map;

bool skip_callback_flag = false;

uint32_t instr_begin_interval = 0;
uint32_t instr_end_interval = UINT32_MAX;
int verbose = 0;

uint64_t grid_launch_id = 0;

/* output trace file (binary, one uint64_t PC per executed warp instr) */
FILE *pc_trace_fp = NULL;

void *recv_thread_fun(void *args);

void nvbit_at_init() {
    setenv("CUDA_MANAGED_FORCE_DEVICE_ALLOC", "1", 1);
    GET_VAR_INT(
        instr_begin_interval, "INSTR_BEGIN", 0,
        "Beginning of the instruction interval where to apply instrumentation");
    GET_VAR_INT(
        instr_end_interval, "INSTR_END", UINT32_MAX,
        "End of the instruction interval where to apply instrumentation");
    GET_VAR_INT(verbose, "TOOL_VERBOSE", 0, "Enable verbosity inside the tool");
    std::string pad(100, '-');
    printf("%s\n", pad.c_str());

    const char *out_path = getenv("PC_TRACE_FILE");
    if (!out_path) out_path = "pc_trace.bin";
    pc_trace_fp = fopen(out_path, "wb");
    if (!pc_trace_fp) {
        fprintf(stderr, "PC_TRACE: failed to open output file %s\n", out_path);
        exit(1);
    }
    printf("PC_TRACE: writing PC trace to %s\n", out_path);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex, &attr);
    pthread_mutex_init(&cuda_event_mutex, &attr);
}

std::unordered_set<CUfunction> already_instrumented;

void instrument_function_if_needed(CUcontext ctx, CUfunction func) {
    assert(ctx_state_map.find(ctx) != ctx_state_map.end());
    CTXstate *ctx_state = ctx_state_map[ctx];

    std::vector<CUfunction> related_functions =
        nvbit_get_related_functions(ctx, func);
    related_functions.push_back(func);

    for (auto f : related_functions) {
        if (!already_instrumented.insert(f).second) {
            continue;
        }

        const std::vector<Instr *> &instrs = nvbit_get_instrs(ctx, f);

        if (verbose) {
            printf(
                "PC_TRACE: CTX %p, Inspecting CUfunction %p name %s at "
                "address 0x%lx\n",
                ctx, f, nvbit_get_func_name(ctx, f), nvbit_get_func_addr(f));
        }

        /* resolved base address of this function; instr->getOffset() is
         * the static byte offset of the instruction within the function,
         * so base + offset is the instruction's fully resolved PC. Both
         * are known at instrumentation time (the function is already
         * loaded), so we pass the PC as a compile-time constant. */
        uint64_t func_addr = nvbit_get_func_addr(f);

        uint32_t cnt = 0;
        for (auto instr : instrs) {
            if (cnt < instr_begin_interval || cnt >= instr_end_interval) {
                cnt++;
                continue;
            }
            if (verbose) {
                instr->printDecoded();
            }

            uint64_t pc = func_addr + instr->getOffset();

            /* instrument EVERY instruction (not just memory references):
             * we want the PC of every instruction executed by an active
             * warp, ignoring predicates */
            nvbit_insert_call(instr, "record_pc", IPOINT_BEFORE);
            nvbit_add_call_arg_const_val64(instr, pc);
            nvbit_add_call_arg_const_val64(instr,
                                            (uint64_t)ctx_state->channel_dev);
            cnt++;
        }
    }
}

__global__ void flush_channel(ChannelDev *ch_dev) { ch_dev->flush(); }

void init_context_state(CUcontext ctx) {
    CTXstate *ctx_state = ctx_state_map[ctx];
    ctx_state->recv_thread_done = false;
    cudaMallocManaged(&ctx_state->channel_dev, sizeof(ChannelDev));
    ctx_state->channel_host.init((int)ctx_state_map.size() - 1, CHANNEL_SIZE,
                                  ctx_state->channel_dev, recv_thread_fun, ctx);
    nvbit_set_tool_pthread(ctx_state->channel_host.get_thread());
}

void nvbit_at_cuda_event(CUcontext ctx, int is_exit, nvbit_api_cuda_t cbid,
                          const char *name, void *params, CUresult *pStatus) {
    pthread_mutex_lock(&cuda_event_mutex);

    if (skip_callback_flag) {
        pthread_mutex_unlock(&cuda_event_mutex);
        return;
    }
    skip_callback_flag = true;

    if (cbid == API_CUDA_cuLaunchKernel_ptsz ||
        cbid == API_CUDA_cuLaunchKernel ||
        cbid == API_CUDA_cuLaunchCooperativeKernel ||
        cbid == API_CUDA_cuLaunchCooperativeKernel_ptsz ||
        cbid == API_CUDA_cuLaunchKernelEx ||
        cbid == API_CUDA_cuLaunchKernelEx_ptsz) {
        CTXstate *ctx_state = ctx_state_map[ctx];

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
            cudaDeviceSynchronize();
            assert(cudaGetLastError() == cudaSuccess);

            instrument_function_if_needed(ctx, func);

            const char *func_name = nvbit_get_func_name(ctx, func);
            uint64_t pc = nvbit_get_func_addr(func);

            nvbit_set_at_launch(ctx, func, (uint64_t)&grid_launch_id);
            nvbit_enable_instrumented(ctx, func, true);

            printf(
                "PC_TRACE: CTX 0x%016lx - LAUNCH - Kernel pc 0x%016lx - "
                "Kernel name %s - grid launch id %ld\n",
                (uint64_t)ctx, pc, func_name, grid_launch_id);
        } else {
            cudaDeviceSynchronize();
            flush_channel<<<1, 1>>>(ctx_state->channel_dev);
            cudaDeviceSynchronize();
            assert(cudaGetLastError() == cudaSuccess);
            grid_launch_id++;
        }
    }
    skip_callback_flag = false;
    pthread_mutex_unlock(&cuda_event_mutex);
}

void *recv_thread_fun(void *args) {
    CUcontext ctx = (CUcontext)args;

    pthread_mutex_lock(&mutex);
    assert(ctx_state_map.find(ctx) != ctx_state_map.end());
    CTXstate *ctx_state = ctx_state_map[ctx];

    ChannelHost *ch_host = &ctx_state->channel_host;
    pthread_mutex_unlock(&mutex);
    char *recv_buffer = (char *)malloc(CHANNEL_SIZE);

    while (!ctx_state->recv_thread_done) {
        uint32_t num_recv_bytes = ch_host->recv(recv_buffer, CHANNEL_SIZE);
        if (num_recv_bytes > 0) {
            uint32_t num_processed_bytes = 0;
            while (num_processed_bytes < num_recv_bytes) {
                pc_access_t *pa =
                    (pc_access_t *)&recv_buffer[num_processed_bytes];

                pthread_mutex_lock(&mutex);
                fwrite(&pa->pc, sizeof(uint64_t), 1, pc_trace_fp);
                pthread_mutex_unlock(&mutex);

                if (verbose) {
                    printf("PC_TRACE: 0x%016lx\n", (unsigned long)pa->pc);
                }
                num_processed_bytes += sizeof(pc_access_t);
            }
        }
    }
    ctx_state->recv_thread_done = false;
    free(recv_buffer);
    return NULL;
}

void nvbit_at_ctx_init(CUcontext ctx) {
    pthread_mutex_lock(&mutex);
    if (verbose) {
        printf("PC_TRACE: STARTING CONTEXT %p\n", ctx);
    }
    assert(ctx_state_map.find(ctx) == ctx_state_map.end());
    CTXstate *ctx_state = new CTXstate;
    ctx_state_map[ctx] = ctx_state;
    pthread_mutex_unlock(&mutex);
}

void nvbit_tool_init(CUcontext ctx) {
    pthread_mutex_lock(&mutex);
    assert(ctx_state_map.find(ctx) != ctx_state_map.end());
    init_context_state(ctx);
    pthread_mutex_unlock(&mutex);
}

void nvbit_at_ctx_term(CUcontext ctx) {
    pthread_mutex_lock(&mutex);
    skip_callback_flag = true;
    if (verbose) {
        printf("PC_TRACE: TERMINATING CONTEXT %p\n", ctx);
    }
    assert(ctx_state_map.find(ctx) != ctx_state_map.end());
    CTXstate *ctx_state = ctx_state_map[ctx];

    ctx_state->recv_thread_done = true;
    while (!ctx_state->recv_thread_done)
        ;

    ctx_state->channel_host.destroy(false);
    cudaFree(ctx_state->channel_dev);
    skip_callback_flag = false;
    delete ctx_state;
    pthread_mutex_unlock(&mutex);
}

void nvbit_at_term() {
    if (pc_trace_fp) fclose(pc_trace_fp);
}
