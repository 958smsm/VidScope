#pragma once

#include "core/Cancellation.h"
#include "media/MediaSource.h"

#include <cstdint>

namespace vidscope::media {

enum class PacketReadStatus {
    Packet,
    EndOfFile,
    Cancelled,
};

class Demuxer final {
public:
    explicit Demuxer(MediaSource& source) noexcept;

    [[nodiscard]] PacketReadStatus readNextVideoPacket(
        AVPacket* destination,
        core::CancellationToken cancellation = {});

    bool seekToTimestamp(
        std::int64_t absoluteStreamTimestamp,
        core::CancellationToken cancellation = {});
    bool seekToStart(core::CancellationToken cancellation = {});

private:
    MediaSource& source_;
};

} // namespace vidscope::media
