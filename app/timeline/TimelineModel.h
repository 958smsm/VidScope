#pragma once

#include "media/MediaTypes.h"

#include <QtCore/QString>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace vidscope::timeline {

enum class TimelineMarkerKind : std::uint8_t {
    Keyframe,
    Scene,
    Chapter,
    Bookmark,
};

struct TimelineMarker final {
    std::uint64_t id = 0;
    media::MediaTime time{};
    TimelineMarkerKind kind = TimelineMarkerKind::Bookmark;
    QString label;
    QString category;
    QString note;

    friend bool operator==(const TimelineMarker&, const TimelineMarker&) = default;
};

struct FrameBoundary final {
    media::FrameId id;
    media::MediaTime time{};
    media::MediaTime duration{};
    bool keyFrame = false;

    friend bool operator==(const FrameBoundary&, const FrameBoundary&) = default;
};

class FrameBoundaryView final {
public:
    using const_iterator = std::deque<FrameBoundary>::const_iterator;

    FrameBoundaryView() noexcept = default;

    [[nodiscard]] const_iterator begin() const noexcept;
    [[nodiscard]] const_iterator end() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] const FrameBoundary& operator[](std::size_t index) const noexcept;
    [[nodiscard]] const FrameBoundary& front() const noexcept;
    [[nodiscard]] const FrameBoundary& back() const noexcept;

private:
    friend class TimelineModel;

    FrameBoundaryView(
        const std::deque<FrameBoundary>& storage,
        std::size_t offset,
        std::size_t count) noexcept;
    [[nodiscard]] static const std::deque<FrameBoundary>& emptyStorage() noexcept;

    const std::deque<FrameBoundary>* storage_ = nullptr;
    std::size_t offset_ = 0;
    std::size_t count_ = 0;
};

struct TimelineSelection final {
    media::MediaTime start{};
    media::MediaTime end{};

    friend bool operator==(const TimelineSelection&, const TimelineSelection&) = default;
};

struct TimelineSelectionDetails final {
    TimelineSelection range;
    std::optional<media::FrameId> firstFrame;
    std::optional<media::FrameId> lastFrame;
    std::optional<std::int64_t> frameCount;
    std::size_t knownFrameCount = 0;
};

// GUI-thread-owned timeline state. Time is authoritative; frame identities are
// attached only when decoding has established them. All pixel/time conversion
// lives here so QWidget event handlers do not duplicate timeline mathematics.
class TimelineModel final {
public:
    static constexpr std::size_t kDefaultMaximumKnownFrames = 100'000;
    static constexpr std::size_t kDefaultMaximumMarkers = 10'000;
    static constexpr media::MediaTime kMinimumViewportDuration{1'000};

    explicit TimelineModel(
        std::size_t maximumKnownFrames = kDefaultMaximumKnownFrames,
        std::size_t maximumMarkers = kDefaultMaximumMarkers);

    void reset(media::MediaTime duration = {});

    [[nodiscard]] media::MediaTime duration() const noexcept;
    [[nodiscard]] media::MediaTime viewportStart() const noexcept;
    [[nodiscard]] media::MediaTime viewportEnd() const noexcept;
    [[nodiscard]] media::MediaTime visibleDuration() const noexcept;
    [[nodiscard]] media::MediaTime playhead() const noexcept;
    [[nodiscard]] bool hasMedia() const noexcept;
    [[nodiscard]] bool isShowingEntireMedia() const noexcept;

    bool setPlayhead(media::MediaTime time) noexcept;
    bool setViewport(media::MediaTime start, media::MediaTime end) noexcept;
    bool showEntireMedia() noexcept;
    bool zoomAt(double factor, media::MediaTime anchor) noexcept;
    bool panBy(media::MediaTime delta) noexcept;
    bool panByPixels(double pixelsTowardLaterTime, double pixelWidth) noexcept;

    [[nodiscard]] double timeToPixel(
        media::MediaTime time,
        double pixelLeft,
        double pixelWidth) const noexcept;
    [[nodiscard]] media::MediaTime pixelToTime(
        double pixel,
        double pixelLeft,
        double pixelWidth) const noexcept;
    [[nodiscard]] std::optional<double> frameToPixel(
        const media::FrameId& frame,
        double pixelLeft,
        double pixelWidth) const noexcept;
    [[nodiscard]] double pixelsPerSecond(double pixelWidth) const noexcept;
    [[nodiscard]] media::MediaTime timePerPixel(double pixelWidth) const noexcept;

    [[nodiscard]] media::MediaTime majorTickInterval(
        double pixelWidth,
        double minimumPixelSpacing = 90.0) const noexcept;
    [[nodiscard]] std::vector<media::MediaTime> majorTicks(
        double pixelWidth,
        double minimumPixelSpacing = 90.0,
        std::size_t maximumTicks = 2'048) const;

    bool observeFrame(const FrameBoundary& frame);
    bool observeFrame(const media::DecodedFrame& frame);
    [[nodiscard]] std::size_t knownFrameCount() const noexcept;
    [[nodiscard]] std::size_t maximumKnownFrames() const noexcept;
    [[nodiscard]] FrameBoundaryView knownFrames() const noexcept;
    [[nodiscard]] FrameBoundaryView visibleFrameBoundaries(
        double pixelWidth,
        double minimumPixelSpacing = 4.0,
        std::size_t maximumTicks = 4'096) const noexcept;

    [[nodiscard]] std::optional<std::uint64_t> addMarker(
        media::MediaTime time,
        TimelineMarkerKind kind,
        QString label = {},
        QString category = {},
        QString note = {});
    bool updateMarker(
        std::uint64_t id,
        media::MediaTime time,
        TimelineMarkerKind kind,
        QString label = {},
        QString category = {},
        QString note = {});
    bool removeMarker(std::uint64_t id);
    void clearMarkers(std::optional<TimelineMarkerKind> kind = std::nullopt);
    [[nodiscard]] std::span<const TimelineMarker> markers() const noexcept;
    [[nodiscard]] std::span<const TimelineMarker> visibleMarkers() const noexcept;
    [[nodiscard]] std::optional<media::MediaTime> adjacentMarkerTime(
        TimelineMarkerKind kind,
        media::MediaTime reference,
        bool forward) const noexcept;

    bool setSelection(media::MediaTime anchor, media::MediaTime extent) noexcept;
    bool setSelectionIn(media::MediaTime time) noexcept;
    bool setSelectionOut(media::MediaTime time) noexcept;
    bool clearSelection() noexcept;
    [[nodiscard]] const std::optional<TimelineSelection>& selection() const noexcept;
    [[nodiscard]] TimelineSelectionDetails selectionDetails() const;

private:
    struct PresentationIndexLocation final {
        media::MediaTime time{};
        media::FrameId id;
    };

    [[nodiscard]] media::MediaTime clampTime(media::MediaTime time) const noexcept;
    void enforceKnownFrameBound();

    media::MediaTime duration_{};
    media::MediaTime viewportStart_{};
    media::MediaTime viewportEnd_{};
    media::MediaTime playhead_{};
    std::size_t maximumKnownFrames_ = 0;
    std::size_t maximumMarkers_ = 0;
    std::uint64_t nextMarkerId_ = 1;
    std::deque<FrameBoundary> knownFrames_;
    std::unordered_map<std::int64_t, PresentationIndexLocation>
        presentationIndexLocations_;
    std::vector<TimelineMarker> markers_;
    std::optional<TimelineSelection> selection_;
    mutable std::optional<TimelineSelectionDetails> selectionDetailsCache_;
};

} // namespace vidscope::timeline
