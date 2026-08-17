#pragma once

#include <QtCore/QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(logApp)
Q_DECLARE_LOGGING_CATEGORY(logPlayer)
Q_DECLARE_LOGGING_CATEGORY(logDemux)
Q_DECLARE_LOGGING_CATEGORY(logDecoder)
Q_DECLARE_LOGGING_CATEGORY(logSeek)
Q_DECLARE_LOGGING_CATEGORY(logFrame)
Q_DECLARE_LOGGING_CATEGORY(logRender)
Q_DECLARE_LOGGING_CATEGORY(logTimeline)
Q_DECLARE_LOGGING_CATEGORY(logCache)
Q_DECLARE_LOGGING_CATEGORY(logGpu)
Q_DECLARE_LOGGING_CATEGORY(logTest)

namespace vidscope::core {

void installLogging();
void installFfmpegLogBridge();

} // namespace vidscope::core
