#include "TestHarness.h"

#include "media/FfmpegRaii.h"
#include "media/MediaTypes.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

namespace {

vidscope::media::DecodedFramePtr makeGrayFrame(
    const bool negativeStride,
    const std::uint8_t padding,
    const bool alterVisibleByte = false)
{
    auto surface = vidscope::media::makeFrame();
    surface->format = AV_PIX_FMT_GRAY8;
    surface->width = 3;
    surface->height = 2;
    VIDSCOPE_REQUIRE(av_frame_get_buffer(surface.get(), 32) >= 0);
    VIDSCOPE_REQUIRE(surface->linesize[0] >= surface->width);

    const int stride = surface->linesize[0];
    std::memset(
        surface->data[0],
        padding,
        static_cast<std::size_t>(stride * surface->height));

    constexpr std::array<std::uint8_t, 3> top{10, 20, 30};
    constexpr std::array<std::uint8_t, 3> bottom{40, 50, 60};
    auto* physicalTop = surface->data[0];
    auto* physicalBottom = surface->data[0] + stride;
    if (negativeStride) {
        std::memcpy(physicalTop, bottom.data(), bottom.size());
        std::memcpy(physicalBottom, top.data(), top.size());
        if (alterVisibleByte) {
            physicalBottom[1] ^= 0x1U;
        }
        surface->data[0] = physicalBottom;
        surface->linesize[0] = -stride;
    } else {
        std::memcpy(physicalTop, top.data(), top.size());
        std::memcpy(physicalBottom, bottom.data(), bottom.size());
        if (alterVisibleByte) {
            physicalTop[1] ^= 0x1U;
        }
    }

    auto decoded = std::make_shared<vidscope::media::DecodedFrame>();
    decoded->storage = std::make_shared<vidscope::media::FrameStorage>(surface.get());
    decoded->width = surface->width;
    decoded->height = surface->height;
    decoded->pixelFormat = AV_PIX_FMT_GRAY8;
    return decoded;
}

} // namespace

VIDSCOPE_TEST(Visible_image_comparison_handles_negative_stride_and_ignores_padding)
{
    const auto positive = makeGrayFrame(false, 0x11U);
    const auto negative = makeGrayFrame(true, 0xeeU);

    VIDSCOPE_REQUIRE(vidscope::media::visibleImagesEqual(*positive, *negative));
    VIDSCOPE_REQUIRE(vidscope::media::visibleImagesEqual(*negative, *positive));
}

VIDSCOPE_TEST(Visible_image_comparison_detects_a_changed_visible_byte)
{
    const auto reference = makeGrayFrame(false, 0x22U);
    const auto changed = makeGrayFrame(true, 0xddU, true);

    VIDSCOPE_REQUIRE(!vidscope::media::visibleImagesEqual(*reference, *changed));
}
