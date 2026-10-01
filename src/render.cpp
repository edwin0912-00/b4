// SPDX-License-Identifier: MPL-2.0
#include "render.hpp"
#include "effects.hpp"
#include "media.hpp"
#include <QBuffer>
#include <QByteArrayView>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QImageReader>
#include <QPainter>
#include <QRawFont>
#include <QTextLayout>
#include <algorithm>
#include <cmath>
#include <set>
namespace motion {
float srgbToLinear(float v) { return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f); }
float linearToSrgb(float v) { return v <= .0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - .055f; }
Pixel sourceOver(Pixel s, Pixel d) {
    float r = 1 - s.a;
    return {s.r + d.r * r, s.g + d.g * r, s.b + d.b * r, s.a + d.a * r};
}
namespace {
constexpr size_t maxWorking = 512u * 1024u * 1024u;
void cancelled(std::stop_token token) {
    if (token.stop_requested())
        throw RenderCancelled{};
}
Frame makeFrame(int w, int h) {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || std::int64_t(w) * h > 16777216)
        throw std::runtime_error("Render dimensions exceed proof limits");
    return {w, h, std::vector<Pixel>(size_t(w) * h)};
}
QByteArray assetBytes(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error(("Missing source: " + path).toStdString());
    if (f.size() > 128 * 1024 * 1024)
        throw std::runtime_error("PNG input exceeds 128 MiB");
    return f.readAll();
}
QImage decodePng(const QByteArray &bytes, bool &assumed) {
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader::setAllocationLimit(256);
    QImageReader reader(&buffer, "PNG");
    auto size = reader.size();
    if (size.width() <= 0 || size.height() <= 0 || size.width() > 8192 || size.height() > 8192 ||
        std::int64_t(size.width()) * size.height() > 16777216)
        throw std::runtime_error("PNG dimensions exceed supported limits");
    auto image = reader.read();
    if (image.isNull())
        throw std::runtime_error(("Invalid PNG: " + reader.errorString()).toStdString());
    assumed = !image.colorSpace().isValid();
    if (!assumed && image.colorSpace() != QColorSpace(QColorSpace::SRgb))
        throw std::runtime_error("Only sRGB PNG profiles are supported in this proof");
    return image.convertToFormat(QImage::Format_RGBA8888);
}
Frame linearImage(const QImage &image, std::stop_token stop) {
    Frame frame = makeFrame(image.width(), image.height());
    for (int y = 0; y < frame.height; ++y) {
        cancelled(stop);
        const auto *bytes = image.constScanLine(y);
        for (int x = 0; x < frame.width; ++x) {
            float a = bytes[x * 4 + 3] / 255.f;
            frame.pixels[size_t(y) * frame.width + x] = {srgbToLinear(bytes[x * 4] / 255.f) * a,
                                                         srgbToLinear(bytes[x * 4 + 1] / 255.f) * a,
                                                         srgbToLinear(bytes[x * 4 + 2] / 255.f) * a, a};
        }
    }
    return frame;
}
double maskCoverage(const Mask &m, double x, double y) {
    bool inside = false;
    if (m.width > 0 && m.height > 0) {
        double nx = (x - m.x) / m.width, ny = (y - m.y) / m.height;
        inside = m.shape == MaskShape::Rectangle ? nx >= 0 && nx < 1 && ny >= 0 && ny < 1
                                                 : (nx - .5) * (nx - .5) + (ny - .5) * (ny - .5) <= .25;
    }
    if (m.inverted)
        inside = !inside;
    if (m.mode == MaskMode::Subtract)
        inside = !inside;
    return inside ? 1 : 0;
}
void applyMask(Frame &f, const Mask &m, std::stop_token stop) {
    for (int y = 0; y < f.height; ++y) {
        cancelled(stop);
        for (int x = 0; x < f.width; ++x) {
            float coverage = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                    coverage += float(maskCoverage(m, x + (sx + .5) / 4, y + (sy + .5) / 4)) / 16;
            auto &p = f.pixels[size_t(y) * f.width + x];
            p = {p.r * coverage, p.g * coverage, p.b * coverage, p.a * coverage};
        }
    }
}
struct RasterSettings {
    QColor color;
    QString text, family, style;
    double size, leading;
    int alignment;
    std::optional<Mask> mask;
};
RasterSettings rasterSettings(const Layer &l, Time t) {
    auto v = [&](const char *id, int component = 0) { return valueAt(l, {id, component}, t); };
    RasterSettings r{QColor::fromRgbF(v("source.color", 0), v("source.color", 1), v("source.color", 2),
                                      v("source.color", 3)),
                     l.string("text.source"),
                     l.string("text.family"),
                     l.string("text.style"),
                     v("text.size"),
                     v("text.leading"),
                     int(v("text.alignment")),
                     {}};
    if (v("mask.enabled"))
        r.mask = Mask{MaskShape(int(v("mask.shape"))),
                      MaskMode(int(v("mask.mode"))),
                      bool(v("mask.inverted")),
                      v("mask.position", 0),
                      v("mask.position", 1),
                      v("mask.size", 0),
                      v("mask.size", 1)};
    return r;
}
QString sourceKey(const Project &p, const Layer &l, const RasterSettings &r, Time at = time(0)) {
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s << int(l.kind) << l.width << l.height;
    if (l.kind == LayerKind::Image || l.kind == LayerKind::Video) {
        const auto &a = asset(p, l.source);
        s << a.sha256 << a.path;
        if (l.kind == LayerKind::Video)
            s << qint64(at.numerator) << qint64(at.denominator);
    } else {
        s << r.color;
        if (l.kind == LayerKind::Text)
            s << r.text << r.family << r.style << r.size << r.leading << r.alignment;
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}
std::shared_ptr<const Frame> cacheRaster(const QString &key, Frame frame, Assets &assets, bool required) {
    const size_t bytes = frame.pixels.size() * sizeof(Pixel);
    const bool fits =
        assets.pinnedBytes <= assets.sourceBudget && bytes <= assets.sourceBudget - assets.pinnedBytes;
    if (!fits && required)
        throw std::runtime_error("Raster source exceeds the source-cache memory allowance");
    auto shared = std::make_shared<const Frame>(std::move(frame));
    if (!fits)
        return shared;
    // ponytail: flush derived rasters at the budget; use LRU if measured churn warrants it.
    if (assets.sourceBytes + bytes > assets.sourceBudget - assets.pinnedBytes) {
        assets.sources.clear();
        assets.sourceBytes = 0;
    }
    assets.sources[key] = shared;
    assets.sourceBytes += bytes;
    return shared;
}
std::shared_ptr<const Frame> source(const Project &p, const Layer &l, Assets &assets, std::stop_token stop,
                                    const RasterSettings &r, Time at = time(0),
                                    QString *resolvedKey = nullptr) {
    QImage video;
    if (l.kind == LayerKind::Video) {
        const auto &a = asset(p, l.source);
        const auto path = QDir(assets.baseDirectory).absoluteFilePath(a.path);
        const auto id = a.sha256 + ":" + path;
        // ponytail: four retained decoders bound native frame/index memory; revisit for large multicam comps.
        if (!assets.movies.contains(id) && assets.movies.size() >= 4)
            assets.movies.erase(assets.movies.begin());
        auto &reader = assets.movies[id];
        if (!reader)
            reader = std::make_shared<MovieSource>(path, a.sha256, stop);
        auto frame = reader->frame(at, stop);
        if (frame.image.isNull())
            return {};
        video = std::move(frame.image);
        at = frame.at;
    }
    const auto key = sourceKey(p, l, r, at);
    if (resolvedKey)
        *resolvedKey = key;
    if (auto it = assets.pinnedSources.find(key); it != assets.pinnedSources.end())
        return it->second;
    if (auto it = assets.sources.find(key); it != assets.sources.end())
        return it->second;
    if (assets.pinned && l.kind == LayerKind::Image)
        throw std::runtime_error("Export source was not pinned during preflight");
    Frame frame;
    if (l.kind == LayerKind::Solid) {
        frame = makeFrame(l.width, l.height);
        float a = r.color.alphaF();
        Pixel color{srgbToLinear(r.color.redF()) * a, srgbToLinear(r.color.greenF()) * a,
                    srgbToLinear(r.color.blueF()) * a, a};
        std::fill(frame.pixels.begin(), frame.pixels.end(), color);
    } else if (l.kind == LayerKind::Image) {
        const auto &a = asset(p, l.source);
        auto bytes = assetBytes(QDir(assets.baseDirectory).absoluteFilePath(a.path));
        if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != a.sha256.toLatin1())
            throw std::runtime_error("Source image changed on disk; reimport it explicitly");
        bool assumed;
        auto image = decodePng(bytes, assumed);
        if (image.width() != a.width || image.height() != a.height)
            throw std::runtime_error("Source dimensions changed");
        if (assumed && !assets.warnings.contains(a.path))
            assets.warnings.append(a.path);
        frame = linearImage(image, stop);
    } else if (l.kind == LayerKind::Video) {
        if (video.width() != l.width || video.height() != l.height)
            throw std::runtime_error("Video dimensions differ from imported metadata");
        frame = linearImage(video, stop);
    } else if (l.kind == LayerKind::Text) {
        if (!QFontDatabase::families().contains(r.family) ||
            !QFontDatabase::styles(r.family).contains(r.style))
            throw std::runtime_error(("Missing font: " + r.family + " / " + r.style).toStdString());
        QFont font = QFontDatabase::font(r.family, r.style, 12);
        font.setPixelSize(std::max(1, int(std::lround(r.size))));
        if (QFontInfo(font).family() != r.family)
            throw std::runtime_error("Font substitution refused");
        QRawFont raw = QRawFont::fromFont(font);
        for (char32_t ch : r.text.toUcs4())
            if (ch != '\n' && ch != '\r' && ch != '\t' && !raw.supportsCharacter(ch))
                throw std::runtime_error("Font lacks a required text glyph");
        QImage image(l.width, l.height, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            throw std::runtime_error("Cannot allocate text image");
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setPen(r.color);
        painter.setFont(font);
        painter.setRenderHint(QPainter::TextAntialiasing);
        double y = 0;
        for (const auto &paragraph : r.text.split('\n')) {
            QTextLayout layout(paragraph, font);
            layout.beginLayout();
            while (true) {
                auto line = layout.createLine();
                if (!line.isValid())
                    break;
                line.setLineWidth(l.width);
                double x = 0;
                if (r.alignment == 1)
                    x = (l.width - line.naturalTextWidth()) / 2;
                if (r.alignment == 2)
                    x = l.width - line.naturalTextWidth();
                line.setPosition(QPointF(x, y));
                y += line.height() * r.leading;
            }
            layout.endLayout();
            layout.draw(&painter, QPointF(0, 0));
            if (paragraph.isEmpty())
                y += QFontMetricsF(font).height() * r.leading;
        }
        painter.end();
        frame = linearImage(image.convertToFormat(QImage::Format_RGBA8888), stop);
    } else
        throw std::runtime_error("Unsupported raster source type");
    return cacheRaster(key, std::move(frame), assets, true);
}
Pixel sample(const Frame &f, double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y) || x < -.5 || y < -.5 || x > f.width + .5 || y > f.height + .5)
        return {};
    x -= .5;
    y -= .5;
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float dx = float(x - ix), dy = float(y - iy);
    Pixel out;
    for (int sy = 0; sy < 2; ++sy)
        for (int sx = 0; sx < 2; ++sx) {
            int px = ix + sx, py = iy + sy;
            if (px < 0 || py < 0 || px >= f.width || py >= f.height)
                continue;
            const auto &p = f.pixels[size_t(py) * f.width + px];
            float w = (sx ? dx : 1 - dx) * (sy ? dy : 1 - dy);
            out.r += p.r * w;
            out.g += p.g * w;
            out.b += p.b * w;
            out.a += p.a * w;
        }
    return out;
}

struct RenderPass {
    RenderOptions options;
    bool cacheAppearances = false;
    size_t cachedAppearanceBytes = 0;
    std::map<QString, std::shared_ptr<const Frame>> appearances;
    struct EffectFrame {
        QByteArray inputHash;
        std::shared_ptr<const Frame> output;
    };
    std::map<QString, EffectFrame> precompEffects;

    explicit RenderPass(RenderOptions renderOptions) : options(renderOptions) {}
};

size_t frameBytes(const Frame &frame) { return frame.pixels.size() * sizeof(Pixel); }

size_t effectScratchBytes(const Layer &layer, size_t frameSize) {
    // ponytail: reserve peak scratch per enabled type; tighten neutral-effect budgets if profiling needs it.
    return std::any_of(layer.effects.begin(), layer.effects.end(), [](const Effect &effect) {
               return effect.enabled &&
                      (effect.type == "motion.rgb-split" || effect.type == "motion.gaussian-blur");
           })
               ? frameSize
               : 0;
}

void checkWorking(size_t ancestorBytes, size_t cachedBytes, size_t outputBytes) {
    if (outputBytes > maxWorking || ancestorBytes > maxWorking - outputBytes ||
        cachedBytes > maxWorking - outputBytes - ancestorBytes)
        throw std::runtime_error("Nested render exceeds 512 MiB working allowance");
}

QString appearanceKey(Id comp, const Layer &layer, Time nominalLocalTime) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << qint64(comp) << qint64(layer.id) << qint64(nominalLocalTime.numerator)
           << qint64(nominalLocalTime.denominator);
    return "appearance:" +
           QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray frameHash(const Frame &frame) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qint32 dimensions[] = {frame.width, frame.height};
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(dimensions), qsizetype(sizeof(dimensions))));
    if (!frame.pixels.empty())
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(frame.pixels.data()),
                                    qsizetype(frame.pixels.size() * sizeof(Pixel))));
    return hash.result();
}

bool layerMotionBlurEnabled(const Layer &layer, const Composition &comp, bool ancestorsEnabled,
                            MotionBlurOverride override) {
    if (!layer.motionBlur)
        return false;
    if (override == MotionBlurOverride::OnForCheckedLayers)
        return true;
    return override == MotionBlurOverride::CurrentSettings && ancestorsEnabled && comp.motionBlurEnabled;
}

bool hasMotionBlurLayer(const Project &project, Id compId, Time nominal, MotionBlurOverride override,
                        bool ancestorsEnabled, int depth = 1) {
    if (depth > 16)
        throw std::runtime_error("Nesting exceeds 16");
    const auto &comp = composition(project, compId);
    if (nominal < time(0) || !(nominal < comp.duration))
        return false;
    const bool localGate = override == MotionBlurOverride::OnForCheckedLayers ||
                           (override == MotionBlurOverride::CurrentSettings && ancestorsEnabled &&
                            comp.motionBlurEnabled);
    for (const auto &layer : comp.layers) {
        if (!isVisible(layer, nominal) || layer.kind == LayerKind::Null || layer.kind == LayerKind::Audio)
            continue;
        const Time localTime = sourceTime(layer, nominal);
        const double opacity = std::clamp(valueAt(layer.channel(Property(7)), localTime), 0.0, 1.0);
        if (opacity == 0)
            continue;
        if (localGate && layer.motionBlur)
            return true;
        if (layer.kind == LayerKind::Precomp &&
            hasMotionBlurLayer(project, layer.source, localTime, override, localGate, depth + 1))
            return true;
    }
    return false;
}

void retainAppearance(RenderPass &pass, const QString &key, std::shared_ptr<const Frame> frame,
                      size_t ancestorBytes) {
    const size_t bytes = frameBytes(*frame);
    checkWorking(ancestorBytes, pass.cachedAppearanceBytes + bytes, 0);
    pass.cachedAppearanceBytes += bytes;
    pass.appearances.emplace(key, std::move(frame));
}

Frame renderInner(const Project &p, Id id, Time nominal, Time sampled, QSize size, Assets &assets,
                  std::stop_token stop, size_t ancestorBytes, int depth,
                  bool ancestorsBlurEnabled, RenderPass &pass) {
    cancelled(stop);
    if (depth > 16)
        throw std::runtime_error("Nesting exceeds 16");
    const auto &comp = composition(p, id);
    const size_t outputBytes = size_t(size.width()) * size.height() * sizeof(Pixel);
    checkWorking(ancestorBytes, pass.cachedAppearanceBytes, outputBytes);
    Frame out = makeFrame(size.width(), size.height());
    if (nominal < time(0) || !(nominal < comp.duration))
        return out;
    Mat3 resolution{
        {double(size.width()) / comp.width, 0, 0, 0, double(size.height()) / comp.height, 0, 0, 0, 1}};
    for (auto it = comp.layers.rbegin(); it != comp.layers.rend(); ++it) {
        const auto &l = *it;
        if (!isVisible(l, nominal) || l.kind == LayerKind::Null || l.kind == LayerKind::Audio)
            continue;
        cancelled(stop);
        const Time localNominal = sourceTime(l, nominal);
        float opacity = float(std::clamp(valueAt(l.channel(Property(7)), localNominal), 0.0, 1.0));
        if (opacity == 0)
            continue;
        const Time transformTime = layerMotionBlurEnabled(l, comp, ancestorsBlurEnabled, pass.options.motionBlur)
                                       ? sampled
                                       : nominal;
        Mat3 transform = multiply(resolution, worldTransform(p, id, l.id, transformTime));
        double det = transform.values[0] * transform.values[4] - transform.values[1] * transform.values[3];
        if (det == 0)
            continue;
        auto inv = inverse(transform);
        const auto settings = rasterSettings(l, localNominal);
        std::shared_ptr<const Frame> src;
        QString rasterKey;
        const QString localAppearanceKey = pass.cacheAppearances ? appearanceKey(id, l, localNominal) : QString{};
        const bool reusableAppearance = pass.cacheAppearances && l.kind != LayerKind::Precomp;
        if (reusableAppearance) {
            if (const auto found = pass.appearances.find(localAppearanceKey); found != pass.appearances.end())
                src = found->second;
        }
        if (l.kind == LayerKind::Precomp) {
            const auto &child = composition(p, l.source);
            const bool childAncestorsBlurEnabled = pass.options.motionBlur == MotionBlurOverride::OnForCheckedLayers ||
                                                   (pass.options.motionBlur == MotionBlurOverride::CurrentSettings &&
                                                    ancestorsBlurEnabled && comp.motionBlurEnabled);
            Frame nested = renderInner(p, l.source, localNominal, sourceTime(l, sampled),
                                       QSize(child.width, child.height), assets, stop,
                                       ancestorBytes + outputBytes, depth + 1, childAncestorsBlurEnabled, pass);
            src = std::make_shared<const Frame>(std::move(nested));
        } else if (!src) {
            src = source(p, l, assets, stop, settings, localNominal, &rasterKey);
        }
        if (!src)
            continue;
        if (settings.mask && !(reusableAppearance && pass.appearances.contains(localAppearanceKey))) {
            QByteArray maskBytes;
            QDataStream data(&maskBytes, QIODevice::WriteOnly);
            const auto &m = *settings.mask;
            data << rasterKey << int(m.shape) << int(m.mode) << m.inverted << m.x << m.y << m.width
                 << m.height;
            const auto key =
                "mask:" +
                QString::fromLatin1(QCryptographicHash::hash(maskBytes, QCryptographicHash::Sha256).toHex());
            const auto found = assets.sources.find(key);
            if (l.kind != LayerKind::Precomp && found != assets.sources.end())
                src = found->second;
            else {
                const size_t sourceBytes = frameBytes(*src);
                checkWorking(ancestorBytes + outputBytes, pass.cachedAppearanceBytes + sourceBytes,
                             sourceBytes);
                Frame masked = *src;
                applyMask(masked, m, stop);
                src = l.kind == LayerKind::Precomp ? std::make_shared<const Frame>(std::move(masked))
                                                   : cacheRaster(key, std::move(masked), assets, false);
            }
        }
        const bool apply = std::any_of(l.effects.begin(), l.effects.end(), [](const Effect &e) { return e.enabled; });
        if (apply && !(reusableAppearance && pass.appearances.contains(localAppearanceKey))) {
            if (l.kind == LayerKind::Precomp && pass.cacheAppearances) {
                const QByteArray inputHash = frameHash(*src);
                auto &cached = pass.precompEffects[localAppearanceKey];
                if (cached.output && cached.inputHash == inputHash) {
                    src = cached.output;
                } else {
                    const size_t inputFrameBytes = frameBytes(*src);
                    const size_t scratchBytes = effectScratchBytes(l, inputFrameBytes);
                    checkWorking(ancestorBytes + outputBytes,
                                 pass.cachedAppearanceBytes + inputFrameBytes + scratchBytes,
                                 inputFrameBytes);
                    Frame processed = *src;
                    applyEffects(l, localNominal, processed, stop);
                    auto output = std::make_shared<const Frame>(std::move(processed));
                    const size_t priorBytes = cached.output ? frameBytes(*cached.output) : 0;
                    const size_t outputFrameBytes = frameBytes(*output);
                    cached = {inputHash, output};
                    pass.cachedAppearanceBytes = pass.cachedAppearanceBytes - priorBytes + outputFrameBytes;
                    src = std::move(output);
                }
            } else {
                const size_t inputFrameBytes = frameBytes(*src);
                const size_t scratchBytes = effectScratchBytes(l, inputFrameBytes);
                checkWorking(ancestorBytes + outputBytes,
                             pass.cachedAppearanceBytes + inputFrameBytes + scratchBytes, inputFrameBytes);
                Frame processed = *src;
                applyEffects(l, localNominal, processed, stop);
                src = std::make_shared<const Frame>(std::move(processed));
            }
        }
        if (reusableAppearance && !pass.appearances.contains(localAppearanceKey)) {
            retainAppearance(pass, localAppearanceKey, src, ancestorBytes + outputBytes);
        }
        double minX = size.width(), minY = size.height(), maxX = 0, maxY = 0;
        for (auto corner : std::array<Vec2, 4>{{{0, 0},
                                                {double(src->width), 0},
                                                {0, double(src->height)},
                                                {double(src->width), double(src->height)}}}) {
            auto q = mapPoint(transform, corner);
            minX = std::min(minX, q.x);
            minY = std::min(minY, q.y);
            maxX = std::max(maxX, q.x);
            maxY = std::max(maxY, q.y);
        }
        int x0 = int(std::clamp(std::floor(minX - 1), 0.0, double(out.width))),
            x1 = int(std::clamp(std::ceil(maxX + 1), 0.0, double(out.width)));
        int y0 = int(std::clamp(std::floor(minY - 1), 0.0, double(out.height))),
            y1 = int(std::clamp(std::ceil(maxY + 1), 0.0, double(out.height)));
        for (int y = y0; y < y1; ++y) {
            cancelled(stop);
            for (int x = x0; x < x1; ++x) {
                auto q = mapPoint(inv, {x + .5, y + .5});
                auto pixel = sample(*src, q.x, q.y);
                pixel = {pixel.r * opacity, pixel.g * opacity, pixel.b * opacity, pixel.a * opacity};
                auto &target = out.pixels[size_t(y) * out.width + x];
                target = sourceOver(pixel, target);
            }
        }
    }
    return out;
}

Time exposureSampleTime(Time nominal, Time fps, int angle, int phase, int samples, int index) {
    const std::int64_t n = samples;
    const std::int64_t numerator = 2 * n * phase + std::int64_t(angle) * (2 * index + 1);
    return nominal + scaleTime(frameTime(1, fps), numerator, 720 * n);
}
} // namespace
Frame render(const Project &p, Id id, Time t, QSize size, Assets &assets, std::stop_token stop,
             const RenderOptions &options) {
    cancelled(stop);
    const auto &comp = composition(p, id);
    if (options.motionBlur != MotionBlurOverride::CurrentSettings &&
        options.motionBlur != MotionBlurOverride::Off &&
        options.motionBlur != MotionBlurOverride::OnForCheckedLayers)
        throw std::runtime_error("Invalid motion blur override");
    if (comp.shutterAngle < 0 || comp.shutterAngle > 720 || comp.shutterPhase < -360 ||
        comp.shutterPhase > 360 || comp.motionBlurSamples < 1 || comp.motionBlurSamples > 64)
        throw std::runtime_error("Invalid motion blur settings");
    auto direct = [&] {
        RenderPass pass{options};
        return renderInner(p, id, t, t, size, assets, stop, 0, 1, true, pass);
    };
    if (options.motionBlur == MotionBlurOverride::Off || comp.shutterAngle == 0 ||
        !hasMotionBlurLayer(p, id, t, options.motionBlur, true))
        return direct();

    RenderPass pass{options};
    const int samples = comp.motionBlurSamples;
    if (samples == 1) {
        return renderInner(p, id, t,
                           exposureSampleTime(t, comp.fps, comp.shutterAngle, comp.shutterPhase, samples, 0),
                           size, assets, stop, 0, 1, true, pass);
    }

    const size_t accumulatorBytes = size_t(size.width()) * size.height() * sizeof(Pixel);
    checkWorking(accumulatorBytes, 0, accumulatorBytes);
    pass.cacheAppearances = true;
    Frame accumulated = makeFrame(size.width(), size.height());
    for (int i = 0; i < samples; ++i) {
        cancelled(stop);
        Frame frame = renderInner(p, id, t,
                                  exposureSampleTime(t, comp.fps, comp.shutterAngle, comp.shutterPhase,
                                                     samples, i),
                                  size, assets, stop, accumulatorBytes, 1, true, pass);
        if (i == 0) {
            accumulated.pixels = std::move(frame.pixels);
            continue;
        }
        const double count = i + 1;
        for (int y = 0; y < accumulated.height; ++y) {
            cancelled(stop);
            for (int x = 0; x < accumulated.width; ++x) {
                auto &dst = accumulated.pixels[size_t(y) * accumulated.width + x];
                const auto &src = frame.pixels[size_t(y) * frame.width + x];
                dst.r = float(double(dst.r) + (double(src.r) - dst.r) / count);
                dst.g = float(double(dst.g) + (double(src.g) - dst.g) / count);
                dst.b = float(double(dst.b) + (double(src.b) - dst.b) / count);
                dst.a = float(double(dst.a) + (double(src.a) - dst.a) / count);
            }
        }
    }
    return accumulated;
}
QImage opaqueImage(const Frame &f) {
    QImage image(f.width, f.height, QImage::Format_RGBA8888);
    if (image.isNull())
        throw std::runtime_error("Cannot allocate movie output image");
    for (int y = 0; y < f.height; ++y) {
        auto *out = image.scanLine(y);
        for (int x = 0; x < f.width; ++x) {
            const auto &p = f.pixels[size_t(y) * f.width + x];
            auto byte = [](float v) {
                return uchar(std::lround(std::clamp(linearToSrgb(v), 0.f, 1.f) * 255));
            };
            out[x * 4] = byte(p.r);
            out[x * 4 + 1] = byte(p.g);
            out[x * 4 + 2] = byte(p.b);
            out[x * 4 + 3] = 255;
        }
    }
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}
QImage pngImage(const Frame &f) {
    QImage image(f.width, f.height, QImage::Format_RGBA8888);
    if (image.isNull())
        throw std::runtime_error("Cannot allocate output image");
    for (int y = 0; y < f.height; ++y) {
        auto *b = image.scanLine(y);
        for (int x = 0; x < f.width; ++x) {
            const auto &p = f.pixels[size_t(y) * f.width + x];
            auto byte = [](float v) {
                return static_cast<uchar>(std::lround(std::clamp(v, 0.f, 1.f) * 255));
            };
            b[x * 4] = p.a > 0 ? byte(linearToSrgb(p.r / p.a)) : 0;
            b[x * 4 + 1] = p.a > 0 ? byte(linearToSrgb(p.g / p.a)) : 0;
            b[x * 4 + 2] = p.a > 0 ? byte(linearToSrgb(p.b / p.a)) : 0;
            b[x * 4 + 3] = byte(p.a);
        }
    }
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}
void registerFixtureFont(const QString &path) {
    int id = QFontDatabase::addApplicationFont(path);
    if (id < 0 || !QFontDatabase::applicationFontFamilies(id).contains("Noto Sans"))
        throw std::runtime_error("Noto Sans fixture font is missing or invalid");
}
Asset inspectPng(const QString &path, Id id) {
    auto bytes = assetBytes(path);
    bool assumed;
    auto image = decodePng(bytes, assumed);
    return {id,
            path,
            QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()),
            image.width(),
            image.height(),
            assumed};
}
void preflightSources(const Project &p, Assets &a) {
    validateProject(p);
    a.pinned = false;
    a.sources.clear();
    a.pinnedSources.clear();
    a.pinnedBytes = 0;
    a.sourceBytes = 0;
    std::set<QString> needed;
    for (const auto &c : p.compositions)
        for (const auto &l : c.layers)
            if (l.kind != LayerKind::Null && l.kind != LayerKind::Precomp && l.kind != LayerKind::Audio &&
                l.kind != LayerKind::Video) {
                const auto settings = rasterSettings(l, time(0));
                source(p, l, a, {}, settings);
                needed.insert(sourceKey(p, l, settings));
            }
    if (needed.size() != a.sources.size())
        throw std::runtime_error("Export sources exceed the pinned source memory allowance");
    a.pinnedSources = std::move(a.sources);
    a.pinnedBytes = a.sourceBytes;
    a.sources.clear();
    a.sourceBytes = 0;
    a.pinned = true;
}
} // namespace motion
