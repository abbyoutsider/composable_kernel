#include <iostream>
#include <numeric>
#include <initializer_list>
#include <cstdlib>
#include <stdlib.h>
#include "config.hpp"
#include "ConstantTensorDescriptor.hpp"
#include "device.hpp"
#include "conv_common.hpp"
#include "device_convolution_direct_v2_nchw_kcyx_nkhw.hpp"
#include "device_convolution_implicit_gemm_v1_chwn_cyxk_khwn.hpp"
#include "device_convolution_implicit_gemm_v1_nchw_cyxk_nkhw.hpp"
#include "device_convolution_implicit_gemm_v2_chwn_cyxk_khwn.hpp"
#include "device_convolution_implicit_gemm_v3_nchw_cyxk_nkhw.hpp"
#include "device_convolution_implicit_gemm_v4_nchw_kcyx_nkhw.hpp"

using namespace ck;

struct GeneratorTensor_1
{
    template <class... Is>
    double operator()(Is... is)
    {
        return 1;
    }
};

struct GeneratorTensor_2
{
    int min_value = 0;
    int max_value = 1;

    template <class... Is>
    double operator()(Is...)
    {
        return (std::rand() % (max_value - min_value)) + min_value;
    }
};

struct GeneratorTensor_3
{
    template <class... Is>
    double operator()(Is... is)
    {
        std::array<index_t, sizeof...(Is)> dims = {{static_cast<index_t>(is)...}};

        auto f_acc = [](auto a, auto b) { return 100 * a + b; };

        return std::accumulate(dims.begin(), dims.end(), index_t(0), f_acc);
    }
};

struct GeneratorTensor_Checkboard
{
    template <class... Ts>
    double operator()(Ts... Xs) const
    {
        std::array<index_t, sizeof...(Ts)> dims = {{Xs...}};
        return std::accumulate(dims.begin(),
                               dims.end(),
                               true,
                               [](bool init, index_t x) -> int { return init != (x % 2); })
                   ? 1
                   : -1;
    }
};

// this is ugly, only for 4d
template <class TConstTensorDesc>
void ostream_ConstantTensorDescriptor(TConstTensorDesc, std::ostream& os = std::cout)
{
    static_assert(TConstTensorDesc::nDim == 4, "nDim is not 4");

    constexpr auto I0   = Number<0>{};
    constexpr auto I1   = Number<1>{};
    constexpr auto I2   = Number<2>{};
    constexpr auto I3   = Number<3>{};
    constexpr auto desc = TConstTensorDesc{};

    os << "Lengths: {" << desc.GetLength(I0) << ", " << desc.GetLength(I1) << ", "
       << desc.GetLength(I2) << ", " << desc.GetLength(I3) << "}, "
       << "Strides: {" << desc.GetStride(I0) << ", " << desc.GetStride(I1) << ", "
       << desc.GetStride(I2) << ", " << desc.GetStride(I3) << "}" << std::endl;
}

// this is ugly, only for 4d
template <class TConstTensorDesc>
auto make_TensorDescriptor(TConstTensorDesc)
{
    static_assert(TConstTensorDesc::nDim == 4, "nDim is not 4");

    constexpr auto I0   = Number<0>{};
    constexpr auto I1   = Number<1>{};
    constexpr auto I2   = Number<2>{};
    constexpr auto I3   = Number<3>{};
    constexpr auto desc = TConstTensorDesc{};

    std::initializer_list<index_t> lengths = {
        desc.GetLength(I0), desc.GetLength(I1), desc.GetLength(I2), desc.GetLength(I3)};
    std::initializer_list<index_t> strides = {
        desc.GetStride(I0), desc.GetStride(I1), desc.GetStride(I2), desc.GetStride(I3)};

    return TensorDescriptor(lengths, strides);
}

template <class TIn,
          class TWei,
          class TOut,
          class ConvStrides,
          class ConvDilations,
          class LowerPads,
          class UpperPads>
void host_direct_convolution(const Tensor<TIn>& in_nchw,
                             const Tensor<TWei>& wei_kcyx,
                             Tensor<TOut>& out_nkhw,
                             ConvStrides,
                             ConvDilations,
                             LowerPads,
                             UpperPads)
{
    index_t h_pad_low = LowerPads{}.Get(Number<0>{});
    index_t w_pad_low = LowerPads{}.Get(Number<1>{});

    index_t h_pad_up = UpperPads{}.Get(Number<0>{});
    index_t w_pad_up = UpperPads{}.Get(Number<1>{});

    auto f = [&](auto n, auto k, auto ho, auto wo) {
        double v = 0;
        for(int c = 0; c < wei_kcyx.mDesc.GetLengths()[1]; ++c)
        {
            for(int y = 0; y < wei_kcyx.mDesc.GetLengths()[2]; ++y)
            {
                int hi = ho * ConvStrides{}[0] + y * ConvDilations{}[0] - h_pad_low;
                for(int x = 0; x < wei_kcyx.mDesc.GetLengths()[3]; ++x)
                {
                    int wi = wo * ConvStrides{}[1] + x * ConvDilations{}[1] - w_pad_low;
                    if(hi >= 0 && hi < in_nchw.mDesc.GetLengths()[2] && wi >= 0 &&
                       wi < in_nchw.mDesc.GetLengths()[3])
                    {
                        v += double(in_nchw(n, c, hi, wi)) * double(wei_kcyx(k, c, y, x));
                    }
                }
            }
        }
        out_nkhw(n, k, ho, wo) = v;
    };

    auto f_par = make_ParallelTensorFunctor(f,
                                            out_nkhw.mDesc.GetLengths()[0],
                                            out_nkhw.mDesc.GetLengths()[1],
                                            out_nkhw.mDesc.GetLengths()[2],
                                            out_nkhw.mDesc.GetLengths()[3]);

    f_par(std::thread::hardware_concurrency());
}

template <class TIn, class TWei, class TOut, class LowerPads, class UpperPads>
void host_winograd_3x3_convolution(const Tensor<TIn>& in_nchw,
                                   const Tensor<TWei>& wei_kcyx,
                                   Tensor<TOut>& out_nkhw,
                                   LowerPads,
                                   UpperPads)
{
    constexpr std::size_t HoPerTile = 2;
    constexpr std::size_t WoPerTile = 2;

    std::size_t N  = in_nchw.mDesc.GetLengths()[0];
    std::size_t C  = in_nchw.mDesc.GetLengths()[1];
    std::size_t HI = in_nchw.mDesc.GetLengths()[2];
    std::size_t WI = in_nchw.mDesc.GetLengths()[3];

    std::size_t K = wei_kcyx.mDesc.GetLengths()[0];
    std::size_t Y = wei_kcyx.mDesc.GetLengths()[2];
    std::size_t X = wei_kcyx.mDesc.GetLengths()[3];

    std::size_t HO = out_nkhw.mDesc.GetLengths()[2];
    std::size_t WO = out_nkhw.mDesc.GetLengths()[3];

    index_t h_pad_low = LowerPads{}.Get(Number<0>{});
    index_t w_pad_low = LowerPads{}.Get(Number<1>{});

    index_t h_pad_up = UpperPads{}.Get(Number<0>{});
    index_t w_pad_up = UpperPads{}.Get(Number<1>{});

    std::size_t HiPerTile = HoPerTile + Y - 1;
    std::size_t WiPerTile = WoPerTile + X - 1;

    std::size_t HTile = (HO + HoPerTile - 1) / HoPerTile;
    std::size_t WTile = (WO + WoPerTile - 1) / WoPerTile;

    Tensor<double> in_hold({N, C, HTile, WTile, HiPerTile, WiPerTile});
    Tensor<double> in_transform({N, C, HTile, WTile, HiPerTile, WiPerTile});
    Tensor<double> wei_transform({K, C, HiPerTile, WiPerTile});
    Tensor<double> out_transform({N, K, HTile, WTile, HiPerTile, HiPerTile});
    Tensor<double> out_hold({N, K, HTile, WTile, HoPerTile, WoPerTile});

    auto f_in_hold = [&](auto n, auto c, auto htile, auto wtile) {
        for(int j = 0; j < HiPerTile; ++j)
        {
            int hi = HoPerTile * htile + j - h_pad_low;
            for(int i = 0; i < WiPerTile; ++i)
            {
                int wi = WoPerTile * wtile + i - w_pad_low;

                if(hi >= 0 && hi < in_nchw.mDesc.GetLengths()[2] && wi >= 0 &&
                   wi < in_nchw.mDesc.GetLengths()[3])
                {
                    in_hold(n, c, htile, wtile, j, i) = in_nchw(n, c, hi, wi);
                }
                else
                {
                    in_hold(n, c, htile, wtile, j, i) = TIn(0);
                }
            }
        }
    };

    auto f_in_transform = [&](auto n, auto c, auto htile, auto wtile) {
        in_transform(n, c, htile, wtile, 0, 0) =
            in_hold(n, c, htile, wtile, 0, 0) - in_hold(n, c, htile, wtile, 0, 2) -
            in_hold(n, c, htile, wtile, 2, 0) + in_hold(n, c, htile, wtile, 2, 2);
        in_transform(n, c, htile, wtile, 0, 1) =
            in_hold(n, c, htile, wtile, 0, 1) + in_hold(n, c, htile, wtile, 0, 2) -
            in_hold(n, c, htile, wtile, 2, 1) - in_hold(n, c, htile, wtile, 2, 2);
        in_transform(n, c, htile, wtile, 0, 2) =
            -in_hold(n, c, htile, wtile, 0, 1) + in_hold(n, c, htile, wtile, 0, 2) +
            in_hold(n, c, htile, wtile, 2, 1) - in_hold(n, c, htile, wtile, 2, 2);
        in_transform(n, c, htile, wtile, 0, 3) =
            in_hold(n, c, htile, wtile, 0, 1) - in_hold(n, c, htile, wtile, 0, 3) -
            in_hold(n, c, htile, wtile, 2, 1) + in_hold(n, c, htile, wtile, 2, 3);

        in_transform(n, c, htile, wtile, 1, 0) =
            in_hold(n, c, htile, wtile, 1, 0) - in_hold(n, c, htile, wtile, 1, 2) +
            in_hold(n, c, htile, wtile, 2, 0) - in_hold(n, c, htile, wtile, 2, 2);
        in_transform(n, c, htile, wtile, 1, 1) =
            in_hold(n, c, htile, wtile, 1, 1) + in_hold(n, c, htile, wtile, 1, 2) +
            in_hold(n, c, htile, wtile, 2, 1) + in_hold(n, c, htile, wtile, 2, 2);
        in_transform(n, c, htile, wtile, 1, 2) =
            -in_hold(n, c, htile, wtile, 1, 1) + in_hold(n, c, htile, wtile, 1, 2) -
            in_hold(n, c, htile, wtile, 2, 1) + in_hold(n, c, htile, wtile, 2, 2);
        in_transform(n, c, htile, wtile, 1, 3) =
            in_hold(n, c, htile, wtile, 1, 1) - in_hold(n, c, htile, wtile, 1, 3) +
            in_hold(n, c, htile, wtile, 2, 1) - in_hold(n, c, htile, wtile, 2, 3);

        in_transform(n, c, htile, wtile, 2, 0) =
            -in_hold(n, c, htile, wtile, 1, 0) + in_hold(n, c, htile, wtile, 1, 2) +
            in_hold(n, c, htile, wtile, 2, 0) - in_hold(n, c, htile, wtile, 2, 2);
        in_transform(n, c, htile, wtile, 2, 1) =
            -in_hold(n, c, htile, wtile, 1, 1) - in_hold(n, c, htile, wtile, 1, 2) +
            in_hold(n, c, htile, wtile, 2, 1) + in_hold(n, c, htile, wtile, 2, 2);
        in_transform(n, c, htile, wtile, 2, 2) =
            in_hold(n, c, htile, wtile, 1, 1) - in_hold(n, c, htile, wtile, 1, 2) -
            in_hold(n, c, htile, wtile, 2, 1) + in_hold(n, c, htile, wtile, 2, 2);
        in_transform(n, c, htile, wtile, 2, 3) =
            -in_hold(n, c, htile, wtile, 1, 1) + in_hold(n, c, htile, wtile, 1, 3) +
            in_hold(n, c, htile, wtile, 2, 1) - in_hold(n, c, htile, wtile, 2, 3);

        in_transform(n, c, htile, wtile, 3, 0) =
            in_hold(n, c, htile, wtile, 1, 0) - in_hold(n, c, htile, wtile, 1, 2) -
            in_hold(n, c, htile, wtile, 3, 0) + in_hold(n, c, htile, wtile, 3, 2);
        in_transform(n, c, htile, wtile, 3, 1) =
            in_hold(n, c, htile, wtile, 1, 1) + in_hold(n, c, htile, wtile, 1, 2) -
            in_hold(n, c, htile, wtile, 3, 1) - in_hold(n, c, htile, wtile, 3, 2);
        in_transform(n, c, htile, wtile, 3, 2) =
            -in_hold(n, c, htile, wtile, 1, 1) + in_hold(n, c, htile, wtile, 1, 2) +
            in_hold(n, c, htile, wtile, 3, 1) - in_hold(n, c, htile, wtile, 3, 2);
        in_transform(n, c, htile, wtile, 3, 3) =
            in_hold(n, c, htile, wtile, 1, 1) - in_hold(n, c, htile, wtile, 1, 3) -
            in_hold(n, c, htile, wtile, 3, 1) + in_hold(n, c, htile, wtile, 3, 3);
    };

    auto f_wei_transform = [&](auto k, auto c) {
        wei_transform(k, c, 0, 0) = double(wei_kcyx(k, c, 0, 0));
        wei_transform(k, c, 0, 1) = 0.5 * double(wei_kcyx(k, c, 0, 0)) +
                                    0.5 * double(wei_kcyx(k, c, 0, 1)) +
                                    0.5 * double(wei_kcyx(k, c, 0, 2));
        wei_transform(k, c, 0, 2) = 0.5 * double(wei_kcyx(k, c, 0, 0)) -
                                    0.5 * double(wei_kcyx(k, c, 0, 1)) +
                                    0.5 * double(wei_kcyx(k, c, 0, 2));
        wei_transform(k, c, 0, 3) = double(wei_kcyx(k, c, 0, 2));

        wei_transform(k, c, 1, 0) = 0.5 * double(wei_kcyx(k, c, 0, 0)) +
                                    0.5 * double(wei_kcyx(k, c, 1, 0)) +
                                    0.5 * double(wei_kcyx(k, c, 2, 0));
        wei_transform(k, c, 1, 1) =
            0.25 * double(wei_kcyx(k, c, 0, 0)) + 0.25 * double(wei_kcyx(k, c, 0, 1)) +
            0.25 * double(wei_kcyx(k, c, 0, 2)) + 0.25 * double(wei_kcyx(k, c, 1, 0)) +
            0.25 * double(wei_kcyx(k, c, 1, 1)) + 0.25 * double(wei_kcyx(k, c, 1, 2)) +
            0.25 * double(wei_kcyx(k, c, 2, 0)) + 0.25 * double(wei_kcyx(k, c, 2, 1)) +
            0.25 * double(wei_kcyx(k, c, 2, 2));
        wei_transform(k, c, 1, 2) =
            0.25 * double(wei_kcyx(k, c, 0, 0)) - 0.25 * double(wei_kcyx(k, c, 0, 1)) +
            0.25 * double(wei_kcyx(k, c, 0, 2)) + 0.25 * double(wei_kcyx(k, c, 1, 0)) -
            0.25 * double(wei_kcyx(k, c, 1, 1)) + 0.25 * double(wei_kcyx(k, c, 1, 2)) +
            0.25 * double(wei_kcyx(k, c, 2, 0)) - 0.25 * double(wei_kcyx(k, c, 2, 1)) +
            0.25 * double(wei_kcyx(k, c, 2, 2));
        wei_transform(k, c, 1, 3) = 0.5 * double(wei_kcyx(k, c, 0, 2)) +
                                    0.5 * double(wei_kcyx(k, c, 1, 2)) +
                                    0.5 * double(wei_kcyx(k, c, 2, 2));

        wei_transform(k, c, 2, 0) = 0.5 * double(wei_kcyx(k, c, 0, 0)) -
                                    0.5 * double(wei_kcyx(k, c, 1, 0)) +
                                    0.5 * double(wei_kcyx(k, c, 2, 0));
        wei_transform(k, c, 2, 1) =
            0.25 * double(wei_kcyx(k, c, 0, 0)) + 0.25 * double(wei_kcyx(k, c, 0, 1)) +
            0.25 * double(wei_kcyx(k, c, 0, 2)) - 0.25 * double(wei_kcyx(k, c, 1, 0)) -
            0.25 * double(wei_kcyx(k, c, 1, 1)) - 0.25 * double(wei_kcyx(k, c, 1, 2)) +
            0.25 * double(wei_kcyx(k, c, 2, 0)) + 0.25 * double(wei_kcyx(k, c, 2, 1)) +
            0.25 * double(wei_kcyx(k, c, 2, 2));
        wei_transform(k, c, 2, 2) =
            0.25 * double(wei_kcyx(k, c, 0, 0)) - 0.25 * double(wei_kcyx(k, c, 0, 1)) +
            0.25 * double(wei_kcyx(k, c, 0, 2)) - 0.25 * double(wei_kcyx(k, c, 1, 0)) +
            0.25 * double(wei_kcyx(k, c, 1, 1)) - 0.25 * double(wei_kcyx(k, c, 1, 2)) +
            0.25 * double(wei_kcyx(k, c, 2, 0)) - 0.25 * double(wei_kcyx(k, c, 2, 1)) +
            0.25 * double(wei_kcyx(k, c, 2, 2));
        wei_transform(k, c, 2, 3) = 0.5 * double(wei_kcyx(k, c, 0, 2)) -
                                    0.5 * double(wei_kcyx(k, c, 1, 2)) +
                                    0.5 * double(wei_kcyx(k, c, 2, 2));

        wei_transform(k, c, 3, 0) = double(wei_kcyx(k, c, 2, 0));
        wei_transform(k, c, 3, 1) = 0.5 * double(wei_kcyx(k, c, 2, 0)) +
                                    0.5 * double(wei_kcyx(k, c, 2, 1)) +
                                    0.5 * double(wei_kcyx(k, c, 2, 2));
        wei_transform(k, c, 3, 2) = 0.5 * double(wei_kcyx(k, c, 2, 0)) -
                                    0.5 * double(wei_kcyx(k, c, 2, 1)) +
                                    0.5 * double(wei_kcyx(k, c, 2, 2));
        wei_transform(k, c, 3, 3) = double(wei_kcyx(k, c, 2, 2));
    };

    auto f_out_transform = [&](auto n, auto k, auto htile, auto wtile) {
        for(int j = 0; j < HiPerTile; ++j)
        {
            for(int i = 0; i < WiPerTile; ++i)
            {
                double v = 0;
                for(int c = 0; c < C; ++c)
                {
                    v += in_transform(n, c, htile, wtile, j, i) * wei_transform(k, c, j, i);
                }

                out_transform(n, k, htile, wtile, j, i) = v;
            }
        }
    };

    auto f_out_hold = [&](auto n, auto k, auto htile, auto wtile) {
        out_hold(n, k, htile, wtile, 0, 0) =
            out_transform(n, k, htile, wtile, 0, 0) + out_transform(n, k, htile, wtile, 0, 1) +
            out_transform(n, k, htile, wtile, 0, 2) + out_transform(n, k, htile, wtile, 1, 0) +
            out_transform(n, k, htile, wtile, 1, 1) + out_transform(n, k, htile, wtile, 1, 2) +
            out_transform(n, k, htile, wtile, 2, 0) + out_transform(n, k, htile, wtile, 2, 1) +
            out_transform(n, k, htile, wtile, 2, 2);
        out_hold(n, k, htile, wtile, 0, 1) =
            out_transform(n, k, htile, wtile, 0, 1) - out_transform(n, k, htile, wtile, 0, 2) -
            out_transform(n, k, htile, wtile, 0, 3) + out_transform(n, k, htile, wtile, 1, 1) -
            out_transform(n, k, htile, wtile, 1, 2) - out_transform(n, k, htile, wtile, 1, 3) +
            out_transform(n, k, htile, wtile, 2, 1) - out_transform(n, k, htile, wtile, 2, 2) -
            out_transform(n, k, htile, wtile, 2, 3);
        out_hold(n, k, htile, wtile, 1, 0) =
            out_transform(n, k, htile, wtile, 1, 0) + out_transform(n, k, htile, wtile, 1, 1) +
            out_transform(n, k, htile, wtile, 1, 2) - out_transform(n, k, htile, wtile, 2, 0) -
            out_transform(n, k, htile, wtile, 2, 1) - out_transform(n, k, htile, wtile, 2, 2) -
            out_transform(n, k, htile, wtile, 3, 0) - out_transform(n, k, htile, wtile, 3, 1) -
            out_transform(n, k, htile, wtile, 3, 2);
        out_hold(n, k, htile, wtile, 1, 1) =
            out_transform(n, k, htile, wtile, 1, 1) - out_transform(n, k, htile, wtile, 1, 2) -
            out_transform(n, k, htile, wtile, 1, 3) - out_transform(n, k, htile, wtile, 2, 1) +
            out_transform(n, k, htile, wtile, 2, 2) + out_transform(n, k, htile, wtile, 2, 3) -
            out_transform(n, k, htile, wtile, 3, 1) + out_transform(n, k, htile, wtile, 3, 2) +
            out_transform(n, k, htile, wtile, 3, 3);
    };

    auto f_out = [&](auto n, auto k, auto htile, auto wtile) {
        for(int j = 0; j < HoPerTile; ++j)
        {
            std::size_t ho = HoPerTile * htile + j;
            for(int i = 0; i < WoPerTile; ++i)
            {
                std::size_t wo = WoPerTile * wtile + i;
                out_nkhw(n, k, ho, wo) = out_hold(n, k, htile, wtile, j, i);
            }
        }
    };

    std::size_t num_thread = std::thread::hardware_concurrency();

    make_ParallelTensorFunctor(f_in_hold, N, C, HTile, WTile)(num_thread);
    make_ParallelTensorFunctor(f_in_transform, N, C, HTile, WTile)(num_thread);
    make_ParallelTensorFunctor(f_wei_transform, K, C)(num_thread);
    make_ParallelTensorFunctor(f_out_transform, N, K, HTile, WTile)(num_thread);
    make_ParallelTensorFunctor(f_out_hold, N, K, HTile, WTile)(num_thread);
    make_ParallelTensorFunctor(f_out, N, K, HTile, WTile)(num_thread);
}

template <class T>
void check_error(const Tensor<T>& ref, const Tensor<T>& result)
{
    float error     = 0;
    float max_diff  = -1;
    float ref_value = 0, result_value = 0;
    for(int i = 0; i < ref.mData.size(); ++i)
    {
        error += std::abs(double(ref.mData[i]) - double(result.mData[i]));
        float diff = std::abs(double(ref.mData[i]) - double(result.mData[i]));
        if(max_diff < diff)
        {
            max_diff     = diff;
            ref_value    = ref.mData[i];
            result_value = result.mData[i];
        }
    }

    std::cout << "error: " << error << std::endl;
    std::cout << "max_diff: " << max_diff << ", " << ref_value << ", " << result_value << std::endl;
}

int main(int argc, char* argv[])
{
#if 0
    constexpr index_t N  = 8;
    constexpr index_t C  = 16;
    constexpr index_t HI = 3;
    constexpr index_t WI = 18;
    constexpr index_t K  = 128;
    constexpr index_t Y  = 3;
    constexpr index_t X  = 3;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 3x3, 34x34
    constexpr index_t N  = 128;
    constexpr index_t C  = 256;
    constexpr index_t HI = 34;
    constexpr index_t WI = 34;
    constexpr index_t K  = 128;
    constexpr index_t Y  = 3;
    constexpr index_t X  = 3;

    using ConvStrides   = Sequence<2, 2>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 3x3, 56x56
    constexpr index_t N  = 64;
    constexpr index_t C  = 64;
    constexpr index_t HI = 56;
    constexpr index_t WI = 56;
    constexpr index_t K  = 128;
    constexpr index_t Y  = 3;
    constexpr index_t X  = 3;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 3x3 filter, 28x28 image
    constexpr index_t N  = 128;
    constexpr index_t C  = 256;
    constexpr index_t HI = 28;
    constexpr index_t WI = 28;
    constexpr index_t K  = 128;
    constexpr index_t Y  = 3;
    constexpr index_t X  = 3;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 28x28 image
    constexpr index_t N  = 128;
    constexpr index_t C  = 512;
    constexpr index_t HI = 28;
    constexpr index_t WI = 28;
    constexpr index_t K  = 512;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 3x3 filter, 20x84 image, 1x1 padding
    constexpr index_t N  = 16;
    constexpr index_t C  = 256;
    constexpr index_t HI = 20;
    constexpr index_t WI = 84;
    constexpr index_t K  = 256;
    constexpr index_t Y  = 3;
    constexpr index_t X  = 3;

    constexpr index_t HPad = 1;
    constexpr index_t WPad = 1;
#elif 0
    // 3x3 filter, 112x112 image, 1x1 padding
    constexpr index_t N  = 16;
    constexpr index_t C  = 64;
    constexpr index_t HI = 112;
    constexpr index_t WI = 112;
    constexpr index_t K  = 128;
    constexpr index_t Y  = 3;
    constexpr index_t X  = 3;

    constexpr index_t HPad = 1;
    constexpr index_t WPad = 1;
#elif 0
    // 5x5 filter, 20x86 image
    constexpr index_t N  = 16;
    constexpr index_t C  = 256;
    constexpr index_t HI = 20;
    constexpr index_t WI = 86;
    constexpr index_t K  = 512;
    constexpr index_t Y  = 5;
    constexpr index_t X  = 5;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 5x5 filter, 20x86 image, 1x1 padding
    constexpr index_t N  = 16;
    constexpr index_t C  = 256;
    constexpr index_t HI = 20;
    constexpr index_t WI = 86;
    constexpr index_t K  = 512;
    constexpr index_t Y  = 5;
    constexpr index_t X  = 5;

    constexpr index_t HPad = 1;
    constexpr index_t WPad = 1;
#elif 0
    // 5x5 filter, 28x28 image, 2x2 padding
    constexpr index_t N  = 16;
    constexpr index_t C  = 192;
    constexpr index_t HI = 28;
    constexpr index_t WI = 28;
    constexpr index_t K  = 32;
    constexpr index_t Y  = 5;
    constexpr index_t X  = 5;

    constexpr index_t HPad = 2;
    constexpr index_t WPad = 2;
#elif 0
    // 3x3 filter, 14x14 image
    constexpr index_t N  = 128;
    constexpr index_t C  = 256;
    constexpr index_t HI = 14;
    constexpr index_t WI = 14;
    constexpr index_t K  = 128;
    constexpr index_t Y  = 3;
    constexpr index_t X  = 3;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 14x14 image
    constexpr index_t N  = 128;
    constexpr index_t C  = 512;
    constexpr index_t HI = 14;
    constexpr index_t WI = 14;
    constexpr index_t K  = 512;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 7x7 image
    constexpr index_t N  = 128;
    constexpr index_t C  = 512;
    constexpr index_t HI = 7;
    constexpr index_t WI = 7;
    constexpr index_t K  = 2048;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 73x73 image
    constexpr index_t N  = 128;
    constexpr index_t C  = 512;
    constexpr index_t HI = 73;
    constexpr index_t WI = 73;
    constexpr index_t K  = 128;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 8x8 image
    // cudnn@V100 68%, ck@V100 72%, ck@P100 52%, ck@VII 42%
    constexpr index_t N  = 64;
    constexpr index_t C  = 1536;
    constexpr index_t HI = 8;
    constexpr index_t WI = 8;
    constexpr index_t K  = 256;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 8x8 image
    // cudnn@V100 77%, ck@V100 76%, ck@P100 79%, ck@VII 51%
    constexpr index_t N  = 128;
    constexpr index_t C  = 2048;
    constexpr index_t HI = 8;
    constexpr index_t WI = 8;
    constexpr index_t K  = 384;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 7x7 image
    // cudnn@V100 82%, ck@V100 76%, ck@P100 67%, ck@VII 64%
    constexpr index_t N  = 128;
    constexpr index_t C  = 832;
    constexpr index_t HI = 7;
    constexpr index_t WI = 7;
    constexpr index_t K  = 384;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 8x8 image
    // cudnn@V100 83%, ck@V100 75%, ck@P100 78%, ck@VII 65%
    constexpr index_t N  = 128;
    constexpr index_t C  = 1280;
    constexpr index_t HI = 8;
    constexpr index_t WI = 8;
    constexpr index_t K  = 384;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 14x14 image
    // cudnn@V100 62%, ck@V100 68%, ck@P100 70%, ck@VII 50%
    constexpr index_t N  = 128;
    constexpr index_t C  = 512;
    constexpr index_t HI = 14;
    constexpr index_t WI = 14;
    constexpr index_t K  = 128;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 8x8 image
    // cudnn@V100 74%, ck@V100 57%, ck@P100 78%, ck@VII 61%
    constexpr index_t N  = 64;
    constexpr index_t C  = 1536;
    constexpr index_t HI = 8;
    constexpr index_t WI = 8;
    constexpr index_t K  = 384;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 28x28 image
    // cudnn@V100 86%, ck@V100 84%, ck@P100 80%, ck@VII 69%
    constexpr index_t N  = 128;
    constexpr index_t C  = 256;
    constexpr index_t HI = 28;
    constexpr index_t WI = 28;
    constexpr index_t K  = 128;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 7x7 image
    // cudnn@V100 71%, ck@V100 55%, ck@P100 70%, ck@VII 62%
    constexpr index_t N  = 128;
    constexpr index_t C  = 832;
    constexpr index_t HI = 7;
    constexpr index_t WI = 7;
    constexpr index_t K  = 256;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 3x3 filter, 2x2 stride, 35x35 input, 17x17 output
    // cudnn@V100 90%, ck@V100 93%, ck@P100 83%, ck@VII 81%
    constexpr index_t N  = 128;
    constexpr index_t C  = 288;
    constexpr index_t HI = 35;
    constexpr index_t WI = 35;
    constexpr index_t K  = 384;
    constexpr index_t Y  = 3;
    constexpr index_t X  = 3;

    using ConvStrides   = Sequence<2, 2>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 17x17 input
    // cudnn@V100 81%, ck@V100 76%, ck@P100 70%, ck@VII 76%
    constexpr index_t N  = 128;
    constexpr index_t C  = 768;
    constexpr index_t HI = 17;
    constexpr index_t WI = 17;
    constexpr index_t K  = 128;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 14x14 image
    // cudnn@V100 73%, ck@V100 71%, ck@P100 70%, ck@VII 64%
    constexpr index_t N  = 128;
    constexpr index_t C  = 528;
    constexpr index_t HI = 14;
    constexpr index_t WI = 14;
    constexpr index_t K  = 128;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 0
    // 1x1 filter, 14x14 image
    // cudnn@V100 73%, ck@V100 72%, ck@P100 79%, ck@VII 75%
    constexpr index_t N  = 128;
    constexpr index_t C  = 528;
    constexpr index_t HI = 14;
    constexpr index_t WI = 14;
    constexpr index_t K  = 256;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#elif 1
    // 1x1 filter, 7x7 image
    // cudnn@V100 49%, ck@V100 50%, ck@P100 61%, ck@VII 52%
    constexpr index_t N  = 128;
    constexpr index_t C  = 832;
    constexpr index_t HI = 7;
    constexpr index_t WI = 7;
    constexpr index_t K  = 128;
    constexpr index_t Y  = 1;
    constexpr index_t X  = 1;

    using ConvStrides   = Sequence<1, 1>;
    using ConvDilations = Sequence<1, 1>;

    constexpr index_t HPad = 0;
    constexpr index_t WPad = 0;
#endif

    auto lower_pads = Sequence<HPad, WPad>{};
    auto upper_pads = Sequence<HPad, WPad>{};

    auto in_nchw_desc  = make_ConstantTensorDescriptor_packed(Sequence<N, C, HI, WI>{});
    auto wei_kcyx_desc = make_ConstantTensorDescriptor_packed(Sequence<K, C, Y, X>{});
    auto out_nkhw_desc = get_convolution_with_padding_output_default_4d_tensor_descriptor(
        in_nchw_desc, wei_kcyx_desc, ConvStrides{}, ConvDilations{}, lower_pads, upper_pads);

    ostream_ConstantTensorDescriptor(in_nchw_desc, std::cout << "in_nchw_desc: ");
    ostream_ConstantTensorDescriptor(wei_kcyx_desc, std::cout << "wei_kcyx_desc: ");
    ostream_ConstantTensorDescriptor(out_nkhw_desc, std::cout << "out_nkhw_desc: ");

    using in_data_t  = float;
    using out_data_t = float;
    Tensor<in_data_t> in_nchw(make_TensorDescriptor(in_nchw_desc));
    Tensor<in_data_t> wei_kcyx(make_TensorDescriptor(wei_kcyx_desc));
    Tensor<out_data_t> out_nkhw_host(make_TensorDescriptor(out_nkhw_desc));
    Tensor<out_data_t> out_nkhw_device(make_TensorDescriptor(out_nkhw_desc));

    std::size_t num_thread = std::thread::hardware_concurrency();

    if(argc != 3)
    {
        printf("arg1: do_verification, arg2: nrepeat\n");
        exit(1);
    }

    bool do_verification = atoi(argv[1]);
    index_t nrepeat      = atoi(argv[2]);

    if(do_verification)
    {
#if 0
        in_nchw.GenerateTensorValue(GeneratorTensor_1{}, num_thread);
        wei_kcyx.GenerateTensorValue(GeneratorTensor_1{}, num_thread);
#elif 0
        in_nchw.GenerateTensorValue(GeneratorTensor_1{}, num_thread);
        wei_kcyx.GenerateTensorValue(GeneratorTensor_2{-5, 5}, num_thread);
#elif 0
        in_nchw.GenerateTensorValue(GeneratorTensor_3{}, num_thread);
        wei_kcyx.GenerateTensorValue(GeneratorTensor_1{}, num_thread);
#elif 1
        in_nchw.GenerateTensorValue(GeneratorTensor_2{-5, 5}, num_thread);
        wei_kcyx.GenerateTensorValue(GeneratorTensor_2{-5, 5}, num_thread);
#elif 0
        in_nchw.GenerateTensorValue(GeneratorTensor_2{1, 5}, num_thread);

        auto gen_wei = [](auto... is) {
            return GeneratorTensor_2{1, 5}(is...) * GeneratorTensor_Checkboard{}(is...);
        };
        wei_kcyx.GenerateTensorValue(gen_wei, num_thread);
#endif
    }

#if 1
#if 0
    device_convolution_direct_v2_nchw_kcyx_nkhw
#elif 0
    device_convolution_implicit_gemm_v1_chwn_cyxk_khwn
#elif 0
    device_convolution_implicit_gemm_v1_nchw_cyxk_nkhw
#elif 0
    device_convolution_implicit_gemm_v2_chwn_cyxk_khwn
#elif 0
    device_convolution_implicit_gemm_v3_nchw_cyxk_nkhw
#elif 1
    device_convolution_implicit_gemm_v4_nchw_kcyx_nkhw
#endif
    (in_nchw_desc,
     in_nchw,
     wei_kcyx_desc,
     wei_kcyx,
     out_nkhw_desc,
     out_nkhw_device,
     ConvStrides{},
     ConvDilations{},
     nrepeat);

#elif 0
    device_implicit_gemm_convolution_1_chwn_cyxk_khwn_padded(in_nchw_desc,
                                                             in_nchw,
                                                             wei_kcyx_desc,
                                                             wei_kcyx,
                                                             out_nkhw_desc,
                                                             out_nkhw_device,
                                                             lower_pads,
                                                             upper_pads,
                                                             nrepeat);
#endif

    if(do_verification)
    {
#if 1
        if(Y == 3 && X == 3 && ConvStrides{}[0] == 1 && ConvStrides{}[1] == 1 &&
           ConvDilations{}[0] == 1 && ConvDilations{}[1] == 1)
        {
            host_winograd_3x3_convolution(in_nchw, wei_kcyx, out_nkhw_host, lower_pads, upper_pads);
        }
        else
#endif
        {
            host_direct_convolution(in_nchw,
                                    wei_kcyx,
                                    out_nkhw_host,
                                    ConvStrides{},
                                    ConvDilations{},
                                    lower_pads,
                                    upper_pads);
        }
        check_error(out_nkhw_host, out_nkhw_device);

#if 0
        LogRange(std::cout << "in_nchw : ", in_nchw.mData, ",") << std::endl;
        LogRange(std::cout << "wei_kcyx: ", wei_kcyx.mData, ",") << std::endl;
        LogRange(std::cout << "out_nkhw_host  : ", out_nkhw_host.mData, ",") << std::endl;
        LogRange(std::cout << "out_nkhw_device: ", out_nkhw_device.mData, ",") << std::endl;
#endif
    }
}
