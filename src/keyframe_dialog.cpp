// SPDX-License-Identifier: MPL-2.0
#include "keyframe_dialog.hpp"
#include "numeric_control.hpp"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
namespace motion {
namespace {
class VelocityNumber final : public QDoubleSpinBox {
    QString textFromValue(double value) const override {
        return compactFixedNumber(QDoubleSpinBox::textFromValue(value), locale().decimalPoint());
    }
};
} // namespace
void editKeyframeVelocity(Editor *editor, const std::vector<KeyRef> &keys, QWidget *parent) {
    if (keys.empty())
        throw std::runtime_error("Select a keyframe first");
    const auto unit = parameterSpec(keys.front().property.id).unit;
    for (auto key : keys) {
        if (layer(editor->project(), key.layer).locked)
            throw std::runtime_error("Unlock selected layers first");
        if (discreteProperty(key.property))
            throw std::runtime_error("Discrete properties require Hold interpolation");
        if (parameterSpec(key.property.id).unit != unit)
            throw std::runtime_error("Select keyframes with the same display units");
    }
    QDialog dialog(parent);
    dialog.setObjectName("keyframeVelocityDialog");
    dialog.setWindowTitle("Keyframe Velocity");
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QString("%1 selected component keyframe(s)").arg(keys.size())));
    struct Side {
        QCheckBox *apply;
        QDoubleSpinBox *speed, *influence;
    } sides[2];
    for (int side = 0; side < 2; ++side) {
        const bool incoming = side == 0;
        const QString prefix = incoming ? "incoming" : "outgoing";
        auto *group = new QGroupBox(incoming ? "Incoming Velocity" : "Outgoing Velocity");
        auto *form = new QFormLayout(group);
        auto &widgets = sides[side];
        widgets.apply = new QCheckBox("Apply this side");
        widgets.apply->setObjectName(prefix + "Apply");
        form->addRow(widgets.apply);
        widgets.speed = new VelocityNumber;
        widgets.speed->setObjectName(prefix + "Speed");
        widgets.speed->setDecimals(8);
        widgets.speed->setRange(-1e12, 1e12);
        widgets.speed->setSuffix(" " + (unit.isEmpty() ? "units" : unit) + "/s");
        widgets.influence = new VelocityNumber;
        widgets.influence->setObjectName(prefix + "Influence");
        widgets.influence->setDecimals(8);
        widgets.influence->setRange(.1, 100);
        widgets.influence->setSuffix(" %");
        form->addRow("Speed", widgets.speed);
        form->addRow("Influence", widgets.influence);
        std::optional<KeyVelocity> initial;
        bool mixed = false;
        for (auto key : keys) {
            auto value = keyVelocity(editor->project(), key, incoming);
            if (!value)
                continue;
            value->speed = toDisplay(key.property, value->speed);
            if (!initial)
                initial = value;
            else
                mixed |= initial->speed != value->speed || initial->influence != value->influence;
        }
        if (initial) {
            widgets.speed->setValue(initial->speed);
            widgets.influence->setValue(initial->influence);
        } else {
            group->setEnabled(false);
            form->addRow(new QLabel("No adjacent segment or collapsed handle. Use Value Graph."));
        }
        if (mixed)
            form->addRow(new QLabel("Mixed values — edited fields apply to all selected keys."));
        QObject::connect(widgets.speed, qOverload<double>(&QDoubleSpinBox::valueChanged), widgets.apply,
                         [apply = widgets.apply, box = widgets.speed] {
                             box->setProperty("edited", true);
                             apply->setChecked(true);
                         });
        QObject::connect(widgets.influence, qOverload<double>(&QDoubleSpinBox::valueChanged), widgets.apply,
                         [apply = widgets.apply, box = widgets.influence] {
                             box->setProperty("edited", true);
                             apply->setChecked(true);
                         });
        for (auto *box : {widgets.speed, widgets.influence})
            QObject::connect(box->findChild<QLineEdit *>(), &QLineEdit::textEdited, widgets.apply,
                             [box, apply = widgets.apply] {
                                 box->setProperty("edited", true);
                                 apply->setChecked(true);
                             });
        layout->addWidget(group);
    }
    layout->addWidget(new QLabel("Only checked sides change. Speed is signed for the selected component."));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    editor->apply("Keyframe velocity", [&](Project &p) {
        for (const auto &key : keys)
            for (int side = 0; side < 2; ++side)
                if (sides[side].apply->isChecked() && sides[side].apply->isEnabled()) {
                    const auto old = keyVelocity(p, key, side == 0);
                    const auto &widgets = sides[side];
                    const double speed = old && !widgets.speed->property("edited").toBool()
                                             ? old->speed
                                             : fromDisplay(key.property, widgets.speed->value());
                    const double influence =
                        old && old->influence >= .1 && !widgets.influence->property("edited").toBool()
                            ? old->influence
                            : widgets.influence->value();
                    setKeyVelocity(p, key, side == 0, {speed, influence});
                }
    });
}
} // namespace motion
