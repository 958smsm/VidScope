#include "core/Logging.h"

#include <QtCore/QString>

#include <array>
#include <cstdarg>
#include <mutex>

extern "C" {
#include <libavutil/log.h>
}

Q_LOGGING_CATEGORY(logApp, "vidscope.app", QtInfoMsg)
Q_LOGGING_CATEGORY(logPlayer, "vidscope.player", QtInfoMsg)
Q_LOGGING_CATEGORY(logDemux, "vidscope.demux", QtInfoMsg)
Q_LOGGING_CATEGORY(logDecoder, "vidscope.decoder", QtInfoMsg)
Q_LOGGING_CATEGORY(logSeek, "vidscope.seek", QtInfoMsg)
Q_LOGGING_CATEGORY(logFrame, "vidscope.frame", QtWarningMsg)
Q_LOGGING_CATEGORY(logRender, "vidscope.render", QtInfoMsg)
Q_LOGGING_CATEGORY(logCache, "vidscope.cache", QtInfoMsg)
Q_LOGGING_CATEGORY(logGpu, "vidscope.gpu", QtInfoMsg)
Q_LOGGING_CATEGORY(logTest, "vidscope.test", QtInfoMsg)

namespace {

Q_LOGGING_CATEGORY(logFfmpeg, "vidscope.ffmpeg", QtInfoMsg)

void ffmpegLogCallback(void* context, int level, const char* format, va_list arguments)
{
    if (format == nullptr || level > av_log_get_level()) {
        return;
    }

    QtMsgType messageType = QtDebugMsg;
    if (level <= AV_LOG_ERROR) {
        messageType = QtCriticalMsg;
    } else if (level <= AV_LOG_WARNING) {
        messageType = QtWarningMsg;
    } else if (level <= AV_LOG_INFO) {
        messageType = QtInfoMsg;
    }
    if (!logFfmpeg().isEnabled(messageType)) {
        return;
    }

    std::array<char, 4'096> buffer{};
    thread_local int printPrefix = 1;
    va_list copy;
    va_copy(copy, arguments);
    av_log_format_line2(
        context,
        level,
        format,
        copy,
        buffer.data(),
        static_cast<int>(buffer.size()),
        &printPrefix);
    va_end(copy);

    const QString message = QString::fromUtf8(buffer.data()).trimmed();
    if (message.isEmpty()) {
        return;
    }

    switch (messageType) {
    case QtCriticalMsg:
        qCCritical(logFfmpeg).noquote() << message;
        break;
    case QtWarningMsg:
        qCWarning(logFfmpeg).noquote() << message;
        break;
    case QtInfoMsg:
        qCInfo(logFfmpeg).noquote() << message;
        break;
    case QtDebugMsg:
    default:
        qCDebug(logFfmpeg).noquote() << message;
        break;
    }
}

} // namespace

namespace vidscope::core {

void installLogging()
{
    static std::once_flag installed;
    std::call_once(installed, [] {
        if (qEnvironmentVariableIsEmpty("QT_MESSAGE_PATTERN")) {
            qSetMessagePattern(
                QStringLiteral("[%{time yyyy-MM-dd hh:mm:ss.zzz}] "
                               "[%{type}] [%{category}] %{message}"));
        }
    });
}

void installFfmpegLogBridge()
{
    static std::once_flag installed;
    std::call_once(installed, [] { av_log_set_callback(&ffmpegLogCallback); });
}

} // namespace vidscope::core
