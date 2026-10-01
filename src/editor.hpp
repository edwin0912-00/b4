// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "project_io.hpp"
#include <QObject>
#include <QUndoStack>
#include <functional>
namespace motion {
struct KeyRef {
    Id layer;
    PropertyRef property;
    Time at;
};
struct KeyVelocity {
    double speed = 0, influence = 100. / 3;
};
std::optional<KeyVelocity> keyVelocity(const Project &, KeyRef, bool incoming);
void setKeyVelocity(Project &, KeyRef, bool incoming, KeyVelocity);
void setKeyInterpolation(Project &, KeyRef, Interpolation);
void easeKey(Project &, KeyRef, bool incoming = true, bool outgoing = true);
void setProperty(Project &, Id, PropertyRef, Time, double);
QByteArray copyPropertyValues(const Project &, Id, const std::vector<PropertyRef> &, Time);
void pastePropertyValues(Project &, Id, const std::vector<PropertyRef> &, Time, const QByteArray &);
void setAnimated(Project &, Id, PropertyRef, Time, bool);
void resetProperty(Project &, Id, PropertyRef);
Id addEffect(Project &, Id layer);
Id addEffect(Project &, Id layer, const QString &type);
void moveEffect(Project &, Id layer, Id effect, int offset);
void putKey(Channel &, Key);
void eraseKey(Channel &, Time);
void moveKey(Channel &, Time, Time);
void nativeEase(Channel &, Time, Time);
void reparent(Project &, Id comp, Id child, std::optional<Id> parent, Time);
Id precompose(Project &, Id comp, const std::vector<Id> &);
Id duplicateLayer(Project &, Id comp, Id source);
class Editor : public QObject {
    Q_OBJECT
  public:
    explicit Editor(Project initial = {}, QObject *parent = nullptr);
    const Project &project() const { return document_; }
    std::uint64_t revision() const { return revision_; }
    QUndoStack &undoStack() { return undo_; }
    bool scaleLinked() const { return scaleLinked_; }
    void setScaleLinked(bool on) {
        scaleLinked_ = on;
        emit selectionChanged();
    }
    void setNumericValue(Project &, Id, PropertyRef, Time, double) const;
    bool gesturing() const { return gesture_.has_value(); }
    bool dirty() const { return !undo_.isClean() || gesture_.has_value(); }
    const std::vector<Id> &selection() const { return selection_; }
    void select(std::vector<Id>);
    void apply(const QString &, std::function<void(Project &)>);
    void beginGesture();
    void previewGesture(std::function<void(Project &)>);
    void commitGesture(const QString &);
    void cancelGesture();
    void replace(Project);
    void install(Project); // used by undo commands; input was already validated
  signals:
    void changed();
    void selectionChanged();
    void keySelectionReset();
    void documentReplaced();

  private:
    bool scaleLinked_ = true;
    Project document_;
    std::uint64_t revision_ = 0;
    QUndoStack undo_;
    std::vector<Id> selection_;
    std::optional<Project> gesture_;
};
} // namespace motion
