// SPDX-License-Identifier: MPL-2.0
#include "media.hpp"
#include "render.hpp"
#include "support.hpp"
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <iostream>
using namespace motion;

namespace {
Project project(int width, int height, Time fps, Time duration = motion::time(1)) {
    Project result;
    auto &comp = result.compositions.front();
    comp.width = width;
    comp.height = height;
    comp.fps = fps;
    comp.duration = duration;
    return result;
}

Layer solid(Id id, int width, int height, QColor color, Time out = motion::time(1)) {
    Layer result;
    result.id = id;
    result.width = width;
    result.height = height;
    result.out = out;
    result.setColor(color);
    return result;
}

void exact(const Frame &a, const Frame &b, const char *message) {
    check(a.width == b.width && a.height == b.height && a.pixels.size() == b.pixels.size(), message);
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        const auto &x = a.pixels[i];
        const auto &y = b.pixels[i];
        check(x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a, message);
    }
}

void pixelNear(const Pixel &pixel, double r, double g, double b, double a, const char *message) {
    check(near(pixel.r, r, 1e-5) && near(pixel.g, g, 1e-5) && near(pixel.b, b, 1e-5) &&
              near(pixel.a, a, 1e-5),
          message);
}

Project crossingLayers(bool blur = true) {
    auto result = project(2, 1, motion::time(1));
    auto &comp = result.compositions.front();
    comp.motionBlurEnabled = true;
    comp.shutterAngle = 360;
    comp.shutterPhase = 0;
    comp.motionBlurSamples = 2;
    auto red = solid(2, 1, 1, QColor(Qt::red));
    auto blue = solid(3, 1, 1, QColor(Qt::blue));
    red.motionBlur = blue.motionBlur = blur;
    red.channel(Property::PositionX).keys = {{motion::time(0), 0, Interpolation::Hold},
                                             {motion::time(1, 2), 1, Interpolation::Hold},
                                             {motion::time(1), 1, Interpolation::Hold}};
    blue.channel(Property::PositionX).keys = {{motion::time(0), 1, Interpolation::Hold},
                                              {motion::time(1, 2), 0, Interpolation::Hold},
                                              {motion::time(1), 0, Interpolation::Hold}};
    comp.layers = {red, blue};
    result.nextId = 4;
    return result;
}

Project boundaryScene(Time fps, Time duration, Time layerIn = motion::time(0), bool final = false) {
    auto result = project(4, 1, fps, duration);
    auto &comp = result.compositions.front();
    comp.motionBlurEnabled = true;
    comp.shutterAngle = 180;
    comp.shutterPhase = final ? 360 : -90;
    comp.motionBlurSamples = 16;
    auto background = solid(2, 4, 1, QColor(Qt::blue), duration);
    auto moving = solid(3, 1, 1, QColor(Qt::red), duration);
    background.motionBlur = false;
    moving.motionBlur = true;
    moving.in = layerIn;
    if (final) {
        moving.channel(Property::PositionX).keys = {{motion::time(0), 0, Interpolation::Linear},
                                                    {motion::time(29, 30), 2, Interpolation::Hold}};
    } else {
        const Time next = layerIn + frameTime(1, fps);
        moving.channel(Property::PositionX).keys = {{layerIn, 0, Interpolation::Linear},
                                                    {next, 100, Interpolation::Linear}};
    }
    comp.layers = {moving, background};
    result.nextId = 4;
    return result;
}

Project nestedScene(bool rootEnabled, bool childEnabled) {
    auto result = project(4, 1, motion::time(1));
    auto &root = result.compositions.front();
    root.motionBlurEnabled = rootEnabled;
    root.shutterAngle = 360;
    root.shutterPhase = 0;
    root.motionBlurSamples = 2;
    Composition child;
    child.id = 4;
    child.width = 4;
    child.height = 1;
    child.fps = motion::time(1);
    child.duration = motion::time(1);
    child.motionBlurEnabled = childEnabled;
    child.shutterAngle = 720;
    child.shutterPhase = 0;
    child.motionBlurSamples = 64;
    auto moving = solid(5, 1, 1, QColor(Qt::red));
    moving.motionBlur = true;
    moving.channel(Property::PositionX).keys = {{motion::time(0), 0, Interpolation::Hold},
                                                {motion::time(1, 2), 2, Interpolation::Hold},
                                                {motion::time(1), 2, Interpolation::Hold}};
    child.layers = {moving};
    auto precomp = solid(2, 4, 1, QColor(Qt::transparent));
    precomp.kind = LayerKind::Precomp;
    precomp.source = child.id;
    precomp.motionBlur = false;
    root.layers = {precomp};
    result.compositions.push_back(child);
    result.nextId = 6;
    return result;
}

void testOverlapAndOverrides() {
    Assets assets;
    const auto blur = crossingLayers();
    const auto image = render(blur, 1, motion::time(0), QSize(2, 1), assets);
    pixelNear(image.pixels[0], .5, 0, .5, 1, "temporal samples composite the full scene before averaging");
    pixelNear(image.pixels[1], .5, 0, .5, 1, "moving overlap is sampled in both layer orders");

    auto noSwitches = crossingLayers(false);
    const auto current = render(noSwitches, 1, motion::time(0), QSize(2, 1), assets);
    const auto off = render(noSwitches, 1, motion::time(0), QSize(2, 1), assets, {},
                            {MotionBlurOverride::Off});
    const auto checked = render(noSwitches, 1, motion::time(0), QSize(2, 1), assets, {},
                                {MotionBlurOverride::OnForCheckedLayers});
    exact(current, off, "unchecked layers preserve the direct legacy path");
    exact(current, checked, "On for Checked Layers does nothing when no layer is checked");

    auto masterOff = blur;
    masterOff.compositions.front().motionBlurEnabled = false;
    const auto currentSettings = render(masterOff, 1, motion::time(0), QSize(2, 1), assets);
    const auto finalOverride = render(masterOff, 1, motion::time(0), QSize(2, 1), assets, {},
                                      {MotionBlurOverride::OnForCheckedLayers});
    exact(currentSettings, render(masterOff, 1, motion::time(0), QSize(2, 1), assets, {},
                                  {MotionBlurOverride::Off}),
          "Current Settings respects the composition master");
    pixelNear(finalOverride.pixels[0], .5, 0, .5, 1,
              "On for Checked Layers bypasses the composition master");

    auto zeroAngle = blur;
    zeroAngle.compositions.front().shutterAngle = 0;
    exact(render(zeroAngle, 1, motion::time(0), QSize(2, 1), assets),
          render(zeroAngle, 1, motion::time(0), QSize(2, 1), assets, {}, {MotionBlurOverride::Off}),
          "zero-degree shutter takes the exact direct render path");
    check(rejects([&] {
              (void)render(blur, 1, motion::time(0), QSize(2, 1), assets, {},
                           {static_cast<MotionBlurOverride>(-1)});
          }),
          "invalid motion blur override is rejected");
}

void testBoundaries() {
    Assets assets;
    auto firstFrame = boundaryScene(motion::time(30), motion::time(1));
    const auto first = render(firstFrame, 1, motion::time(0), QSize(4, 1), assets);
    pixelNear(first.pixels[0], .5, 0, .5, 1,
              "negative shutter samples retain the first transform-key pose");
    check(first.pixels[1].a == 1, "unchecked background stays opaque beneath sampled foreground");
    pixelNear(first.pixels[3], 0, 0, 1, 1,
              "unswept static background stays unchanged at frame zero");

    auto layerIn = boundaryScene(motion::time(30), motion::time(1), motion::time(1, 2));
    const auto atLayerIn = render(layerIn, 1, motion::time(1, 2), QSize(4, 1), assets);
    pixelNear(atLayerIn.pixels[0], .5, 0, .5, 1,
              "nominal layer in-point remains visible through the full exposure");

    auto lastFrame = boundaryScene(motion::time(30), motion::time(1), motion::time(0), true);
    const auto last = render(lastFrame, 1, frameTime(29, motion::time(30)), QSize(4, 1), assets);
    pixelNear(last.pixels[2], 1, 0, 0, 1,
              "samples beyond composition out retain the final transform-key pose");
    pixelNear(last.pixels[3], 0, 0, 1, 1,
              "frame-29 static background remains opaque past the composition boundary");
}

void testParentAndPrecomp() {
    Assets assets;
    auto parented = project(4, 1, motion::time(1));
    auto &comp = parented.compositions.front();
    comp.motionBlurEnabled = true;
    comp.shutterAngle = 360;
    comp.shutterPhase = 0;
    comp.motionBlurSamples = 2;
    Layer parent;
    parent.id = 2;
    parent.kind = LayerKind::Null;
    parent.channel(Property::PositionX).keys = {{motion::time(0), 0, Interpolation::Hold},
                                                {motion::time(1, 2), 2, Interpolation::Hold},
                                                {motion::time(1), 2, Interpolation::Hold}};
    auto child = solid(3, 1, 1, QColor(Qt::red));
    child.parent = parent.id;
    child.motionBlur = true;
    comp.layers = {child, parent};
    parented.nextId = 4;
    const auto movedByParent = render(parented, 1, motion::time(0), QSize(4, 1), assets);
    check(near(movedByParent.pixels[0].a, .5) && near(movedByParent.pixels[2].a, .5),
          "checked child samples the complete animated parent chain");

    const auto currentSettings = render(nestedScene(true, false), 1, motion::time(0), QSize(4, 1), assets);
    check(near(currentSettings.pixels[0].r, 1) && currentSettings.pixels[2].a == 0,
          "Current Settings respects the nested composition master");
    const auto nestedMastersOn = render(nestedScene(true, true), 1, motion::time(0), QSize(4, 1), assets);
    pixelNear(nestedMastersOn.pixels[0], .5, 0, 0, .5,
              "Current Settings samples a nested layer when every composition master is enabled");
    pixelNear(nestedMastersOn.pixels[2], .5, 0, 0, .5,
              "nested positive gate uses the root shutter interval");
    const auto nestedOverride = render(nestedScene(false, false), 1, motion::time(0), QSize(4, 1), assets, {},
                                       {MotionBlurOverride::OnForCheckedLayers});
    check(near(nestedOverride.pixels[0].r, .5) && near(nestedOverride.pixels[2].r, .5) &&
              nestedOverride.pixels[1].a == 0,
          "one root exposure interval samples checked nested transforms without nested resampling");
}

void testFractionalRateAndFootage() {
    Assets assets;
    const Time fps = motion::time(30000, 1001);
    auto p = project(4, 1, fps, motion::time(1));
    auto &comp = p.compositions.front();
    comp.motionBlurEnabled = true;
    comp.shutterAngle = 180;
    comp.shutterPhase = -90;
    comp.motionBlurSamples = 16;
    auto moving = solid(2, 1, 1, QColor(Qt::red), motion::time(1));
    moving.motionBlur = true;
    const Time nominal = frameTime(10, fps);
    const std::int64_t offsetNumerator = 2 * 16 * -90 + 180 * 17;
    const Time exactSample = nominal + scaleTime(frameTime(1, fps), offsetNumerator, 720 * 16);
    const Time quantizedSample = fromSeconds(seconds(exactSample));
    check(!(quantizedSample == exactSample), "fractional-rate fixture distinguishes rational samples from microseconds");
    const Time cut = exactSample + scaleTime(quantizedSample - exactSample, 1, 2);
    moving.channel(Property::PositionX).keys = {{nominal - frameTime(1, fps), 0, Interpolation::Hold},
                                                {cut, 2, Interpolation::Hold}};
    comp.layers = {moving};
    p.nextId = 3;
    const auto rational = render(p, 1, nominal, QSize(4, 1), assets);
    check(near(rational.pixels[0].a, .5) && near(rational.pixels[2].a, .5),
          "fractional FPS shutter samples retain exact rational offsets");

    const QString moviePath = QString(MOTION_SOURCE_DIR) + "/fixtures/media/source.mp4";
    const Asset movieAsset = inspectMedia(moviePath, 2);
    MovieSource movie(moviePath, movieAsset.sha256);
    QTemporaryDir temp;
    check(temp.isValid(), "temporary directory for held-footage fixture");
    const QString stillPath = temp.filePath("nominal.png");
    check(movie.frame(motion::time(0)).image.save(stillPath), "save nominal video frame as still reference");

    Project video = project(movieAsset.width, movieAsset.height, movieAsset.fps, movieAsset.duration);
    video.assets = {movieAsset};
    auto &videoComp = video.compositions.front();
    videoComp.motionBlurEnabled = true;
    videoComp.shutterAngle = 180;
    videoComp.shutterPhase = -90;
    videoComp.motionBlurSamples = 16;
    Layer footage;
    footage.id = 3;
    footage.kind = LayerKind::Video;
    footage.source = movieAsset.id;
    footage.width = movieAsset.width;
    footage.height = movieAsset.height;
    footage.out = movieAsset.duration;
    footage.motionBlur = true;
    footage.channel(Property::PositionX).keys = {{motion::time(0), 0, Interpolation::Linear},
                                                  {motion::time(1), 100, Interpolation::Linear}};
    videoComp.layers = {footage};
    video.nextId = 4;

    Project still = video;
    still.assets = {inspectPng(stillPath, movieAsset.id)};
    layer(still, 3).kind = LayerKind::Image;
    Assets videoAssets;
    videoAssets.baseDirectory = QFileInfo(moviePath).absolutePath();
    Assets stillAssets;
    stillAssets.baseDirectory = temp.path();
    exact(render(video, 1, motion::time(0), QSize(movieAsset.width, movieAsset.height), videoAssets),
          render(still, 1, motion::time(0), QSize(movieAsset.width, movieAsset.height), stillAssets),
          "footage pixels stay on the nominal source frame while the layer transform blurs");
}

void testCancellationAndBudget() {
    auto p = crossingLayers();
    std::stop_source stopped;
    stopped.request_stop();
    bool cancelledRender = false;
    try {
        Assets assets;
        (void)render(p, 1, motion::time(0), QSize(2, 1), assets, stopped.get_token());
    } catch (const RenderCancelled &) {
        cancelledRender = true;
    }
    check(cancelledRender, "temporal render observes cancellation before sampling");

    auto large = project(5000, 1700, motion::time(30), motion::time(1));
    auto &root = large.compositions.front();
    root.motionBlurEnabled = true;
    root.shutterAngle = 180;
    root.shutterPhase = -90;
    root.motionBlurSamples = 16;
    Composition child;
    child.id = 4;
    child.width = 8192;
    child.height = 2048;
    child.fps = motion::time(30);
    child.duration = motion::time(1);
    auto precomp = solid(2, 1, 1, QColor(Qt::transparent));
    precomp.kind = LayerKind::Precomp;
    precomp.source = child.id;
    precomp.motionBlur = true;
    root.layers = {precomp};
    large.compositions.push_back(child);
    large.nextId = 5;
    check(rejects([&] {
              Assets assets;
              (void)render(large, 1, motion::time(0), QSize(5000, 1700), assets);
          }),
          "accumulator, sample and nested frames stay inside the 512 MiB working allowance");
}

void testPrecompMaskBudget() {
    auto rejectsWorkingLimit = [](const Project &p, QSize outputSize) {
        try {
            Assets assets;
            (void)render(p, 1, motion::time(0), outputSize, assets, {},
                         {MotionBlurOverride::Off});
        } catch (const std::runtime_error &error) {
            return QString::fromUtf8(error.what()).contains("512 MiB");
        }
        return false;
    };

    auto large = project(1, 1, motion::time(30), motion::time(1));
    auto &root = large.compositions.front();
    Composition child;
    child.id = 4;
    child.width = 8192;
    child.height = 2048;
    child.fps = motion::time(30);
    child.duration = motion::time(1);
    auto precomp = solid(2, child.width, child.height, QColor(Qt::transparent));
    precomp.kind = LayerKind::Precomp;
    precomp.source = child.id;
    precomp.setMask(Mask{MaskShape::Rectangle, MaskMode::Add, false, 0, 0, 1, 1});
    root.layers = {precomp};
    large.compositions.push_back(child);
    large.nextId = 5;

    auto rasterProject = project(4096, 2048, motion::time(30), motion::time(1));
    auto raster = solid(2, 6300, 2000, QColor(Qt::blue));
    raster.setMask(Mask{MaskShape::Rectangle, MaskMode::Add, false, 0, 0, 1, 1});
    rasterProject.compositions.front().layers = {raster};
    rasterProject.nextId = 3;

    // Nested input is 256 MiB; raster input is 192.3 MiB beside a 128 MiB output.
    const bool nestedRejected = rejectsWorkingLimit(large, QSize(1, 1));
    const bool rasterRejected = rejectsWorkingLimit(rasterProject, QSize(4096, 2048));
    check(nestedRejected,
          "precomp mask copy is rejected before its second nested frame exceeds the working limit");
    check(rasterRejected,
          "direct raster mask copy is rejected before its second source frame exceeds the working limit");
}

void testDirectEffectBudgets() {
    auto rejectsWorkingLimit = [](const Project &p) {
        try {
            Assets assets;
            const auto &comp = composition(p, 1);
            (void)render(p, 1, motion::time(0), QSize(comp.width, comp.height), assets, {},
                         {MotionBlurOverride::Off});
        } catch (const std::runtime_error &error) {
            return QString::fromUtf8(error.what()).contains("512 MiB");
        }
        return false;
    };

    auto precompProject = project(1, 1, motion::time(30), motion::time(1));
    Composition child;
    child.id = 4;
    child.width = 8192;
    child.height = 2048;
    child.fps = motion::time(30);
    child.duration = motion::time(1);
    auto precomp = solid(2, child.width, child.height, QColor(Qt::transparent));
    precomp.kind = LayerKind::Precomp;
    precomp.source = child.id;
    precompProject.compositions.front().layers = {precomp};
    precompProject.compositions.push_back(child);
    precompProject.nextId = 5;
    Effect precompEffect;
    precompEffect.id = precompProject.nextId++;
    layer(precompProject, 2).effects.push_back(precompEffect);

    auto rasterProject = project(1, 1, motion::time(30), motion::time(1));
    auto raster = solid(2, 8192, 2048, QColor(Qt::red));
    rasterProject.compositions.front().layers = {raster};
    rasterProject.nextId = 3;
    Effect rasterEffect;
    rasterEffect.id = rasterProject.nextId++;
    layer(rasterProject, 2).effects.push_back(rasterEffect);

    auto gaussianProject = project(4096, 2048, motion::time(30), motion::time(1));
    auto gaussianLayer = solid(2, 2900, 3000, QColor(Qt::red));
    gaussianProject.compositions.front().layers = {gaussianLayer};
    gaussianProject.nextId = 3;
    Effect gaussianEffect;
    gaussianEffect.id = gaussianProject.nextId++;
    gaussianEffect.type = "motion.gaussian-blur";
    gaussianEffect.name = "Gaussian Blur";
    gaussianEffect.parameters = defaultParameters(effectParameterSpecs(gaussianEffect.type));
    layer(gaussianProject, 2).effects.push_back(gaussianEffect);
    layer(gaussianProject, 2).channel({"gaussian-blur.sigma", 0, gaussianEffect.id}).base = 1;

    const bool precompRejected = rejectsWorkingLimit(precompProject);
    const bool rasterRejected = rejectsWorkingLimit(rasterProject);
    const bool gaussianRejected = rejectsWorkingLimit(gaussianProject);
    check(precompRejected, "direct precomp effect copy is rejected before exceeding the working limit");
    check(rasterRejected, "direct raster effect copy is rejected before exceeding the working limit");
    check(gaussianRejected, "Gaussian helper scratch is included before allocating its full-frame copy");
}
} // namespace

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        testOverlapAndOverrides();
        testBoundaries();
        testParentAndPrecomp();
        testFractionalRateAndFootage();
        testCancellationAndBudget();
        testDirectEffectBudgets();
        testPrecompMaskBudget();
        std::cout << checkCount << " motion blur assertions passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
