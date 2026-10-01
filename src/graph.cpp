// SPDX-License-Identifier: MPL-2.0
#include "graph.hpp"
#include "evaluate.hpp"
#include "keyframe_dialog.hpp"
#include "numeric_control.hpp"
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <limits>
namespace motion {
Graph::Graph(Editor *e, QWidget *parent) : QWidget(parent), editor_(e) {
    setMinimumSize(400, 220);
    setFocusPolicy(Qt::StrongFocus);
    connect(e, &Editor::keySelectionReset, this, [this] {
        selectedTime_.reset();
        update();
    });
    connect(e, &Editor::changed, this, [this] {
        if (!dragging_ && !selectedKey())
            selectedTime_.reset();
        update();
    });
    connect(e, &Editor::selectionChanged, this, [this] {
        const auto *l = selected();
        const auto id = l ? l->id : 0;
        if (id != selectedLayer_)
            selectedTime_.reset();
        selectedLayer_ = id;
        update();
    });
}
const Layer *Graph::selected() const {
    if (editor_->selection().empty())
        return nullptr;
    try {
        const auto &l = layer(editor_->project(), editor_->selection().front());
        l.channel(property_);
        return &l;
    } catch (const std::exception &) {
        return nullptr;
    }
}
std::optional<KeyRef> Graph::selectedKey() const {
    const auto *l = selected();
    if (!l || !selectedTime_)
        return {};
    const auto &keys = l->channel(property_).keys;
    if (std::none_of(keys.begin(), keys.end(), [&](const Key &k) { return k.at == *selectedTime_; }))
        return {};
    return KeyRef{l->id, property_, *selectedTime_};
}
void Graph::selectKey(KeyRef ref) {
    setProperty(ref.property);
    const auto *l = selected();
    selectedTime_ = l && l->id == ref.layer ? std::optional<Time>(ref.at) : std::nullopt;
    update();
}
double Graph::graphValue(const Channel &ch, Time at) const {
    return speedGraph_ ? velocityAt(ch, at).value_or(std::numeric_limits<double>::quiet_NaN())
                       : valueAt(ch, at);
}
QPointF Graph::keyPosition(Time at) {
    range();
    const auto *l = selected();
    if (!l)
        return {-100, -100};
    const double value = graphValue(l->channel(property_), at);
    return std::isfinite(value) ? point(seconds(at + l->start), value) : QPointF(-100, -100);
}
void Graph::editVelocity() {
    try {
        auto ref = selectedKey();
        if (!ref)
            throw std::runtime_error("Select a graph keyframe first");
        editKeyframeVelocity(editor_, {*ref}, this);
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}
void Graph::easeSelected(bool incoming, bool outgoing) {
    try {
        auto ref = selectedKey();
        if (!ref)
            throw std::runtime_error("Select a graph keyframe first");
        editor_->apply("Ease graph keyframe", [&](Project &p) { easeKey(p, *ref, incoming, outgoing); });
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}
void Graph::interpolateSelected(Interpolation mode) {
    try {
        auto ref = selectedKey();
        if (!ref)
            throw std::runtime_error("Select a graph keyframe first");
        editor_->apply("Graph keyframe interpolation",
                       [&](Project &p) { setKeyInterpolation(p, *ref, mode); });
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}
void Graph::range() {
    try {
        duration_ = seconds(composition(editor_->project(), comp_).duration);
    } catch (const std::exception &) {
        return;
    }
    auto *l = selected();
    if (!l)
        return;
    const auto &channel = l->channel(property_);
    low_ = high_ = speedGraph_ ? 0 : channel.base;
    if (speedGraph_) {
        for (int i = 0; i <= 400; ++i) {
            const double v = graphValue(channel, fromSeconds(duration_ * i / 400) - l->start);
            if (std::isfinite(v)) {
                low_ = std::min(low_, v);
                high_ = std::max(high_, v);
            }
        }
    } else
        for (const auto &k : channel.keys) {
            low_ = std::min({low_, k.value, k.value + k.inHandle.dv, k.value + k.outHandle.dv});
            high_ = std::max({high_, k.value, k.value + k.inHandle.dv, k.value + k.outHandle.dv});
        }
    double margin = std::max(1.0, (high_ - low_) * .15);
    low_ -= margin;
    high_ += margin;
}
QPointF Graph::point(double t, double v) const {
    return {60 + t / duration_ * (width() - 80),
            height() - 30 - (v - low_) / (high_ - low_) * (height() - 65)};
}
Vec2 Graph::graphPoint(QPointF p) const {
    return {(p.x() - 60) / std::max(1, width() - 80) * duration_,
            low_ + (height() - 30 - p.y()) / std::max(1, height() - 65) * (high_ - low_)};
}
void Graph::paintEvent(QPaintEvent *) {
    if (!dragging_)
        range();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#232323"));
    p.setPen(QColor("#c9c9c9"));
    p.drawText(15, 21,
               (speedGraph_ ? QString("Speed · ") : QString("Value · ")) + propertyLabel(property_) +
                   (speedGraph_ ? " / s — edit with Keyframe Velocity" : ""));
    for (int i = 0; i <= 4; ++i) {
        double v = low_ + (high_ - low_) * i / 4;
        double y = point(0, v).y();
        p.setPen(QColor("#343434"));
        p.drawLine(QPointF(55, y), QPointF(width() - 15, y));
        p.setPen(QColor("#b0b0b0"));
        p.drawText(QRectF(0, y - 10, 50, 20), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(toDisplay(property_, v), 'g', 4));
    }
    const auto &compositions = editor_->project().compositions;
    const auto found = std::find_if(compositions.begin(), compositions.end(),
                                    [this](const Composition &c) { return c.id == comp_; });
    if (found == compositions.end()) {
        p.drawText(rect(), Qt::AlignCenter, "Select a composition");
        return;
    }
    const auto &comp = *found;
    const auto nominal = std::max<qint64>(1, std::llround(seconds(comp.fps)));
    const double needed = std::max(1., duration_ * seconds(comp.fps) * 85 / std::max(1, width() - 80));
    const double power = std::pow(10., std::floor(std::log10(needed)));
    qint64 step = std::llround(10 * power);
    for (double multiplier : {1., 2., 5., 10.})
        if (multiplier * power >= needed) {
            step = std::llround(multiplier * power);
            break;
        }
    for (qint64 frame = 0; frame <= duration_ * seconds(comp.fps); frame += step) {
        const auto x = point(seconds(frameTime(frame, comp.fps)), low_).x();
        p.setPen(QColor("#303030"));
        p.drawLine(QPointF(x, 35), QPointF(x, height() - 30));
        p.setPen(QColor("#aaaaaa"));
        const auto label =
            frame / nominal < 3600 ? frameText(frame, nominal).section(':', 1) : frameText(frame, nominal);
        if (x + p.fontMetrics().horizontalAdvance(label) < width())
            p.drawText(QPointF(x + 2, height() - 10), label);
    }
    const auto *l = selected();
    if (!l) {
        p.drawText(rect(), Qt::AlignCenter, "Select a layer and property to edit its value curve");
        return;
    }
    const auto &ch = l->channel(property_);
    QPainterPath path;
    bool started = false;
    for (int i = 0; i <= 400; ++i) {
        double t = duration_ * i / 400;
        const double v = graphValue(ch, fromSeconds(t) - l->start);
        if (!std::isfinite(v)) {
            started = false;
            continue;
        }
        auto pt = point(t, v);
        if (!started)
            path.moveTo(pt);
        else
            path.lineTo(pt);
        started = true;
    }
    p.setPen(QPen(QColor("#49a3ec"), 2));
    p.drawPath(path);
    for (size_t i = 0; i < ch.keys.size(); ++i) {
        const auto &k = ch.keys[i];
        double kt = seconds(k.at + l->start);
        const double v = graphValue(ch, k.at);
        if (!std::isfinite(v))
            continue;
        auto center = point(kt, v);
        p.setPen(QPen(QColor("#8aa5b7"), 1));
        p.setBrush(QColor("#252525"));
        if (!speedGraph_ && i && ch.keys[i - 1].outgoing == Interpolation::Cubic) {
            auto in = point(kt + k.inHandle.dtSeconds, k.value + k.inHandle.dv);
            p.drawLine(center, in);
            p.drawEllipse(in, 4, 4);
        }
        if (!speedGraph_ && i + 1 < ch.keys.size() && k.outgoing == Interpolation::Cubic) {
            auto out = point(kt + k.outHandle.dtSeconds, k.value + k.outHandle.dv);
            p.drawLine(center, out);
            p.drawEllipse(out, 4, 4);
        }
        p.setBrush(QColor(selectedTime_ && k.at == *selectedTime_ ? "#43a5f5" : "#b8b8b8"));
        p.setPen(Qt::NoPen);
        p.drawEllipse(center, 5, 5);
    }
    p.setPen(QPen(QColor("#efbb74"), 1));
    p.drawLine(point(seconds(now_), low_), point(seconds(now_), high_));
    if (hasFocus()) {
        p.setPen(QColor("#248ddd"));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}
void Graph::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton)
        return;
    setFocus();
    const auto *l = selected();
    if (!l)
        return;
    range();
    const auto &keys = l->channel(property_).keys;
    selectedTime_.reset();
    key_ = -1;
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto &k = keys[i];
        double kt = seconds(k.at + l->start);
        for (int handle : {0, -1, 1}) {
            if (speedGraph_ && handle != 0)
                continue;
            if (handle == -1 && (!i || keys[i - 1].outgoing != Interpolation::Cubic))
                continue;
            if (handle == 1 && (i + 1 == keys.size() || k.outgoing != Interpolation::Cubic))
                continue;
            double t = kt, v = graphValue(l->channel(property_), k.at);
            if (!std::isfinite(v))
                continue;
            if (handle == -1) {
                t += k.inHandle.dtSeconds;
                v += k.inHandle.dv;
            }
            if (handle == 1) {
                t += k.outHandle.dtSeconds;
                v += k.outHandle.dv;
            }
            if (QLineF(point(t, v), event->position()).length() < 9) {
                selectedTime_ = k.at;
                emit keySelected({l->id, property_, k.at});
                if (speedGraph_) {
                    update();
                    return;
                }
                if (l->locked) {
                    emit error("Unlock the layer before editing the curve");
                    return;
                }
                key_ = int(i);
                handle_ = handle;
                layer_ = l->id;
                original_ = k;
                dragging_ = true;
                editor_->beginGesture();
                update();
                return;
            }
        }
    }
    auto coord = graphPoint(event->position());
    emit timeChanged(fromSeconds(std::clamp(coord.x, 0.0, duration_)));
}
void Graph::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging_)
        return;
    try {
        auto coord = graphPoint(event->position());
        Time selectedAt = original_.at;
        editor_->previewGesture([&](Project &p) {
            auto &l = layer(p, layer_);
            auto &ch = l.channel(property_);
            auto &k = ch.keys.at(size_t(key_));
            if (handle_ == 0) {
                Key moved = k;
                const auto &comp = composition(p, comp_);
                auto compTime = frameTime(
                    std::llround(std::clamp(coord.x, 0.0, duration_) * seconds(comp.fps)), comp.fps);
                moved.at = compTime - l.start;
                selectedAt = moved.at;
                const auto &spec = parameterSpec(property_.id);
                moved.value = std::clamp(discreteProperty(property_) ? std::round(coord.y) : coord.y,
                                         spec.minimum, spec.maximum);
                eraseKey(ch, original_.at);
                putKey(ch, moved);
            } else if (handle_ == 1) {
                double maxTime = seconds(ch.keys.at(size_t(key_ + 1)).at - k.at);
                k.outHandle = {std::clamp(coord.x - seconds(k.at + l.start), 0.0, maxTime),
                               coord.y - k.value};
            } else {
                const auto &previous = ch.keys.at(size_t(key_ - 1));
                double minTime = seconds(previous.at - k.at);
                k.inHandle = {std::clamp(coord.x - seconds(k.at + l.start), minTime, 0.0), coord.y - k.value};
            }
        });
        selectedTime_ = selectedAt;
        emit keySelected({layer_, property_, selectedAt});
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}
void Graph::mouseReleaseEvent(QMouseEvent *) {
    if (dragging_) {
        dragging_ = false;
        editor_->commitGesture("Edit value curve");
        update();
    }
}
void Graph::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        dragging_ = false;
        editor_->cancelGesture();
        selectedTime_ = original_.at;
        if (auto ref = selectedKey())
            emit keySelected(*ref);
        update();
        return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (auto ref = selectedKey())
            try {
                editor_->apply("Delete graph key", [&](Project &p) {
                    auto &l = layer(p, ref->layer);
                    if (l.locked)
                        throw std::runtime_error("Unlock layer first");
                    eraseKey(l.channel(ref->property), ref->at);
                });
            } catch (const std::exception &e) {
                emit error(e.what());
            }
        return;
    }
    QWidget::keyPressEvent(event);
}
void Graph::contextMenuEvent(QContextMenuEvent *event) {
    const auto *l = selected();
    if (!l)
        return;
    for (const auto &k : l->channel(property_).keys)
        if (QLineF(keyPosition(k.at), event->pos()).length() < 10) {
            selectedTime_ = k.at;
            emit keySelected({l->id, property_, k.at});
            break;
        }
    QMenu menu;
    auto *add = menu.addAction("Add key at this time");
    auto *velocity = menu.addAction("Keyframe Velocity…");
    auto *ease = menu.addAction("Ease selected key");
    auto *easeIn = menu.addAction("Ease In");
    auto *easeOut = menu.addAction("Ease Out");
    menu.addSeparator();
    auto *linear = menu.addAction("Outgoing segment: Linear");
    auto *hold = menu.addAction("Outgoing segment: Hold");
    auto *cubic = menu.addAction("Outgoing segment: Bezier");
    auto *remove = menu.addAction("Delete selected key");
    const bool valid = selectedKey().has_value() && !l->locked;
    add->setEnabled(!l->locked);
    for (auto *a : {hold, remove})
        a->setEnabled(valid);
    for (auto *a : {velocity, ease, easeIn, easeOut, linear, cubic})
        a->setEnabled(valid && !discreteProperty(property_));
    auto *result = menu.exec(event->globalPos());
    if (!result)
        return;
    if (result == velocity) {
        editVelocity();
        return;
    }
    if (result == ease || result == easeIn || result == easeOut) {
        easeSelected(result != easeOut, result != easeIn);
        return;
    }
    if (result == hold || result == linear || result == cubic) {
        interpolateSelected(result == hold     ? Interpolation::Hold
                            : result == linear ? Interpolation::Linear
                                               : Interpolation::Cubic);
        return;
    }
    try {
        const auto *current = selected();
        if (!current)
            return;
        const Id id = current->id;
        editor_->apply(result->text(), [&](Project &p) {
            auto &layerRef = layer(p, id);
            if (layerRef.locked)
                throw std::runtime_error("Unlock layer first");
            auto &ch = layerRef.channel(property_);
            if (result == remove) {
                if (selectedTime_)
                    eraseKey(ch, *selectedTime_);
                return;
            }
            auto coord = graphPoint(event->pos());
            const auto fps = composition(p, comp_).fps;
            const auto at = frameTime(std::llround(std::clamp(coord.x, 0., duration_) * seconds(fps)), fps) -
                            layerRef.start;
            const auto &spec = parameterSpec(property_.id);
            double value = speedGraph_
                               ? valueAt(ch, at)
                               : std::clamp(discreteProperty(property_) ? std::round(coord.y) : coord.y,
                                            spec.minimum, spec.maximum);
            putKey(ch, {at,
                        value,
                        discreteProperty(property_) ? Interpolation::Hold : Interpolation::Linear,
                        {},
                        {}});
        });
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}

} // namespace motion
