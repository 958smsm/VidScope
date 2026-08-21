#pragma once

#include "core/Cancellation.h"
#include "media/MediaTypes.h"

#include <QtCore/QSize>
#include <QtGui/QImage>

#include <memory>

namespace vidscope::media {

class FrameConverter final {
public:
    FrameConverter();
    ~FrameConverter();
    FrameConverter(const FrameConverter&) = delete;
    FrameConverter& operator=(const FrameConverter&) = delete;

    [[nodiscard]] QImage toBgraImage(
        const DecodedFrame& frame,
        core::CancellationToken cancellation = {});
    [[nodiscard]] QImage toBgraImage(
        const DecodedFrame& frame,
        QSize outputSize,
        core::CancellationToken cancellation = {});
    void reset() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vidscope::media
