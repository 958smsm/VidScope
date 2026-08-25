#pragma once

#include "analysis/AnalysisTypes.h"
#include "media/MediaTypes.h"

#include <QtCore/QString>

#include <cstddef>
#include <memory>
#include <vector>

namespace vidscope::analysis {

struct AnalysisCacheConfig final {
    QString diskDirectory;
    std::size_t diskBudgetBytes = 512ULL * 1024ULL * 1024ULL;
    std::size_t maximumDocumentBytes = 256ULL * 1024ULL * 1024ULL;
    std::size_t maximumSamples = 2'000'000;
};

struct AnalysisCacheDocument final {
    std::vector<AnalysisSample> samples;
    bool complete = false;
};

class AnalysisCache final {
public:
    explicit AnalysisCache(AnalysisCacheConfig config = {});
    ~AnalysisCache();
    AnalysisCache(const AnalysisCache&) = delete;
    AnalysisCache& operator=(const AnalysisCache&) = delete;

    [[nodiscard]] static QString mediaIdentity(const media::MediaInfo& info);
    [[nodiscard]] AnalysisCacheDocument load(const media::MediaInfo& info) const;
    [[nodiscard]] bool save(
        const media::MediaInfo& info,
        const std::vector<AnalysisSample>& samples,
        bool complete);
    void clearMedia(const media::MediaInfo& info);
    void prune();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vidscope::analysis

