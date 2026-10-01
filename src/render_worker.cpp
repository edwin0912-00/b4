// SPDX-License-Identifier: MPL-2.0
#include "render_worker.hpp"
#include "media.hpp"
#include "project_io.hpp"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <set>
#include <sys/clonefile.h>
namespace motion {
namespace {
void pinMedia(Project &project, const QString &base, const QString &folder, bool audio,
              std::stop_token stop) {
    std::set<Id> used;
    for (const auto &c : project.compositions)
        for (const auto &l : c.layers)
            if (l.kind == LayerKind::Video || (audio && l.kind == LayerKind::Audio))
                used.insert(l.source);
    for (auto &a : project.assets)
        if (used.count(a.id)) {
            if (stop.stop_requested())
                throw RenderCancelled{};
            const auto source = QDir(base).absoluteFilePath(a.path);
            if (mediaFingerprint(source, stop) != a.sha256)
                throw std::runtime_error("Media changed before export; reimport it explicitly");
            const auto directory = QDir(folder).filePath(QString::number(a.id));
            if (!QDir().mkdir(directory))
                throw std::runtime_error("Cannot create media snapshot folder");
            const auto target = QDir(directory).filePath(QFileInfo(source).fileName());
            if (clonefile(source.toUtf8().constData(), target.toUtf8().constData(), 0) != 0 &&
                !QFile::copy(source, target))
                throw std::runtime_error("Cannot snapshot media source for export");
            if (mediaFingerprint(target, stop) != a.sha256)
                throw std::runtime_error("Media changed while snapshotting");
            a.path = target;
            MovieSource check(target, a.sha256, stop);
            const auto &actual = check.info();
            if (actual.kind != a.kind || actual.width != a.width || actual.height != a.height ||
                !(actual.duration == a.duration) || actual.hasAudio != a.hasAudio)
                throw std::runtime_error("Stored media metadata differs from source; reimport it");
        }
}
} // namespace
bool matches(const PreviewResult &r, const PreviewRequest &a) {
    return r.requestId == a.requestId && r.revision == a.revision && r.at == a.at;
}
ExportResult exportFrames(const ExportRequest &request, std::stop_token stop,
                          std::function<void(std::int64_t)> progress) {
    ExportResult result{"failed", request.newDirectory, {}, 0};
    bool created = false;
    auto status = [&] {
        atomicWrite(QDir(request.newDirectory).filePath("export-status.json"),
                    QJsonDocument(QJsonObject{{"state", result.state},
                                              {"firstFrame", QString::number(request.firstFrame)},
                                              {"requestedFrames", QString::number(request.frameCount)},
                                              {"completedFrames", QString::number(result.completed)},
                                              {"error", result.error}})
                        .toJson());
    };
    try {
        validateProject(request.project);
        const auto &comp = composition(request.project, request.comp);
        if (request.frameCount < 1 || request.frameCount > 1000000 || request.firstFrame < 0 ||
            request.firstFrame > std::numeric_limits<std::int64_t>::max() - request.frameCount ||
            !(frameTime(request.firstFrame + request.frameCount - 1, comp.fps) < comp.duration))
            throw std::runtime_error("Export frame range is outside the composition");
        if (QFileInfo::exists(request.newDirectory))
            throw std::runtime_error("Export requires a new directory; existing files were not touched");
        Project project = request.project;
        QTemporaryDir snapshot;
        if (!snapshot.isValid())
            throw std::runtime_error("Cannot create export snapshot");
        pinMedia(project, request.baseDirectory, snapshot.path(), false, stop);
        Assets assets;
        assets.baseDirectory = request.baseDirectory;
        preflightSources(project, assets);
        if (stop.stop_requested())
            throw RenderCancelled{};
        QDir parent = QFileInfo(request.newDirectory).absoluteDir();
        if (!parent.exists() || !parent.mkdir(QFileInfo(request.newDirectory).fileName()))
            throw std::runtime_error("Cannot create the new output directory");
        created = true;
        result.state = "running";
        status();
        for (std::int64_t i = 0; i < request.frameCount; ++i) {
            if (stop.stop_requested())
                throw RenderCancelled{};
            auto frame = render(project, request.comp, frameTime(request.firstFrame + i, comp.fps),
                                QSize(comp.width, comp.height), assets, stop, request.options);
            auto image = pngImage(frame);
            auto file = QDir(request.newDirectory)
                            .filePath(QString("frame_%1.png").arg(request.firstFrame + i, 6, 10, QChar('0')));
            QSaveFile writer(file);
            writer.setDirectWriteFallback(false);
            if (!writer.open(QIODevice::WriteOnly) || !image.save(&writer, "PNG") || !writer.commit())
                throw std::runtime_error("Could not write a complete PNG frame");
            QImage decoded(file);
            if (decoded.isNull() || decoded.size() != image.size() ||
                decoded.convertToFormat(QImage::Format_RGBA8888) != image)
                throw std::runtime_error("PNG verification failed");
            ++result.completed;
            status();
            if (progress)
                progress(result.completed);
        }
        result.state = "complete";
        status();
    } catch (const RenderCancelled &) {
        result.state = "cancelled";
    } catch (const std::exception &e) {
        result.state = "failed";
        result.error = QString::fromUtf8(e.what());
    }
    if (created && result.state != "complete") {
        try {
            status();
        } catch (const std::exception &e) {
            result.error += "; status write failed: " + QString::fromUtf8(e.what());
        }
    }
    return result;
}
ExportResult exportMovie(const ExportRequest &request, std::stop_token stop,
                         std::function<void(std::int64_t)> progress) {
    ExportResult result{"failed", request.newDirectory, {}, 0};
    bool created = false;
    auto status = [&] {
        atomicWrite(QDir(request.newDirectory).filePath("export-status.json"),
                    QJsonDocument(QJsonObject{{"state", result.state},
                                              {"format", "H.264/AAC MP4"},
                                              {"requestedFrames", QString::number(request.frameCount)},
                                              {"completedFrames", QString::number(result.completed)},
                                              {"error", result.error}})
                        .toJson());
    };
    try {
        validateProject(request.project);
        const auto &comp = composition(request.project, request.comp);
        if (comp.width % 2 || comp.height % 2)
            throw std::runtime_error("H.264 output requires even composition dimensions");
        if (request.frameCount < 1 || request.frameCount > 1000000 || request.firstFrame < 0 ||
            request.firstFrame > INT64_MAX - request.frameCount ||
            !(frameTime(request.firstFrame + request.frameCount - 1, comp.fps) < comp.duration))
            throw std::runtime_error("Movie frame range is outside composition");
        if (QFileInfo::exists(request.newDirectory))
            throw std::runtime_error("Movie export requires a new folder");
        const auto origin = frameTime(request.firstFrame, comp.fps);
        const auto duration = std::min(frameTime(request.frameCount, comp.fps), comp.duration - origin);
        QStorageInfo storage(QFileInfo(request.newDirectory).absoluteDir().absolutePath());
        const auto estimate =
            std::clamp(double(comp.width) * comp.height * seconds(comp.fps) * .35, 4000000., 80000000.) *
            seconds(duration) / 8 * 1.3;
        if (storage.bytesAvailable() < 10ll * 1024 * 1024 * 1024 || storage.bytesAvailable() < estimate * 3)
            throw std::runtime_error("Movie export needs at least10GiB and3x estimated output free");
        QTemporaryDir snapshot;
        if (!snapshot.isValid())
            throw std::runtime_error("Cannot create source snapshot");
        auto project = request.project;
        pinMedia(project, request.baseDirectory, snapshot.path(), true, stop);
        Assets assets;
        assets.baseDirectory = request.baseDirectory;
        preflightSources(project, assets);
        AudioMix mix(project, request.comp, request.baseDirectory, stop);
        if (stop.stop_requested())
            throw RenderCancelled{};
        QDir parent = QFileInfo(request.newDirectory).absoluteDir();
        if (!parent.mkdir(QFileInfo(request.newDirectory).fileName()))
            throw std::runtime_error("Cannot create movie output folder");
        created = true;
        result.state = "running";
        status();
        auto partial = QDir(request.newDirectory).filePath("Movie.partial.mp4");
        {
            MovieWriter writer(partial, QSize(comp.width, comp.height), comp.fps, duration, mix.hasAudio());
            std::stop_source work;
            std::stop_callback parentCancel(stop, [&] { work.request_stop(); });
            std::exception_ptr audioFailure;
            std::jthread audioWriter;
            if (mix.hasAudio())
                audioWriter = std::jthread([&](std::stop_token threadStop) {
                    std::stop_callback cancel(threadStop, [&] { work.request_stop(); });
                    try {
                        std::vector<float> audio(1024 * 2);
                        const auto total = std::llround(seconds(duration) * audioSampleRate);
                        for (std::int64_t at = 0; at < total;) {
                            if (work.stop_requested())
                                throw RenderCancelled{};
                            int n = int(std::min<std::int64_t>(1024, total - at));
                            mix.read(origin + frameTime(at, time(audioSampleRate)), n, audio.data());
                            writer.audio(audio.data(), n, at, work.get_token());
                            at += n;
                        }
                        writer.finishAudio();
                    } catch (const RenderCancelled &) {
                    } catch (...) {
                        audioFailure = std::current_exception();
                        work.request_stop();
                    }
                });
            try {
                for (std::int64_t i = 0; i < request.frameCount; ++i) {
                    if (work.stop_requested())
                        throw RenderCancelled{};
                    auto frame = render(project, request.comp, origin + frameTime(i, comp.fps),
                                        QSize(comp.width, comp.height), assets, work.get_token(), request.options);
                    writer.video(opaqueImage(frame), frameTime(i, comp.fps), work.get_token());
                    ++result.completed;
                    if (progress)
                        progress(result.completed);
                    status();
                }
                writer.finishVideo();
            } catch (...) {
                work.request_stop();
                if (audioWriter.joinable())
                    audioWriter.join();
                if (audioFailure)
                    std::rethrow_exception(audioFailure);
                throw;
            }
            if (audioWriter.joinable())
                audioWriter.join();
            if (audioFailure)
                std::rethrow_exception(audioFailure);
            writer.finish(duration, stop);
        }
        if (stop.stop_requested())
            throw RenderCancelled{};
        if (!QFile::rename(partial, QDir(request.newDirectory).filePath("Movie.mp4")))
            throw std::runtime_error("Cannot publish completed movie");
        result.state = "complete";
        status();
    } catch (const RenderCancelled &) {
        result.state = "cancelled";
    } catch (const std::exception &error) {
        result.state = "failed";
        result.error = error.what();
    }
    if (created && result.state != "complete") {
        QFile::remove(QDir(request.newDirectory).filePath("Movie.partial.mp4"));
        try {
            status();
        } catch (const std::exception &e) {
            result.error += "; " + QString::fromUtf8(e.what());
        }
    }
    return result;
}
RenderWorker::RenderWorker(QObject *parent) : QObject(parent), thread_([this] { run(); }) {}
RenderWorker::~RenderWorker() {
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
        cancel_.request_stop();
    }
    wake_.notify_all();
    thread_.join();
}
void RenderWorker::preview(PreviewRequest request) {
    {
        std::lock_guard lock(mutex_);
        if (exporting_)
            return;
        cancel_.request_stop();
        preview_ = std::move(request);
    }
    wake_.notify_one();
}
void RenderWorker::exportSequence(ExportRequest request) {
    {
        std::lock_guard lock(mutex_);
        if (exporting_)
            throw std::runtime_error("An export is already active");
        exporting_ = true;
        cancel_.request_stop();
        preview_.reset();
        export_ = std::move(request);
    }
    wake_.notify_one();
}
void RenderWorker::cancel() {
    std::optional<ExportResult> result;
    {
        std::lock_guard lock(mutex_);
        cancel_.request_stop();
        preview_.reset();
        if (export_) {
            result = ExportResult{"cancelled", export_->newDirectory, {}, 0};
            export_.reset();
            exporting_ = false;
        }
    }
    if (result)
        QMetaObject::invokeMethod(
            this, [this, result] { emit exportFinished(*result); }, Qt::QueuedConnection);
}
void RenderWorker::setCacheMiB(int value) {
    std::lock_guard lock(mutex_);
    cacheMiB_ = std::clamp(value, 16, 1024);
}
void RenderWorker::run() {
    QCache<QString, QImage> cache(256 * 1024 * 1024);
    Assets assets;
    std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
    for (;;) {
        std::optional<PreviewRequest> preview;
        std::optional<ExportRequest> exportJob;
        std::stop_token stop;
        int cacheMiB;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return closing_ || preview_ || export_; });
            if (closing_)
                return;
            preview = std::move(preview_);
            preview_.reset();
            exportJob = std::move(export_);
            export_.reset();
            cancel_ = std::stop_source();
            stop = cancel_.get_token();
            cacheMiB = cacheMiB_;
        }
        if (exportJob) {
            auto exporter = exportJob->movie ? exportMovie : exportFrames;
            auto result = exporter(*exportJob, stop, [&](std::int64_t done) {
                auto total = exportJob->frameCount;
                QMetaObject::invokeMethod(
                    this, [this, done, total] { emit exportProgress(done, total); }, Qt::QueuedConnection);
            });
            {
                std::lock_guard lock(mutex_);
                exporting_ = false;
            }
            QMetaObject::invokeMethod(
                this, [this, result] { emit exportFinished(result); }, Qt::QueuedConnection);
            continue;
        }
        if (!preview)
            continue;
        const auto &request = *preview;
        PreviewResult result{request.requestId, request.revision, request.at, {}, {}, 0};
        QElapsedTimer timer;
        timer.start();
        try {
            cache.setMaxCost(cacheMiB * 1024 * 1024);
            // ponytail: invalidate all previews per revision; add dependency invalidation only after
            // profiling.
            if (revision != request.revision || assets.baseDirectory != request.baseDirectory) {
                cache.clear();
                if (assets.baseDirectory != request.baseDirectory)
                    assets = Assets{};
                assets.baseDirectory = request.baseDirectory;
                revision = request.revision;
            }
            auto key = request.context + ":" + QString::number(request.comp) + ":" +
                       QString::number(request.at.numerator) + "/" + QString::number(request.at.denominator) +
                       ":" + QString::number(request.size.width()) + "x" +
                       QString::number(request.size.height()) + ":channel:" +
                       QString::number(static_cast<int>(request.display.channel)) + ":grayscale:" +
                       QString::number(request.display.grayscale) + ":exposure:" +
                       QString::number(request.display.exposureStops, 'g', 17) + ":motionBlur:" +
                       QString::number(static_cast<int>(request.options.motionBlur));
            if (auto *image = cache.object(key))
                result.image = *image;
            else {
                const auto frame = render(request.project, request.comp, request.at, request.size, assets,
                                          stop, request.options);
                result.image = displayImage(frame, request.display, stop);
                if (result.image.sizeInBytes() <= cache.maxCost())
                    cache.insert(key, new QImage(result.image), int(result.image.sizeInBytes()));
            }
        } catch (const RenderCancelled &) {
            continue;
        } catch (const std::exception &e) {
            result.error = QString::fromUtf8(e.what());
        }
        result.milliseconds = double(timer.nsecsElapsed()) / 1e6;
        if (stop.stop_requested())
            continue;
        QMetaObject::invokeMethod(this, [this, result] { emit previewReady(result); }, Qt::QueuedConnection);
    }
}
} // namespace motion
