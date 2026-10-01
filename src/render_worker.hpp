// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "display_transform.hpp"
#include "render.hpp"
#include <QCache>
#include <QObject>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
namespace motion {
struct PreviewRequest {
    std::uint64_t requestId = 0, revision = 0;
    Project project;
    Id comp = 1;
    Time at;
    QSize size;
    QString baseDirectory;
    QString context{};
    DisplayOptions display{};
    RenderOptions options{};
};
struct PreviewResult {
    std::uint64_t requestId = 0, revision = 0;
    Time at;
    QImage image;
    QString error;
    double milliseconds = 0;
};
bool matches(const PreviewResult &, const PreviewRequest &);
struct ExportRequest {
    Project project;
    Id comp = 1;
    std::int64_t firstFrame = 0, frameCount = 1;
    QString newDirectory, baseDirectory;
    bool movie = false;
    RenderOptions options{};
};
struct ExportResult {
    QString state, directory, error;
    std::int64_t completed = 0;
};
ExportResult exportFrames(const ExportRequest &, std::stop_token = {},
                          std::function<void(std::int64_t)> progress = {});
ExportResult exportMovie(const ExportRequest &, std::stop_token = {},
                         std::function<void(std::int64_t)> progress = {});
class RenderWorker : public QObject {
    Q_OBJECT
  public:
    explicit RenderWorker(QObject *parent = nullptr);
    ~RenderWorker() override;
    void preview(PreviewRequest);
    void exportSequence(ExportRequest);
    void cancel();
    void setCacheMiB(int);
  signals:
    void previewReady(motion::PreviewResult);
    void exportProgress(qint64 completed, qint64 total);
    void exportFinished(motion::ExportResult);

  private:
    void run();
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<PreviewRequest> preview_;
    std::optional<ExportRequest> export_;
    std::stop_source cancel_;
    bool closing_ = false, exporting_ = false;
    int cacheMiB_ = 256;
    std::thread thread_;
};
} // namespace motion
