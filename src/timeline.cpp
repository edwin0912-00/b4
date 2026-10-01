// SPDX-License-Identifier: MPL-2.0
#include "timeline.hpp"
#include "evaluate.hpp"
#include "keyframe_dialog.hpp"
#include "numeric_control.hpp"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QUrl>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
namespace motion {
namespace {
constexpr int labelWidth = 538, rowHeight = 17, timelineHeaderHeight = Timeline::headerHeight();
constexpr int motionBlurColumnLeft = 268, motionBlurColumnRight = 288;
constexpr int navigatorTop = 5, navigatorHeight = 16, rulerTop = 27;
constexpr int workAreaTrackTop = 54, workAreaTrackHeight = 18;
constexpr double frameTickSpacing = 40;
constexpr double navigatorFootprint = 24, navigatorHandleWidth = 6, navigatorHandleHit = 5;
constexpr double workAreaHandleWidth = 8, workAreaHandleHit = 7;
void drawMotionBlurGlyph(QPainter &p, const QRectF &cell, bool enabled) {
    const QColor color(enabled ? "#66b7f2" : "#707070");
    std::array<QRectF, 3> rings;
    for (int i = 0; i < int(rings.size()); ++i)
        rings[i] = {cell.left() + 1 + i * 5, cell.top() + 4, 9, 9};
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(enabled ? QBrush(QColor("#234b67")) : QBrush(Qt::NoBrush));
    for (const auto &ring : rings)
        p.drawEllipse(ring);
    p.setPen(QPen(color, 1.25));
    p.setBrush(QBrush(Qt::NoBrush));
    for (const auto &ring : rings)
        p.drawEllipse(ring);
    p.restore();
}
}
Timeline::Timeline(Editor *e, QWidget *parent) : QWidget(parent), editor_(e) {
    setObjectName("timeline");
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    setMinimumWidth(600);
    setAccessibleName("Timeline");
    setAccessibleDescription("Timeline with a composition time navigator, Work Area, layer motion blur "
                             "switches, and keyframes");
    setToolTip("Drag the navigator handles to resize the visible time range or drag its center to pan. "
               "Drag Work Area handles to set boundaries, drag its band to move it, and double-click "
               "the track to reset it; B and N set its start and end. "
               "Use = and - to zoom, D to center on the current time, and Shift+; to toggle the prior range. "
               "Motion blur is transform-only; the root composition owns the shutter when nested.");
    connect(e, &Editor::changed, this, &Timeline::changed);
    connect(e, &Editor::documentReplaced, this, [this] { resetVisibleRange(); });
    connect(e, &Editor::keySelectionReset, this, [this] {
        keys_.clear();
        update();
    });
    connect(e, &Editor::selectionChanged, this, [this] {
        const auto &selected = editor_->selection();
        std::erase_if(keys_, [&](const KeyRef &key) {
            return std::find(selected.begin(), selected.end(), key.layer) == selected.end();
        });
        changed();
    });
    changed();
}
void Timeline::setComposition(Id id) {
    comp_ = id;
    keys_.clear();
    resetVisibleRange();
    changed();
}
void Timeline::setTime(Time t) {
    now_ = t;
    update();
}
void Timeline::setFilter(const QString &f) {
    filter_ = f;
    for (Id id : editor_->selection())
        expanded_.insert(id);
    changed();
}
std::vector<Timeline::Row> Timeline::rows() const {
    std::vector<Row> out;
    const auto &project = editor_->project();
    if (std::none_of(project.compositions.begin(), project.compositions.end(),
                     [&](const Composition &c) { return c.id == comp_; }))
        return out;
    for (const auto &l : composition(project, comp_).layers) {
        std::vector<Row> properties;
        const auto entries = propertyRows(l, &project);
        QString lastGroup;
        for (size_t i = 0; i < entries.size();) {
            const auto &first = entries[i];
            const auto &spec = parameterSpec(first.ref.id);
            std::vector<PropertyRef> refs;
            bool animated = false, modified = false;
            do {
                const auto &ref = entries[i].ref;
                refs.push_back(ref);
                const auto &ch = l.channel(ref);
                animated |= !ch.keys.empty();
                modified |= !ch.keys.empty() || ch.base != spec.defaults.at(ref.component);
                ++i;
            } while (i < entries.size() && entries[i].ref.id == first.ref.id &&
                     entries[i].ref.effect == first.ref.effect);
            bool show = filter_.isEmpty() || (filter_ == "P" && spec.id == "transform.position") ||
                        (filter_ == "A" && spec.id == "transform.anchor") ||
                        (filter_ == "S" && spec.id == "transform.scale") ||
                        (filter_ == "R" && spec.id == "transform.rotation") ||
                        (filter_ == "T" && spec.id == "transform.opacity") ||
                        (filter_ == "M" && spec.group == "Mask") || (filter_ == "E" && first.ref.effect) ||
                        (filter_ == "L" && spec.group == "Audio") || (filter_ == "U" && animated) ||
                        (filter_ == "UU" && modified);
            if (!show)
                continue;
            if (!search_.isEmpty() && !l.name.contains(search_, Qt::CaseInsensitive) &&
                !spec.label.contains(search_, Qt::CaseInsensitive) &&
                !spec.group.contains(search_, Qt::CaseInsensitive))
                continue;
            if (filter_.isEmpty() && search_.isEmpty() && first.group != lastGroup) {
                lastGroup = first.group;
                QString label =
                    first.ref.effect ? "Effects · " + first.group.section(" #", 0, 0) : first.group;
                properties.push_back({l.id, -2, {}, first.group, label, {}});
            }
            if (!filter_.isEmpty() || !search_.isEmpty() ||
                !closedGroups_.count(QString::number(l.id) + "/" + first.group))
                properties.push_back({l.id, 0, first.ref, first.group, spec.label, refs});
        }
        if (!search_.isEmpty() && properties.empty() && !l.name.contains(search_, Qt::CaseInsensitive))
            continue;
        out.push_back({l.id, -1, {}, {}, l.name, {}});
        if (expanded_.count(l.id) || !search_.isEmpty())
            out.insert(out.end(), properties.begin(), properties.end());
    }
    return out;
}
double Timeline::propertyY(Id id, PropertyRef ref) const {
    const auto all = rows();
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].id == id &&
            std::find(all[i].components.begin(), all[i].components.end(), ref) != all[i].components.end())
            return timelineHeaderHeight + i * rowHeight + rowHeight / 2.;
    return -1;
}
bool Timeline::hasComposition() const {
    return std::any_of(editor_->project().compositions.begin(), editor_->project().compositions.end(),
                       [&](const Composition &c) { return c.id == comp_; });
}
double Timeline::plotWidth() const { return std::max(1, width() - labelWidth - 16); }
QRectF Timeline::navigatorRect() const {
    return hasComposition() ? QRectF(labelWidth, navigatorTop, plotWidth(), navigatorHeight) : QRectF{};
}
QRectF Timeline::navigatorSelectionRect() const {
    const auto rect = navigatorRect();
    if (rect.isEmpty())
        return {};
    double left = navigatorX(viewStart_), right = navigatorX(viewEnd_);
    double span = right - left;
    if (span < navigatorFootprint) {
        span = std::min(navigatorFootprint, rect.width());
        left = std::clamp((left + right - span) / 2, rect.left(), rect.right() - span);
    }
    return {left, rect.top(), span, rect.height()};
}
double Timeline::navigatorX(Time t) const {
    if (!hasComposition())
        return labelWidth;
    const auto &c = composition(editor_->project(), comp_);
    return labelWidth + std::clamp(seconds(t) / seconds(c.duration), 0., 1.) * plotWidth();
}
QPointF Timeline::navigatorStartHandleCenter() const {
    const auto rect = navigatorSelectionRect();
    return {rect.left() + navigatorHandleWidth / 2, rect.center().y()};
}
QPointF Timeline::navigatorEndHandleCenter() const {
    const auto rect = navigatorSelectionRect();
    return {rect.right() - navigatorHandleWidth / 2, rect.center().y()};
}
QRectF Timeline::workAreaTrackRect() const {
    return hasComposition() ? QRectF(labelWidth, workAreaTrackTop, plotWidth(), workAreaTrackHeight) : QRectF{};
}
QRectF Timeline::workAreaRect() const {
    if (!hasComposition())
        return {};
    const auto track = workAreaTrackRect();
    const auto range = resolvedWorkArea(composition(editor_->project(), comp_));
    const double left = std::max(track.left(), x(range.start));
    const double right = std::min(track.right(), x(range.end));
    return right > left ? QRectF(left, track.top(), right - left, track.height()) : QRectF{};
}
QPointF Timeline::workAreaStartHandleCenter() const {
    if (!hasComposition())
        return {};
    const auto track = workAreaTrackRect();
    return {x(resolvedWorkArea(composition(editor_->project(), comp_)).start), track.center().y()};
}
QPointF Timeline::workAreaEndHandleCenter() const {
    if (!hasComposition())
        return {};
    const auto track = workAreaTrackRect();
    return {x(resolvedWorkArea(composition(editor_->project(), comp_)).end), track.center().y()};
}
void Timeline::setWorkAreaBoundary(bool end) {
    if (!hasComposition())
        return;
    emit navigationStarted();
    try {
        editor_->apply(end ? "Set work area end" : "Set work area start", [&](Project &p) {
            auto &c = composition(p, comp_);
            if (end)
                setWorkAreaEnd(c, now_);
            else
                setWorkAreaStart(c, now_);
        });
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}
void Timeline::updateWorkAreaDrag(double px) {
    if (!hasComposition())
        return;
    const auto &c = composition(editor_->project(), comp_);
    const double clampedX = std::clamp(px, double(labelWidth), double(labelWidth + plotWidth()));
    const auto boundary = compositionBoundaryIndex(c, at(clampedX, false));
    if (drag_ == Drag::WorkAreaStart) {
        const auto first = std::clamp<std::int64_t>(boundary, 0, dragWorkAreaEndFrame_ - 1);
        const auto end = dragWorkAreaEndFrame_;
        editor_->previewGesture([&](Project &p) { setWorkAreaFrames(composition(p, comp_), first, end); });
    } else if (drag_ == Drag::WorkAreaEnd) {
        const auto range = compositionFrameRange(c, false);
        const auto end = std::clamp<std::int64_t>(boundary, dragWorkAreaStartFrame_ + 1,
                                                  range.frameCount);
        const auto first = dragWorkAreaStartFrame_;
        editor_->previewGesture([&](Project &p) { setWorkAreaFrames(composition(p, comp_), first, end); });
    } else if (drag_ == Drag::WorkAreaMove) {
        const auto delta = boundary - dragWorkAreaPointerFrame_;
        editor_->previewGesture([&](Project &p) { moveWorkArea(composition(p, comp_), delta); });
    }
}
QRectF Timeline::motionBlurCellRect(Id id) const {
    const auto all = rows();
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].id == id && all[i].property == -1)
            return {motionBlurColumnLeft, qreal(timelineHeaderHeight + i * rowHeight),
                    motionBlurColumnRight - motionBlurColumnLeft, rowHeight};
    return {};
}
void Timeline::changed() {
    // Selection can outlive a removed layer or effect; prune it at the shared refresh boundary.
    std::erase_if(keys_, [&](const KeyRef &key) {
        try {
            layer(editor_->project(), key.layer).channel(key.property);
            return false;
        } catch (const std::exception &) {
            return true;
        }
    });
    if (!hasComposition()) {
        resetVisibleRange();
    } else {
        const auto &c = composition(editor_->project(), comp_);
        if (viewDuration_ != c.duration || viewFps_ != c.fps)
            resetVisibleRange();
    }
    setMinimumHeight(std::max(180, timelineHeaderHeight + int(rows().size()) * rowHeight));
    update();
}
void Timeline::resetVisibleRange() {
    cancelViewDrag(false);
    previousRangeValid_ = false;
    viewStart_ = previousStart_ = time(0);
    if (!hasComposition()) {
        viewEnd_ = previousEnd_ = viewDuration_ = time(0);
        update();
        return;
    }
    viewDuration_ = composition(editor_->project(), comp_).duration;
    viewFps_ = composition(editor_->project(), comp_).fps;
    viewEnd_ = previousEnd_ = viewDuration_;
    update();
}
void Timeline::setVisibleRange(Time start, Time end, bool remember) {
    if (!hasComposition())
        return;
    const auto &c = composition(editor_->project(), comp_);
    const Time oneFrame = frameTime(1, c.fps);
    const Time minimum = c.duration < oneFrame ? c.duration : oneFrame;
    if (start < time(0))
        start = time(0);
    if (c.duration < end)
        end = c.duration;
    if (!(start < end)) {
        end = c.duration;
        start = time(0);
    }
    if (end - start < minimum) {
        if (!(c.duration - start < minimum))
            end = start + minimum;
        else {
            end = c.duration;
            start = end - minimum;
        }
    }
    if (start == viewStart_ && end == viewEnd_)
        return;
    if (remember) {
        previousStart_ = viewStart_;
        previousEnd_ = viewEnd_;
        previousRangeValid_ = true;
    }
    viewStart_ = start;
    viewEnd_ = end;
    update();
}
void Timeline::zoomVisible(double factor) {
    if (!hasComposition() || std::isnan(factor) || factor < 0)
        return;
    const auto &c = composition(editor_->project(), comp_);
    const auto totalFrames = frameCount(c.duration, c.fps);
    const auto visibleFrames = qint64(std::clamp(
        std::round(static_cast<long double>(seconds(viewEnd_ - viewStart_)) * seconds(c.fps)), 1.L,
        static_cast<long double>(totalFrames)));
    const auto centerFrame = qint64(std::clamp(
        std::round((static_cast<long double>(seconds(viewStart_)) + seconds(viewEnd_)) * .5L *
                   seconds(c.fps)),
        0.L, static_cast<long double>(totalFrames - 1)));
    const long double scaled = static_cast<long double>(visibleFrames) / factor;
    const long double bounded = std::clamp(scaled, 1.L, static_cast<long double>(totalFrames));
    const auto frames = qint64(factor > 1 ? std::floor(bounded) : std::ceil(bounded));
    auto first = std::clamp<qint64>(centerFrame - frames / 2, 0, totalFrames - frames);
    const auto last = first + frames;
    setVisibleRange(frameTime(first, c.fps), last >= totalFrames ? c.duration : frameTime(last, c.fps));
    emit navigationStarted();
}
void Timeline::centerVisibleRange(Time center) {
    if (!hasComposition())
        return;
    const auto &c = composition(editor_->project(), comp_);
    const auto totalFrames = frameCount(c.duration, c.fps);
    const auto frames = std::clamp<qint64>(
        std::llround(seconds(viewEnd_ - viewStart_) * seconds(c.fps)), 1, totalFrames);
    const auto centerFrame = std::clamp<qint64>(std::llround(seconds(center) * seconds(c.fps)), 0,
                                                totalFrames - 1);
    const auto first = std::clamp<qint64>(centerFrame - frames / 2, 0, totalFrames - frames);
    const auto last = first + frames;
    setVisibleRange(frameTime(first, c.fps), last >= totalFrames ? c.duration : frameTime(last, c.fps));
    emit navigationStarted();
}
void Timeline::toggleFrameGrid() {
    if (!hasComposition())
        return;
    const auto &c = composition(editor_->project(), comp_);
    if (viewStart_ == time(0) && viewEnd_ == c.duration) {
        const auto totalFrames = frameCount(c.duration, c.fps);
        const auto frames = totalFrames < 2 ? totalFrames
                                            : std::clamp<qint64>(qint64(plotWidth() / frameTickSpacing), 2,
                                                                totalFrames);
        const auto centerFrame = std::clamp<qint64>(std::llround(seconds(now_) * seconds(c.fps)), 0,
                                                    totalFrames - 1);
        const auto first = std::clamp<qint64>(centerFrame - frames / 2, 0, totalFrames - frames);
        const auto last = first + frames;
        setVisibleRange(frameTime(first, c.fps), last >= totalFrames ? c.duration : frameTime(last, c.fps));
    } else {
        setVisibleRange(time(0), c.duration);
    }
    emit navigationStarted();
}
void Timeline::togglePreviousRange() {
    if (!hasComposition())
        return;
    const auto &c = composition(editor_->project(), comp_);
    if (viewStart_ == time(0) && viewEnd_ == c.duration) {
        if (previousRangeValid_)
            setVisibleRange(previousStart_, previousEnd_, false);
    } else {
        previousStart_ = viewStart_;
        previousEnd_ = viewEnd_;
        previousRangeValid_ = true;
        setVisibleRange(time(0), c.duration, false);
    }
    emit navigationStarted();
}
void Timeline::cancelViewDrag(bool restore) {
    if (drag_ != Drag::ViewStart && drag_ != Drag::ViewEnd && drag_ != Drag::ViewPan)
        return;
    if (restore && hasComposition()) {
        viewStart_ = dragViewStart_;
        viewEnd_ = dragViewEnd_;
        previousStart_ = dragPreviousStart_;
        previousEnd_ = dragPreviousEnd_;
        previousRangeValid_ = dragPreviousRangeValid_;
    }
    drag_ = Drag::None;
    update();
}
void Timeline::moveViewDrag(double px) {
    if (!hasComposition())
        return;
    const auto &c = composition(editor_->project(), comp_);
    const Time oneFrame = frameTime(1, c.fps);
    const Time minimum = c.duration < oneFrame ? c.duration : oneFrame;
    const auto deltaFrames = std::llround((px - origin_.x()) / plotWidth() * seconds(c.duration) *
                                          seconds(c.fps));
    if (!deltaFrames)
        return;
    const Time delta = frameTime(deltaFrames, c.fps);
    const auto nearestFrame = [&](Time t) {
        return frameTime(std::llround(seconds(t) * seconds(c.fps)), c.fps);
    };
    if (drag_ == Drag::ViewStart) {
        Time start = dragViewStart_ + delta;
        const Time latest = dragViewEnd_ - minimum;
        if (start < time(0))
            start = time(0);
        else
            start = nearestFrame(start);
        if (latest < start)
            start = latest;
        setVisibleRange(start, dragViewEnd_, false);
    } else if (drag_ == Drag::ViewEnd) {
        Time end = dragViewEnd_ + delta;
        const Time earliest = dragViewStart_ + minimum;
        if (end < time(0))
            end = time(0);
        else if (!(end < c.duration))
            end = c.duration;
        else {
            end = nearestFrame(end);
            if (c.duration < end)
                end = c.duration;
        }
        if (end < earliest)
            end = earliest;
        setVisibleRange(dragViewStart_, end, false);
    } else if (drag_ == Drag::ViewPan) {
        const Time span = dragViewEnd_ - dragViewStart_;
        Time start = dragViewStart_ + delta;
        const Time latest = c.duration - span;
        if (start < time(0))
            start = time(0);
        if (latest < start)
            start = latest;
        setVisibleRange(start, start + span, false);
    }
}
double Timeline::x(Time t) const {
    const double span = seconds(viewEnd_ - viewStart_);
    return span > 0 ? labelWidth + (seconds(t) - seconds(viewStart_)) / span * plotWidth() : labelWidth;
}
Time Timeline::at(double px, bool snap) const {
    if (!hasComposition())
        return time(0);
    const auto &c = composition(editor_->project(), comp_);
    double s = seconds(viewStart_) + (px - labelWidth) / plotWidth() * seconds(viewEnd_ - viewStart_);
    s = std::clamp(s, seconds(viewStart_), seconds(viewEnd_));
    if (!snap)
        return fromSeconds(s);
    const auto frame = std::clamp<qint64>(std::llround(s * seconds(c.fps)), 0,
                                          std::max<qint64>(0, frameCount(c.duration, c.fps) - 1));
    return frameTime(frame, c.fps);
}
qint64 Timeline::rulerStepFrames() const {
    if (!hasComposition())
        return 1;
    const auto &c = composition(editor_->project(), comp_);
    const double frames = seconds(viewEnd_ - viewStart_) * seconds(c.fps);
    const double frameSpacing = plotWidth() / std::max(1., frames);
    if (frameSpacing >= frameTickSpacing || frames <= 2)
        return 1;
    const double needed = std::max(1., frames * 70 / plotWidth());
    const double power = std::pow(10, std::floor(std::log10(needed)));
    for (double multiple : {1., 2., 5., 10.})
        if (multiple * power >= needed)
            return std::llround(multiple * power);
    return std::llround(10 * power);
}
void Timeline::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#232323"));
    p.setFont(font());
    if (!hasComposition())
        return;
    const auto &comp = composition(editor_->project(), comp_);
    p.fillRect(0, 0, width(), timelineHeaderHeight, QColor("#1e1e1e"));
    const auto nav = navigatorRect();
    p.fillRect(nav, QColor("#303438"));
    p.setPen(QColor("#17191b"));
    p.drawRect(nav.adjusted(0, 0, -1, -1));
    const auto selected = navigatorSelectionRect();
    p.fillRect(QRectF(selected.left(), nav.top() + 2, selected.width(), nav.height() - 4), QColor("#626262"));
    const auto handle = [&](double x) {
        const double left = std::clamp(x - navigatorHandleWidth / 2, nav.left(),
                                       nav.right() - navigatorHandleWidth);
        p.fillRect(QRectF(left, nav.top(), navigatorHandleWidth, nav.height()), QColor("#3a9ee8"));
    };
    handle(navigatorStartHandleCenter().x());
    handle(navigatorEndHandleCenter().x());
    const double miniNeedle = navigatorX(now_);
    p.setPen(QPen(QColor("#419df0"), 2));
    p.drawLine(QPointF(miniNeedle, nav.top()), QPointF(miniNeedle, nav.bottom()));
    p.setBrush(QColor("#419df0"));
    p.setPen(Qt::NoPen);
    p.drawPolygon(QPolygonF{{miniNeedle - 4, nav.top()}, {miniNeedle + 4, nav.top()},
                            {miniNeedle, nav.top() + 5}});
    p.setPen(QColor("#aaaeb2"));
    p.drawText(QRect(105, 17, 165, 18), Qt::AlignVCenter, "#    Layer Name");
    drawMotionBlurGlyph(p, QRectF(motionBlurColumnLeft, 17,
                                  motionBlurColumnRight - motionBlurColumnLeft, 18),
                        comp.motionBlurEnabled);
    p.drawText(QRect(motionBlurColumnRight + 4, 17, 400 - motionBlurColumnRight - 4, 18),
               Qt::AlignVCenter, "Motion Blur");
    p.save();
    p.setBrush(Qt::NoBrush);
    const auto workTrack = workAreaTrackRect();
    p.fillRect(workTrack, QColor("#292d31"));
    p.setPen(QColor("#17191b"));
    p.drawRect(workTrack.adjusted(0, 0, -1, -1));
    p.setPen(QColor("#aaaeb2"));
    p.drawText(QRect(105, workAreaTrackTop, 160, workAreaTrackHeight), Qt::AlignVCenter, "Work Area");
    const auto workBand = workAreaRect();
    if (!workBand.isEmpty()) {
        p.fillRect(workBand.adjusted(0, 4, 0, -4), QColor("#626262"));
        const auto start = workAreaStartHandleCenter();
        const auto end = workAreaEndHandleCenter();
        const auto drawHandle = [&](QPointF center) {
            if (center.x() < workTrack.left() || center.x() > workTrack.right())
                return;
            const QRectF handle(center.x() - workAreaHandleWidth / 2, workTrack.top() + 1,
                                workAreaHandleWidth, workTrack.height() - 2);
            p.setPen(QPen(QColor("#3a9ee8"), 1));
            p.setBrush(QColor("#3a9ee8"));
            p.drawRoundedRect(handle, 2, 2);
        };
        drawHandle(start);
        drawHandle(end);
    }
    p.restore();
    p.drawText(QRect(408, 17, 125, 18), Qt::AlignVCenter, "Parent & Link");
    p.drawText(8, 29, "◉");
    p.drawText(47, 29, "L");
    const auto step = rulerStepFrames();
    const auto rate = seconds(comp.fps);
    const auto nominal = std::max<qint64>(1, std::llround(rate));
    const auto first = qint64(std::floor(seconds(viewStart_) * rate / step)) * step;
    const auto last = qint64(std::ceil(seconds(viewEnd_) * rate));
    const auto frameSpacing = plotWidth() / std::max(1., seconds(viewEnd_ - viewStart_) * rate);
    for (qint64 frame = first; frame <= last; frame += step) {
        double xx = x(frameTime(frame, comp.fps));
        if (xx < labelWidth || xx > labelWidth + plotWidth())
            continue;
        p.setPen(QColor("#303030"));
        p.drawLine(QPointF(xx, timelineHeaderHeight), QPointF(xx, height()));
        p.setPen(QColor("#9b9b9b"));
        const auto label = frameSpacing >= frameTickSpacing ? QString::number(frame)
                           : frame < nominal ? QString::number(frame) + "f"
                           : frame < nominal * 60
                               ? QString::number(frame / nominal) + ":" +
                                     QString::number(frame % nominal).rightJustified(2, '0')
                               : frameText(frame, nominal);
        p.drawText(QPointF(xx + 3, 44), label);
    }
    p.setPen(QColor("#101010"));
    for (int xx : {20, 40, 60, 86, 108, 268, 402, 538})
        p.drawLine(xx, 17, xx, height());
    auto all = rows();
    for (size_t index = 0; index < all.size(); ++index) {
        const auto &row = all[index];
        const auto &l = layer(editor_->project(), row.id);
        const double y = timelineHeaderHeight + index * rowHeight;
        const bool selected = std::find(editor_->selection().begin(), editor_->selection().end(), l.id) !=
                              editor_->selection().end();
        p.fillRect(QRectF(0, y, labelWidth, rowHeight),
                   QColor(row.property == -1 && selected ? "#4b4b4b" : "#292929"));
        p.setPen(QColor("#1c1c1c"));
        p.drawLine(QPointF(0, y + rowHeight), QPointF(width(), y + rowHeight));
        if (row.property == -1) {
            if (layerHasAudio(editor_->project(), l)) {
                p.setPen(QColor(l.audioEnabled ? "#c9c9c9" : "#555555"));
                p.drawRect(QRectF(24, y + 6, 4, 6));
                p.drawLine(QPointF(28, y + 6), QPointF(33, y + 3));
                p.drawLine(QPointF(33, y + 3), QPointF(33, y + 14));
                p.drawLine(QPointF(33, y + 14), QPointF(28, y + 12));
                if (!l.audioEnabled)
                    p.drawLine(QPointF(22, y + 14), QPointF(36, y + 3));
            }
            if (l.kind != LayerKind::Audio) {
                p.setPen(QColor(l.visible ? "#d3d3d3" : "#555555"));
                p.drawEllipse(QRectF(6, y + 5, 10, 6));
                if (l.visible)
                    p.fillRect(QRectF(10, y + 6, 3, 3), QColor("#d3d3d3"));
            }
            if (l.locked) {
                p.setPen(QColor("#d3d3d3"));
                p.drawArc(QRectF(45, y + 2, 6, 8), 0, 180 * 16);
                p.fillRect(QRectF(44, y + 7, 8, 7), QColor("#bcbcbc"));
            }
            static const std::array<QColor, 7> colors = {
                QColor("#b15458"), QColor("#aaa4c5"), QColor("#c7ba72"), QColor("#84a3b9"),
                QColor("#86a59a"), QColor("#aaa4c5"), QColor("#75a98d")};
            p.fillRect(QRectF(91, y + 3, 10, 11), colors.at(int(l.kind)));
            p.setPen(QColor("#c0c0c0"));
            p.drawText(QPointF(73, y + 13), expanded_.count(l.id) ? "⌄" : "›");
            const auto &layers = comp.layers;
            auto it =
                std::find_if(layers.begin(), layers.end(), [&](const Layer &v) { return v.id == l.id; });
            p.drawText(QRectF(110, y, 22, rowHeight), Qt::AlignVCenter,
                       QString::number(it - layers.begin() + 1));
            p.drawText(QRectF(138, y, 128, rowHeight), Qt::AlignVCenter,
                       p.fontMetrics().elidedText(l.name, Qt::ElideRight, 125));
            drawMotionBlurGlyph(p, QRectF(motionBlurColumnLeft, y,
                                          motionBlurColumnRight - motionBlurColumnLeft, rowHeight),
                                l.motionBlur);
            p.drawText(
                QRectF(420, y, 108, rowHeight), Qt::AlignVCenter,
                p.fontMetrics().elidedText(l.parent ? layer(editor_->project(), *l.parent).name : "None",
                                           Qt::ElideRight, 98));
            p.drawText(QPointF(523, y + 12), "⌄");
            p.save();
            p.setClipRect(QRect(labelWidth, timelineHeaderHeight, width() - labelWidth, height()));
            p.setBrush(colors.at(int(l.kind)).darker(120));
            p.setPen(colors.at(int(l.kind)));
            p.drawRect(QRectF(x(l.in), y + 1, std::max(1., x(l.out) - x(l.in)), rowHeight - 2));
            if ((l.kind == LayerKind::Video || l.kind == LayerKind::Audio) &&
                asset(editor_->project(), l.source).hasAudio) {
                const auto &a = asset(editor_->project(), l.source);
                p.setPen(QColor(l.audioEnabled ? "#203a31" : "#555555"));
                p.setClipRect(QRectF(std::max(double(labelWidth), x(l.in)), y,
                                     std::max(0., x(l.out) - std::max(double(labelWidth), x(l.in))),
                                     rowHeight));
                for (size_t i = 0; i < a.waveform.size(); ++i) {
                    double xx = x(l.start + fromSeconds(seconds(a.duration) * (i + .5) / a.waveform.size()));
                    double h = a.waveform[i] * 6;
                    p.drawLine(QPointF(xx, y + 8 - h), QPointF(xx, y + 8 + h));
                }
            }
            p.restore();
        } else if (row.property == -2) {
            p.setPen(QColor("#b9b9b9"));
            p.drawText(QPointF(112, y + 13),
                       closedGroups_.count(QString::number(l.id) + "/" + row.group) ? "›" : "⌄");
            p.drawText(QRectF(138, y, 250, rowHeight), Qt::AlignVCenter, row.label);
        } else {
            bool animated = false, current = false;
            for (auto ref : row.components) {
                const auto &ch = l.channel(ref);
                animated |= !ch.keys.empty();
                current |= std::any_of(ch.keys.begin(), ch.keys.end(),
                                       [&](const Key &k) { return k.at == sourceTime(l, now_); });
            }
            p.setPen(QColor(animated ? "#459ff0" : "#aaaaaa"));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QRectF(122, y + 4, 9, 10));
            p.drawLine(QPointF(126, y + 1), QPointF(126, y + 4));
            p.drawLine(QPointF(124, y + 1), QPointF(129, y + 1));
            p.drawLine(QPointF(126, y + 6), QPointF(126, y + 10));
            if (animated) {
                p.setPen(QColor("#a4a4a4"));
                p.drawText(QPointF(7, y + 13), "‹");
                p.drawText(QPointF(33, y + 13), "›");
                p.setBrush(current ? QColor("#419eea") : QColor("#292929"));
                p.drawPolygon(QPolygonF{{24., y + 4}, {28., y + 8}, {24., y + 12}, {20., y + 8}});
            }
            p.setPen(QColor("#c5c5c5"));
            p.drawText(QRectF(138, y, 127, rowHeight), Qt::AlignVCenter,
                       p.fontMetrics().elidedText(row.label, Qt::ElideRight, 125));
            p.setPen(QColor("#43a5f5"));
            const double left = row.ref.id == "transform.scale" ? 286 : 270;
            const double cell = (400 - left) / row.components.size();
            for (size_t i = 0; i < row.components.size(); ++i) {
                auto ref = row.components[i];
                double value = toDisplay(ref, valueAt(l.channel(ref), sourceTime(l, now_)));
                const auto &spec = parameterSpec(ref.id);
                QString text =
                    ref.id == "transform.rotation"
                        ? angleText(value)
                        : QLocale().toString(value, 'f',
                                             spec.type == ParameterType::Enum ||
                                                     spec.type == ParameterType::Boolean ||
                                                     (spec.unit == "%" && std::floor(value) == value)
                                                 ? 0
                                                 : (ref.effect ? 3 : 1));
                if (discreteProperty(ref))
                    text = spec.type == ParameterType::Boolean ? (value ? "On" : "Off")
                                                               : spec.choices.value(int(value));
                else if (ref.effect)
                    text = compactFixedNumber(text, QLocale().decimalPoint());
                if (i + 1 == row.components.size() && spec.unit == "%")
                    text += "%";
                p.drawText(QRectF(left + i * cell, y, cell - 2, rowHeight), Qt::AlignVCenter, text);
            }
            if (row.ref.id == "transform.scale") {
                p.setPen(QColor(editor_->scaleLinked() ? "#aaaeb2" : "#555555"));
                p.drawText(QPointF(270, y + 13), "↔");
            }
            p.save();
            p.setClipRect(QRect(labelWidth, timelineHeaderHeight, width() - labelWidth, height()));
            for (auto ref : row.components)
                for (const auto &key : l.channel(ref).keys) {
                    double xx = x(key.at + l.start), yy = y + rowHeight / 2.;
                    bool chosen = std::any_of(keys_.begin(), keys_.end(), [&](const KeyRef &k) {
                        return k.layer == l.id && k.property == ref && k.at == key.at;
                    });
                    p.setPen(chosen ? QColor("#42a6ff") : QColor("#969696"));
                    p.setBrush(chosen ? QColor("#49617a") : QColor("#969696"));
                    p.drawPolygon(QPolygonF{{xx, yy - 4}, {xx + 4, yy}, {xx, yy + 4}, {xx - 4, yy}});
                }
            p.restore();
        }
    }
    if (!(now_ < viewStart_) && !(viewEnd_ < now_)) {
        const double needle = x(now_);
        p.fillRect(QRectF(needle, timelineHeaderHeight, 13, height() - timelineHeaderHeight),
                   QColor(255, 255, 255, 12));
        p.setPen(QColor("#419df0"));
        p.drawLine(QPointF(needle, 30), QPointF(needle, height()));
        p.setBrush(QColor("#419df0"));
        p.drawPolygon(QPolygonF{{needle - 5, 31}, {needle + 5, 31}, {needle, 39}});
    }
    if (drag_ == Drag::Box) {
        p.setBrush(QColor(70, 140, 200, 30));
        p.setPen(QColor("#70a7d4"));
        p.drawRect(QRectF(origin_, cursor_).normalized());
    }
    if (hasFocus()) {
        p.setPen(QColor("#248ddd"));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}
void Timeline::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton)
        return;
    if (!hasComposition())
        return;
    setFocus();
    origin_ = cursor_ = event->position();
    originTime_ = at(origin_.x());
    try {
        const auto nav = navigatorRect();
        if (nav.contains(origin_)) {
            const double startX = navigatorStartHandleCenter().x();
            const double endX = navigatorEndHandleCenter().x();
            const bool hitStart = std::abs(origin_.x() - startX) <= navigatorHandleHit;
            const bool hitEnd = std::abs(origin_.x() - endX) <= navigatorHandleHit;
            if (hitStart && hitEnd)
                drag_ = origin_.x() <= (startX + endX) / 2 ? Drag::ViewStart : Drag::ViewEnd;
            else if (hitStart)
                drag_ = Drag::ViewStart;
            else if (hitEnd)
                drag_ = Drag::ViewEnd;
            else if (origin_.x() >= startX && origin_.x() <= endX)
                drag_ = Drag::ViewPan;
            else
                return;
            dragViewStart_ = viewStart_;
            dragViewEnd_ = viewEnd_;
            dragPreviousStart_ = previousStart_;
            dragPreviousEnd_ = previousEnd_;
            dragPreviousRangeValid_ = previousRangeValid_;
            previousStart_ = viewStart_;
            previousEnd_ = viewEnd_;
            previousRangeValid_ = true;
            emit navigationStarted();
            return;
        }
        const auto workTrack = workAreaTrackRect();
        if (workTrack.contains(origin_)) {
            const auto workBand = workAreaRect();
            if (workBand.isEmpty())
                return;
            const auto start = workAreaStartHandleCenter();
            const auto end = workAreaEndHandleCenter();
            const bool startVisible = start.x() >= workTrack.left() && start.x() <= workTrack.right();
            const bool endVisible = end.x() >= workTrack.left() && end.x() <= workTrack.right();
            const double startDistance = std::abs(origin_.x() - start.x());
            const double endDistance = std::abs(origin_.x() - end.x());
            const bool hitStart = startVisible && startDistance <= workAreaHandleHit;
            const bool hitEnd = endVisible && endDistance <= workAreaHandleHit;
            if (hitStart && hitEnd) {
                if (startDistance < endDistance ||
                    (startDistance == endDistance && origin_.x() <= (start.x() + end.x()) / 2))
                    drag_ = Drag::WorkAreaStart;
                else
                    drag_ = Drag::WorkAreaEnd;
            } else if (hitStart) {
                drag_ = Drag::WorkAreaStart;
            } else if (hitEnd) {
                drag_ = Drag::WorkAreaEnd;
            } else if (workBand.contains(origin_)) {
                drag_ = Drag::WorkAreaMove;
            } else {
                return;
            }
            const auto range = compositionFrameRange(composition(editor_->project(), comp_), true);
            dragWorkAreaStartFrame_ = range.firstFrame;
            dragWorkAreaEndFrame_ = range.firstFrame + range.frameCount;
            dragWorkAreaPointerFrame_ =
                compositionBoundaryIndex(composition(editor_->project(), comp_), at(origin_.x(), false));
            emit navigationStarted();
            editor_->beginGesture();
            return;
        }
        if (origin_.y() < timelineHeaderHeight) {
            if (origin_.y() >= rulerTop && origin_.x() >= labelWidth) {
                drag_ = Drag::Scrub;
                emit navigationStarted();
                if (originTime_ != now_)
                    emit timeChanged(originTime_);
            }
            return;
        }
        const auto all = rows();
        const int index = int((origin_.y() - timelineHeaderHeight) / rowHeight);
        if (index < 0 || index >= int(all.size()))
            return;
        const auto row = all[index];
        const auto &l = layer(editor_->project(), row.id);
        dragLayer_ = row.id;
        if (row.property == -2) {
            if (origin_.x() < labelWidth) {
                auto key = QString::number(l.id) + "/" + row.group;
                if (closedGroups_.count(key))
                    closedGroups_.erase(key);
                else
                    closedGroups_.insert(key);
                changed();
            }
            return;
        }
        if (origin_.x() < labelWidth) {
            if (row.property == -1 && motionBlurCellRect(row.id).contains(origin_)) {
                editor_->apply("Layer motion blur", [&](Project &p) {
                    auto &v = layer(p, row.id);
                    if (v.locked)
                        throw std::runtime_error("Unlock layer first");
                    v.motionBlur = !v.motionBlur;
                });
                return;
            }
            if (row.property == -1 && origin_.x() < 60) {
                if (origin_.x() < 20 && l.kind != LayerKind::Audio)
                    editor_->apply("Layer visibility", [&](Project &p) {
                        auto &v = layer(p, row.id);
                        if (v.locked)
                            throw std::runtime_error("Unlock layer first");
                        v.visible = !v.visible;
                    });
                else if (origin_.x() < 40 && layerHasAudio(editor_->project(), l))
                    editor_->apply("Layer audio", [&](Project &p) {
                        auto &v = layer(p, row.id);
                        if (v.locked)
                            throw std::runtime_error("Unlock layer first");
                        v.audioEnabled = !v.audioEnabled;
                    });
                else if (origin_.x() >= 40)
                    editor_->apply("Layer lock",
                                   [&](Project &p) { layer(p, row.id).locked = !layer(p, row.id).locked; });
                return;
            }
            if (row.property == -1 && origin_.x() >= 402) {
                QMenu menu;
                auto *none = menu.addAction("None");
                std::map<QAction *, Id> targets;
                for (const auto &v : composition(editor_->project(), comp_).layers)
                    if (v.id != row.id)
                        targets[menu.addAction(v.name)] = v.id;
                auto *picked = menu.exec(event->globalPosition().toPoint());
                if (!picked)
                    return;
                auto parent = picked == none ? std::optional<Id>{} : std::optional<Id>{targets.at(picked)};
                editor_->apply("Parent layer", [&](Project &p) { reparent(p, comp_, row.id, parent, now_); });
                return;
            }
            if (row.property >= 0 && origin_.x() < 45) {
                if (origin_.x() >= 18 && origin_.x() < 31) {
                    editor_->apply("Toggle keyframe", [&](Project &p) {
                        auto &v = layer(p, row.id);
                        if (v.locked)
                            throw std::runtime_error("Unlock layer first");
                        auto t = sourceTime(v, now_);
                        bool all = true;
                        for (auto ref : row.components)
                            all &= std::any_of(v.channel(ref).keys.begin(), v.channel(ref).keys.end(),
                                               [&](const Key &k) { return k.at == t; });
                        for (auto ref : row.components) {
                            auto &ch = v.channel(ref);
                            if (all)
                                eraseKey(ch, t);
                            else
                                putKey(ch,
                                       {t, valueAt(ch, t),
                                        discreteProperty(ref) ? Interpolation::Hold : Interpolation::Linear});
                        }
                    });
                } else {
                    std::optional<Time> target;
                    const bool forward = origin_.x() >= 31;
                    for (auto ref : row.components)
                        for (const auto &key : l.channel(ref).keys) {
                            auto t = key.at + l.start;
                            if (forward && now_ < t && (!target || t < *target))
                                target = t;
                            if (!forward && t < now_ && (!target || *target < t))
                                target = t;
                        }
                    if (target)
                        emit timeChanged(*target);
                }
                return;
            }
            if (row.property >= 0 && origin_.x() >= 120 && origin_.x() < 136) {
                bool on = false;
                for (auto ref : row.components)
                    on |= !l.channel(ref).keys.empty();
                editor_->apply("Toggle animation", [&](Project &p) {
                    for (auto ref : row.components)
                        setAnimated(p, row.id, ref, sourceTime(layer(p, row.id), now_), !on);
                });
                return;
            }
            if (row.property >= 0 && row.ref.id == "transform.scale" && origin_.x() >= 268 &&
                origin_.x() < 286) {
                editor_->setScaleLinked(!editor_->scaleLinked());
                return;
            }
            if (row.property >= 0 && origin_.x() >= 268 && origin_.x() < 400) {
                if (l.locked)
                    throw std::runtime_error("Unlock layer first");
                const double left = row.ref.id == "transform.scale" ? 286 : 270;
                const int component =
                    std::clamp(int((origin_.x() - left) / (400 - left) * row.components.size()), 0,
                               int(row.components.size()) - 1);
                editProperty_ = row.components.at(component);
                emit propertySelected(editProperty_);
                editor_->select({l.id});
                if (discreteProperty(editProperty_)) {
                    QMenu menu;
                    const auto &spec = parameterSpec(editProperty_.id);
                    const auto choices =
                        spec.type == ParameterType::Boolean ? QStringList{"Off", "On"} : spec.choices;
                    for (const auto &choice : choices)
                        menu.addAction(choice);
                    auto *picked = menu.exec(event->globalPosition().toPoint());
                    if (picked) {
                        int value = choices.indexOf(picked->text());
                        editor_->apply("Edit property", [&](Project &p) {
                            motion::setProperty(p, row.id, editProperty_, sourceTime(layer(p, row.id), now_),
                                                value);
                        });
                    }
                    return;
                }
                editStart_ = toDisplay(editProperty_, valueAt(l.channel(editProperty_), sourceTime(l, now_)));
                drag_ = Drag::Value;
                editor_->beginGesture();
                return;
            }
            if (row.property == -1 && origin_.x() >= 65 && origin_.x() < 90) {
                if (expanded_.count(row.id))
                    expanded_.erase(row.id);
                else
                    expanded_.insert(row.id);
                changed();
                return;
            }
            auto selection = editor_->selection();
            if ((event->modifiers() & Qt::ShiftModifier) && !selection.empty()) {
                const auto &layers = composition(editor_->project(), comp_).layers;
                auto first = std::find_if(layers.begin(), layers.end(),
                                          [&](const Layer &v) { return v.id == selection.front(); });
                auto last = std::find_if(layers.begin(), layers.end(),
                                         [&](const Layer &v) { return v.id == row.id; });
                if (first != layers.end() && last != layers.end()) {
                    if (first > last)
                        std::swap(first, last);
                    selection.clear();
                    for (auto it = first; it <= last; ++it)
                        selection.push_back(it->id);
                }
            } else if (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
                auto it = std::find(selection.begin(), selection.end(), row.id);
                if (it == selection.end())
                    selection.push_back(row.id);
                else
                    selection.erase(it);
            } else
                selection = {row.id};
            if (row.property == -1)
                keys_.clear();
            editor_->select(selection);
            if (row.property >= 0)
                emit propertySelected(row.ref);
            else if (!l.locked)
                drag_ = Drag::Reorder;
            return;
        }
        if (row.property >= 0) {
            emit propertySelected(row.ref);
            std::vector<KeyRef> hits;
            for (auto ref : row.components)
                for (const auto &key : l.channel(ref).keys)
                    if (std::abs(x(key.at + l.start) - origin_.x()) <= 6)
                        hits.push_back({l.id, ref, key.at});
            if (!hits.empty()) {
                bool selected = std::any_of(keys_.begin(), keys_.end(), [&](const KeyRef &k) {
                    return k.layer == hits.front().layer && k.property == hits.front().property &&
                           k.at == hits.front().at;
                });
                if (!(event->modifiers() & Qt::ShiftModifier) && !selected)
                    keys_.clear();
                for (auto hit : hits)
                    if (std::none_of(keys_.begin(), keys_.end(), [&](const KeyRef &k) {
                            return k.layer == hit.layer && k.property == hit.property && k.at == hit.at;
                        }))
                        keys_.push_back(hit);
                std::set<Id> selectedLayers;
                for (const auto &key : keys_)
                    selectedLayers.insert(key.layer);
                editor_->select(std::vector<Id>(selectedLayers.begin(), selectedLayers.end()));
                dragCopies_.clear();
                for (auto ref : keys_) {
                    const auto &source = layer(editor_->project(), ref.layer);
                    if (source.locked)
                        throw std::runtime_error("Unlock selected keyframe layers first");
                    const auto &list = source.channel(ref.property).keys;
                    auto key =
                        std::find_if(list.begin(), list.end(), [&](const Key &k) { return k.at == ref.at; });
                    if (key != list.end())
                        dragCopies_.push_back({ref.layer, ref.property, *key});
                }
                if (!keys_.empty())
                    emit keySelected(keys_.front());
                keyDelta_ = time(0, 1);
                drag_ = Drag::Key;
                editor_->beginGesture();
                update();
                return;
            }
            keys_.clear();
            drag_ = Drag::Box;
            update();
            return;
        }
        editor_->select({l.id});
        if (l.locked)
            return;
        if (std::abs(origin_.x() - x(l.in)) < 8)
            drag_ = Drag::TrimIn;
        else if (std::abs(origin_.x() - x(l.out)) < 8)
            drag_ = Drag::TrimOut;
        else if (origin_.x() >= x(l.in) && origin_.x() <= x(l.out))
            drag_ = Drag::Move;
        else {
            drag_ = Drag::Scrub;
            emit timeChanged(originTime_);
            return;
        }
        editor_->beginGesture();
    } catch (const std::exception &e) {
        drag_ = Drag::None;
        editor_->cancelGesture();
        emit error(e.what());
    }
}
void Timeline::mouseMoveEvent(QMouseEvent *event) {
    cursor_ = event->position();
    try {
        if (drag_ == Drag::ViewStart || drag_ == Drag::ViewEnd || drag_ == Drag::ViewPan) {
            moveViewDrag(cursor_.x());
            return;
        }
        if (drag_ == Drag::WorkAreaStart || drag_ == Drag::WorkAreaEnd ||
            drag_ == Drag::WorkAreaMove) {
            updateWorkAreaDrag(cursor_.x());
            return;
        }
        if (drag_ == Drag::Scrub) {
            const auto next = at(cursor_.x());
            if (next != now_)
                emit timeChanged(next);
            return;
        }
        if (drag_ == Drag::Box || drag_ == Drag::Reorder) {
            update();
            return;
        }
        if (drag_ == Drag::None)
            return;
        if (drag_ == Drag::Value) {
            double speed = event->modifiers() & Qt::ShiftModifier ? 10
                           : event->modifiers() & Qt::AltModifier ? .1
                                                                  : 1;
            double value = fromDisplay(editProperty_, editStart_ + (cursor_.x() - origin_.x()) * speed);
            const auto &spec = parameterSpec(editProperty_.id);
            value = std::clamp(value, spec.minimum, spec.maximum);
            editor_->previewGesture([&](Project &p) {
                editor_->setNumericValue(p, dragLayer_, editProperty_, sourceTime(layer(p, dragLayer_), now_),
                                         value);
            });
            return;
        }
        Time delta = at(cursor_.x()) - originTime_;
        editor_->previewGesture([&](Project &p) {
            if (drag_ == Drag::Key) {
                for (const auto &key : dragCopies_)
                    eraseKey(layer(p, key.layer).channel(key.property), key.key.at);
                for (const auto &key : dragCopies_) {
                    auto moved = key.key;
                    moved.at = moved.at + delta;
                    putKey(layer(p, key.layer).channel(key.property), moved);
                }
            } else {
                auto &l = layer(p, dragLayer_);
                if (drag_ == Drag::Move) {
                    l.in = l.in + delta;
                    l.out = l.out + delta;
                    l.start = l.start + delta;
                }
                if (drag_ == Drag::TrimIn)
                    l.in = l.in + delta;
                if (drag_ == Drag::TrimOut)
                    l.out = l.out + delta;
            }
        });
        if (drag_ == Drag::Key)
            keyDelta_ = delta;
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}
void Timeline::mouseReleaseEvent(QMouseEvent *event) {
    cursor_ = event->position();
    if (drag_ == Drag::ViewStart || drag_ == Drag::ViewEnd || drag_ == Drag::ViewPan) {
        moveViewDrag(cursor_.x());
        drag_ = Drag::None;
        update();
        return;
    }
    try {
        if (drag_ == Drag::WorkAreaStart || drag_ == Drag::WorkAreaEnd ||
            drag_ == Drag::WorkAreaMove) {
            updateWorkAreaDrag(cursor_.x());
            editor_->commitGesture("Set work area");
        } else if (drag_ == Drag::Reorder && std::abs(cursor_.y() - origin_.y()) > 8) {
            auto all = rows();
            int row = int((cursor_.y() - timelineHeaderHeight) / rowHeight);
            if (row >= 0 && row < int(all.size())) {
                Id target = all[row].id;
                editor_->apply("Reorder layer", [&](Project &p) {
                    auto &list = composition(p, comp_).layers;
                    auto from = std::find_if(list.begin(), list.end(),
                                             [&](const Layer &l) { return l.id == dragLayer_; });
                    auto to = std::find_if(list.begin(), list.end(),
                                           [&](const Layer &l) { return l.id == target; });
                    if (from != list.end() && to != list.end()) {
                        auto copy = *from;
                        auto index = to - list.begin();
                        list.erase(from);
                        list.insert(list.begin() + std::min<ptrdiff_t>(index, list.size()), copy);
                    }
                });
            }
        } else if (drag_ == Drag::Box) {
            QRectF box = QRectF(origin_, cursor_).normalized();
            auto all = rows();
            keys_.clear();
            for (size_t row = 0; row < all.size(); ++row)
                if (all[row].property >= 0) {
                    const auto &l = layer(editor_->project(), all[row].id);
                    for (auto ref : all[row].components)
                        for (const auto &key : l.channel(ref).keys)
                            if (box.contains(QPointF(x(key.at + l.start),
                                                     timelineHeaderHeight + row * rowHeight + rowHeight / 2.)))
                                keys_.push_back({l.id, ref, key.at});
                }
            std::set<Id> selectedLayers;
            for (const auto &key : keys_)
                selectedLayers.insert(key.layer);
            editor_->select(std::vector<Id>(selectedLayers.begin(), selectedLayers.end()));
        } else if (drag_ == Drag::Key || drag_ == Drag::Move || drag_ == Drag::TrimIn ||
                   drag_ == Drag::TrimOut || drag_ == Drag::Value) {
            editor_->commitGesture("Timeline edit");
            if (drag_ == Drag::Key) {
                keys_.clear();
                for (const auto &key : dragCopies_)
                    keys_.push_back({key.layer, key.property, key.key.at + keyDelta_});
            } else
                keys_.clear();
        }
    } catch (const std::exception &e) {
        editor_->cancelGesture();
        emit error(e.what());
    }
    drag_ = Drag::None;
    update();
}
void Timeline::navigateKey(bool forward) {
    std::optional<Time> target;
    for (const auto &row : rows())
        if (row.property >= 0 && (editor_->selection().empty() ||
                                  std::find(editor_->selection().begin(), editor_->selection().end(),
                                            row.id) != editor_->selection().end())) {
            const auto &l = layer(editor_->project(), row.id);
            for (auto ref : row.components)
                for (const auto &key : l.channel(ref).keys) {
                    auto t = key.at + l.start;
                    if (forward && now_ < t && (!target || t < *target))
                        target = t;
                    if (!forward && t < now_ && (!target || *target < t))
                        target = t;
                }
        }
    if (target)
        emit timeChanged(*target);
}
void Timeline::mouseDoubleClickEvent(QMouseEvent *event) {
    if (navigatorRect().contains(event->position())) {
        cancelViewDrag(false);
        if (event->modifiers() & Qt::ShiftModifier)
            togglePreviousRange();
        else
            toggleFrameGrid();
        return;
    }
    if (workAreaTrackRect().contains(event->position())) {
        editor_->cancelGesture();
        drag_ = Drag::None;
        emit navigationStarted();
        try {
            editor_->apply("Reset work area", [&](Project &p) { resetWorkArea(composition(p, comp_)); });
        } catch (const std::exception &e) {
            emit error(e.what());
        }
        return;
    }
    if (event->position().y() < timelineHeaderHeight)
        return;
    editor_->cancelGesture();
    drag_ = Drag::None;
    auto all = rows();
    const int index = int((event->position().y() - timelineHeaderHeight) / rowHeight);
    if (index < 0 || index >= int(all.size()))
        return;
    const auto row = all[index];
    if (row.property == -1 && motionBlurCellRect(row.id).contains(event->position()))
        return;
    if (row.property == -1) {
        editor_->select({row.id});
        emit layerOpened(row.id);
        return;
    }
    if (row.property < 0 || event->position().x() < 268 || event->position().x() >= 400)
        return;
    const auto &l = layer(editor_->project(), row.id);
    if (l.locked)
        return;
    const int left = row.ref.id == "transform.scale" ? 286 : 270;
    if (event->position().x() < left)
        return;
    const int cell = (400 - left) / row.components.size();
    const int component =
        std::clamp(int((event->position().x() - left) / cell), 0, int(row.components.size()) - 1);
    const auto ref = row.components[component];
    if (discreteProperty(ref))
        return;
    auto *box = new ScrubNumber(this);
    box->setObjectName("timelineInlineValue");
    const auto &spec = parameterSpec(ref.id);
    box->setDecimals(10);
    box->setRange(toDisplay(ref, spec.minimum), toDisplay(ref, spec.maximum));
    box->angle = ref.id == "transform.rotation";
    box->setValue(toDisplay(ref, valueAt(l.channel(ref), sourceTime(l, now_))));
    box->setGeometry(left + component * cell, timelineHeaderHeight + index * rowHeight, cell, rowHeight);
    box->setStyleSheet(
        "QDoubleSpinBox{min-height:0;padding:0;border:1px solid #459ddf;background:#151515;color:white;}");
    connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), box,
            [box] { box->setProperty("edited", true); });
    auto close = [box] {
        box->setProperty("closing", true);
        box->hide();
        box->deleteLater();
    };
    connect(box, &QDoubleSpinBox::editingFinished, this, [=, this] {
        if (box->property("closing").toBool())
            return;
        if (box->property("edited").toBool())
            try {
                editor_->apply("Edit timeline value", [&](Project &p) {
                    editor_->setNumericValue(p, row.id, ref, sourceTime(layer(p, row.id), now_),
                                             fromDisplay(ref, box->value()));
                });
            } catch (const std::exception &e) {
                emit error(e.what());
            }
        close();
        update();
    });
    box->dismiss = [this, close] {
        editor_->cancelGesture();
        close();
    };
    box->begin = [this] { editor_->beginGesture(); };
    box->scrub = [=, this](double value) {
        try {
            editor_->previewGesture([&](Project &p) {
                editor_->setNumericValue(p, row.id, ref, sourceTime(layer(p, row.id), now_),
                                         fromDisplay(ref, value));
            });
        } catch (const std::exception &e) {
            emit error(e.what());
        }
    };
    box->finish = [this, close] {
        editor_->commitGesture("Scrub timeline value");
        close();
    };
    box->cancel = [this, close] {
        editor_->cancelGesture();
        close();
    };
    box->show();
    box->setFocus();
    box->selectAll();
}
void Timeline::contextMenuEvent(QContextMenuEvent *event) {
    if (event->pos().y() < timelineHeaderHeight)
        return;
    const auto all = rows();
    const int index = int((event->pos().y() - timelineHeaderHeight) / rowHeight);
    if (index < 0 || index >= int(all.size()))
        return;
    const auto row = all[index];
    if (row.property >= 0 && event->pos().x() >= labelWidth) {
        const auto &l = layer(editor_->project(), row.id);
        std::vector<KeyRef> hits;
        for (auto ref : row.components)
            for (const auto &key : l.channel(ref).keys)
                if (std::abs(x(key.at + l.start) - event->pos().x()) <= 6)
                    hits.push_back({l.id, ref, key.at});
        if (!hits.empty() && std::none_of(keys_.begin(), keys_.end(), [&](const KeyRef &key) {
                return key.layer == hits.front().layer && key.property == hits.front().property &&
                       key.at == hits.front().at;
            })) {
            keys_ = std::move(hits);
            editor_->select({row.id});
            update();
        }
    }
    QMenu menu;
    if (row.property == -1) {
        auto *rename = menu.addAction("Rename layer");
        auto *open = menu.addAction("Open layer view");
        auto *chosen = menu.exec(event->globalPos());
        if (chosen == open) {
            editor_->select({row.id});
            emit layerOpened(row.id);
        } else if (chosen == rename) {
            bool ok;
            auto value = QInputDialog::getText(this, "Layer name", "Name", QLineEdit::Normal,
                                               layer(editor_->project(), row.id).name, &ok);
            if (ok)
                try {
                    editor_->apply("Rename layer", [&](Project &p) {
                        if (layer(p, row.id).locked)
                            throw std::runtime_error("Unlock layer first");
                        layer(p, row.id).name = value;
                    });
                } catch (const std::exception &e) {
                    emit error(e.what());
                }
        }
        return;
    }
    if (row.property < 0)
        return;
    auto *cutKeysAction = menu.addAction("Cut selected keys");
    auto *copyKeysAction = menu.addAction("Copy selected keys");
    auto *pasteKeysAction = menu.addAction("Paste keys at current time");
    auto *deleteKeysAction = menu.addAction("Delete selected keys");
    cutKeysAction->setEnabled(!keys_.empty());
    copyKeysAction->setEnabled(!keys_.empty());
    pasteKeysAction->setEnabled(!copied_.empty() && !editor_->selection().empty());
    deleteKeysAction->setEnabled(!keys_.empty());
    menu.addSeparator();
    auto *copy = menu.addAction("Copy value");
    auto *paste = menu.addAction("Paste value");
    menu.addSeparator();
    auto *reset = menu.addAction("Reset " + row.label);
    menu.addSeparator();
    auto *velocity = menu.addAction("Keyframe Velocity…");
    auto *ease = menu.addAction("Ease selected keys");
    auto *easeIn = menu.addAction("Ease In");
    auto *easeOut = menu.addAction("Ease Out");
    auto *hold = menu.addAction("Selected outgoing segments: Hold");
    auto *linear = menu.addAction("Selected outgoing segments: Linear");
    auto *bezier = menu.addAction("Selected outgoing segments: Bezier");
    hold->setEnabled(!keys_.empty());
    for (auto *action : {velocity, ease, easeIn, easeOut, linear, bezier})
        action->setEnabled(!keys_.empty() && !discreteProperty(row.ref));
    auto *chosen = menu.exec(event->globalPos());
    if (!chosen)
        return;
    if (chosen == velocity) {
        editVelocity();
        return;
    }
    if (chosen == ease || chosen == easeIn || chosen == easeOut) {
        easeKeys(chosen != easeOut, chosen != easeIn);
        return;
    }
    if (chosen == hold || chosen == linear || chosen == bezier) {
        interpolateKeys(chosen == hold     ? Interpolation::Hold
                        : chosen == linear ? Interpolation::Linear
                                           : Interpolation::Cubic);
        return;
    }
    if (chosen == cutKeysAction) {
        cutKeys();
        return;
    }
    if (chosen == copyKeysAction) {
        copyKeys();
        return;
    }
    if (chosen == pasteKeysAction) {
        pasteKeys();
        return;
    }
    if (chosen == deleteKeysAction) {
        deleteKeys();
        return;
    }
    try {
        if (chosen == copy) {
            QApplication::clipboard()->setText(
                copyPropertyValues(editor_->project(), row.id, row.components, now_));
            return;
        }
        editor_->apply(chosen->text(), [&](Project &p) {
            auto &l = layer(p, row.id);
            if (l.locked)
                throw std::runtime_error("Unlock layer first");
            if (chosen == paste) {
                pastePropertyValues(p, row.id, row.components, now_,
                                    QApplication::clipboard()->text().toUtf8());
            } else if (chosen == reset)
                for (auto ref : row.components)
                    resetProperty(p, row.id, ref);
        });
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}
void Timeline::keyPressEvent(QKeyEvent *event) {
    auto *focus = QApplication::focusWidget();
    if (focus && focus != this && isAncestorOf(focus) &&
        (qobject_cast<QLineEdit *>(focus) || qobject_cast<QAbstractSpinBox *>(focus))) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (drag_ == Drag::ViewStart || drag_ == Drag::ViewEnd || drag_ == Drag::ViewPan) {
            cancelViewDrag(true);
            return;
        }
        editor_->cancelGesture();
        drag_ = Drag::None;
        update();
        return;
    }
    if ((event->key() == Qt::Key_Semicolon || event->key() == Qt::Key_Colon) &&
        event->modifiers() == Qt::ShiftModifier) {
        togglePreviousRange();
        return;
    }
    if (event->modifiers() == Qt::NoModifier && event->key() == Qt::Key_Equal) {
        zoomVisible(1.25);
        return;
    }
    if (event->modifiers() == Qt::NoModifier && event->key() == Qt::Key_Minus) {
        zoomVisible(1 / 1.25);
        return;
    }
    if (event->modifiers() == Qt::NoModifier && event->key() == Qt::Key_D) {
        centerVisibleRange(now_);
        return;
    }
    if (event->key() == Qt::Key_J || event->key() == Qt::Key_K) {
        navigateKey(event->key() == Qt::Key_K);
        return;
    }
    if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
        const auto &layers = composition(editor_->project(), comp_).layers;
        if (layers.empty())
            return;
        auto it = std::find_if(layers.begin(), layers.end(), [&](const Layer &l) {
            return !editor_->selection().empty() && l.id == editor_->selection().back();
        });
        int index =
            it == layers.end() ? 0 : int(it - layers.begin()) + (event->key() == Qt::Key_Down ? 1 : -1);
        index = std::clamp(index, 0, int(layers.size()) - 1);
        auto selection = event->modifiers() & Qt::ShiftModifier ? editor_->selection() : std::vector<Id>{};
        if (std::find(selection.begin(), selection.end(), layers[index].id) == selection.end())
            selection.push_back(layers[index].id);
        editor_->select(selection);
        return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        deleteKeys();
        return;
    }
    QWidget::keyPressEvent(event);
}
void Timeline::wheelEvent(QWheelEvent *event) {
    const auto wheelDelta = event->angleDelta().y() ? event->angleDelta().y() : event->pixelDelta().y();
    if (!hasComposition() || !wheelDelta) {
        event->ignore();
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        zoomVisible(std::pow(1.001, wheelDelta));
    } else if (event->modifiers() & Qt::ShiftModifier) {
        const auto &c = composition(editor_->project(), comp_);
        const auto spanFrames = std::max<qint64>(1, std::llround(seconds(viewEnd_ - viewStart_) * seconds(c.fps)));
        auto deltaFrames = std::llround(spanFrames * (wheelDelta / 120.0) / 10.0);
        if (!deltaFrames)
            deltaFrames = wheelDelta > 0 ? 1 : -1;
        const Time delta = frameTime(deltaFrames, c.fps);
        const Time span = viewEnd_ - viewStart_;
        Time start = viewStart_ - delta;
        const Time latest = c.duration - span;
        if (start < time(0))
            start = time(0);
        if (latest < start)
            start = latest;
        setVisibleRange(start, start + span);
        emit navigationStarted();
    } else {
        event->ignore();
        return;
    }
    event->accept();
}
void Timeline::copyKeys() {
    copied_.clear();
    for (const auto &ref : keys_) {
        const auto &source = layer(editor_->project(), ref.layer);
        const auto &ch = source.channel(ref.property);
        auto it = std::find_if(ch.keys.begin(), ch.keys.end(), [&](const Key &k) { return k.at == ref.at; });
        if (it == ch.keys.end())
            continue;
        Copied copy{ref.layer, ref.property, *it, source.start};
        if (ref.property.effect) {
            auto effect = std::find_if(source.effects.begin(), source.effects.end(),
                                       [&](const Effect &e) { return e.id == ref.property.effect; });
            copy.effectSlot = int(effect - source.effects.begin());
            copy.effectType = effect->type;
        }
        copied_.push_back(copy);
    }
}
void Timeline::cutKeys() {
    copyKeys();
    deleteKeys();
}
void Timeline::pasteKeys() {
    if (copied_.empty())
        return;
    try {
        const auto targets = editor_->selection();
        if (targets.empty())
            throw std::runtime_error("Select the destination layer before pasting keys");
        std::set<Id> sources;
        Time first = copied_.front().key.at + copied_.front().start;
        for (const auto &item : copied_) {
            sources.insert(item.layer);
            first = std::min(first, item.key.at + item.start);
        }
        if (sources.size() > 1 && sources != std::set<Id>(targets.begin(), targets.end()))
            throw std::runtime_error("Copy keys from one layer to transfer to other layers");
        std::vector<KeyRef> pasted;
        editor_->apply("Paste keyframes", [&](Project &p) {
            for (Id target : targets) {
                auto &l = layer(p, target);
                if (l.locked)
                    throw std::runtime_error("Unlock layer before pasting keys");
                const auto supported = propertyRows(l, &p);
                for (const auto &item : copied_) {
                    if (sources.size() > 1 && item.layer != target)
                        continue;
                    auto ref = item.property;
                    if (ref.effect) {
                        auto effect = std::find_if(l.effects.begin(), l.effects.end(),
                                                   [&](const Effect &e) { return e.id == ref.effect; });
                        if (effect == l.effects.end()) {
                            if (item.effectSlot < 0 || item.effectSlot >= int(l.effects.size()) ||
                                l.effects[item.effectSlot].type != item.effectType)
                                throw std::runtime_error(
                                    "Destination needs the matching effect at the copied stack position");
                            ref.effect = l.effects[item.effectSlot].id;
                        }
                    }
                    if (std::none_of(supported.begin(), supported.end(),
                                     [&](const PropertyRow &row) { return row.ref == ref; }))
                        throw std::runtime_error("The destination layer does not expose this property");
                    auto key = item.key;
                    key.at = now_ + (item.key.at + item.start - first) - l.start;
                    putKey(l.channel(ref), key);
                    pasted.push_back({target, ref, key.at});
                }
            }
        });
        keys_ = std::move(pasted);
        if (!keys_.empty())
            emit keySelected(keys_.front());
        emit status(QString("Pasted %1 keyframes onto %2 layer(s)").arg(keys_.size()).arg(targets.size()));
        for (Id id : targets)
            expanded_.insert(id);
        changed();
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}
void Timeline::deleteKeys() {
    if (keys_.empty())
        return;
    try {
        editor_->apply("Delete keyframes", [&](Project &p) {
            for (const auto &key : keys_) {
                auto &l = layer(p, key.layer);
                if (l.locked)
                    throw std::runtime_error("Unlock layer before deleting keys");
                eraseKey(l.channel(key.property), key.at);
            }
        });
        keys_.clear();
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}
void Timeline::editVelocity() {
    try {
        editKeyframeVelocity(editor_, keys_, this);
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}
void Timeline::interpolateKeys(Interpolation mode) {
    if (keys_.empty()) {
        emit error("Select keyframes first");
        return;
    }
    try {
        editor_->apply("Keyframe interpolation", [&](Project &p) {
            for (const auto &key : keys_)
                setKeyInterpolation(p, key, mode);
        });
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}
void Timeline::easeKeys(bool incoming, bool outgoing) {
    if (keys_.empty()) {
        emit error("Select keyframes first");
        return;
    }
    try {
        editor_->apply("Ease selected keyframes", [&](Project &p) {
            for (const auto &key : keys_)
                easeKey(p, key, incoming, outgoing);
        });
    } catch (const std::exception &e) {
        emit error(e.what());
    }
}

} // namespace motion

namespace motion {
void Timeline::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasFormat("application/x-motionproof-item") || e->mimeData()->hasUrls())
        e->acceptProposedAction();
}
void Timeline::dragMoveEvent(QDragMoveEvent *e) {
    if (e->mimeData()->hasFormat("application/x-motionproof-item") || e->mimeData()->hasUrls())
        e->acceptProposedAction();
}
void Timeline::dropEvent(QDropEvent *e) {
    const auto t = e->position().x() < labelWidth ? now_ : at(e->position().x());
    auto bytes = e->mimeData()->data("application/x-motionproof-item");
    if (!bytes.isEmpty() && bytes.size() < 4096) {
        auto object = QJsonDocument::fromJson(bytes).object();
        bool ok;
        auto id = object["id"].toString().toULongLong(&ok);
        auto kind = object["kind"].toString();
        if (ok && (kind == "asset" || kind == "comp")) {
            emit itemDropped(id, kind == "comp", object["token"].toString(), t);
            e->acceptProposedAction();
            return;
        }
    }
    const auto urls = e->mimeData()->urls();
    if (urls.size() == 1 && urls.front().isLocalFile()) {
        emit fileDropped(urls.front().toLocalFile(), t);
        e->acceptProposedAction();
    } else
        emit error("Drop one local media file at a time");
}
} // namespace motion
