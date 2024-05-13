// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2024, Advanced Micro Devices, Inc. All rights reserved.

// auto generated by generate.py
#include "fmha_fwd.hpp"

float fmha_fwd(fmha_fwd_traits t, fmha_fwd_args a, const ck_tile::stream_config& s)
{
    float r = -1;
    if(t.data_type.compare("fp16") == 0)
    {
        if(t.hdim_q <= 128 && t.hdim_v <= 128)
        {
            if((t.is_group_mode == false) && (t.is_v_rowmajor == true) &&
               (t.mask_type == mask_enum::no_mask) && (t.has_bias == false) &&
               (t.has_lse == true) && (t.do_fp8_static_quant == false) && (a.seqlen_q % 128 == 0) &&
               (a.seqlen_k % 128 == 0) && (a.hdim_q % 128 == 0) && (a.hdim_v % 128 == 0))
            {
                using trait_ = fmha_fwd_traits_<128,
                                                ck_tile::fp16_t,
                                                false,
                                                128,
                                                128,
                                                32,
                                                128,
                                                32,
                                                128,
                                                true,
                                                ck_tile::BlockFmhaPipelineEnum::QRKSVS,
                                                ck_tile::SimplifiedGenericAttentionMask<false>,
                                                false,
                                                true,
                                                false,
                                                false,
                                                false,
                                                false,
                                                false>;
                return fmha_fwd_<trait_>(s, a);
            }
        }
    }

    return r;
}
