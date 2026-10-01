// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "editor.hpp"
#include <QWidget>
namespace motion {
class Graph : public QWidget {
    Q_OBJECT
  public:
    Graph(Editor *, QWidget *parent = nullptr);
    void setComposition(Id id) {
        if (comp_ != id)
            selectedTime_.reset();
        comp_ = id;
        update();
    }
    void setProperty(PropertyRef p) {
        if (!(property_ == p))
            selectedTime_.reset();
        property_ = p;
        update();
    }
    void setSpeedGraph(bool on) {
        speedGraph_ = on;
        update();
    }
    bool speedGraph() const { return speedGraph_; }
    std::optional<KeyRef> keySelection() const { return selectedKey(); }
    void selectKey(KeyRef);
    QPointF keyPosition(Time);
    void editVelocity();
    void easeSelected(bool incoming = true, bool outgoing = true);
    void interpolateSelected(Interpolation);
    void setTime(Time t) {
        now_ = t;
        update();
    }
  signals:
    void error(QString);
    void timeChanged(motion::Time);
    void keySelected(motion::KeyRef);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

  private:
    const Layer *selected() const;
    std::optional<KeyRef> selectedKey() const;
    double graphValue(const Channel &, Time) const;
    QPointF point(double seconds, double value) const;
    Vec2 graphPoint(QPointF) const;
    void range();
    Editor *editor_;
    Id comp_ = 1;
    PropertyRef property_ = Property::PositionX;
    Time now_;
    double low_ = -10, high_ = 110, duration_ = 12;
    int key_ = -1, handle_ = 0;
    Id layer_ = 0;
    Key original_;
    bool dragging_ = false, speedGraph_ = false;
    std::optional<Time> selectedTime_;
    Id selectedLayer_ = 0;
};
} // namespace motion
