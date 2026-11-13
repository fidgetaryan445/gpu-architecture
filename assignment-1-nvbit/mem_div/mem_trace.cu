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

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string>
#include <unordered_map>
#include <unordered_set>

/* every tool needs to include this once */
#include "nvbit_tool.h"

/* nvbit interface file */
#include "nvbit.h"

/* for channel */
#include "utils/channel.hpp"

/* contains definition of the mem_access_t structure */
#include "common.h"

#define CHANNEL_SIZE (1l << 20)

struct CTXstate {
    /* context id */
    int id;

    /* Channel used to communicate from GPU to CPU receiving thread */
    ChannelDev* channel_dev;
    ChannelHost channel_host;

    volatile bool recv_thread_done = false;
};

/* lock */
pthread_mutex_t mutex;
pthread_mutex_t cuda_event_mutex;

/* map to store context state */
std::unordered_map<CUcontext, CTXstate*> ctx_state_map;

/* skip flag used to avoid re-entry on the nvbit_callback when issuing
 * flush_channel kernel call */
bool skip_callback_flag = false;

/* global control variables for this tool */
uint32_t instr_begin_interval = 0;
uint32_t instr_end_interval = UINT32_MAX;
int verbose = 0;

/* grid launch id, incremented at every launch (informational only) */
uint64_t grid_launch_id = 0;

/* Grid dimensions of the CURRENT kernel launch, captured on the host at
 * launch time. Needed to linearize a CTA's (x,y,z) id into a single
 * "thread block k" index for Part B's round-robin SM assignment (SM =
 * k mod n). This is correct for any grid shape (1D/2D/3D), not just 1D.
 * Safe to update here because the tool's existing cudaDeviceSynchronize()
 * calls around every launch already guarantee the previous kernel's
 * trace records are fully drained before the next kernel starts. */
uint32_t cur_grid_dim_x = 1;
uint32_t cur_grid_dim_y = 1;

/* output trace file: "<block_id> <addr1_hex> <addr2_hex> ... <addrN_hex>",
 * one line per warp-level memory reference, N = number of active
 * threads for that reference */
FILE* mem_trace_fp = NULL;

void* recv_thread_fun(void* args);

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

    /* set mutex as recursive */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex, &attr);

    pthread_mutex_init(&cuda_event_mutex, &attr);

    /* open the memory trace output file, once, at tool start */
    mem_trace_fp = fopen("mem_trace.txt", "w");
    assert(mem_trace_fp != NULL);
}

/* Set used to avoid re-instrumenting the same functions multiple times */
std::unordered_set<CUfunction> already_instrumented;

void instrument_function_if_needed(CUcontext ctx, CUfunction func) {
    assert(ctx_state_map.find(ctx) != ctx_state_map.end());
    CTXstate* ctx_state = ctx_state_map[ctx];

    /* Get related functions of the kernel (device function that can be
     * called by the kernel) */
    std::vector<CUfunction> related_functions =
        nvbit_get_related_functions(ctx, func);

    /* add kernel itself to the related function vector */
    related_functions.push_back(func);

    /* iterate on function */
    for (auto f : related_functions) {
        /* "recording" function was instrumented, if set insertion failed
         * we have already encountered this function */
        if (!already_instrumented.insert(f).second) {
            continue;
        }

        /* get vector of instructions of function "f" */
        const std::vector<Instr*>& instrs = nvbit_get_instrs(ctx, f);

        if (verbose) {
            printf(
                "MEMTRACE: CTX %p, Inspecting CUfunction %p name %s at address "
                "0x%lx\n",
                ctx, f, nvbit_get_func_name(ctx, f), nvbit_get_func_addr(f));
        }

        uint32_t cnt = 0;
        /* iterate on all the static instructions in the function --
         * only memory instructions are instrumented (getMemorySpace()
         * NONE/CONSTANT are skipped, same as the original mem_trace
         * tool), since this problem is specifically about the memory
         * address trace */
        for (auto instr : instrs) {
            if (cnt < instr_begin_interval || cnt >= instr_end_interval ||
                instr->getMemorySpace() == InstrType::MemorySpace::NONE ||
                instr->getMemorySpace() == InstrType::MemorySpace::CONSTANT) {
                cnt++;
                continue;
            }
            if (verbose) {
                instr->printDecoded();
            }

            int mref_idx = 0;
            /* iterate on the operands */
            for (int i = 0; i < instr->getNumOperands(); i++) {
                const InstrType::operand_t* op = instr->getOperand(i);

                if (op->type == InstrType::OperandType::MREF) {
                    nvbit_insert_call(instr, "instrument_mem", IPOINT_BEFORE);
                    /* real predicate value -- NOT forced to 1, since a
                     * predicated-off thread never performs this memory
                     * access and must not be counted as active */
                    nvbit_add_call_arg_guard_pred_val(instr);
                    /* memory reference 64 bit address */
                    nvbit_add_call_arg_mref_addr64(instr, mref_idx);
                    /* pointer to channel_dev */
                    nvbit_add_call_arg_const_val64(
                        instr, (uint64_t)ctx_state->channel_dev);
                    mref_idx++;
                }
            }
            cnt++;
        }
    }
}

/* flush channel */
__global__ void flush_channel(ChannelDev* ch_dev) { ch_dev->flush(); }

void init_context_state(CUcontext ctx) {
    CTXstate* ctx_state = ctx_state_map[ctx];
    ctx_state->recv_thread_done = false;
    cudaMallocManaged(&ctx_state->channel_dev, sizeof(ChannelDev));
    ctx_state->channel_host.init((int)ctx_state_map.size() - 1, CHANNEL_SIZE,
                                 ctx_state->channel_dev, recv_thread_fun, ctx);
    nvbit_set_tool_pthread(ctx_state->channel_host.get_thread());
}

void nvbit_at_cuda_event(CUcontext ctx, int is_exit, nvbit_api_cuda_t cbid,
                         const char* name, void* params, CUresult* pStatus) {
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
        CTXstate* ctx_state = ctx_state_map[ctx];

        CUfunction func;
        if (cbid == API_CUDA_cuLaunchKernelEx_ptsz ||
            cbid == API_CUDA_cuLaunchKernelEx) {
            cuLaunchKernelEx_params* p = (cuLaunchKernelEx_params*)params;
            func = p->f;
        } else {
            cuLaunchKernel_params* p = (cuLaunchKernel_params*)params;
            func = p->f;
        }

        if (!is_exit) {
            /* Make sure GPU is idle */
            cudaDeviceSynchronize();
            assert(cudaGetLastError() == cudaSuccess);

            /* capture this kernel's grid dimensions BEFORE it runs, so
             * recv_thread_fun can correctly linearize CTA ids for the
             * records this kernel is about to produce */
            if (cbid == API_CUDA_cuLaunchKernelEx_ptsz ||
                cbid == API_CUDA_cuLaunchKernelEx) {
                cuLaunchKernelEx_params* pe = (cuLaunchKernelEx_params*)params;
                cur_grid_dim_x = pe->config->gridDimX;
                cur_grid_dim_y = pe->config->gridDimY;
            } else {
                cuLaunchKernel_params* pk = (cuLaunchKernel_params*)params;
                cur_grid_dim_x = pk->gridDimX;
                cur_grid_dim_y = pk->gridDimY;
            }

            /* instrument */
            instrument_function_if_needed(ctx, func);

            int nregs = 0;
            CUDA_SAFECALL(
                cuFuncGetAttribute(&nregs, CU_FUNC_ATTRIBUTE_NUM_REGS, func));

            int shmem_static_nbytes = 0;
            CUDA_SAFECALL(
                cuFuncGetAttribute(&shmem_static_nbytes,
                                   CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES, func));

            const char* func_name = nvbit_get_func_name(ctx, func);
            uint64_t pc = nvbit_get_func_addr(func);

            /* enable instrumented code to run */
            nvbit_enable_instrumented(ctx, func, true);

            if (verbose) {
                printf(
                    "MEMTRACE: CTX 0x%016lx - LAUNCH - Kernel pc 0x%016lx - "
                    "Kernel name %s - grid launch id %ld - grid size %d,%d - "
                    "nregs %d - shmem %d\n",
                    (uint64_t)ctx, pc, func_name, grid_launch_id,
                    cur_grid_dim_x, cur_grid_dim_y, nregs, shmem_static_nbytes);
            }

        } else {
            // make sure user kernel finishes to avoid deadlock
            cudaDeviceSynchronize();
            /* push a flush channel kernel */
            flush_channel<<<1, 1>>>(ctx_state->channel_dev);

            cudaDeviceSynchronize();
            assert(cudaGetLastError() == cudaSuccess);

            grid_launch_id++;
        }
    }
    skip_callback_flag = false;
    pthread_mutex_unlock(&cuda_event_mutex);
}

void* recv_thread_fun(void* args) {
    CUcontext ctx = (CUcontext)args;

    pthread_mutex_lock(&mutex);
    assert(ctx_state_map.find(ctx) != ctx_state_map.end());
    CTXstate* ctx_state = ctx_state_map[ctx];

    ChannelHost* ch_host = &ctx_state->channel_host;
    pthread_mutex_unlock(&mutex);
    char* recv_buffer = (char*)malloc(CHANNEL_SIZE);

    while (!ctx_state->recv_thread_done) {
        uint32_t num_recv_bytes = ch_host->recv(recv_buffer, CHANNEL_SIZE);
        if (num_recv_bytes > 0) {
            uint32_t num_processed_bytes = 0;
            while (num_processed_bytes < num_recv_bytes) {
                mem_access_t* ma =
                    (mem_access_t*)&recv_buffer[num_processed_bytes];

                /* linearize (cta_x, cta_y, cta_z) into one thread-block
                 * id k, using this record's kernel's grid dimensions */
                uint64_t block_id =
                    (uint64_t)ma->cta_id_z * cur_grid_dim_y * cur_grid_dim_x +
                    (uint64_t)ma->cta_id_y * cur_grid_dim_x +
                    (uint64_t)ma->cta_id_x;

                fprintf(mem_trace_fp, "%lu", (unsigned long)block_id);

                /* only emit addresses from lanes that were genuinely
                 * active for this reference (active_mask), skipping the
                 * meaningless entries -- this both keeps the trace file
                 * small and makes "number of addresses on this line"
                 * equal to "number of active threads" for Part A */
                for (int i = 0; i < 32; i++) {
                    if (ma->active_mask & (1u << i)) {
                        fprintf(mem_trace_fp, " 0x%016lx", ma->addrs[i]);
                    }
                }
                fprintf(mem_trace_fp, "\n");

                num_processed_bytes += sizeof(mem_access_t);
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
        printf("MEMTRACE: STARTING CONTEXT %p\n", ctx);
    }
    assert(ctx_state_map.find(ctx) == ctx_state_map.end());
    CTXstate* ctx_state = new CTXstate;
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
        printf("MEMTRACE: TERMINATING CONTEXT %p\n", ctx);
    }
    assert(ctx_state_map.find(ctx) != ctx_state_map.end());
    CTXstate* ctx_state = ctx_state_map[ctx];

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
    if (mem_trace_fp != NULL) {
        fclose(mem_trace_fp);
    }
}
