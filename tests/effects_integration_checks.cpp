// SPDX-License-Identifier: MPL-2.0
#include "display_transform.hpp"
#include "editor.hpp"
#include "project_io.hpp"
#include "render_worker.hpp"
#include "support.hpp"
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>
#include <vector>

using namespace motion;

namespace {
Layer solid(Id id, QColor color, int width = 1, int height = 1) {
    Layer result;
    result.id = id;
    result.width = width;
    result.height = height;
    result.in = motion::time(0);
    result.out = motion::time(1);
    result.setColor(color);
    return result;
}

Project scene(int width, int height, Time fps, Time duration, std::vector<Layer> layers) {
    Project result;
    auto &comp = result.compositions.front();
    comp.width = width;
    comp.height = height;
    comp.fps = fps;
    comp.duration = duration;
    comp.layers = std::move(layers);
    for (const auto &item : comp.layers)
        result.nextId = std::max(result.nextId, item.id + 1);
    return result;
}

void blur(Composition &comp, int angle, int phase, int samples) {
    comp.motionBlurEnabled = true;
    comp.shutterAngle = angle;
    comp.shutterPhase = phase;
    comp.motionBlurSamples = samples;
}

bool sameFrame(const Frame &a, const Frame &b) {
    if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size())
        return false;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        const auto &x = a.pixels[i];
        const auto &y = b.pixels[i];
        if (x.r != y.r || x.g != y.g || x.b != y.b || x.a != y.a)
            return false;
    }
    return true;
}

Pixel pixel(const Frame &frame, int x, int y = 0) { return frame.pixels[size_t(y) * frame.width + x]; }

double decodeSrgb(int byte) {
    const double value = byte / 255.0;
    return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
}

int encodeSrgb(double linear) {
    const double value = linear <= .0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - .055;
    return int(std::lround(std::clamp(value, 0.0, 1.0) * 255));
}

int runBenchmark(int width, int height) {
    auto moving = solid(2, QColor(Qt::white), width, height);
    moving.out = motion::time(2);
    moving.motionBlur = true;
    const double travel = std::min(32.0, std::max(.5, width * .1));
    moving.channel(Property::PositionX).keys = {{motion::time(0), 0}, {motion::time(2), travel}};
    auto p = scene(width, height, motion::time(30), motion::time(2), {moving});
    auto &comp = composition(p, 1);
    blur(comp, 360, 0, 1);
    Assets assets;
    const RenderOptions options{};
    constexpr int warmups = 2;
    constexpr int repetitions = 5;

    for (int samples : {1, 4, 16}) {
        comp.motionBlurSamples = samples;
        if (!comp.motionBlurEnabled || !layer(p, 2).motionBlur)
            throw std::runtime_error("Benchmark blur gates are not enabled");
        for (int i = 0; i < warmups; ++i)
            (void)render(p, 1, motion::time(1), QSize(width, height), assets, {}, options);
        std::vector<double> milliseconds;
        for (int i = 0; i < repetitions; ++i) {
            QElapsedTimer timer;
            timer.start();
            const auto frame = render(p, 1, motion::time(1), QSize(width, height), assets, {}, options);
            milliseconds.push_back(timer.nsecsElapsed() / 1e6);
            if (frame.pixels.empty() ||
                std::none_of(frame.pixels.begin(), frame.pixels.end(), [](Pixel p) { return p.a > 0; }))
                throw std::runtime_error("Benchmark shutter rendered no visible coverage");
        }
        std::sort(milliseconds.begin(), milliseconds.end());
        std::cout << "benchmark scene=" << width << 'x' << height << " samples=" << samples
                  << " warmups=" << warmups << " repetitions=" << repetitions
                  << " median_ms=" << milliseconds[repetitions / 2] << '\n';
    }
    return 0;
}

int benchmarkDimension(const QStringList &arguments, const QString &name, int fallback) {
    const QString prefix = "--" + name + "=";
    for (const auto &argument : arguments)
        if (argument.startsWith(prefix)) {
            bool ok = false;
            const int value = argument.mid(prefix.size()).toInt(&ok);
            if (!ok || value < 1 || value > 8192)
                throw std::runtime_error((name + " must be in 1..8192").toStdString());
            return value;
        }
    return fallback;
}

void effectOrderAndPersistence() {
    auto p = scene(1, 1, motion::time(1), motion::time(1), {solid(2, QColor(Qt::black))});
    const Id color = addEffect(p, 2, "motion.linear-color");
    const Id exposure = addEffect(p, 2, "motion.exposure");
    layer(p, 2).channel({"color.bias", 0, color}).base = .25;
    layer(p, 2).channel({"exposure.stops", 0, exposure}).base = 1;

    Assets assets;
    auto ordered = render(p, 1, motion::time(0), QSize(1, 1), assets);
    check(near(pixel(ordered, 0).r, .5) && near(pixel(ordered, 0).g, .5) &&
              near(pixel(ordered, 0).b, .5) && pixel(ordered, 0).a == 1,
          "render applies stored effect order: black + .25 bias, then +1 stop gives .5 RGB");
    moveEffect(p, 2, exposure, -1);
    const auto reversed = render(p, 1, motion::time(0), QSize(1, 1), assets);
    check(near(pixel(reversed, 0).r, .25) && near(pixel(reversed, 0).g, .25) &&
              near(pixel(reversed, 0).b, .25),
          "reordering the same effects changes output to .25 RGB");

    auto persisted = scene(2, 1, motion::time(1), motion::time(1), {solid(2, QColor(128, 32, 0))});
    auto &comp = composition(persisted, 1);
    blur(comp, 270, -45, 4);
    auto &animated = layer(persisted, 2);
    animated.motionBlur = true;
    animated.channel(Property::PositionX).keys = {{motion::time(0), 0}, {motion::time(1), 1}};
    const Id split = addEffect(persisted, 2, "motion.rgb-split");
    const Id animatedExposure = addEffect(persisted, 2, "motion.exposure");
    const Id gaussian = addEffect(persisted, 2, "motion.gaussian-blur");
    animated.channel({"rgb-split.red-offset", 0, split}).base = .25;
    animated.channel({"rgb-split.amount", 0, split}).base = .75;
    animated.channel({"exposure.stops", 0, animatedExposure}).keys = {{motion::time(0), 1}, {motion::time(1), 2}};
    animated.channel({"gaussian-blur.sigma", 0, gaussian}).base = .75;
    animated.channel({"gaussian-blur.amount", 0, gaussian}).base = .6;

    const auto originalBytes = encodeProject(persisted);
    QTemporaryDir temp;
    check(temp.isValid(), "private project round-trip directory is available");
    const auto path = temp.filePath("effects-motion.json");
    saveProject(persisted, path);
    const auto reopened = loadProject(path);
    check(reopened.schemaVersion == 5 && encodeProject(reopened) == originalBytes,
          "schema5 effect order, animation, layer gate and shutter settings save and reopen exactly");
    Assets beforeAssets, afterAssets;
    const auto before = render(persisted, 1, motion::time(0), QSize(2, 1), beforeAssets);
    const auto after = render(reopened, 1, motion::time(0), QSize(2, 1), afterAssets);
    check(sameFrame(before, after), "saved and reopened effect-animation render is pixel-identical");
}

void blurGatesAndOverlap() {
    auto moving = solid(2, QColor(Qt::red));
    moving.motionBlur = true;
    moving.channel(Property::PositionX).keys = {{motion::time(0), 0}, {motion::time(1), 1}};
    auto p = scene(2, 1, motion::time(1), motion::time(1), {moving});
    blur(composition(p, 1), 360, 0, 2); // exact subframe samples at 1/4 and 3/4 frame
    composition(p, 1).motionBlurEnabled = false;

    Assets assets;
    const auto defaultPath = render(p, 1, motion::time(0), QSize(2, 1), assets);
    const auto off = render(p, 1, motion::time(0), QSize(2, 1), assets, {}, {MotionBlurOverride::Off});
    check(sameFrame(defaultPath, off),
          "disabled composition master and explicit Off preserve the exact legacy frame");
    check(pixel(off, 0).a == 1 && pixel(off, 1).a == 0,
          "legacy frame has the moving red pixel at its nominal x=0 transform");

    const auto forced = render(p, 1, motion::time(0), QSize(2, 1), assets, {},
                               {MotionBlurOverride::OnForCheckedLayers});
    check(near(pixel(forced, 0).r, .5) && near(pixel(forced, 0).a, .5) &&
              near(pixel(forced, 1).r, .5) && near(pixel(forced, 1).a, .5),
          "On for Checked Layers bypasses the composition master and averages the two analytic positions");

    composition(p, 1).motionBlurEnabled = true;
    const auto enabledSettings = render(p, 1, motion::time(0), QSize(2, 1), assets);
    check(sameFrame(enabledSettings, forced), "Current Settings enables the checked layer when its comp gate is on");
    layer(p, 2).motionBlur = false;
    const auto uncheckedSettings = render(p, 1, motion::time(0), QSize(2, 1), assets);
    const auto forcedUnchecked = render(p, 1, motion::time(0), QSize(2, 1), assets, {},
                                        {MotionBlurOverride::OnForCheckedLayers});
    check(sameFrame(uncheckedSettings, off) && sameFrame(forcedUnchecked, off),
          "the global On override still honors each layer switch");

    auto foreground = solid(2, QColor(Qt::blue));
    foreground.motionBlur = true;
    foreground.channel(Property::PositionX).keys = {
        {motion::time(1, 4), 0, Interpolation::Hold}, {motion::time(3, 4), 1}};
    auto background = solid(3, QColor(Qt::red));
    auto overlap = scene(2, 1, motion::time(1), motion::time(1), {foreground, background});
    blur(composition(overlap, 1), 360, 0, 2);
    const auto integrated = render(overlap, 1, motion::time(0), QSize(2, 1), assets);
    check(near(pixel(integrated, 0).r, .5) && near(pixel(integrated, 0).g, 0) &&
              near(pixel(integrated, 0).b, .5) && pixel(integrated, 0).a == 1,
          "whole-scene shutter integration averages blue-over-red at 1/4 frame and red alone at 3/4");
    check(near(pixel(integrated, 1).b, .5) && near(pixel(integrated, 1).a, .5),
          "the moving foreground contributes only its second-sample half to x=1");
}

void shutterBoundaries() {
    auto atStart = solid(2, QColor(Qt::green));
    atStart.motionBlur = true;
    atStart.channel(Property::PositionX).keys = {{motion::time(0), 1}, {motion::time(1), 1}};
    auto beginning = scene(3, 1, motion::time(30), motion::time(1), {atStart});
    blur(composition(beginning, 1), 180, -90, 16);
    Assets assets;
    const auto startBlur = render(beginning, 1, motion::time(0), QSize(3, 1), assets);
    const auto startOff = render(beginning, 1, motion::time(0), QSize(3, 1), assets, {},
                                 {MotionBlurOverride::Off});
    check(sameFrame(startBlur, startOff) && pixel(startBlur, 1).a == 1,
          "frame 0 keeps all 16 samples, including negative shutter times, at the held first-key pose");

    auto atIn = solid(2, QColor(Qt::green));
    atIn.in = frameTime(15, motion::time(30));
    atIn.out = motion::time(2);
    atIn.motionBlur = true;
    atIn.channel(Property::PositionX).base = 1;
    auto inPoint = scene(3, 1, motion::time(30), motion::time(2), {atIn});
    blur(composition(inPoint, 1), 180, -90, 16);
    const auto inBlur = render(inPoint, 1, frameTime(15, motion::time(30)), QSize(3, 1), assets);
    const auto inOff = render(inPoint, 1, frameTime(15, motion::time(30)), QSize(3, 1), assets, {},
                              {MotionBlurOverride::Off});
    check(sameFrame(inBlur, inOff) && pixel(inBlur, 1).a == 1,
          "a nominally visible layer remains present for samples before its in-point");

    auto atEnd = solid(2, QColor(Qt::green));
    atEnd.motionBlur = true;
    atEnd.channel(Property::PositionX).keys = {{motion::time(0), 0}, {frameTime(29, motion::time(30)), 1}};
    auto ending = scene(3, 1, motion::time(30), motion::time(1), {atEnd});
    blur(composition(ending, 1), 180, 360, 16);
    const auto requested = frameTime(29, motion::time(30));
    const auto endBlur = render(ending, 1, requested, QSize(3, 1), assets);
    const auto endOff = render(ending, 1, requested, QSize(3, 1), assets, {},
                               {MotionBlurOverride::Off});
    check(sameFrame(endBlur, endOff) && pixel(endBlur, 1).a == 1,
          "frame 29 keeps samples beyond the 1-second comp/layer out-point at the final key pose");
}

void exportedPixelsIgnoreViewerState() {
    auto moving = solid(2, QColor(128, 0, 0));
    moving.motionBlur = true;
    moving.channel(Property::PositionX).keys = {{motion::time(0), 0}, {motion::time(1), 1}};
    auto p = scene(2, 1, motion::time(1), motion::time(1), {moving});
    blur(composition(p, 1), 360, 0, 2);
    composition(p, 1).motionBlurEnabled = false;
    const Id exposure = addEffect(p, 2, "motion.exposure");
    layer(p, 2).channel({"exposure.stops", 0, exposure}).base = 1;

    const RenderOptions options{MotionBlurOverride::OnForCheckedLayers};
    Assets previewAssets;
    const auto rendered = render(p, 1, motion::time(0), QSize(2, 1), previewAssets, {}, options);
    const auto canonicalPng = pngImage(rendered);
    const auto channelView = displayImage(rendered, {DisplayChannel::Green, false, 0});
    const auto exposedView = displayImage(rendered, {DisplayChannel::RGB, true, 2});
    check(channelView != canonicalPng && exposedView != canonicalPng,
          "viewer channel and exposure diagnostics differ from canonical render pixels");

    QTemporaryDir temp;
    check(temp.isValid(), "private PNG export directory is available");
    ExportRequest request;
    request.project = p;
    request.comp = 1;
    request.firstFrame = 0;
    request.frameCount = 1;
    request.newDirectory = temp.filePath("sequence");
    request.options = options;
    const auto result = exportFrames(request);
    check(result.state == "complete" && result.completed == 1,
          "one-frame PNG export completes with the explicit motion-blur override");
    QImage decoded(QDir(request.newDirectory).filePath("frame_000000.png"));
    decoded = decoded.convertToFormat(QImage::Format_RGBA8888);
    check(!decoded.isNull() && decoded.size() == QSize(2, 1), "exported PNG reopens at the composition size");
    const int expectedRed = encodeSrgb(decodeSrgb(128) * 2);
    for (int x = 0; x < 2; ++x) {
        const auto *rgba = decoded.constScanLine(0) + x * 4;
        check(rgba[0] == expectedRed && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 128,
              "exported PNG contains the analytically exposed red pixel with half shutter coverage");
    }
    check(decoded == canonicalPng,
          "export PNG pixels match the effect-and-blur render regardless of viewer channel/exposure state");
}
} // namespace

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (app.arguments().contains("--benchmark")) {
        try {
            const int width = benchmarkDimension(app.arguments(), "benchmark-width", 1280);
            const int height = benchmarkDimension(app.arguments(), "benchmark-height", 720);
            if (std::int64_t(width) * height > 16777216)
                throw std::runtime_error("Benchmark dimensions exceed 16,777,216 pixels");
            return runBenchmark(width, height);
        } catch (const std::exception &error) {
            std::cerr << "benchmark failed: " << error.what() << '\n';
            return 1;
        }
    }
    try {
        effectOrderAndPersistence();
        blurGatesAndOverlap();
        shutterBoundaries();
        exportedPixelsIgnoreViewerState();
        std::cout << checkCount << " effect/render/export integration checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "effect/render/export integration checks failed: " << error.what() << '\n';
        return 1;
    }
}
