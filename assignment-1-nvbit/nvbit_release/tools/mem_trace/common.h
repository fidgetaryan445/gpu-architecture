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

#include <stdint.h>

/* CS623 Assignment 1 - Problem III
 *
 * Stripped-down version of NVBit's mem_trace record. Problem III only
 * needs, per executed warp memory reference: which thread block (CTA)
 * issued it, and the 32 lane addresses. We drop grid_launch_id,
 * warp_id and opcode_id (not needed downstream) to shrink the trace.
 *
 * "active_mask" is kept (it is NOT extra bookkeeping we were told to
 * drop -- it is required to correctly identify which of the 32 "addrs"
 * entries belong to active lanes, since addrs[] for inactive lanes is
 * undefined after __shfl_sync). Without it, Problem III-A/III-B cannot
 * correctly compute "active threads of the warp".
 *
 * cta_id is the thread block's linear id within its grid:
 *   cta_id = cta_id_x + gridDim.x * (cta_id_y + gridDim.y * cta_id_z)
 * computed on the device using the grid dimensions passed at launch
 * time (see nvbit_add_call_arg_launch_val64 usage in mem_trace.cu). */
typedef struct {
    int cta_id;
    unsigned active_mask;
    uint64_t addrs[32];
} mem_access_t;
