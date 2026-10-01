// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "editor.hpp"
#include <QRectF>
#include <QWidget>
#include <set>
#include <utility>
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
namespace motion {
class Timeline : public QWidget {
    Q_OBJECT
  public:
    explicit Timeline(Editor *, QWidget *parent = nullptr);
    void setComposition(Id);
    void setTime(Time);
    void setFilter(const QString &);
    void setSearch(const QString &text) {
        search_ = text;
        changed();
    }
    static constexpr int rowPitch() { return 17; }
    static constexpr int headerHeight() { return 76; }
    qint64 rulerStepFrames() const;
    double timeX(Time at) const { return x(at); }
    std::pair<Time, Time> visibleRange() const { return {viewStart_, viewEnd_}; }
    QRectF navigatorRect() const;
    QRectF navigatorSelectionRect() const;
    QPointF navigatorStartHandleCenter() const;
    QPointF navigatorEndHandleCenter() const;
    QRectF workAreaRect() const;
    QPointF workAreaStartHandleCenter() const;
    QPointF workAreaEndHandleCenter() const;
    void setWorkAreaBoundary(bool end);
    QRectF motionBlurCellRect(Id layer) const;
    double propertyY(Id, PropertyRef) const;
    void navigateKey(bool forward);
    QString filter() const { return filter_; }
    void copyKeys();
    void cutKeys();
    void pasteKeys();
    void deleteKeys();
    void easeKeys(bool incoming = true, bool outgoing = true);
    void editVelocity();
    void selectKey(KeyRef ref) {
        keys_ = {ref};
        changed();
    }
    void interpolateKeys(Interpolation);
  signals:
    void timeChanged(motion::Time);
    void navigationStarted();
    void propertySelected(motion::PropertyRef);
    void error(QString);
    void status(QString);
    void layerOpened(motion::Id);
    void keySelected(motion::KeyRef);
    void itemDropped(motion::Id, bool composition, QString token, motion::Time);
    void fileDropped(QString, motion::Time);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dragMoveEvent(QDragMoveEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    struct Row {
        Id id;
        int property;
        PropertyRef ref{};
        QString group{}, label{};
        std::vector<PropertyRef> components;
    };
    std::vector<Row> rows() const;
    bool hasComposition() const;
    double plotWidth() const;
    double x(Time) const;
    double navigatorX(Time) const;
    Time at(double, bool snap = true) const;
    QRectF workAreaTrackRect() const;
    void updateWorkAreaDrag(double px);
    void setVisibleRange(Time, Time, bool remember = true);
    void resetVisibleRange();
    void zoomVisible(double factor);
    void centerVisibleRange(Time);
    void toggleFrameGrid();
    void togglePreviousRange();
    void moveViewDrag(double x);
    void cancelViewDrag(bool restore);
    void changed();
    Editor *editor_;
    Id comp_ = 1;
    Time now_;
    QString filter_, search_;
    PropertyRef editProperty_;
    double editStart_ = 0;
    std::set<Id> expanded_;
    std::set<QString> closedGroups_;
    std::vector<KeyRef> keys_;
    struct Copied {
        Id layer;
        PropertyRef property;
        Key key;
        Time start{};
        int effectSlot = -1;
        QString effectType{};
    };
    std::vector<Copied> copied_, dragCopies_;
    enum class Drag {
        None, Scrub, Key, Move, TrimIn, TrimOut, Reorder, Box, Value, ViewStart, ViewEnd, ViewPan,
        WorkAreaStart, WorkAreaEnd, WorkAreaMove
    } drag_ = Drag::None;
    QPointF origin_, cursor_;
    Id dragLayer_ = 0;
    Time originTime_, keyDelta_;
    Time viewStart_{}, viewEnd_{12, 1}, viewDuration_{}, viewFps_{};
    Time previousStart_{}, previousEnd_{12, 1};
    bool previousRangeValid_ = false;
    Time dragViewStart_{}, dragViewEnd_{}, dragPreviousStart_{}, dragPreviousEnd_{};
    bool dragPreviousRangeValid_ = false;
    std::int64_t dragWorkAreaStartFrame_ = 0, dragWorkAreaEndFrame_ = 0;
    std::int64_t dragWorkAreaPointerFrame_ = 0;
};
} // namespace motion
