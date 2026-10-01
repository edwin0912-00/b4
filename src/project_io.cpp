// SPDX-License-Identifier: MPL-2.0
#include "project_io.hpp"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <cmath>
#include <limits>
namespace motion {
namespace {
[[noreturn]] void bad(const char *message) { throw std::runtime_error(message); }
QJsonObject object(QJsonValue v, std::initializer_list<const char *> fields) {
    if (!v.isObject())
        bad("Expected a JSON object");
    auto o = v.toObject();
    if (o.size() != static_cast<qsizetype>(fields.size()))
        bad("Missing or unknown project fields");
    for (auto f : fields)
        if (!o.contains(f))
            bad("Missing required project field");
    return o;
}
QJsonArray array(QJsonValue v, int max) {
    if (!v.isArray() || v.toArray().size() > max)
        bad("Invalid or oversized JSON array");
    return v.toArray();
}
QString string(QJsonValue v) {
    if (!v.isString())
        bad("Expected a string");
    return v.toString();
}
double number(QJsonValue v) {
    if (!v.isDouble() || !std::isfinite(v.toDouble()))
        bad("Expected a finite number");
    return v.toDouble();
}
int integer(QJsonValue v) {
    double d = number(v);
    if (d != std::floor(d) || d < std::numeric_limits<int>::min() || d > std::numeric_limits<int>::max())
        bad("Expected a bounded integer");
    return int(d);
}
bool boolean(QJsonValue v) {
    if (!v.isBool())
        bad("Expected a boolean");
    return v.toBool();
}
QString idText(Id v) { return QString::number(v); }
Id idValue(QJsonValue v) {
    QString s = string(v);
    bool ok = false;
    if (!QRegularExpression("^(0|[1-9][0-9]*)$").match(s).hasMatch())
        bad("Invalid ID string");
    Id id = s.toULongLong(&ok);
    if (!ok)
        bad("ID exceeds 64-bit range");
    return id;
}
QJsonArray timeJson(Time t) {
    t = time(t.numerator, t.denominator);
    return {QString::number(t.numerator), QString::number(t.denominator)};
}
Time timeValue(QJsonValue v) {
    auto a = array(v, 2);
    if (a.size() != 2)
        bad("Time needs numerator and denominator");
    auto parse = [](QJsonValue n) {
        const auto s = string(n);
        bool ok = false;
        if (!QRegularExpression("^-?(0|[1-9][0-9]*)$").match(s).hasMatch())
            bad("Invalid time string");
        auto value = s.toLongLong(&ok);
        if (!ok)
            bad("Time exceeds 64-bit range");
        return value;
    };
    auto n = parse(a[0]), d = parse(a[1]);
    if (d <= 0)
        bad("Time denominator must be positive");
    return time(n, d);
}
const QStringList interpolationNames = {"hold", "linear", "cubic"};
const QStringList kindNames = {"solid", "image", "text", "null", "precomp", "video", "audio"};
const QStringList assetKinds = {"image", "video", "audio"};
int enumValue(QJsonValue v, const QStringList &names) {
    auto i = names.indexOf(string(v));
    if (i < 0)
        bad("Unknown rendering enum");
    return int(i);
}
QJsonObject channelJson(const Channel &c) {
    QJsonArray keys;
    for (const auto &k : c.keys)
        keys.append(QJsonObject{{"time", timeJson(k.at)},
                                {"value", k.value},
                                {"interpolation", interpolationNames.at(int(k.outgoing))},
                                {"in", QJsonArray{k.inHandle.dtSeconds, k.inHandle.dv}},
                                {"out", QJsonArray{k.outHandle.dtSeconds, k.outHandle.dv}}});
    return {{"base", c.base}, {"keys", keys}};
}
Channel channelValue(QJsonValue v) {
    auto o = object(v, {"base", "keys"});
    Channel c;
    c.base = number(o["base"]);
    for (auto kv : array(o["keys"], 100000)) {
        auto k = object(kv, {"time", "value", "interpolation", "in", "out"});
        auto in = array(k["in"], 2), out = array(k["out"], 2);
        if (in.size() != 2 || out.size() != 2)
            bad("Curve handle needs two coordinates");
        c.keys.push_back({timeValue(k["time"]),
                          number(k["value"]),
                          Interpolation(enumValue(k["interpolation"], interpolationNames)),
                          {number(in[0]), number(in[1])},
                          {number(out[0]), number(out[1])}});
    }
    return c;
}
const QStringList parameterTypes = {"scalar", "vector", "color", "boolean", "enum", "text"};
QJsonArray parametersJson(const Parameters &parameters) {
    QJsonArray out;
    for (const auto &[id, p] : parameters) {
        QJsonArray components;
        for (const auto &c : p.components)
            components.append(channelJson(c));
        out.append(QJsonObject{{"id", id},
                               {"type", parameterTypes.at(int(p.type))},
                               {"components", components},
                               {"text", p.text}});
    }
    return out;
}
Parameters parametersValue(QJsonValue value) {
    Parameters result;
    for (auto entry : array(value, 128)) {
        auto o = object(entry, {"id", "type", "components", "text"});
        Parameter p{ParameterType(enumValue(o["type"], parameterTypes)), {}, string(o["text"])};
        for (auto c : array(o["components"], 4))
            p.components.push_back(channelValue(c));
        if (!result.emplace(string(o["id"]), std::move(p)).second)
            bad("Duplicate parameter ID");
    }
    return result;
}
QJsonObject layerJson(const Layer &l) {
    QJsonArray effects;
    for (const auto &e : l.effects)
        effects.append(QJsonObject{{"id", idText(e.id)},
                                   {"type", e.type},
                                   {"name", e.name},
                                   {"enabled", e.enabled},
                                   {"parameters", parametersJson(e.parameters)}});
    return {{"id", idText(l.id)},
            {"name", l.name},
            {"kind", kindNames.at(int(l.kind))},
            {"source", idText(l.source)},
            {"parent", l.parent ? QJsonValue(idText(*l.parent)) : QJsonValue(QJsonValue::Null)},
            {"in", timeJson(l.in)},
            {"out", timeJson(l.out)},
            {"start", timeJson(l.start)},
            {"visible", l.visible},
            {"locked", l.locked},
            {"audioEnabled", l.audioEnabled},
            {"motionBlur", l.motionBlur},
            {"width", l.width},
            {"height", l.height},
            {"parameters", parametersJson(l.parameters)},
            {"effects", effects}};
}
Layer layerValue(QJsonValue v, int schema) {
    auto o =
        schema == 1   ? object(v, {"id", "name", "kind", "source", "parent", "in", "out", "start", "visible",
                                   "locked", "channels", "mask", "width", "height", "color", "text", "font"})
        : schema == 2 ? object(v, {"id", "name", "kind", "source", "parent", "in", "out", "start", "visible",
                                   "locked", "width", "height", "parameters", "effects"})
        : schema == 3 ? object(v, {"id", "name", "kind", "source", "parent", "in", "out", "start", "visible",
                                   "locked", "audioEnabled", "width", "height", "parameters", "effects"})
                      : object(v, {"id", "name", "kind", "source", "parent", "in", "out", "start", "visible",
                                   "locked", "audioEnabled", "motionBlur", "width", "height", "parameters",
                                   "effects"});
    Layer l;
    l.id = idValue(o["id"]);
    l.name = string(o["name"]);
    l.kind = LayerKind(enumValue(o["kind"], kindNames));
    l.source = idValue(o["source"]);
    if (!o["parent"].isNull())
        l.parent = idValue(o["parent"]);
    l.in = timeValue(o["in"]);
    l.out = timeValue(o["out"]);
    l.start = timeValue(o["start"]);
    l.visible = boolean(o["visible"]);
    l.locked = boolean(o["locked"]);
    if (schema >= 3)
        l.audioEnabled = boolean(o["audioEnabled"]);
    if (schema >= 4)
        l.motionBlur = boolean(o["motionBlur"]);
    l.width = integer(o["width"]);
    l.height = integer(o["height"]);
    if (schema >= 2) {
        l.parameters = parametersValue(o["parameters"]);
        if (schema == 2) {
            if (l.parameters.count("audio.levels"))
                bad("Unexpected audio parameter in legacy schema");
            l.parameters.emplace("audio.levels", defaultParameters(layerParameterSpecs()).at("audio.levels"));
        }
        for (auto v : array(o["effects"], 128)) {
            auto e = object(v, {"id", "type", "name", "enabled", "parameters"});
            const auto type = string(e["type"]);
            if (schema < 4 && type != "motion.linear-color")
                bad("Legacy project contains an unsupported effect type");
            l.effects.push_back({idValue(e["id"]), type, string(e["name"]),
                                 boolean(e["enabled"]), parametersValue(e["parameters"])});
        }
        return l;
    }
    auto channels = array(o["channels"], 8);
    if (channels.size() != 8)
        bad("Layer needs eight channels");
    for (int i = 0; i < 8; ++i)
        l.channel(Property(i)) = channelValue(channels[i]);
    l.width = integer(o["width"]);
    l.height = integer(o["height"]);
    auto color = string(o["color"]);
    if (!QRegularExpression("^#[0-9a-fA-F]{8}$").match(color).hasMatch())
        bad("Color needs eight hexadecimal digits");
    l.setColor(QColor(color));
    l.string("text.source") = string(o["text"]);
    auto font = object(o["font"], {"family", "style", "size", "lineSpacing", "alignment"});
    l.string("text.family") = string(font["family"]);
    l.string("text.style") = string(font["style"]);
    l.base("text.size") = number(font["size"]);
    l.base("text.leading") = number(font["lineSpacing"]);
    l.base("text.alignment") = integer(font["alignment"]);
    if (!o["mask"].isNull()) {
        auto m = object(o["mask"], {"shape", "mode", "inverted", "x", "y", "width", "height"});
        l.setMask(Mask{MaskShape(enumValue(m["shape"], {"rectangle", "ellipse"})),
                       MaskMode(enumValue(m["mode"], {"add", "subtract"})), boolean(m["inverted"]),
                       number(m["x"]), number(m["y"]), number(m["width"]), number(m["height"])});
    }
    return l;
}
QJsonValue workAreaJson(const Composition &c) {
    if (!c.workArea ||
        (c.workArea->start == time(0) && c.workArea->end == c.duration))
        return QJsonValue(QJsonValue::Null);
    return QJsonObject{{"start", timeJson(c.workArea->start)}, {"end", timeJson(c.workArea->end)}};
}
std::optional<WorkArea> workAreaValue(QJsonValue value, const Composition &c) {
    if (value.isNull())
        return std::nullopt;
    auto o = object(value, {"start", "end"});
    WorkArea result{timeValue(o["start"]), timeValue(o["end"])};
    if (result.start == time(0) && result.end == c.duration)
        return std::nullopt;
    return result;
}
QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error(("Cannot open " + path + ": " + f.errorString()).toStdString());
    if (f.size() > 32 * 1024 * 1024)
        bad("Project exceeds the 32 MiB input limit");
    return f.readAll();
}
QJsonDocument document(const QByteArray &bytes) {
    if (bytes.size() > 32 * 1024 * 1024)
        bad("Project exceeds the 32 MiB input limit");
    QJsonParseError error;
    auto d = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !d.isObject())
        bad("Malformed project JSON");
    return d;
}
} // namespace
QByteArray encodeProject(const Project &p) {
    validateProject(p);
    QJsonArray comps, assets;
    for (const auto &c : p.compositions) {
        QJsonArray layers;
        for (const auto &l : c.layers)
            layers.append(layerJson(l));
        comps.append(QJsonObject{{"id", idText(c.id)},
                                 {"name", c.name},
                                 {"width", c.width},
                                 {"height", c.height},
                                 {"fps", timeJson(c.fps)},
                                 {"duration", timeJson(c.duration)},
                                 {"motionBlurEnabled", c.motionBlurEnabled},
                                 {"shutterAngle", c.shutterAngle},
                                 {"shutterPhase", c.shutterPhase},
                                 {"motionBlurSamples", c.motionBlurSamples},
                                 {"workArea", workAreaJson(c)},
                                 {"layers", layers}});
    }
    for (const auto &a : p.assets) {
        QJsonArray peaks;
        for (double v : a.waveform)
            peaks.append(v);
        assets.append(QJsonObject{{"id", idText(a.id)},
                                  {"path", a.path},
                                  {"sha256", a.sha256},
                                  {"width", a.width},
                                  {"height", a.height},
                                  {"assumedSrgb", a.assumedSrgb},
                                  {"kind", assetKinds.at(int(a.kind))},
                                  {"duration", timeJson(a.duration)},
                                  {"fps", timeJson(a.fps)},
                                  {"hasAudio", a.hasAudio},
                                  {"audioChannels", a.audioChannels},
                                  {"audioRate", a.audioRate},
                                  {"variableRate", a.variableRate},
                                  {"waveform", peaks}});
    }
    auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", p.schemaVersion},
                                           {"nextId", idText(p.nextId)},
                                           {"compositions", comps},
                                           {"assets", assets}})
                     .toJson();
    if (bytes.size() > 32 * 1024 * 1024)
        bad("Serialized project exceeds 32 MiB");
    return bytes;
}
Project decodeProject(const QByteArray &bytes) {
    auto o = object(document(bytes).object(), {"schemaVersion", "nextId", "compositions", "assets"});
    Project p;
    const int schema = integer(o["schemaVersion"]);
    if (schema < 1 || schema > 5)
        bad("Unsupported project schema");
    p.schemaVersion = 5;
    p.nextId = idValue(o["nextId"]);
    p.compositions.clear();
    for (auto v : array(o["assets"], 4096)) {
        auto a =
            schema < 3
                ? object(v, {"id", "path", "sha256", "width", "height", "assumedSrgb"})
                : object(v, {"id", "path", "sha256", "width", "height", "assumedSrgb", "kind", "duration",
                             "fps", "hasAudio", "audioChannels", "audioRate", "variableRate", "waveform"});
        Asset result{idValue(a["id"]),    string(a["path"]),    string(a["sha256"]),
                     integer(a["width"]), integer(a["height"]), boolean(a["assumedSrgb"])};
        if (schema >= 3) {
            result.kind = AssetKind(enumValue(a["kind"], assetKinds));
            result.duration = timeValue(a["duration"]);
            result.fps = timeValue(a["fps"]);
            result.hasAudio = boolean(a["hasAudio"]);
            result.audioChannels = integer(a["audioChannels"]);
            result.audioRate = integer(a["audioRate"]);
            result.variableRate = boolean(a["variableRate"]);
            for (auto peak : array(a["waveform"], 128))
                result.waveform.push_back(number(peak));
        }
        p.assets.push_back(std::move(result));
    }
    for (auto v : array(o["compositions"], 256)) {
        auto c = schema >= 5 ? object(v, {"id", "name", "width", "height", "fps", "duration",
                                          "motionBlurEnabled", "shutterAngle", "shutterPhase",
                                          "motionBlurSamples", "workArea", "layers"})
                : schema >= 4 ? object(v, {"id", "name", "width", "height", "fps", "duration",
                                           "motionBlurEnabled", "shutterAngle", "shutterPhase",
                                           "motionBlurSamples", "layers"})
                              : object(v, {"id", "name", "width", "height", "fps", "duration", "layers"});
        Composition out;
        out.id = idValue(c["id"]);
        out.name = string(c["name"]);
        out.width = integer(c["width"]);
        out.height = integer(c["height"]);
        out.fps = timeValue(c["fps"]);
        out.duration = timeValue(c["duration"]);
        if (schema >= 4) {
            out.motionBlurEnabled = boolean(c["motionBlurEnabled"]);
            out.shutterAngle = integer(c["shutterAngle"]);
            out.shutterPhase = integer(c["shutterPhase"]);
            out.motionBlurSamples = integer(c["motionBlurSamples"]);
        }
        if (schema >= 5)
            out.workArea = workAreaValue(c["workArea"], out);
        for (auto l : array(c["layers"], 1024))
            out.layers.push_back(layerValue(l, schema));
        p.compositions.push_back(out);
    }
    validateProject(p);
    return p;
}
QString projectDirectory(const QString &path) {
    // Relative media paths must use the physical parent (macOS /var is a symlink).
    const auto directory = QFileInfo(path).absoluteDir().canonicalPath();
    if (directory.isEmpty())
        throw std::runtime_error("Project directory does not exist");
    return directory;
}
Project loadProject(const QString &path) { return decodeProject(readFile(path)); }
void atomicWrite(const QString &path, const QByteArray &bytes) {
    QSaveFile f(path);
    f.setDirectWriteFallback(false);
    if (!f.open(QIODevice::WriteOnly))
        throw std::runtime_error(("Cannot save " + path + ": " + f.errorString()).toStdString());
    if (f.write(bytes) != bytes.size()) {
        f.cancelWriting();
        throw std::runtime_error("Incomplete write; original file retained");
    }
    if (!f.commit())
        throw std::runtime_error(("Save could not commit: " + f.errorString()).toStdString());
}
void saveProject(const Project &p, const QString &path) { atomicWrite(path, encodeProject(p)); }
QString saveRecovery(const Project &p, const QString &identity, std::uint64_t revision,
                     const QString &directory) {
    if (!QRegularExpression("^[a-zA-Z0-9-]{1,100}$").match(identity).hasMatch())
        bad("Invalid recovery identity");
    QDir dir(directory);
    if (!dir.mkpath(identity))
        bad("Cannot create recovery directory");
    dir.cd(identity);
    auto now = QDateTime::currentDateTimeUtc();
    QString path =
        dir.filePath(now.toString("yyyyMMddTHHmmsszzz") + "-" + QString::number(revision) + ".json");
    atomicWrite(path, QJsonDocument(QJsonObject{{"recoveryVersion", 1},
                                                {"projectIdentity", identity},
                                                {"revision", QString::number(revision)},
                                                {"savedAtUtc", now.toString(Qt::ISODateWithMs)},
                                                {"project", document(encodeProject(p)).object()}})
                          .toJson());
    auto old = dir.entryList({"*.json"}, QDir::Files, QDir::Name);
    while (old.size() > 3) {
        dir.remove(old.takeFirst());
    }
    return path;
}
Project loadRecovery(const QString &path) {
    auto o = object(document(readFile(path)).object(),
                    {"recoveryVersion", "projectIdentity", "revision", "savedAtUtc", "project"});
    if (integer(o["recoveryVersion"]) != 1)
        bad("Unsupported recovery version");
    return decodeProject(QJsonDocument(o["project"].toObject()).toJson());
}
} // namespace motion
