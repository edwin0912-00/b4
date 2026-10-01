// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <QColor>
#include <QString>
#include <QStringList>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

namespace motion {
using Id = std::uint64_t;
struct Time {
    std::int64_t numerator = 0, denominator = 1;
};
Time time(std::int64_t n, std::int64_t d = 1);
Time fromSeconds(double);
Time frameTime(std::int64_t frame, Time fps);
Time scaleTime(Time value, std::int64_t numerator, std::int64_t denominator = 1);
std::int64_t frameCount(Time duration, Time fps);
std::int64_t frameIndexFloor(Time at, Time fps);
std::int64_t frameIndexNearest(Time at, Time fps); // exact halves choose the later frame
double seconds(Time);
bool operator==(Time, Time);
bool operator<(Time, Time);
Time operator+(Time, Time);
Time operator-(Time, Time);
struct WorkArea {
    Time start{}, end{};
    bool operator==(const WorkArea &) const = default;
};
struct FrameRange {
    std::int64_t firstFrame = 0, frameCount = 0;
};
struct Vec2 {
    double x = 0, y = 0;
};
struct Handle {
    double dtSeconds = 0, dv = 0;
};
enum class Interpolation { Hold, Linear, Cubic };
struct Key {
    Time at;
    double value = 0;
    Interpolation outgoing = Interpolation::Linear;
    Handle inHandle{}, outHandle{};
};
struct Channel {
    double base = 0;
    std::vector<Key> keys;
};
enum class Property { AnchorX, AnchorY, PositionX, PositionY, ScaleX, ScaleY, Rotation, Opacity };
// Stable parameter IDs are serialized; Property only names legacy transform components.
enum class ParameterType { Scalar, Vector, Color, Boolean, Enum, Text };
struct Parameter {
    ParameterType type = ParameterType::Scalar;
    std::vector<Channel> components;
    QString text;
};
struct ParameterSpec {
    QString id, label, group;
    ParameterType type;
    std::vector<double> defaults;
    QString defaultText;
    double minimum = -1e12, maximum = 1e12, displayScale = 1;
    QString unit{};
    QStringList choices{};
    bool animated = true;
};
using Parameters = std::map<QString, Parameter>;
struct EffectSpec {
    QString type, name, category;
};
const std::vector<ParameterSpec> &layerParameterSpecs();
const std::vector<EffectSpec> &effectSpecs();
const EffectSpec &effectSpec(const QString &type);
const std::vector<ParameterSpec> &effectParameterSpecs();
const std::vector<ParameterSpec> &effectParameterSpecs(const QString &type);
const ParameterSpec &parameterSpec(const QString &id);
Parameters defaultParameters(const std::vector<ParameterSpec> &);
struct PropertyRef {
    QString id = "transform.position";
    int component = 0;
    Id effect = 0;
    PropertyRef() = default;
    PropertyRef(Property);
    PropertyRef(QString name, int index = 0, Id effectId = 0)
        : id(std::move(name)), component(index), effect(effectId) {}
    bool operator==(const PropertyRef &) const = default;
};
QString propertyLabel(const PropertyRef &);
double toDisplay(PropertyRef, double);
double fromDisplay(PropertyRef, double);
struct Effect {
    Id id = 0;
    QString type = "motion.linear-color";
    QString name = "Linear Color";
    bool enabled = true;
    Parameters parameters = defaultParameters(effectParameterSpecs());
};
enum class LayerKind { Solid, Image, Text, Null, Precomp, Video, Audio };
enum class AssetKind { Image, Video, Audio };
enum class MaskShape { Rectangle, Ellipse };
enum class MaskMode { Add, Subtract };
struct Mask {
    MaskShape shape = MaskShape::Rectangle;
    MaskMode mode = MaskMode::Add;
    bool inverted = false;
    double x = 0, y = 0, width = 100, height = 100;
};
struct Layer {
    Id id = 0;
    QString name = "Layer";
    LayerKind kind = LayerKind::Solid;
    Id source = 0;
    std::optional<Id> parent;
    Time in{}, out{12, 1}, start{};
    bool visible = true, locked = false, audioEnabled = true;
    Parameters parameters = defaultParameters(layerParameterSpecs());
    std::vector<Effect> effects;
    int width = 1920, height = 1080;
    bool motionBlur = false;
    Parameter &parameter(const QString &, Id effect = 0);
    const Parameter &parameter(const QString &, Id effect = 0) const;
    Channel &channel(PropertyRef p) { return parameter(p.id, p.effect).components.at(p.component); }
    const Channel &channel(PropertyRef p) const {
        return parameter(p.id, p.effect).components.at(p.component);
    }
    double &base(const QString &id, int component = 0) { return channel({id, component}).base; }
    double base(const QString &id, int component = 0) const { return channel({id, component}).base; }
    QString &string(const QString &id) { return parameter(id).text; }
    const QString &string(const QString &id) const { return parameter(id).text; }
    void setColor(const QColor &);
    void setMask(std::optional<Mask>);
};
struct PropertyRow {
    PropertyRef ref;
    QString group, label;
};
struct Project;
bool layerHasAudio(const Project &, const Layer &);
std::vector<PropertyRow> propertyRows(const Layer &, const Project * = nullptr);
bool discreteProperty(PropertyRef);
struct Composition {
    Id id = 1;
    QString name = "Composition";
    int width = 1920, height = 1080;
    Time fps{30, 1}, duration{12, 1};
    std::optional<WorkArea> workArea;
    std::vector<Layer> layers;
    bool motionBlurEnabled = false;
    int shutterAngle = 180, shutterPhase = -90, motionBlurSamples = 16;
};
WorkArea resolvedWorkArea(const Composition &);
FrameRange compositionFrameRange(const Composition &, bool workAreaOnly);
std::int64_t compositionBoundaryIndex(const Composition &, Time);
void setWorkAreaFrames(Composition &, std::int64_t firstFrame, std::int64_t exclusiveEndFrame);
void setWorkAreaStart(Composition &, Time cti);
void setWorkAreaEnd(Composition &, Time cti);
void moveWorkArea(Composition &, std::int64_t deltaFrames);
void resetWorkArea(Composition &);
void setCompositionTiming(Composition &, Time fps, Time duration);
struct Asset {
    Id id = 0;
    QString path, sha256;
    int width = 0, height = 0;
    bool assumedSrgb = false;
    AssetKind kind = AssetKind::Image;
    Time duration{}, fps{};
    bool hasAudio = false, variableRate = false;
    int audioChannels = 0, audioRate = 0;
    std::vector<double> waveform{};
};
struct Project {
    int schemaVersion = 5;
    Id nextId = 2;
    std::vector<Composition> compositions{Composition{}};
    std::vector<Asset> assets;
};
Composition &composition(Project &, Id);
const Composition &composition(const Project &, Id);
Layer &layer(Project &, Id);
const Layer &layer(const Project &, Id);
const Asset &asset(const Project &, Id);
void validateProject(const Project &);
QString layerKindName(LayerKind);
} // namespace motion
