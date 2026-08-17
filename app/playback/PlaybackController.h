#pragma once

#include "media/MediaTypes.h"
#include "playback/PlaybackSession.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <memory>

namespace vidscope::playback {

enum class PlaybackState {
    Closed,
    Stopped,
    Paused,
    Playing,
    Ended,
    Error,
};

class PlaybackController final : public QObject {
    Q_OBJECT

public:
    explicit PlaybackController(PlaybackSessionConfig config = {}, QObject* parent = nullptr);
    ~PlaybackController() override;
    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    [[nodiscard]] PlaybackState state() const noexcept;

public slots:
    void openFile(const QString& path);
    void play();
    void pause();
    void togglePlayPause();
    void stop();
    void seekToNanoseconds(qint64 nanoseconds);
    void stepFrames(int frameCount);
    void nextFrame();
    void previousFrame();
    void nextKeyframe();
    void previousKeyframe();

signals:
    void mediaOpened(vidscope::media::MediaInfoPtr info);
    void mediaClosed();
    void frameReady(vidscope::media::DecodedFramePtr frame, const QImage& image);
    void positionChanged(qint64 nanoseconds);
    void durationChanged(qint64 nanoseconds);
    void stateChanged(vidscope::playback::PlaybackState state);
    void errorOccurred(const QString& title, const QString& detail);
    void metricsUpdated(double decodeFramesPerSecond, qint64 seekMicroseconds, qsizetype cachedFrames);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vidscope::playback

Q_DECLARE_METATYPE(vidscope::media::MediaInfoPtr)
Q_DECLARE_METATYPE(vidscope::media::DecodedFramePtr)
Q_DECLARE_METATYPE(vidscope::playback::PlaybackState)
