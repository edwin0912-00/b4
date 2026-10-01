// SPDX-License-Identifier: MPL-2.0
#include "main_window.hpp"
#include "media.hpp"
#include <QDir>
#include <QFileInfo>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabBar>
#include <QTreeWidget>
#include <QUuid>
#include <algorithm>
namespace motion {
namespace {
Id putAssetLayer(Project &p, Id compId, Id assetId, Time at) {
    auto &c = composition(p, compId);
    const auto &a = asset(p, assetId);
    if (at < time(0) || !(at < c.duration))
        throw std::runtime_error("Drop time is outside the composition");
    Layer l;
    l.id = p.nextId++;
    l.name = QFileInfo(a.path).fileName();
    l.source = a.id;
    l.kind = a.kind == AssetKind::Image   ? LayerKind::Image
             : a.kind == AssetKind::Video ? LayerKind::Video
                                          : LayerKind::Audio;
    l.width = a.width;
    l.height = a.height;
    l.in = l.start = at;
    l.out = a.kind == AssetKind::Image ? c.duration : std::min(c.duration, at + a.duration);
    if (l.kind != LayerKind::Audio) {
        l.base("transform.anchor", 0) = a.width / 2.;
        l.base("transform.anchor", 1) = a.height / 2.;
        l.base("transform.position", 0) = c.width / 2.;
        l.base("transform.position", 1) = c.height / 2.;
    }
    auto id = l.id;
    c.layers.insert(c.layers.begin(), std::move(l));
    return id;
}
} // namespace
MainWindow::~MainWindow() {
    stopPlayback();
    importThread_.request_stop();
    playbackThread_.request_stop();
}
void MainWindow::importMediaFile(const QString &path, bool addLayer, Time at) {
    if (exporting_ || importing_) {
        report("Finish or cancel the active operation first");
        return;
    }
    if (preparingPlayback_)
        stopPlayback();
    importing_ = true;
    const auto ticket = ++importTicket_, epoch = projectEpoch_;
    const auto target = comp_;
    cancelExport_->setText("Cancel import");
    cancelExport_->show();
    report("Importing " + QFileInfo(path).fileName() + "…");
    importThread_ = std::jthread([this, path, addLayer, at, target, ticket, epoch](std::stop_token stop) {
        try {
            auto media = inspectMedia(path, 0, stop);
            QMetaObject::invokeMethod(
                this,
                [this, media = std::move(media), addLayer, at, target, ticket, epoch]() mutable {
                    if (ticket != importTicket_)
                        return;
                    importing_ = false;
                    cancelExport_->hide();
                    cancelExport_->setText("Cancel export");
                    if (epoch != projectEpoch_)
                        return;
                    guarded([&] {
                        Id id = 0, layerId = 0;
                        editor_.apply("Import media", [&](Project &p) {
                            id = p.nextId++;
                            media.id = id;
                            p.assets.push_back(media);
                            if (addLayer)
                                layerId = putAssetLayer(p, target, id, at);
                        });
                        if (layerId && target == comp_)
                            editor_.select({layerId});
                        for (int i = 0; i < projectTree_->topLevelItemCount(); ++i) {
                            auto *item = projectTree_->topLevelItem(i);
                            if (item->data(0, Qt::UserRole + 1) == "asset" &&
                                item->data(0, Qt::UserRole).toULongLong() == id) {
                                projectTree_->setCurrentItem(item);
                                break;
                            }
                        }
                        report("Imported " + QFileInfo(media.path).fileName());
                        emit mediaImported(id);
                    });
                },
                Qt::QueuedConnection);
        } catch (const RenderCancelled &) {
            QMetaObject::invokeMethod(
                this,
                [this, ticket] {
                    if (ticket != importTicket_)
                        return;
                    importing_ = false;
                    cancelExport_->hide();
                    report("Import cancelled");
                },
                Qt::QueuedConnection);
        } catch (const std::exception &e) {
            auto message = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(
                this,
                [this, ticket, message] {
                    if (ticket != importTicket_)
                        return;
                    importing_ = false;
                    cancelExport_->hide();
                    report(message);
                },
                Qt::QueuedConnection);
        }
    });
}
void MainWindow::addAssetLayer(Id id, Time at, Id target) {
    if (exporting_)
        return;
    if (!target)
        target = comp_;
    guarded([&] {
        Id layerId = 0;
        editor_.apply("Add footage layer", [&](Project &p) { layerId = putAssetLayer(p, target, id, at); });
        if (target == comp_)
            editor_.select({layerId});
    });
}
void MainWindow::createCompositionFromAsset(Id id) {
    if (exporting_)
        return;
    stopPlayback();
    guarded([&] {
        Id target = 0, layerId = 0;
        editor_.apply("Composition from footage", [&](Project &p) {
            const auto &a = asset(p, id);
            auto &initial = p.compositions.front();
            bool reuse = p.compositions.size() == 1 && initial.layers.empty() &&
                         initial.name == "Composition" && initial.width == 1920 && initial.height == 1080 &&
                         initial.fps == time(30) && initial.duration == time(12) && !initial.workArea;
            Composition c;
            if (reuse)
                c.id = initial.id;
            else
                c.id = p.nextId++;
            c.name = QFileInfo(a.path).completeBaseName();
            if (a.kind != AssetKind::Audio) {
                c.width = a.width;
                c.height = a.height;
            }
            if (a.kind != AssetKind::Image)
                c.duration = a.duration;
            if (a.kind == AssetKind::Video && seconds(a.fps) > 0)
                c.fps = a.fps;
            target = c.id;
            if (reuse)
                initial = c;
            else
                p.compositions.push_back(c);
            layerId = putAssetLayer(p, target, id, time(0));
        });
        chooseComposition(target);
        editor_.select({layerId});
    });
}
Project MainWindow::footageProject() const {
    Asset source = asset(editor_.project(), footage_);
    source.id = 3;
    Project p;
    p.assets = {source};
    p.nextId = 4;
    auto &c = p.compositions.front();
    c.width = source.width;
    c.height = source.height;
    if (source.kind != AssetKind::Image)
        c.duration = source.duration;
    if (source.kind == AssetKind::Video && seconds(source.fps) > 0)
        c.fps = source.fps;
    Layer l;
    l.id = 2;
    l.name = QFileInfo(source.path).fileName();
    l.source = 3;
    l.kind = source.kind == AssetKind::Image   ? LayerKind::Image
             : source.kind == AssetKind::Video ? LayerKind::Video
                                               : LayerKind::Audio;
    l.width = source.width;
    l.height = source.height;
    l.out = c.duration;
    c.layers = {l};
    return p;
}
Project MainWindow::playbackProject() const {
    if (viewerMode_ == 2)
        return footageProject();
    auto p = editor_.project();
    if (viewerMode_ == 0) {
        if (editor_.selection().empty())
            throw std::runtime_error("Select a layer first");
        auto l = layer(p, editor_.selection().front());
        l.parent.reset();
        composition(p, comp_).layers = {l};
    }
    return p;
}
void MainWindow::stopPlayback() {
    ++playbackTicket_;
    playbackThread_.request_stop();
    preparingPlayback_ = false;
    playing_ = false;
    playbackTimer_.stop();
    audioPlayback_.reset();
    audioMix_.reset();
    if (play_)
        play_->setText("Play");
    if (cancelExport_ && !exporting_ && !importing_)
        cancelExport_->hide();
}
void MainWindow::beginPlayback() {
    if (exporting_ || importing_)
        return;
    guarded([&] {
        if (viewerMode_ == 2 && (!footage_ || asset(editor_.project(), footage_).kind == AssetKind::Image)) {
            report("Select moving video or audio footage to play");
            return;
        }
        const auto snapshot = playbackProject();
        const Id target = viewerMode_ == 2 ? 1 : comp_;
        const auto &c = composition(snapshot, target);
        const bool loopWorkArea = previewWorkArea_ && viewerMode_ != 2;
        const WorkArea range = loopWorkArea ? resolvedWorkArea(c) : WorkArea{time(0), c.duration};
        const Time cti = viewerMode_ == 2 ? footageTime_ : now_;
        const Time initialStart = cti < range.start || !(cti < range.end) ? range.start : cti;
        const Time fps = c.fps, duration = c.duration;
        const auto revision = editor_.revision(), epoch = projectEpoch_, ticket = ++playbackTicket_;
        const auto mode = viewerMode_;
        preparingPlayback_ = true;
        play_->setText("Preparing…");
        cancelExport_->setText("Cancel preview");
        cancelExport_->show();
        playbackThread_ = std::jthread([this, snapshot, target, revision, epoch, ticket, mode, loopWorkArea,
                                        range, initialStart, fps, duration,
                                        base = baseDirectory_](std::stop_token stop) {
            try {
                auto mix = std::make_shared<AudioMix>(snapshot, target, base, stop, loopWorkArea);
                QMetaObject::invokeMethod(
                    this,
                    [this, mix, ticket, revision, epoch, mode, loopWorkArea, range, initialStart, fps,
                     duration] {
                        if (ticket != playbackTicket_ || !preparingPlayback_ || epoch != projectEpoch_ ||
                            mode != viewerMode_ ||
                            loopWorkArea != (previewWorkArea_ && viewerMode_ != 2))
                            return;
                        if (revision != editor_.revision()) {
                            preparingPlayback_ = false;
                            beginPlayback();
                            return;
                        }
                        guarded([&] {
                            playbackRange_ = range;
                            playbackFps_ = fps;
                            playbackDuration_ = duration;
                            playbackStart_ = initialStart;
                            if (mode == 2) {
                                if (footageTime_ != initialStart) {
                                    footageTime_ = initialStart;
                                    QSignalBlocker block(sourceSlider_);
                                    sourceSlider_->setValue(int(frameIndexFloor(initialStart, fps)));
                                    requestFrame();
                                }
                            } else if (now_ != initialStart) {
                                QScopedValueRollback advancing(advancingPlayback_, true);
                                setTime(initialStart);
                            }
                            audioMix_ = mix;
                            audioRevision_ = revision;
                            if (mix->hasAudio())
                                audioPlayback_ = std::make_unique<AudioPlayback>(mix, initialStart);
                            else
                                audioPlayback_.reset();
                            preparingPlayback_ = false;
                            playing_ = true;
                            cancelExport_->hide();
                            cancelExport_->setText("Cancel export");
                            play_->setText("Pause");
                            playbackClock_.restart();
                            playbackTimer_.start();
                        });
                        if (preparingPlayback_) {
                            stopPlayback();
                        }
                    },
                    Qt::QueuedConnection);
            } catch (const RenderCancelled &) {
            } catch (const std::exception &e) {
                auto error = QString::fromUtf8(e.what());
                QMetaObject::invokeMethod(
                    this,
                    [this, ticket, error] {
                        if (ticket != playbackTicket_)
                            return;
                        stopPlayback();
                        report(error);
                    },
                    Qt::QueuedConnection);
            }
        });
    });
}
void MainWindow::advancePlayback() {
    if (!playing_)
        return;
    try {
        const Time elapsed = audioPlayback_
                                 ? frameTime(audioPlayback_->playedSamples(), time(audioSampleRate))
                                 : time(playbackClock_.elapsed(), 1000);
        const Time span = playbackRange_.end - playbackRange_.start;
        if (!(time(0) < span) || !(time(0) < playbackFps_))
            throw std::runtime_error("Invalid playback range or frame rate");
        const Time phase = playbackStart_ - playbackRange_.start + elapsed;
        const auto cycles = frameIndexFloor(phase, time(span.denominator, span.numerator));
        const Time offset = phase - scaleTime(span, cycles);

        const Time at = playbackRange_.start + offset;
        const auto firstFrame = frameIndexFloor(playbackRange_.start, playbackFps_);
        const auto exclusiveEnd = playbackRange_.end == playbackDuration_
                                      ? frameCount(playbackDuration_, playbackFps_)
                                      : frameIndexFloor(playbackRange_.end, playbackFps_);
        if (exclusiveEnd <= firstFrame)
            throw std::runtime_error("Playback range contains no composition frame");
        const auto frame = std::clamp(frameIndexFloor(at, playbackFps_), firstFrame, exclusiveEnd - 1);
        const Time target = frameTime(frame, playbackFps_);
        if (viewerMode_ == 2) {
            footageTime_ = target;
            QSignalBlocker block(sourceSlider_);
            sourceSlider_->setValue(int(frame));
            requestFrame();
        } else {
            QScopedValueRollback advancing(advancingPlayback_, true);
            setTime(target);
        }
    } catch (const std::exception &e) {
        stopPlayback();
        report(e.what());
    }
}
void MainWindow::updateAudio() {
    if (!playing_ || audioRevision_ == editor_.revision())
        return;
    try {
        auto p = playbackProject();
        const Id target = viewerMode_ == 2 ? 1 : comp_;
        const auto &c = composition(p, target);
        const bool loopWorkArea = previewWorkArea_ && viewerMode_ != 2;
        const WorkArea range = loopWorkArea ? resolvedWorkArea(c) : WorkArea{time(0), c.duration};
        if (range != playbackRange_ || c.fps != playbackFps_ || c.duration != playbackDuration_) {
            stopPlayback();
            beginPlayback();
            return;
        }
        auto mix = std::make_shared<AudioMix>(std::move(p), target, baseDirectory_, std::stop_token{},
                                              loopWorkArea);
        if (audioPlayback_)
            audioPlayback_->update(mix);
        else if (mix->hasAudio()) {
            playbackStart_ = viewerMode_ == 2 ? footageTime_ : now_;
            audioPlayback_ = std::make_unique<AudioPlayback>(mix, playbackStart_);
            playbackClock_.restart();
        }
        audioMix_ = std::move(mix);
        audioRevision_ = editor_.revision();
    } catch (const std::exception &e) {
        stopPlayback();
        report(e.what());
    }
}
} // namespace motion
