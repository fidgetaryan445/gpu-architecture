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
#include <stdio.h>

#include "utils/utils.h"

/* for channel */
#include "utils/channel.hpp"

/* contains definition of the mem_access_t structure */
#include "common.h"

/* CS623 Assignment 1 - Problem III
 *
 * Stripped-down memory instrumentation function: records only the
 * linear thread-block (CTA) id and the 32 lane addresses (plus the
 * active-lane mask needed to interpret them) of every executed warp
 * memory reference. opcode/warp-id/grid-launch-id are no longer
 * recorded (removed by the assignment).
 *
 * "packed_griddim" holds gridDim.x in its low 32 bits and gridDim.y in
 * its high 32 bits, set once per kernel launch via nvbit_set_at_launch
 * (see mem_trace.cu). It lets us linearize the CTA id on the device:
 *   cta_id = x + gridDim.x * (y + gridDim.y * z)
 */
extern "C" __device__ __noinline__ void instrument_mem(
    int pred, uint64_t addr, uint64_t packed_griddim, uint64_t pchannel_dev) {
    if (!pred) {
        return;
    }

    int active_mask = __ballot_sync(__activemask(), 1);
    const int laneid = get_laneid();
    const int first_laneid = __ffs(active_mask) - 1;

    mem_access_t ma;
    for (int i = 0; i < 32; i++) {
        ma.addrs[i] = __shfl_sync(active_mask, addr, i);
    }

    const uint32_t griddim_x = (uint32_t)(packed_griddim & 0xffffffffULL);
    const uint32_t griddim_y = (uint32_t)(packed_griddim >> 32);

    int4 cta = get_ctaid();
    ma.cta_id = cta.x + griddim_x * (cta.y + griddim_y * cta.z);
    ma.active_mask = (unsigned)active_mask;

    if (first_laneid == laneid) {
        ChannelDev *channel_dev = (ChannelDev *)pchannel_dev;
        channel_dev->push(&ma, sizeof(mem_access_t));
    }
}
