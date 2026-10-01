// SPDX-License-Identifier: MPL-2.0
#include "project.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace motion {
namespace {
using Wide = __int128_t;
Time normalized(Wide n, Wide d) {
    if (!d)
        throw std::runtime_error("Time denominator must not be zero");
    if (d < 0) {
        n = -n;
        d = -d;
    }
    Wide a = n < 0 ? -n : n, b = d;
    while (b) {
        Wide r = a % b;
        a = b;
        b = r;
    }
    n /= a;
    d /= a;
    if (n < std::numeric_limits<std::int64_t>::min() || n > std::numeric_limits<std::int64_t>::max() ||
        d > std::numeric_limits<std::int64_t>::max())
        throw std::overflow_error("Time exceeds 64-bit range");
    return {static_cast<std::int64_t>(n), static_cast<std::int64_t>(d)};
}
void require(bool ok, const char *msg) {
    if (!ok)
        throw std::runtime_error(msg);
}
void validTime(Time t) { require(t.denominator > 0, "Time needs positive denominator"); }
bool finite(double v) { return std::isfinite(v) && std::abs(v) <= 1e12; }
Wide floorQuotient(Wide numerator, Wide denominator) {
    require(denominator > 0, "Frame rate must be positive");
    Wide quotient = numerator / denominator;
    if (numerator % denominator < 0)
        --quotient;
    return quotient;
}
std::int64_t frameIndexValue(Wide value) {
    if (value < std::numeric_limits<std::int64_t>::min() || value > std::numeric_limits<std::int64_t>::max())
        throw std::overflow_error("Frame index exceeds 64-bit range");
    return static_cast<std::int64_t>(value);
}
void dimensions(int w, int h) {
    require(w > 0 && h > 0 && w <= 8192 && h <= 8192 && static_cast<std::int64_t>(w) * h <= 16777216,
            "Dimensions must be 1..8192 and at most 16,777,216 pixels");
}
} // namespace
Time time(std::int64_t n, std::int64_t d) { return normalized(n, d); }
Time fromSeconds(double v) {
    require(std::isfinite(v) && std::abs(v) <= 864000, "Time value is outside supported range");
    return time(std::llround(v * 1000000), 1000000);
}
double seconds(Time t) {
    validTime(t);
    return double(t.numerator) / double(t.denominator);
}
Time frameTime(std::int64_t f, Time fps) {
    validTime(fps);
    require(fps.numerator > 0, "Frame rate must be positive");
    return normalized(Wide(f) * fps.denominator, fps.numerator);
}
Time scaleTime(Time value, std::int64_t numerator, std::int64_t denominator) {
    validTime(value);
    return normalized(Wide(value.numerator) * numerator, Wide(value.denominator) * denominator);
}
std::int64_t frameCount(Time duration, Time fps) {
    validTime(duration);
    validTime(fps);
    require(duration.numerator > 0 && fps.numerator > 0, "Duration and frame rate must be positive");
    Wide n = Wide(duration.numerator) * fps.numerator, d = Wide(duration.denominator) * fps.denominator;
    Wide frames = n / d + (n % d != 0);
    if (frames > std::numeric_limits<std::int64_t>::max())
        throw std::overflow_error("Frame count exceeds 64-bit range");
    return static_cast<std::int64_t>(frames);
}
std::int64_t frameIndexFloor(Time at, Time fps) {
    validTime(at);
    validTime(fps);
    require(fps.numerator > 0, "Frame rate must be positive");
    return frameIndexValue(floorQuotient(Wide(at.numerator) * fps.numerator,
                                         Wide(at.denominator) * fps.denominator));
}
std::int64_t frameIndexNearest(Time at, Time fps) {
    validTime(at);
    validTime(fps);
    require(fps.numerator > 0, "Frame rate must be positive");
    const Wide numerator = Wide(at.numerator) * fps.numerator;
    const Wide denominator = Wide(at.denominator) * fps.denominator;
    Wide lower = floorQuotient(numerator, denominator);
    const Wide remainder = numerator - lower * denominator;
    if (remainder * 2 >= denominator)
        ++lower;
    return frameIndexValue(lower);
}
bool operator==(Time a, Time b) {
    validTime(a);
    validTime(b);
    return Wide(a.numerator) * b.denominator == Wide(b.numerator) * a.denominator;
}
bool operator<(Time a, Time b) {
    validTime(a);
    validTime(b);
    return Wide(a.numerator) * b.denominator < Wide(b.numerator) * a.denominator;
}
Time operator+(Time a, Time b) {
    validTime(a);
    validTime(b);
    return normalized(Wide(a.numerator) * b.denominator + Wide(b.numerator) * a.denominator,
                      Wide(a.denominator) * b.denominator);
}
Time operator-(Time a, Time b) {
    validTime(a);
    validTime(b);
    return normalized(Wide(a.numerator) * b.denominator - Wide(b.numerator) * a.denominator,
                      Wide(a.denominator) * b.denominator);
}
WorkArea resolvedWorkArea(const Composition &c) {
    return c.workArea.value_or(WorkArea{time(0), c.duration});
}
std::int64_t compositionBoundaryIndex(const Composition &c, Time at) {
    validTime(at);
    validTime(c.fps);
    validTime(c.duration);
    require(c.fps.numerator > 0 && c.duration.numerator > 0,
            "Composition duration and frame rate must be positive");
    const auto total = frameCount(c.duration, c.fps);
    if (!(time(0) < at))
        return 0;
    if (!(at < c.duration))
        return total;

    const auto regular = std::clamp<std::int64_t>(frameIndexNearest(at, c.fps), 0, total - 1);
    const Time regularTime = frameTime(regular, c.fps);
    const Time regularDistance = at < regularTime ? regularTime - at : at - regularTime;
    const Time terminalDistance = c.duration - at;
    // The terminal composition edge is a legal boundary even when it falls
    // between regular frame starts; ties go to the later boundary.
    return !(regularDistance < terminalDistance) ? total : regular;
}
FrameRange compositionFrameRange(const Composition &c, bool workAreaOnly) {
    const auto total = frameCount(c.duration, c.fps);
    if (!workAreaOnly)
        return {0, total};
    const auto area = resolvedWorkArea(c);
    const auto first = frameIndexFloor(area.start, c.fps);
    const auto end = compositionBoundaryIndex(c, area.end);
    require(first >= 0 && first < end && end <= total, "Work Area must select at least one composition frame");
    return {first, end - first};
}
void setWorkAreaFrames(Composition &c, std::int64_t firstFrame, std::int64_t exclusiveEndFrame) {
    const auto total = frameCount(c.duration, c.fps);
    const auto first = std::clamp<std::int64_t>(firstFrame, 0, total - 1);
    const auto end = std::clamp<std::int64_t>(exclusiveEndFrame, first + 1, total);
    const Time start = frameTime(first, c.fps);
    const Time finish = end == total ? c.duration : frameTime(end, c.fps);
    if (start == time(0) && finish == c.duration)
        c.workArea.reset();
    else
        c.workArea = WorkArea{start, finish};
}
void setWorkAreaStart(Composition &c, Time cti) {
    validTime(cti);
    const auto total = frameCount(c.duration, c.fps);
    const Time bounded = cti < time(0) ? time(0) : (c.duration < cti ? c.duration : cti);
    const auto first = std::clamp<std::int64_t>(frameIndexFloor(bounded, c.fps), 0, total - 1);
    const auto oldEnd = compositionBoundaryIndex(c, resolvedWorkArea(c).end);
    setWorkAreaFrames(c, first, std::max(oldEnd, first + 1));
}
void setWorkAreaEnd(Composition &c, Time cti) {
    validTime(cti);
    const auto total = frameCount(c.duration, c.fps);
    const Time bounded = cti < time(0) ? time(0) : (c.duration < cti ? c.duration : cti);
    const auto exclusiveEnd = std::clamp<std::int64_t>(frameIndexFloor(bounded, c.fps) + 1, 1, total);
    const auto oldFirst = frameIndexFloor(resolvedWorkArea(c).start, c.fps);
    const auto first = std::min(oldFirst, exclusiveEnd - 1);
    setWorkAreaFrames(c, first, exclusiveEnd);
}
void moveWorkArea(Composition &c, std::int64_t deltaFrames) {
    const auto current = compositionFrameRange(c, true);
    const auto total = frameCount(c.duration, c.fps);
    const auto maximumFirst = total - current.frameCount;
    const Wide moved = Wide(current.firstFrame) + deltaFrames;
    const auto first = static_cast<std::int64_t>(std::clamp<Wide>(moved, 0, maximumFirst));
    setWorkAreaFrames(c, first, first + current.frameCount);
}
void resetWorkArea(Composition &c) { c.workArea.reset(); }
void setCompositionTiming(Composition &c, Time fps, Time duration) {
    fps = time(fps.numerator, fps.denominator);
    duration = time(duration.numerator, duration.denominator);
    require(time(0) < fps && !(time(1000) < fps), "Frame rate must be in (0,1000]");
    require(time(0) < duration && !(time(86400) < duration), "Duration must be in (0,24h]");

    Composition updated;
    updated.fps = fps;
    updated.duration = duration;
    const bool wasFull = !c.workArea ||
                         (c.workArea->start == time(0) && c.workArea->end == c.duration);
    if (!wasFull) {
        const auto total = frameCount(duration, fps);
        const auto first = std::clamp<std::int64_t>(frameIndexNearest(c.workArea->start, fps), 0, total - 1);
        const Time endTime = duration < c.workArea->end ? duration : c.workArea->end;
        auto end = compositionBoundaryIndex(updated, endTime);
        if (end <= first)
            end = first + 1;
        setWorkAreaFrames(updated, first, end);
    }
    c.fps = updated.fps;
    c.duration = updated.duration;
    c.workArea = updated.workArea;
}
Composition &composition(Project &p, Id id) {
    for (auto &c : p.compositions)
        if (c.id == id)
            return c;
    throw std::runtime_error("Composition reference does not exist");
}
const Composition &composition(const Project &p, Id id) {
    for (const auto &c : p.compositions)
        if (c.id == id)
            return c;
    throw std::runtime_error("Composition reference does not exist");
}
Layer &layer(Project &p, Id id) {
    for (auto &c : p.compositions)
        for (auto &l : c.layers)
            if (l.id == id)
                return l;
    throw std::runtime_error("Layer reference does not exist");
}
const Layer &layer(const Project &p, Id id) {
    for (const auto &c : p.compositions)
        for (const auto &l : c.layers)
            if (l.id == id)
                return l;
    throw std::runtime_error("Layer reference does not exist");
}
const Asset &asset(const Project &p, Id id) {
    for (const auto &a : p.assets)
        if (a.id == id)
            return a;
    throw std::runtime_error("Asset reference does not exist");
}
QString layerKindName(LayerKind k) {
    static const std::array<const char *, 7> names = {"Solid",   "Image", "Text", "Null",
                                                      "Precomp", "Video", "Audio"};
    auto i = static_cast<size_t>(k);
    return i < names.size() ? names[i] : "Invalid";
}
const std::vector<ParameterSpec> &layerParameterSpecs() {
    static const std::vector<ParameterSpec> specs = {
        {"transform.anchor",
         "Anchor Point",
         "Transform",
         ParameterType::Vector,
         {0, 0},
         {},
         -1e12,
         1e12,
         1,
         "px"},
        {"transform.position",
         "Position",
         "Transform",
         ParameterType::Vector,
         {0, 0},
         {},
         -1e12,
         1e12,
         1,
         "px"},
        {"transform.scale", "Scale", "Transform", ParameterType::Vector, {1, 1}, {}, -1e12, 1e12, 100, "%"},
        {"transform.rotation", "Rotation", "Transform", ParameterType::Scalar, {0}, {}, -1e12, 1e12, 1, "°"},
        {"transform.opacity", "Opacity", "Transform", ParameterType::Scalar, {1}, {}, -1e12, 1e12, 100, "%"},
        {"source.color", "Color", "Source", ParameterType::Color, {1, 1, 1, 1}, {}, 0, 1, 1, ""},
        {"text.source", "Source Text", "Text", ParameterType::Text, {}, "Title", 0, 0, 1, "", {}, false},
        {"text.family", "Font Family", "Text", ParameterType::Text, {}, "Noto Sans", 0, 0, 1, "", {}, false},
        {"text.style", "Font Style", "Text", ParameterType::Text, {}, "Regular", 0, 0, 1, "", {}, false},
        {"text.size",
         "Font Size",
         "Text",
         ParameterType::Scalar,
         {96},
         {},
         std::numeric_limits<double>::denorm_min(),
         4096,
         1,
         "px"},
        {"text.leading",
         "Line Spacing",
         "Text",
         ParameterType::Scalar,
         {1.15},
         {},
         std::numeric_limits<double>::denorm_min(),
         10,
         100,
         "%"},
        {"text.alignment",
         "Alignment",
         "Text",
         ParameterType::Enum,
         {0},
         {},
         0,
         2,
         1,
         "",
         {"Left", "Center", "Right"}},
        {"mask.enabled", "Enabled", "Mask", ParameterType::Boolean, {0}, {}, 0, 1},
        {"mask.shape", "Shape", "Mask", ParameterType::Enum, {0}, {}, 0, 1, 1, "", {"Rectangle", "Ellipse"}},
        {"mask.mode", "Mode", "Mask", ParameterType::Enum, {0}, {}, 0, 1, 1, "", {"Add", "Subtract"}},
        {"mask.inverted", "Inverted", "Mask", ParameterType::Boolean, {0}, {}, 0, 1},
        {"mask.position", "Position", "Mask", ParameterType::Vector, {0, 0}, {}, -1e12, 1e12, 1, "px"},
        {"mask.size", "Size", "Mask", ParameterType::Vector, {100, 100}, {}, 0, 1e12, 1, "px"},
        {"audio.levels", "Audio Levels", "Audio", ParameterType::Vector, {0, 0}, {}, -192, 12, 1, "dB"}};
    return specs;
}
const std::vector<EffectSpec> &effectSpecs() {
    static const std::vector<EffectSpec> specs = {
        {"motion.linear-color", "Linear Color", "Color Correction"},
        {"motion.rgb-split", "RGB Split", "Color Correction"},
        {"motion.exposure", "Exposure", "Color Correction"},
        {"motion.gaussian-blur", "Gaussian Blur", "Blur & Sharpen"}};
    return specs;
}
const EffectSpec &effectSpec(const QString &type) {
    for (const auto &spec : effectSpecs())
        if (spec.type == type)
            return spec;
    throw std::runtime_error(("Unknown effect type: " + type).toStdString());
}
const std::vector<ParameterSpec> &effectParameterSpecs() { return effectParameterSpecs("motion.linear-color"); }
const std::vector<ParameterSpec> &effectParameterSpecs(const QString &type) {
    static const std::vector<ParameterSpec> specs = {
        {"color.gain", "Gain", "Linear Color", ParameterType::Scalar, {1}, {}, -1000, 1000},
        {"color.bias", "Bias", "Linear Color", ParameterType::Scalar, {0}, {}, -1000, 1000},
        {"color.amount", "Amount", "Linear Color", ParameterType::Scalar, {1}, {}, 0, 1, 100, "%"}};
    static const std::vector<ParameterSpec> rgbSplit = {
        {"rgb-split.red-offset", "Red Offset", "RGB Split", ParameterType::Vector, {0, 0}, {}, -4096, 4096, 1,
         "px"},
        {"rgb-split.green-offset", "Green Offset", "RGB Split", ParameterType::Vector, {0, 0}, {}, -4096, 4096,
         1, "px"},
        {"rgb-split.blue-offset", "Blue Offset", "RGB Split", ParameterType::Vector, {0, 0}, {}, -4096, 4096, 1,
         "px"},
        {"rgb-split.amount", "Amount", "RGB Split", ParameterType::Scalar, {1}, {}, 0, 1, 100, "%"}};
    static const std::vector<ParameterSpec> exposure = {
        {"exposure.stops", "Stops", "Exposure", ParameterType::Scalar, {0}, {}, -20, 20, 1, "stops"},
        {"exposure.amount", "Amount", "Exposure", ParameterType::Scalar, {1}, {}, 0, 1, 100, "%"}};
    static const std::vector<ParameterSpec> gaussianBlur = {
        {"gaussian-blur.sigma", "Sigma", "Gaussian Blur", ParameterType::Scalar, {0}, {}, 0, 128, 1, "px"},
        {"gaussian-blur.amount", "Amount", "Gaussian Blur", ParameterType::Scalar, {1}, {}, 0, 1, 100, "%"}};
    if (type == "motion.linear-color")
        return specs;
    if (type == "motion.rgb-split")
        return rgbSplit;
    if (type == "motion.exposure")
        return exposure;
    if (type == "motion.gaussian-blur")
        return gaussianBlur;
    effectSpec(type);
    throw std::runtime_error("Effect has no parameter descriptor");
}
const ParameterSpec &parameterSpec(const QString &id) {
    for (const auto &spec : layerParameterSpecs())
        if (spec.id == id)
            return spec;
    for (const auto &effect : effectSpecs())
        for (const auto &spec : effectParameterSpecs(effect.type))
            if (spec.id == id)
                return spec;
    throw std::runtime_error(("Unknown parameter: " + id).toStdString());
}
Parameters defaultParameters(const std::vector<ParameterSpec> &specs) {
    Parameters result;
    for (const auto &spec : specs) {
        Parameter value{spec.type, {}, spec.defaultText};
        for (double v : spec.defaults)
            value.components.push_back({v, {}});
        result.emplace(spec.id, std::move(value));
    }
    return result;
}
PropertyRef::PropertyRef(Property p) {
    static const std::array<const char *, 8> ids = {
        "transform.anchor", "transform.anchor", "transform.position", "transform.position",
        "transform.scale",  "transform.scale",  "transform.rotation", "transform.opacity"};
    const auto i = static_cast<size_t>(p);
    id = ids.at(i);
    component = i < 6 ? int(i % 2) : 0;
}
QString propertyLabel(const PropertyRef &ref) {
    const auto &spec = parameterSpec(ref.id);
    QString suffix;
    if (spec.defaults.size() > 1) {
        const QStringList names = spec.type == ParameterType::Color ? QStringList{"R", "G", "B", "A"}
                                  : ref.id == "audio.levels"        ? QStringList{"Left", "Right"}
                                                                    : QStringList{"X", "Y"};
        suffix = " " + names.at(ref.component);
    }
    return (ref.id == "transform.anchor" ? QString("Anchor") : spec.label) + suffix;
}
double toDisplay(PropertyRef ref, double v) { return v * parameterSpec(ref.id).displayScale; }
double fromDisplay(PropertyRef ref, double v) { return v / parameterSpec(ref.id).displayScale; }
Parameter &Layer::parameter(const QString &id, Id effect) {
    if (!effect)
        return parameters.at(id);
    for (auto &e : effects)
        if (e.id == effect)
            return e.parameters.at(id);
    throw std::runtime_error("Effect reference does not exist");
}
const Parameter &Layer::parameter(const QString &id, Id effect) const {
    if (!effect)
        return parameters.at(id);
    for (const auto &e : effects)
        if (e.id == effect)
            return e.parameters.at(id);
    throw std::runtime_error("Effect reference does not exist");
}
void Layer::setColor(const QColor &c) {
    if (!c.isValid())
        throw std::runtime_error("Invalid color");
    auto &v = parameter("source.color").components;
    v[0].base = c.redF();
    v[1].base = c.greenF();
    v[2].base = c.blueF();
    v[3].base = c.alphaF();
}
void Layer::setMask(std::optional<Mask> m) {
    base("mask.enabled") = m.has_value();
    if (!m)
        return;
    base("mask.shape") = int(m->shape);
    base("mask.mode") = int(m->mode);
    base("mask.inverted") = m->inverted;
    base("mask.position", 0) = m->x;
    base("mask.position", 1) = m->y;
    base("mask.size", 0) = m->width;
    base("mask.size", 1) = m->height;
}
bool discreteProperty(PropertyRef ref) {
    auto type = parameterSpec(ref.id).type;
    return type == ParameterType::Boolean || type == ParameterType::Enum;
}
bool layerHasAudio(const Project &p, const Layer &l) {
    if (l.kind == LayerKind::Video || l.kind == LayerKind::Audio)
        return asset(p, l.source).hasAudio;
    if (l.kind == LayerKind::Precomp)
        for (const auto &child : composition(p, l.source).layers)
            if (layerHasAudio(p, child))
                return true;
    return false;
}
std::vector<PropertyRow> propertyRows(const Layer &l, const Project *project) {
    std::vector<PropertyRow> result;
    auto add = [&](const ParameterSpec &spec, Id effect, QString group) {
        for (int i = 0; i < int(spec.defaults.size()); ++i) {
            PropertyRef ref{spec.id, i, effect};
            result.push_back({ref, group, propertyLabel(ref)});
        }
    };
    for (const auto &spec : layerParameterSpecs()) {
        if (spec.group == "Audio" && (project ? !layerHasAudio(*project, l)
                                              : l.kind != LayerKind::Video && l.kind != LayerKind::Audio &&
                                                    l.kind != LayerKind::Precomp))
            continue;
        if (l.kind == LayerKind::Audio && spec.group != "Audio")
            continue;
        if (spec.group == "Text" && l.kind != LayerKind::Text)
            continue;
        if (spec.group == "Source" && l.kind != LayerKind::Solid && l.kind != LayerKind::Text)
            continue;
        if (spec.group == "Mask" && l.kind == LayerKind::Null)
            continue;
        add(spec, 0, spec.group);
    }
    for (const auto &e : l.effects)
        for (const auto &spec : effectParameterSpecs(e.type))
            add(spec, e.id, e.name + " #" + QString::number(e.id));
    return result;
}
namespace {
void validateParameters(const Parameters &values, const std::vector<ParameterSpec> &specs) {
    require(values.size() == specs.size(), "Missing or unknown parameter");
    for (const auto &spec : specs) {
        auto found = values.find(spec.id);
        require(found != values.end(), "Missing required parameter");
        const auto &v = found->second;
        require(v.type == spec.type && v.components.size() == spec.defaults.size(),
                "Parameter type or dimension mismatch");
        require(v.type == ParameterType::Text || v.text.isEmpty(), "Numeric parameter cannot contain text");
        require(v.text.size() <= 100000, "Parameter text exceeds limit");
        bool discrete = v.type == ParameterType::Boolean || v.type == ParameterType::Enum;
        auto validValue = [&](double x) {
            require(finite(x) && x >= spec.minimum && x <= spec.maximum, "Parameter value exceeds range");
            require(!discrete || std::floor(x) == x, "Discrete parameter requires an integer");
        };
        for (const auto &ch : v.components) {
            validValue(ch.base);
            require(ch.keys.size() <= 100000 && (spec.animated || ch.keys.empty()),
                    "Invalid parameter animation");
            for (size_t i = 0; i < ch.keys.size(); ++i) {
                const auto &k = ch.keys[i];
                validTime(k.at);
                validValue(k.value);
                require(std::abs(seconds(k.at)) <= 864000, "Keyframe time exceeds limits");
                require(int(k.outgoing) >= 0 && int(k.outgoing) <= 2, "Unknown interpolation");
                require(!discrete || k.outgoing == Interpolation::Hold,
                        "Discrete parameters require hold keys");
                require(finite(k.inHandle.dtSeconds) && finite(k.inHandle.dv) &&
                            finite(k.outHandle.dtSeconds) && finite(k.outHandle.dv),
                        "Invalid curve handle");
                if (i)
                    require(ch.keys[i - 1].at < k.at, "Keyframe times must be strictly ordered");
                if (i + 1 < ch.keys.size() && k.outgoing == Interpolation::Cubic) {
                    const auto &next = ch.keys[i + 1];
                    double d = seconds(next.at - k.at), a = k.outHandle.dtSeconds,
                           b = d + next.inHandle.dtSeconds;
                    // Crossing control points still give monotone cubic time when both stay in [0,d].
                    require(a >= 0 && a <= d && b >= 0 && b <= d,
                            "Cubic handle times must stay inside their segment");
                }
            }
        }
    }
}
} // namespace
void validateProject(const Project &p) {
    require(p.schemaVersion == 5, "Unsupported project schema");
    require(!p.compositions.empty() && p.compositions.size() <= 256, "Project needs 1..256 compositions");
    require(p.assets.size() <= 4096, "Project exceeds 4096 assets");
    std::set<Id> ids;
    auto addId = [&](Id id) { require(id && ids.insert(id).second, "IDs must be nonzero and unique"); };
    for (const auto &a : p.assets) {
        addId(a.id);
        dimensions(a.width, a.height);
        require(int(a.kind) >= 0 && int(a.kind) <= 2, "Unknown media kind");
        validTime(a.duration);
        validTime(a.fps);
        require(a.waveform.size() <= 128, "Waveform summary exceeds limit");
        for (double peak : a.waveform)
            require(finite(peak) && peak >= 0 && peak <= 1, "Invalid waveform peak");
        if (a.kind == AssetKind::Image)
            require(a.duration == time(0) && a.fps == time(0) && !a.hasAudio && a.audioChannels == 0 &&
                        a.audioRate == 0 && a.waveform.empty(),
                    "Invalid still-image media fields");
        else {
            require(seconds(a.duration) > 0 && seconds(a.duration) <= 86400,
                    "Media duration must be in (0,24h]");
            require(seconds(a.fps) >= 0 && seconds(a.fps) <= 1000, "Invalid media frame rate");
            require(a.audioChannels >= 0 && a.audioChannels <= 32 && a.hasAudio == (a.audioChannels > 0),
                    "Invalid audio channel metadata");
            require(a.hasAudio ? a.audioRate > 0 && a.audioRate <= 768000 : a.audioRate == 0,
                    "Invalid source audio rate");
            require(a.kind != AssetKind::Audio || a.hasAudio, "Audio media has no audio stream");
        }
        require(!a.path.isEmpty() && a.path.size() < 32768, "Asset path is invalid");
        require(a.sha256.size() == 64, "Asset needs a SHA-256 fingerprint");
        for (QChar ch : a.sha256)
            require((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'), "Invalid SHA-256");
    }
    for (const auto &c : p.compositions) {
        addId(c.id);
        dimensions(c.width, c.height);
        validTime(c.fps);
        validTime(c.duration);
        require(seconds(c.fps) > 0 && seconds(c.fps) <= 1000, "Frame rate must be in (0,1000]");
        require(seconds(c.duration) > 0 && seconds(c.duration) <= 86400, "Duration must be in (0,24h]");
        if (c.workArea) {
            const auto &area = *c.workArea;
            validTime(area.start);
            validTime(area.end);
            require(area.start.numerator >= 0 && area.start < area.end && !(c.duration < area.end),
                    "Work Area must satisfy 0 <= start < end <= composition duration");
            const auto total = frameCount(c.duration, c.fps);
            const auto first = frameIndexNearest(area.start, c.fps);
            require(first >= 0 && first < total && frameTime(first, c.fps) == area.start,
                    "Work Area start must be on a composition frame boundary");
            if (area.end == c.duration)
                require(first < total, "Work Area must select at least one output frame");
            else {
                const auto end = frameIndexNearest(area.end, c.fps);
                require(end > first && end < total && frameTime(end, c.fps) == area.end,
                        "Work Area end must be on a frame boundary or at composition end");
            }
        }
        require(c.shutterAngle >= 0 && c.shutterAngle <= 720, "Shutter angle must be in 0..720 degrees");
        require(c.shutterPhase >= -360 && c.shutterPhase <= 360, "Shutter phase must be in -360..360 degrees");
        require(c.motionBlurSamples >= 1 && c.motionBlurSamples <= 64, "Motion-blur samples must be in 1..64");
        require(c.layers.size() <= 1024, "Composition exceeds 1024 layers");
        std::map<Id, const Layer *> local;
        for (const auto &l : c.layers) {
            addId(l.id);
            local[l.id] = &l;
            require(static_cast<int>(l.kind) >= 0 && static_cast<int>(l.kind) <= 6, "Unknown layer type");
            dimensions(l.width, l.height);
            validTime(l.in);
            validTime(l.out);
            validTime(l.start);
            require(l.in < l.out, "Layer out point must be after in point");
            require(std::abs(seconds(l.in)) <= 864000 && std::abs(seconds(l.out)) <= 864000 &&
                        std::abs(seconds(l.start)) <= 864000,
                    "Layer time exceeds limits");
            require(l.name.size() <= 4096, "Layer name exceeds limit");
            if (l.kind == LayerKind::Image || l.kind == LayerKind::Video || l.kind == LayerKind::Audio) {
                const auto &a = asset(p, l.source);
                require((l.kind == LayerKind::Image && a.kind == AssetKind::Image) ||
                            (l.kind == LayerKind::Video && a.kind == AssetKind::Video) ||
                            (l.kind == LayerKind::Audio && a.kind == AssetKind::Audio),
                        "Layer and media kinds differ");
            }
            if (l.kind == LayerKind::Precomp)
                composition(p, l.source);
            validateParameters(l.parameters, layerParameterSpecs());
            require(l.effects.size() <= 128, "Layer exceeds 128 effects");
            for (const auto &e : l.effects) {
                addId(e.id);
                effectSpec(e.type);
                require(e.name.size() <= 4096, "Effect name exceeds limit");
                validateParameters(e.parameters, effectParameterSpecs(e.type));
            }
        }
        for (const auto &l : c.layers) {
            std::set<Id> path{l.id};
            auto parent = l.parent;
            while (parent) {
                require(local.count(*parent), "Parent must exist in the same composition");
                require(path.insert(*parent).second, "Parent cycle is not allowed");
                parent = local.at(*parent)->parent;
            }
        }
    }
    require(p.nextId > *ids.rbegin() && p.nextId < std::numeric_limits<Id>::max(), "Next ID is invalid");
    std::map<Id, int> depths;
    std::set<Id> visiting;
    std::function<int(Id)> depth = [&](Id id) {
        if (depths.count(id))
            return depths.at(id);
        require(visiting.insert(id).second, "Precomposition cycle is not allowed");
        int d = 1;
        for (const auto &l : composition(p, id).layers)
            if (l.kind == LayerKind::Precomp)
                d = std::max(d, 1 + depth(l.source));
        visiting.erase(id);
        require(d <= 16, "Precomposition depth exceeds 16");
        return depths[id] = d;
    };
    for (const auto &c : p.compositions)
        depth(c.id);
}
} // namespace motion
