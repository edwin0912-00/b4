// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "editor.hpp"
#include <QWidget>
class QTreeWidget;
class QTreeWidgetItem;
class QDoubleSpinBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
namespace motion {
class PropertyPanel : public QWidget {
    Q_OBJECT
  public:
    enum class Section { Properties, Effects };
    explicit PropertyPanel(Editor *, QWidget *parent = nullptr, Section section = Section::Properties);
    void addEffect(const QString &type);
    void addColorEffect();
    void setTime(Time t) {
        now_ = t;
        refresh();
    }
    void refresh();
  signals:
    void propertySelected(motion::PropertyRef);
    void error(QString);
    void timeChanged(motion::Time);

  private:
    struct Control {
        PropertyRef ref;
        QTreeWidgetItem *item;
        QDoubleSpinBox *number = nullptr;
        QComboBox *choice = nullptr;
        QLineEdit *text = nullptr;
        QComboBox *fontChoice = nullptr;
        QPlainTextEdit *multiline = nullptr;
        QPushButton *clock = nullptr;
        QPushButton *link = nullptr;
    };
    void rebuild(const Layer &);
    void edit(const QString &, std::function<void(Project &, Id)>);
    void setValue(PropertyRef, double);
    void filterRows();
    Editor *editor_;
    QTreeWidget *tree_;
    QLineEdit *search_;
    Section section_;
    Time now_;
    bool refreshing_ = false;
    QString signature_;
    std::vector<Control> controls_;
};
} // namespace motion
