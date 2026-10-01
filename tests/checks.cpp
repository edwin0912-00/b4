// SPDX-License-Identifier: MPL-2.0
#include "editor.hpp"
#include "evaluate.hpp"
#include "project_io.hpp"
#include "render.hpp"
#include "render_worker.hpp"
#include "support.hpp"
#include <QColorSpace>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>
#include <limits>
using namespace motion;
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        check(frameTime(30000, time(30000, 1001)) == time(1001), "rational NTSC time");
        check(frameCount(time(1001, 30000), time(30000, 1001)) == 1, "exact rational export count");
        check(frameCount(time(1, 10), time(25)) == 3, "partial last frame uses exact ceiling");
        check(frameTime(359, time(30)) < time(12), "last frame in range");
        check(!(frameTime(360, time(30)) < time(12)), "exclusive end");
        check(rejects([] { time(1, 0); }), "zero denominator");
        check(time(2, -4) == time(-1, 2), "normalization");
        check(rejects([] { time(std::numeric_limits<std::int64_t>::max()) + time(1); }), "overflow");
        auto p = minimalProject();
        validateProject(p);
        p.compositions[0].width = 0;
        check(rejects([&] { validateProject(p); }), "invalid dimensions");
        auto parent = parentScene();
        validateProject(parent);
        layer(parent, 2).parent = 3;
        check(rejects([&] { validateProject(parent); }), "parent cycle");
        Channel ch{0, {{motion::time(0), 0}, {time(1), 100}}};
        check(near(valueAt(ch, time(1, 4)), 25), "linear quarter");
        ch.keys[0].outgoing = Interpolation::Hold;
        check(near(valueAt(ch, time(999, 1000)), 0), "hold before boundary");
        check(near(valueAt(ch, time(1)), 100), "exact key");
        ch.keys[0].outgoing = Interpolation::Cubic;
        ch.keys[0].outHandle = {1.0 / 3, 0};
        ch.keys[1].inHandle = {-1.0 / 3, 0};
        check(near(valueAt(ch, time(1, 4)), 15.625), "independent cubic value");
        const auto world = worldTransform(parentScene(), 1, 3, motion::time(0));
        const auto point = mapPoint(world, {0, 0});
        check(near(point.x, 100) && near(point.y, 60), "parent matrix order");
        auto anchored = parentScene();
        layer(anchored, 3).channel(Property(0)).base = 5;
        auto anchorPoint = mapPoint(worldTransform(anchored, 1, 3, motion::time(0)), {5, 0});
        check(near(anchorPoint.x, 100) && near(anchorPoint.y, 60), "anchor compensation");
        for (Time fps : {time(24), time(25), time(30), time(30000, 1001)}) {
            Layer l;
            l.in = frameTime(2, fps);
            l.out = frameTime(4, fps);
            check(!isVisible(l, frameTime(1, fps)) && isVisible(l, frameTime(2, fps)) &&
                      !isVisible(l, frameTime(4, fps)),
                  "rational layer boundaries");
        }
        auto offset = Layer{};
        offset.start = time(2);
        check(sourceTime(offset, time(3)) == time(1), "source start offset");
        const auto bytes = encodeProject(parentScene());
        check(encodeProject(decodeProject(bytes)) == bytes, "normalized document round trip");
        check(rejects([&] { decodeProject(bytes.left(bytes.size() / 2)); }), "truncated JSON");
        auto unknown = QJsonDocument::fromJson(bytes).object();
        unknown["surprise"] = true;
        check(rejects([&] { decodeProject(QJsonDocument(unknown).toJson()); }), "unknown schema field");
        QTemporaryDir tmp;
        const auto good = tmp.filePath("good.json");
        saveProject(parentScene(), good);
        check(rejects([&] { saveProject(minimalProject(), good + "/child.json"); }), "failed destination");
        check(encodeProject(loadProject(good)) == bytes, "previous saved bytes survive failure");
        const auto recovery = saveRecovery(parentScene(), "test-project", 1, tmp.path());
        check(encodeProject(loadRecovery(recovery)) == bytes, "separate recovery round trip");
        const auto blend = sourceOver({.5f, 0, 0, .5f}, {0, 0, 1, 1});
        check(near(blend.r, .5) && near(blend.b, .5) && near(blend.a, 1), "linear source-over oracle");
        check(near(srgbToLinear(.5f), .21404114048223255), "sRGB decode oracle");
        check(near(linearToSrgb(.5f), .7353569830524495), "sRGB encode oracle");
        Assets assets;
        auto rendered = render(solidScene(), 1, motion::time(0), QSize(2, 2), assets);
        for (const auto &px : rendered.pixels)
            check(near(px.r, .5) && near(px.b, .5) && near(px.a, 1), "independent solid scene");
        auto masked = solidScene();
        layer(masked, 2).setMask(Mask{MaskShape::Rectangle, MaskMode::Add, false, 0, 0, 1, 2});
        auto m = render(masked, 1, motion::time(0), QSize(2, 2), assets);
        check(near(m.pixels[0].r, .5) && near(m.pixels[1].r, 0), "mask expected pixels");
        auto effects = solidScene();
        effects.compositions[0].layers.resize(1);
        auto &effectLayer = layer(effects, 2);
        effectLayer.channel(Property::Opacity).base = 1;
        auto gain = addEffect(effects, 2), bias = addEffect(effects, 2);
        effectLayer.channel({"color.gain", 0, gain}).base = 2;
        effectLayer.channel({"color.bias", 0, bias}).base = .1;
        auto effected = render(effects, 1, motion::time(0), QSize(2, 2), assets);
        check(near(effected.pixels[0].r, 2.1) && near(effected.pixels[0].g, .1) &&
                  near(effected.pixels[0].a, 1),
              "ordered effect gain then bias numerical oracle");
        moveEffect(effects, 2, bias, -1);
        check(near(render(effects, 1, motion::time(0), QSize(2, 2), assets).pixels[0].r, 2.2),
              "effect reorder changes actual pixels");
        for (auto &effect : effectLayer.effects)
            effect.enabled = false;
        check(near(render(effects, 1, motion::time(0), QSize(2, 2), assets).pixels[0].r, 1),
              "effect bypass restores source");
        effectLayer.effects[1].enabled = true;
        effectLayer.channel({"color.gain", 0, gain}).keys = {{motion::time(0), 1}, {time(2), 3}};
        check(near(render(effects, 1, time(1), QSize(2, 2), assets).pixels[0].r, 2),
              "animated effect uses current time instead of cached effect result");
        effectLayer.setColor(QColor(255, 0, 0, 0));
        effectLayer.effects[0].enabled = true;
        const auto transparentEffect = render(effects, 1, time(1), QSize(2, 2), assets).pixels[0];
        check(transparentEffect.r == 0 && transparentEffect.g == 0 && transparentEffect.a == 0,
              "effect bias preserves transparent premultiplied pixels");
        auto overflowing = solidScene();
        for (int i = 0; i < 20; ++i) {
            const auto id = addEffect(overflowing, 2);
            layer(overflowing, 2).channel({"color.gain", 0, id}).base = 1000;
        }
        check(rejects([&] { render(overflowing, 1, motion::time(0), QSize(2, 2), assets); }),
              "effect overflow refuses corrupt pixels");
        auto animatedMask = solidScene();
        auto &maskLayer = layer(animatedMask, 2);
        maskLayer.setMask(Mask{MaskShape::Rectangle, MaskMode::Add, false, 0, 0, 1, 2});
        maskLayer.channel({"mask.position", 0}).keys = {{motion::time(0), 0}, {time(1), 1}};
        const auto movedMask = render(animatedMask, 1, time(1), QSize(2, 2), assets);
        check(near(movedMask.pixels[0].r, 0) && near(movedMask.pixels[1].r, .5),
              "mask geometry animation is evaluated per frame");
        Assets pinned;
        auto animatedColor = solidScene();
        layer(animatedColor, 2).channel({"source.color", 0}).keys = {{motion::time(0), 1}, {time(1), 0}};
        preflightSources(animatedColor, pinned);
        check(near(render(animatedColor, 1, time(1), QSize(2, 2), pinned).pixels[0].r, 0),
              "pinned export sources allow animated derived rasters");
        auto png = pngImage(rendered);
        check(png.pixelColor(0, 0).red() == 188, "linear blend PNG encoding");
        check(png.save(tmp.filePath("frame.png")), "PNG save");
        check(QImage(tmp.filePath("frame.png")).convertToFormat(QImage::Format_RGBA8888) == png,
              "PNG decode match");
        registerFixtureFont(QString(MOTION_SOURCE_DIR) + "/fixtures/font/NotoSans-Regular.ttf");
        auto text = solidScene();
        text.compositions[0].width = 400;
        text.compositions[0].height = 160;
        text.compositions[0].layers.resize(1);
        auto &title = text.compositions[0].layers[0];
        title.kind = LayerKind::Text;
        title.string("text.source") = "Motion\nРух";
        title.base("text.size") = 48;
        title.width = 400;
        title.height = 160;
        auto glyphs = render(text, 1, motion::time(0), QSize(400, 160), assets);
        check(std::count_if(glyphs.pixels.begin(), glyphs.pixels.end(), [](Pixel p) { return p.a > 0; }) >
                  300,
              "Latin and Ukrainian glyph rasterization");
        auto missing = text;
        missing.compositions[0].layers[0].string("text.family") = "Missing-Font-Fixture-9231";
        check(rejects([&] { render(missing, 1, motion::time(0), QSize(400, 160), assets); }),
              "missing font refuses render");
        Editor editor(parentScene());
        auto before = encodeProject(editor.project());
        editor.apply("Move", [](Project &p) { setProperty(p, 3, Property::PositionX, motion::time(0), 20); });
        check(encodeProject(editor.project()) != before, "edit changes document");
        auto revision = editor.revision();
        editor.undoStack().undo();
        check(encodeProject(editor.project()) == before, "undo restores document");
        check(editor.revision() > revision, "undo revision monotonic");
        editor.beginGesture();
        editor.previewGesture(
            [](Project &p) { setProperty(p, 3, Property::PositionX, motion::time(0), 99); });
        editor.cancelGesture();
        check(encodeProject(editor.project()) == before, "cancel drag restores original");
        auto parenting = parentScene();
        auto oldPoint = mapPoint(worldTransform(parenting, 1, 3, motion::time(0)), {0, 0});
        reparent(parenting, 1, 3, std::nullopt, motion::time(0));
        auto newPoint = mapPoint(worldTransform(parenting, 1, 3, motion::time(0)), {0, 0});
        check(near(oldPoint.x, newPoint.x) && near(oldPoint.y, newPoint.y), "unparent preserves appearance");
        auto nested = solidScene();
        auto original = pngImage(render(nested, 1, motion::time(0), QSize(2, 2), assets));
        precompose(nested, 1, {2, 3});
        for (int f = 0; f < 360; ++f)
            check(pngImage(render(nested, 1, frameTime(f, time(30)), QSize(2, 2), assets)) == original,
                  "precompose preserves every test frame");
        check(rejects([&] { precompose(parenting, 1, {3, 999}); }), "foreign precompose selection");
        auto locked = solidScene();
        layer(locked, 2).locked = true;
        check(rejects([&] { setProperty(locked, 2, Property::Opacity, motion::time(0), .2); }),
              "locked property protected");
        PreviewRequest active{12, 7, minimalProject(), 1, time(1), QSize(960, 540), {}};
        PreviewResult late{11, 7, motion::time(0), {}, {}, 0};
        check(!matches(late, active), "late preview request refused");
        late.requestId = 12;
        late.revision = 6;
        check(!matches(late, active), "old document revision refused");
        ExportRequest job{solidScene(), 1, 0, 10, tmp.filePath("complete"), tmp.path()};
        auto output = exportFrames(job);
        check(output.state == "complete" && output.completed == 10, "ten-frame verified export");
        check(exportFrames(job).state == "failed", "destination collision refused");
        std::stop_source stop;
        job.newDirectory = tmp.filePath("cancelled");
        auto partial = exportFrames(job, stop.get_token(), [&](std::int64_t completed) {
            if (completed == 3)
                stop.request_stop();
        });
        check(partial.state == "cancelled" && partial.completed == 3,
              "export cancellation leaves exact completed range");
        job.newDirectory = tmp.filePath("missing-parent/out");
        check(exportFrames(job).state == "failed", "export write failure never complete");
        QImage input(2, 2, QImage::Format_RGBA8888);
        input.fill(Qt::red);
        input.setColorSpace(QColorSpace::DisplayP3);
        check(input.save(tmp.filePath("p3.png")), "create foreign-profile fixture");
        check(rejects([&] { inspectPng(tmp.filePath("p3.png"), 4); }), "foreign PNG profile refused");
        input.setColorSpace(QColorSpace());
        check(input.save(tmp.filePath("untagged.png")), "create untagged fixture");
        auto imageAsset = inspectPng(tmp.filePath("untagged.png"), 4);
        check(imageAsset.assumedSrgb, "untagged source assumption explicit");
        auto missingImage = solidScene();
        missingImage.assets.push_back(imageAsset);
        missingImage.nextId = 5;
        layer(missingImage, 2).kind = LayerKind::Image;
        layer(missingImage, 2).source = 4;
        QFile::remove(imageAsset.path);
        check(encodeProject(decodeProject(encodeProject(missingImage))) == encodeProject(missingImage),
              "missing source reference survives reopen");
        job.project = missingImage;
        job.newDirectory = tmp.filePath("missing-source");
        check(exportFrames(job).state == "failed", "missing source refuses export");
        std::cout << checkCount << " checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
