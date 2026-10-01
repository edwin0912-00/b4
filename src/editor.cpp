// SPDX-License-Identifier: MPL-2.0
#include "editor.hpp"
#include "evaluate.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
namespace motion {
namespace {
void unlocked(const Layer &l) {
    if (l.locked)
        throw std::runtime_error("Unlock this layer before editing");
}
void adjustHandles(Channel &c) {
    for (size_t i = 0; i + 1 < c.keys.size(); ++i) {
        auto &a = c.keys[i];
        auto &b = c.keys[i + 1];
        double d = seconds(b.at - a.at);
        a.outHandle.dtSeconds = std::clamp(a.outHandle.dtSeconds, 0.0, d);
        b.inHandle.dtSeconds = std::clamp(b.inHandle.dtSeconds, -d, 0.0);
    }
}
size_t keyIndex(const Channel &ch, Time at) {
    auto it =
        std::lower_bound(ch.keys.begin(), ch.keys.end(), at, [](const Key &k, Time t) { return k.at < t; });
    if (it == ch.keys.end() || !(it->at == at))
        throw std::runtime_error("Keyframe no longer exists");
    return size_t(it - ch.keys.begin());
}
void cubicSegment(Key &a, Key &b) {
    if (a.outgoing != Interpolation::Cubic) {
        const double dt = seconds(b.at - a.at) / 3, dv = (b.value - a.value) / 3;
        a.outHandle = {dt, dv};
        b.inHandle = {-dt, -dv};
        a.outgoing = Interpolation::Cubic;
    }
}
class DocumentCommand final : public QUndoCommand {
    Editor *editor_;
    Project before_, after_;

  public:
    DocumentCommand(Editor *e, Project before, Project after, const QString &name)
        : QUndoCommand(name), editor_(e), before_(std::move(before)), after_(std::move(after)) {}
    void undo() override { editor_->install(before_); }
    void redo() override { editor_->install(after_); }
};
} // namespace
std::optional<KeyVelocity> keyVelocity(const Project &p, KeyRef ref, bool incoming) {
    const auto &ch = layer(p, ref.layer).channel(ref.property);
    const auto i = keyIndex(ch, ref.at);
    if ((incoming && i == 0) || (!incoming && i + 1 == ch.keys.size()))
        return {};
    const auto &a = ch.keys[incoming ? i - 1 : i], &b = ch.keys[incoming ? i : i + 1];
    const double duration = seconds(b.at - a.at);
    if (a.outgoing == Interpolation::Hold)
        return KeyVelocity{0, 0};
    if (a.outgoing == Interpolation::Linear)
        return KeyVelocity{(b.value - a.value) / duration, 100. / 3};
    const auto h = incoming ? b.inHandle : a.outHandle;
    if (h.dtSeconds == 0)
        return {}; // Collapsed/vertical handles stay editable in the Value Graph.
    const double speed = h.dv / h.dtSeconds;
    if (!std::isfinite(speed))
        return {};
    return KeyVelocity{speed, std::clamp(std::abs(h.dtSeconds) / duration * 100, 0., 100.)};
}
void setKeyVelocity(Project &p, KeyRef ref, bool incoming, KeyVelocity velocity) {
    auto &l = layer(p, ref.layer);
    unlocked(l);
    if (discreteProperty(ref.property))
        throw std::runtime_error("Discrete properties require Hold interpolation");
    if (!std::isfinite(velocity.speed) || !std::isfinite(velocity.influence) || velocity.influence < .1 ||
        velocity.influence > 100)
        throw std::runtime_error("Velocity must be finite; influence must be between 0.1 and 100 percent");
    auto &ch = l.channel(ref.property);
    const auto i = keyIndex(ch, ref.at);
    if ((incoming && i == 0) || (!incoming && i + 1 == ch.keys.size()))
        return;
    auto &a = ch.keys[incoming ? i - 1 : i], &b = ch.keys[incoming ? i : i + 1];
    cubicSegment(a, b);
    const double dt = seconds(b.at - a.at) * (velocity.influence / 100) * (incoming ? -1 : 1);
    (incoming ? b.inHandle : a.outHandle) = {dt, dt * velocity.speed};
}
void setKeyInterpolation(Project &p, KeyRef ref, Interpolation mode) {
    auto &l = layer(p, ref.layer);
    unlocked(l);
    if (discreteProperty(ref.property) && mode != Interpolation::Hold)
        throw std::runtime_error("Discrete properties require Hold interpolation");
    auto &ch = l.channel(ref.property);
    const auto i = keyIndex(ch, ref.at);
    if (mode == Interpolation::Cubic && i + 1 < ch.keys.size())
        cubicSegment(ch.keys[i], ch.keys[i + 1]);
    ch.keys[i].outgoing = mode;
}
void easeKey(Project &p, KeyRef ref, bool incoming, bool outgoing) {
    if (incoming)
        setKeyVelocity(p, ref, true, {0, 100. / 3});
    if (outgoing)
        setKeyVelocity(p, ref, false, {0, 100. / 3});
}
void putKey(Channel &c, Key key) {
    auto it =
        std::lower_bound(c.keys.begin(), c.keys.end(), key.at, [](const Key &a, Time t) { return a.at < t; });
    if (it != c.keys.end() && it->at == key.at)
        *it = key;
    else
        c.keys.insert(it, key);
    adjustHandles(c);
}
void eraseKey(Channel &c, Time t) {
    c.keys.erase(std::remove_if(c.keys.begin(), c.keys.end(), [&](const Key &k) { return k.at == t; }),
                 c.keys.end());
    adjustHandles(c);
}
void moveKey(Channel &c, Time from, Time to) {
    auto it = std::find_if(c.keys.begin(), c.keys.end(), [&](const Key &k) { return k.at == from; });
    if (it == c.keys.end())
        throw std::runtime_error("Keyframe no longer exists");
    Key key = *it;
    eraseKey(c, from);
    key.at = to;
    putKey(c, key);
}
void nativeEase(Channel &c, Time from, Time to) {
    for (size_t i = 0; i + 1 < c.keys.size(); ++i) {
        auto &a = c.keys[i];
        auto &b = c.keys[i + 1];
        if (a.at < from || to < b.at)
            continue;
        double d = seconds(b.at - a.at);
        a.outgoing = Interpolation::Cubic;
        a.outHandle = {d / 3, 0};
        b.inHandle = {-d / 3, 0};
    }
}
QByteArray copyPropertyValues(const Project &p, Id id, const std::vector<PropertyRef> &refs, Time at) {
    const auto &l = layer(p, id);
    QJsonArray values;
    for (auto ref : refs)
        values.append(toDisplay(ref, valueAt(l.channel(ref), sourceTime(l, at))));
    return QJsonDocument(values).toJson(QJsonDocument::Compact);
}
void pastePropertyValues(Project &p, Id id, const std::vector<PropertyRef> &refs, Time at,
                         const QByteArray &bytes) {
    if (bytes.size() > 1024)
        throw std::runtime_error("Clipboard value is too large");
    const auto doc = QJsonDocument::fromJson(bytes);
    if (!doc.isArray() || doc.array().size() != int(refs.size()) || refs.empty())
        throw std::runtime_error("Clipboard dimensions do not match this property");
    auto &l = layer(p, id);
    unlocked(l);
    const auto values = doc.array();
    for (size_t i = 0; i < refs.size(); ++i) {
        if (!values[int(i)].isDouble())
            throw std::runtime_error("Clipboard needs numeric values");
        const double value = fromDisplay(refs[i], values[int(i)].toDouble());
        if (refs[i].id == "transform.opacity" && (value < 0 || value > 1))
            throw std::runtime_error("Opacity must be between 0 and 100 percent");
        // Pasting a vector restores its explicit components, independent of the Scale drag link.
        setProperty(p, id, refs[i], sourceTime(l, at), value);
    }
}
void setProperty(Project &p, Id id, PropertyRef prop, Time at, double value) {
    auto &l = layer(p, id);
    unlocked(l);
    auto &c = l.channel(prop);
    if (c.keys.empty())
        c.base = value;
    else {
        auto it = std::find_if(c.keys.begin(), c.keys.end(), [&](const Key &k) { return k.at == at; });
        if (it != c.keys.end())
            it->value = value;
        else
            putKey(c,
                   {at, value, discreteProperty(prop) ? Interpolation::Hold : Interpolation::Linear, {}, {}});
    }
}
void setAnimated(Project &p, Id id, PropertyRef prop, Time at, bool enabled) {
    auto &l = layer(p, id);
    unlocked(l);
    if (!parameterSpec(prop.id).animated)
        throw std::runtime_error("Parameter does not support animation");
    auto &ch = l.channel(prop);
    const double value = valueAt(ch, at);
    if (enabled && ch.keys.empty())
        putKey(ch, {at, value, discreteProperty(prop) ? Interpolation::Hold : Interpolation::Linear});
    if (!enabled) {
        ch.base = value;
        ch.keys.clear();
    }
}
void resetProperty(Project &p, Id id, PropertyRef prop) {
    auto &l = layer(p, id);
    unlocked(l);
    l.channel(prop) = {parameterSpec(prop.id).defaults.at(prop.component), {}};
}
Id addEffect(Project &p, Id id) { return addEffect(p, id, "motion.linear-color"); }
Id addEffect(Project &p, Id id, const QString &type) {
    auto &l = layer(p, id);
    unlocked(l);
    const auto &spec = effectSpec(type);
    Effect effect;
    effect.id = p.nextId++;
    effect.type = spec.type;
    effect.name = spec.name;
    effect.parameters = defaultParameters(effectParameterSpecs(type));
    l.effects.push_back(effect);
    return effect.id;
}
void moveEffect(Project &p, Id id, Id effect, int offset) {
    auto &l = layer(p, id);
    unlocked(l);
    auto it =
        std::find_if(l.effects.begin(), l.effects.end(), [&](const Effect &e) { return e.id == effect; });
    if (it == l.effects.end())
        throw std::runtime_error("Effect does not exist");
    const auto from = it - l.effects.begin(), to = from + offset;
    if (to < 0 || to >= static_cast<std::ptrdiff_t>(l.effects.size()))
        return;
    std::iter_swap(it, l.effects.begin() + to);
}
void reparent(Project &p, Id comp, Id child, std::optional<Id> parent, Time at) {
    unlocked(layer(p, child));
    Project proposed = p;
    layer(proposed, child).parent = parent;
    validateProject(proposed);
    const auto old = worldTransform(p, comp, child, at);
    const auto local = parent ? multiply(inverse(worldTransform(p, comp, *parent, at)), old) : old;
    const auto &v = local.values;
    double sx = std::hypot(v[0], v[3]), other = std::hypot(v[1], v[4]);
    if (sx < 1e-12 || other < 1e-12)
        throw std::runtime_error("Cannot preserve a singular transform while parenting");
    if (std::abs(v[0] * v[1] + v[3] * v[4]) > 1e-8 * sx * other)
        throw std::runtime_error("Parenting would require unsupported shear; document unchanged");
    double sy = (v[0] * v[4] - v[1] * v[3]) / sx, angle = std::atan2(v[3], v[0]) * 180 / std::numbers::pi;
    const auto &l = layer(p, child);
    Time localTime = sourceTime(l, at);
    double ax = valueAt(l.channel(Property(0)), localTime), ay = valueAt(l.channel(Property(1)), localTime);
    setProperty(proposed, child, Property::PositionX, localTime, v[2] + v[0] * ax + v[1] * ay);
    setProperty(proposed, child, Property::PositionY, localTime, v[5] + v[3] * ax + v[4] * ay);
    setProperty(proposed, child, Property::ScaleX, localTime, sx);
    setProperty(proposed, child, Property::ScaleY, localTime, sy);
    setProperty(proposed, child, Property::Rotation, localTime, angle);
    validateProject(proposed);
    p = std::move(proposed);
}
Id duplicateLayer(Project &p, Id comp, Id source) {
    auto &c = composition(p, comp);
    auto it = std::find_if(c.layers.begin(), c.layers.end(), [&](const Layer &l) { return l.id == source; });
    if (it == c.layers.end())
        throw std::runtime_error("Selected layer is not in this composition");
    unlocked(*it);
    Layer copy = *it;
    copy.id = p.nextId++;
    for (auto &e : copy.effects)
        e.id = p.nextId++;
    copy.name += " copy";
    Id id = copy.id;
    c.layers.insert(it, copy);
    return id;
}
Id precompose(Project &p, Id comp, const std::vector<Id> &selected) {
    if (selected.empty())
        throw std::runtime_error("Select at least one layer");
    std::set<Id> chosen(selected.begin(), selected.end());
    const auto &original = composition(p, comp);
    std::vector<size_t> indices;
    for (size_t i = 0; i < original.layers.size(); ++i)
        if (chosen.count(original.layers[i].id)) {
            unlocked(original.layers[i]);
            indices.push_back(i);
        }
    if (indices.size() != chosen.size() || indices.back() - indices.front() + 1 != indices.size())
        throw std::runtime_error("Precompose requires contiguous layers in this composition");
    for (const auto &l : original.layers)
        if (l.parent && chosen.count(l.id) != chosen.count(*l.parent))
            throw std::runtime_error("Parent links cannot cross the precompose boundary");
    Project next = p;
    auto &outer = composition(next, comp);
    Composition inner = outer;
    resetWorkArea(inner);
    inner.id = next.nextId++;
    inner.name = "Precomp " + QString::number(inner.id);
    inner.layers.assign(outer.layers.begin() + indices.front(), outer.layers.begin() + indices.back() + 1);
    Layer nest;
    nest.id = next.nextId++;
    nest.kind = LayerKind::Precomp;
    nest.source = inner.id;
    nest.name = inner.name;
    nest.width = outer.width;
    nest.height = outer.height;
    nest.out = outer.duration;
    outer.layers.erase(outer.layers.begin() + indices.front(), outer.layers.begin() + indices.back() + 1);
    outer.layers.insert(outer.layers.begin() + indices.front(), nest);
    next.compositions.push_back(inner);
    validateProject(next);
    p = std::move(next);
    return nest.id;
}
Editor::Editor(Project p, QObject *parent) : QObject(parent), document_(std::move(p)), undo_(this) {
    validateProject(document_);
    // ponytail: whole-document snapshots, capped at 100 commands; use delta commands if metadata grows.
    undo_.setUndoLimit(100);
    // ponytail: keys are identified by time; clear selection on undo after merges.
    // Stable key IDs are needed if history must restore key selection itself.
    connect(&undo_, &QUndoStack::indexChanged, this, [this, previous = 0](int index) mutable {
        if (index < previous)
            emit keySelectionReset();
        previous = index;
    });
}
void Editor::select(std::vector<Id> ids) {
    selection_ = std::move(ids);
    emit selectionChanged();
}
void Editor::install(Project p) {
    document_ = std::move(p);
    ++revision_;
    std::erase_if(selection_, [&](Id id) {
        try {
            layer(document_, id);
            return false;
        } catch (const std::exception &) {
            return true;
        }
    });
    emit changed();
}
void Editor::setNumericValue(Project &p, Id id, PropertyRef ref, Time at, double value) const {
    if (!ref.effect && ref.id == "transform.opacity" && (value < 0 || value > 1))
        throw std::runtime_error("Opacity must be between 0 and 100 percent");
    if (scaleLinked_ && !ref.effect && ref.id == "transform.scale") {
        auto &l = layer(p, id);
        const double original = valueAt(l.channel(ref), at);
        PropertyRef other{ref.id, 1 - ref.component};
        const double otherValue = valueAt(l.channel(other), at);
        if (original == 0 && otherValue != 0)
            throw std::runtime_error("Unlink Scale to edit an asymmetric zero scale");
        const double linked = original == 0 ? value : otherValue * value / original;
        motion::setProperty(p, id, other, at, linked);
    }
    motion::setProperty(p, id, ref, at, value);
}
void Editor::apply(const QString &name, std::function<void(Project &)> f) {
    if (gesture_)
        throw std::runtime_error("Finish the active drag first");
    Project next = document_;
    f(next);
    validateProject(next);
    if (encodeProject(next) == encodeProject(document_))
        return;
    undo_.push(new DocumentCommand(this, document_, std::move(next), name));
}
void Editor::beginGesture() {
    if (!gesture_)
        gesture_ = document_;
}
void Editor::previewGesture(std::function<void(Project &)> f) {
    if (!gesture_)
        throw std::runtime_error("No active gesture");
    Project next = *gesture_;
    f(next);
    validateProject(next);
    install(std::move(next));
}
void Editor::commitGesture(const QString &name) {
    if (!gesture_)
        return;
    Project before = std::move(*gesture_);
    gesture_.reset();
    if (encodeProject(before) != encodeProject(document_))
        undo_.push(new DocumentCommand(this, std::move(before), document_, name));
}
void Editor::cancelGesture() {
    if (gesture_) {
        auto before = std::move(*gesture_);
        gesture_.reset();
        install(std::move(before));
    }
}
void Editor::replace(Project p) {
    validateProject(p);
    cancelGesture();
    undo_.clear();
    selection_.clear();
    emit documentReplaced();
    install(std::move(p));
}
} // namespace motion
