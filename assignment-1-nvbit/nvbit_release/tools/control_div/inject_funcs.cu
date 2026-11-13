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

/* CS623 Assignment 1 - Problem I
 *
 * This function is inserted BEFORE every static instruction of the
 * kernel (and its related device functions). It is executed once per
 * thread, but only the first active lane of each warp performs the
 * bookkeeping, so effectively it runs once per *executed warp
 * instruction*.
 *
 * Per the assignment statement we ignore predicates entirely: the set
 * of "active threads executing I" is simply the set of threads that
 * are convergent at this program point, i.e. __activemask(), not the
 * predicated/guarded subset.
 *
 * Divergence contribution of this warp instruction:
 *     1 - (num_active_threads / 32)
 * We accumulate num_active_threads is never needed directly; instead we
 * accumulate (32 - num_active_threads) into total_inactive_lanes and
 * divide by (32 * num_warp_instrs) on the host side once the kernel
 * completes. This avoids doing floating point atomics on the device.
 */
extern "C" __device__ __noinline__ void count_div(uint64_t p_warp_instrs,
                                                    uint64_t p_inactive_lanes) {
    /* mask of threads active at this instruction, ignoring predicates */
    const int active_mask = __ballot_sync(__activemask(), 1);

    const int laneid = get_laneid();
    const int first_laneid = __ffs(active_mask) - 1;
    const int num_active = __popc(active_mask);

    /* only the first active thread of the warp updates the counters,
     * so each executed warp instruction is counted exactly once */
    if (first_laneid == laneid) {
        atomicAdd((unsigned long long *)p_warp_instrs, 1ULL);
        atomicAdd((unsigned long long *)p_inactive_lanes,
                   (unsigned long long)(32 - num_active));
    }
}
