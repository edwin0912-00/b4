// SPDX-License-Identifier: MPL-2.0
#include "media.hpp"
#include "project_io.hpp"
#include "render_worker.hpp"
#include "support.hpp"
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <iostream>
using namespace motion;
static Project movieProject(Asset a) {
    Project p;
    a.id = 2;
    p.assets = {a};
    auto &c = p.compositions.front();
    c.width = a.width;
    c.height = a.height;
    c.duration = a.duration;
    c.fps = a.fps;
    Layer l;
    l.id = 3;
    l.kind = LayerKind::Video;
    l.source = 2;
    l.width = a.width;
    l.height = a.height;
    l.out = a.duration;
    c.layers = {l};
    p.nextId = 4;
    return p;
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        auto path = QString(MOTION_SOURCE_DIR) + "/fixtures/media/source.mp4";
        auto a = inspectMedia(path, 2);
        check(a.kind == AssetKind::Video && a.width == 640 && a.height == 360 && a.hasAudio,
              "inspect native video/audio");
        check(a.fps == time(30000, 1001) && a.duration == time(1001, 250) && !a.variableRate,
              "fractional media rate and duration stay exact");
        MovieSource movie(path, a.sha256);
        auto first = movie.frame(motion::time(0));
        auto later = movie.frame(time(101, 100));
        check(later.at == time(1001, 1000) && first.image != later.image,
              "requested time selects the preceding exact presentation timestamp");
        check(movie.frame(motion::time(0)).image == first.image,
              "backward frame seek returns original frame");
        check(movie.frame(time(-1)).image.isNull() && movie.frame(time(5)).image.isNull(),
              "outside video duration is transparent");
        auto q = inspectMedia(QString(MOTION_SOURCE_DIR) + "/fixtures/media/quadrants.mp4", 5);
        MovieSource quadrants(q.path, q.sha256);
        auto corners = quadrants.frame(motion::time(0)).image;
        auto red = corners.pixelColor(8, 8), blue = corners.pixelColor(8, 24),
             green = corners.pixelColor(48, 8);
        check(red.red() > 200 && red.blue() < 50 && blue.blue() > 200 && blue.red() < 50 &&
                  green.green() > 200,
              "native image conversion preserves independent quadrant orientation and channels");
        auto v = inspectMedia(QString(MOTION_SOURCE_DIR) + "/fixtures/media/variable.mp4", 6);
        MovieSource variable(v.path, v.sha256);
        check(v.variableRate && v.fps == motion::time(0) && variable.frame(time(69, 100)).at == time(1, 5) &&
                  variable.frame(time(7, 10)).at == time(7, 10),
              "VFR source uses actual timestamps, with no invented constant FPS");
        auto sound = inspectMedia(QString(MOTION_SOURCE_DIR) + "/fixtures/media/tone.wav", 7);
        check(sound.kind == AssetKind::Audio && sound.hasAudio && sound.audioChannels == 1 &&
                  sound.waveform.size() == 128,
              "audio-only media imports with waveform metadata");
        auto p = movieProject(a);
        check(encodeProject(decodeProject(encodeProject(p))) == encodeProject(p), "media schema3 round-trip");
        Assets assets;
        assets.baseDirectory = QFileInfo(path).absolutePath();
        auto f0 = pngImage(render(p, 1, motion::time(0), QSize(640, 360), assets));
        auto f1 = pngImage(render(p, 1, time(1), QSize(640, 360), assets));
        check(f0 != f1, "compositor uses changing video source frames");
        auto &l = layer(p, 3);
        p.compositions.front().duration = time(6);
        l.start = l.in = time(1, 2);
        l.out = l.start + a.duration;
        l.base("audio.levels", 0) = -6.020599913279624;
        auto mix = std::make_shared<AudioMix>(p, 1, QFileInfo(path).absolutePath());
        std::vector<float> samples(8000 * 2);
        mix->read(time(37, 25), 8000, samples.data());
        double left = 0, right = 0;
        for (int i = 0; i < 8000; ++i) {
            left += samples[i * 2] * samples[i * 2];
            right += samples[i * 2 + 1] * samples[i * 2 + 1];
        }
        check(right > .01 && near(std::sqrt(left / right), .5, 1e-5),
              "audio source offset and stereo dB levels affect real PCM");

        auto loopProject = movieProject(a);
        auto &loopComp = loopProject.compositions.front();
        setCompositionTiming(loopComp, motion::time(60), motion::time(3));
        auto &loopLayer = layer(loopProject, 3);
        loopLayer.out = loopComp.duration;
        loopLayer.channel({"audio.levels", 0}).keys = {{motion::time(0), -60, Interpolation::Linear},
                                                        {motion::time(1), -3, Interpolation::Linear},
                                                        {motion::time(11, 10), -15, Interpolation::Linear},
                                                        {motion::time(29, 10), -15, Interpolation::Hold}};
        loopLayer.channel({"audio.levels", 1}).keys = {{motion::time(0), -54, Interpolation::Linear},
                                                        {motion::time(1), -6, Interpolation::Linear},
                                                        {motion::time(11, 10), -18, Interpolation::Linear},
                                                        {motion::time(29, 10), -18, Interpolation::Hold}};
        setWorkAreaFrames(loopComp, 60, 66);
        const auto loopRange = resolvedWorkArea(loopComp);
        check(loopRange == WorkArea{motion::time(1), motion::time(11, 10)},
              "audio preview fixture uses an exact nonzero Work Area");
        const auto mediaBase = QFileInfo(path).absolutePath();
        AudioMix fullLoopProject(loopProject, 1, mediaBase);
        AudioMix workAreaMix(loopProject, 1, mediaBase, {}, true);
        AudioMix explicitFullMix(loopProject, 1, mediaBase, {}, false);
        constexpr int loopSamples = 4800;
        constexpr int repeatedSamples = loopSamples * 2 + 300;
        std::vector<float> repeated(size_t(repeatedSamples) * 2), loopPeriod(size_t(loopSamples) * 2),
            fullA(size_t(960) * 2), fullB(size_t(800) * 2), wrongStart(size_t(800) * 2);
        workAreaMix.read(loopRange.start, repeatedSamples, repeated.data());
        fullLoopProject.read(loopRange.start, loopSamples, loopPeriod.data());
        double repeatError = 0;
        for (int i = 0; i < repeatedSamples; ++i)
            for (int channel = 0; channel < 2; ++channel)
                repeatError = std::max(
                    repeatError,
                    std::abs(double(repeated[size_t(i) * 2 + channel] -
                                    loopPeriod[size_t(i % loopSamples) * 2 + channel])));
        check(repeatError < 1e-5, "nonzero Work Area repeats both audio channels over multiple wraps");

        const Time crossingStart = motion::time(27, 25);
        workAreaMix.read(crossingStart, 1760, repeated.data());
        fullLoopProject.read(crossingStart, 960, fullA.data());
        fullLoopProject.read(loopRange.start, 800, fullB.data());
        fullLoopProject.read(motion::time(0), 800, wrongStart.data());
        double beforeError = 0, afterError = 0, compZeroDifference = 0;
        for (int i = 0; i < 960 * 2; ++i)
            beforeError = std::max(beforeError, std::abs(double(repeated[i] - fullA[i])));
        for (int i = 0; i < 800 * 2; ++i) {
            afterError = std::max(afterError, std::abs(double(repeated[960 * 2 + i] - fullB[i])));
            compZeroDifference =
                std::max(compZeroDifference, std::abs(double(fullB[i] - wrongStart[i])));
        }
        check(beforeError < 1e-5 && afterError < 1e-5,
              "audio block straddling Work Area end wraps to its exact nonzero start");
        check(compZeroDifference > .01,
              "animated Audio Levels make Work Area start observably different from composition zero");
        std::vector<float> defaultFull(4096 * 2), explicitFull(4096 * 2);
        fullLoopProject.read(motion::time(37, 25), 4096, defaultFull.data());
        explicitFullMix.read(motion::time(37, 25), 4096, explicitFull.data());
        check(defaultFull == explicitFull, "default and explicit full-composition audio loops stay identical");

        auto fractionalFullProject = movieProject(a);
        auto &fullFractionComp = fractionalFullProject.compositions.front();
        setCompositionTiming(fullFractionComp, motion::time(30000, 1001), motion::time(1001, 10000));
        auto &fullFractionLayer = layer(fractionalFullProject, 3);
        fullFractionLayer.start = motion::time(-1); // The fixture's audible tone starts at source1s.
        fullFractionLayer.out = fullFractionComp.duration;
        AudioMix fractionalFull(fractionalFullProject, 1, mediaBase);
        std::vector<float> fullWhole(12000 * 2), fullPartitioned(12000 * 2);
        fractionalFull.read(frameTime(4800, motion::time(audioSampleRate)), 12000, fullWhole.data());
        const int chunks[] = {1, 17, 509, 2048, 73};
        for (int done = 0, chunk = 0; done < 12000; ++chunk) {
            const int n = std::min(chunks[chunk % 5], 12000 - done);
            fractionalFull.read(frameTime(4800 + done, motion::time(audioSampleRate)), n,
                                fullPartitioned.data() + done * 2);
            done += n;
        }
        double fullPartitionError = 0;
        for (size_t i = 0; i < fullWhole.size(); ++i)
            fullPartitionError = std::max(fullPartitionError,
                                          std::abs(double(fullWhole[i] - fullPartitioned[i])));
        check(fullPartitionError < 1e-5,
              "full-composition fractional audio loops preserve phase across arbitrary block partitions");

        auto shiftedProject = loopProject;
        setWorkAreaFrames(composition(shiftedProject, 1), 61, 67);
        const auto shiftedRange = resolvedWorkArea(composition(shiftedProject, 1));
        AudioMix shiftedMix(shiftedProject, 1, mediaBase, {}, true);
        AudioMix shiftedFull(shiftedProject, 1, mediaBase);
        std::vector<float> shiftedActual(1760 * 2), shiftedFirst(800 * 2), shiftedNext(960 * 2);
        shiftedMix.read(motion::time(11, 10), 1760, shiftedActual.data());
        shiftedFull.read(motion::time(11, 10), 800, shiftedFirst.data());
        shiftedFull.read(shiftedRange.start, 960, shiftedNext.data());
        double shiftedError = 0;
        for (int i = 0; i < 800 * 2; ++i)
            shiftedError = std::max(shiftedError, std::abs(double(shiftedActual[i] - shiftedFirst[i])));
        for (int i = 0; i < 960 * 2; ++i)
            shiftedError = std::max(
                shiftedError, std::abs(double(shiftedActual[800 * 2 + i] - shiftedNext[i])));
        check(shiftedRange.start == motion::time(61, 60) && shiftedError < 1e-5,
              "new Work Area snapshot loops at its updated nonzero range");

        constexpr std::int64_t lateStartFrame = 2492001;
        auto lateProject = movieProject(a);
        auto &lateComp = composition(lateProject, 1);
        setCompositionTiming(lateComp, motion::time(30), motion::time(86400));
        auto &lateLayer = layer(lateProject, 3);
        lateLayer.out = lateComp.duration;
        setWorkAreaFrames(lateComp, lateStartFrame, lateStartFrame + 1);
        const auto lateRange = resolvedWorkArea(lateComp);
        lateLayer.start = lateRange.start - motion::time(1);
        AudioMix lateMix(lateProject, 1, mediaBase, {}, true);
        constexpr int latePeriodSamples = 1600;
        std::vector<float> lateCycles(latePeriodSamples * 4);
        lateMix.read(lateRange.start, latePeriodSamples * 2, lateCycles.data());
        double latePeriodError = 0;
        for (int i = 0; i < latePeriodSamples * 2; ++i)
            latePeriodError = std::max(
                latePeriodError,
                std::abs(double(lateCycles[size_t(i)] - lateCycles[size_t(latePeriodSamples) * 2 + i])));
        const bool lateAudioIsLive = std::any_of(lateCycles.begin(), lateCycles.begin() + latePeriodSamples * 2,
                                                 [](float sample) { return std::abs(sample) > .01f; });
        check(lateRange.start == frameTime(lateStartFrame, lateComp.fps) &&
                  lateRange.end - lateRange.start == motion::time(1, 30) && lateAudioIsLive &&
                  lateRange.start + frameTime(latePeriodSamples, time(audioSampleRate)) == lateRange.end &&
                  latePeriodError < 1e-5,
              "24-hour-scale 30fps Work Area wraps before its exclusive endpoint sample");

        constexpr std::int64_t fractionalStartFrame = 2492001;
        auto fractionalProject = movieProject(a);
        auto &fractionalComp = composition(fractionalProject, 1);
        setCompositionTiming(fractionalComp, motion::time(30000, 1001), motion::time(86400));
        auto &fractionalLayer = layer(fractionalProject, 3);
        fractionalLayer.out = fractionalComp.duration;
        setWorkAreaFrames(fractionalComp, fractionalStartFrame, fractionalStartFrame + 3);
        const auto fractionalRange = resolvedWorkArea(fractionalComp);
        fractionalLayer.start = fractionalRange.start - motion::time(1);
        AudioMix fractionalMix(fractionalProject, 1, mediaBase, {}, true);
        constexpr int fractionalPeriodSamples = 4805;
        constexpr int fractionalTotalSamples = fractionalPeriodSamples * 3 + 321;
        std::vector<float> oneBlock(size_t(fractionalTotalSamples) * 2), partitioned(oneBlock.size());
        fractionalMix.read(fractionalRange.start, fractionalTotalSamples, oneBlock.data());
        const int partitions[] = {1000, 3001, 571, 4096, 1231, 997, 3840};
        int sampleOffset = 0;
        for (const int requested : partitions) {
            const int count = std::min(requested, fractionalTotalSamples - sampleOffset);
            fractionalMix.read(fractionalRange.start + frameTime(sampleOffset, time(audioSampleRate)), count,
                               partitioned.data() + size_t(sampleOffset) * 2);
            sampleOffset += count;
        }
        double partitionError = 0;
        for (size_t i = 0; i < oneBlock.size(); ++i)
            partitionError = std::max(partitionError, std::abs(double(oneBlock[i] - partitioned[i])));
        const bool fractionalAudioIsLive = std::any_of(oneBlock.begin(), oneBlock.end(),
                                                       [](float sample) { return std::abs(sample) > .01f; });
        check(fractionalRange.end - fractionalRange.start == motion::time(1001, 10000) &&
                  frameCount(fractionalRange.end - fractionalRange.start, time(audioSampleRate)) ==
                      fractionalPeriodSamples &&
                  fractionalAudioIsLive && partitionError < 1e-5,
              "near-24-hour 30000/1001 Work Area keeps fractional audio phase across block partitions");

        auto terminalProject = movieProject(a);
        auto &terminalComp = terminalProject.compositions.front();
        setCompositionTiming(terminalComp, motion::time(30), motion::time(13, 120));
        layer(terminalProject, 3).out = terminalComp.duration;
        setWorkAreaFrames(terminalComp, 3, 4);
        const auto terminalRange = compositionFrameRange(terminalComp, true);
        const Time terminalStart = frameTime(terminalRange.firstFrame, terminalComp.fps);
        const Time terminalDuration =
            std::min(frameTime(terminalRange.frameCount, terminalComp.fps), terminalComp.duration - terminalStart);
        const auto terminalSamples = std::llround(seconds(terminalDuration) * audioSampleRate);
        const Time lastTerminalSample = terminalStart + frameTime(terminalSamples - 1, motion::time(audioSampleRate));
        AudioMix terminalMix(terminalProject, 1, mediaBase, {}, true);
        std::vector<float> terminalAudio(size_t(terminalSamples) * 2);
        terminalMix.read(terminalStart, int(terminalSamples), terminalAudio.data());
        check(terminalRange.firstFrame == 3 && terminalRange.frameCount == 1 && terminalSamples == 400 &&
                  lastTerminalSample < motion::time(13, 120) &&
                  std::all_of(terminalAudio.begin(), terminalAudio.end(), [](float v) { return std::isfinite(v); }),
              "partial terminal Work Area keeps all quantized audio sample times before its effective end");

        l.visible = false;
        AudioMix hidden(p, 1, {});
        hidden.read(time(37, 25), 8000, samples.data());
        check(std::any_of(samples.begin(), samples.end(), [](float v) { return std::abs(v) > .01f; }),
              "video visibility does not mute sound");
        l.audioEnabled = false;
        AudioMix muted(p, 1, {});
        muted.read(time(37, 25), 8000, samples.data());
        check(!muted.hasAudio() &&
                  std::all_of(samples.begin(), samples.end(), [](float v) { return v == 0; }),
              "speaker switch mutes audio independently");
        auto nested = movieProject(a);
        auto child = nested.compositions.front();
        child.id = 4;
        child.layers.front().id = 5;
        nested.compositions.push_back(child);
        auto &parent = nested.compositions.front();
        parent.duration = time(6);
        auto &pre = parent.layers.front();
        pre.kind = LayerKind::Precomp;
        pre.source = 4;
        pre.start = pre.in = time(1, 2);
        pre.out = time(3);
        pre.base("audio.levels", 0) = -6.020599913279624;
        pre.base("audio.levels", 1) = -6.020599913279624;
        nested.nextId = 6;
        AudioMix nestedMix(nested, 1, {});
        std::vector<float> reference(8000 * 2);
        AudioMix direct(movieProject(a), 1, {});
        direct.read(time(49, 50), 8000, reference.data());
        nestedMix.read(time(37, 25), 8000, samples.data());
        double error = 0;
        for (size_t i = 0; i < samples.size(); ++i)
            error = std::max(error, std::abs(double(samples[i] - reference[i] * .5f)));
        check(error < 1e-5, "nested comp source offset and inherited audio gain match independent PCM");
        auto &gain = layer(nested, 3).channel({"audio.levels", 0});
        gain.keys = {{motion::time(0), -6.020599913279624}, {time(4), -6.020599913279624}};
        AudioMix animated(nested, 1, {});
        animated.read(time(37, 25), 8000, reference.data());
        check(reference == samples, "animated constant dB keys match the constant-gain path");
        QTemporaryDir temp;
        auto changed = temp.filePath("changed.mp4");
        check(QFile::copy(path, changed), "copy source for isolated mutation check");
        MovieSource changedMovie(changed, a.sha256);
        auto changedPcm = pcmSource(changed, a.sha256);
        QFile file(changed);
        file.open(QIODevice::Append);
        file.write("x");
        file.close();
        check(rejects([&] { changedMovie.frame(motion::time(0)); }),
              "changed source rejected even with cached frame");
        check(rejects([&] { pcmSource(changed, a.sha256); }),
              "changed audio source cannot bypass cached PCM validation");
        auto sourceCopy = temp.filePath("live-source.mp4");
        check(QFile::copy(path, sourceCopy), "create source snapshot mutation fixture");
        auto snapshotProject = movieProject(a);
        snapshotProject.assets.front().path = sourceCopy;
        ExportRequest snapshotRequest{snapshotProject, 1, 0, 2, temp.filePath("snapshot-png"), {}, false};
        auto snapshotResult = exportFrames(snapshotRequest, {}, [&](auto n) {
            if (n == 1) {
                QFile change(sourceCopy);
                check(change.open(QIODevice::Append), "open live source mutation");
                change.write("changed after snapshot");
            }
        });
        check(snapshotResult.state == "complete" && snapshotResult.completed == 2,
              "export uses pinned media after the original source changes");
        auto exportProject = movieProject(a);
        exportProject.compositions.front().width = 320;
        exportProject.compositions.front().height = 180;
        exportProject.compositions.front().duration = time(2);
        exportProject.compositions.front().fps = time(30);
        layer(exportProject, 3).base("transform.scale", 0) =
            layer(exportProject, 3).base("transform.scale", 1) = .5;
        ExportRequest request{exportProject, 1, 0, 60, temp.filePath("movie"), {}, true};
        auto result = exportMovie(request);
        check(result.state == "complete" && result.completed == 60,
              ("native movie export: " + result.error).toUtf8().constData());
        auto output = inspectMedia(QDir(result.directory).filePath("Movie.mp4"), 8);
        check(output.width == 320 && output.height == 180 && output.hasAudio && output.audioChannels == 2 &&
                  std::abs(seconds(output.duration) - 2) < .001,
              "exported movie has video, stereo audio and correct duration");
        struct PartialCase { Time fps, duration; int samples; const char *name; };
        for (const auto &test : {PartialCase{time(30), time(1, 120), 400, "Partial-Work-Area"},
                                 PartialCase{time(30000, 1001), time(247, 30000), 395, "Fractional-Work-Area"}}) {
            auto partialProject = exportProject;
            auto &partialComp = partialProject.compositions.front();
            setCompositionTiming(partialComp, test.fps, time(13, 120));
            layer(partialProject, 3).start = time(-9, 10); // The last partial frame reads the fixture's 1s tone.
            layer(partialProject, 3).out = partialComp.duration;
            setWorkAreaFrames(partialComp, 3, 4);
            const auto partialRange = compositionFrameRange(partialComp, true);
            auto partialResult = exportMovie({partialProject, 1, partialRange.firstFrame, partialRange.frameCount,
                                              temp.filePath(test.name), {}, true});
            check(partialResult.state == "complete" && partialResult.completed == 1,
                  "partial terminal Work Area exports one movie frame");
            const auto partialPath = QDir(partialResult.directory).filePath("Movie.mp4");
            if (qEnvironmentVariableIsSet("MOTION_MEDIA_EVIDENCE")) {
                const auto out = qEnvironmentVariable("MOTION_MEDIA_EVIDENCE");
                QDir().mkpath(out);
                check(QFile::copy(partialPath, QDir(out).filePath(QString(test.name) + ".mp4")),
                      "partial Work Area movie evidence saved");
            }
            const auto partialInfo = inspectMedia(partialPath, 9);
            auto partialPcm = pcmSource(partialPath, partialInfo.sha256);
            std::vector<float> partialSamples(size_t(test.samples) * 2);
            partialPcm->read(0, test.samples, partialSamples.data());
            check(partialInfo.hasAudio &&
                      std::abs(seconds(partialInfo.duration) - seconds(test.duration)) < 1. / audioSampleRate &&
                      partialPcm->frames() == test.samples,
                  "partial Work Area movie trims video and decoded audio to its effective end");
            check(std::any_of(partialSamples.begin(), partialSamples.end(), [](float v) { return std::abs(v) > .01f; }),
                  "selected movie audio reads the nonzero source offset rather than composition zero");
        }
        check(exportMovie(request).state == "failed", "movie export refuses an existing destination");
        std::stop_source stop;
        stop.request_stop();
        request.newDirectory = temp.filePath("cancelled");
        check(exportMovie(request, stop.get_token()).state == "cancelled" &&
                  !QFileInfo::exists(request.newDirectory),
              "cancelled export leaves no completed movie");
        std::stop_source mid;
        request.newDirectory = temp.filePath("mid-cancel");
        auto cancelledMovie = exportMovie(request, mid.get_token(), [&](auto n) {
            if (n >= 2)
                mid.request_stop();
        });
        check(cancelledMovie.state == "cancelled" &&
                  !QFileInfo::exists(QDir(request.newDirectory).filePath("Movie.mp4")) &&
                  !QFileInfo::exists(QDir(request.newDirectory).filePath("Movie.partial.mp4")),
              "mid-export cancellation removes partial and final movie");
        if (qEnvironmentVariableIsSet("MOTION_MEDIA_EVIDENCE")) {
            auto out = qEnvironmentVariable("MOTION_MEDIA_EVIDENCE");
            QDir().mkpath(out);
            check(QFile::copy(QDir(result.directory).filePath("Movie.mp4"),
                              QDir(out).filePath("Native-export.mp4")),
                  "movie evidence saved");
        }
        if (qEnvironmentVariableIsSet("MOTION_AUDIO_DEVICE_CHECK")) {
            AudioPlayback playback(mix, motion::time(0));
            QThread::msleep(150);
            check(playback.playedSamples() > 0, "native audio device advances the playback clock");
        }
        std::cout << checkCount << " media checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
