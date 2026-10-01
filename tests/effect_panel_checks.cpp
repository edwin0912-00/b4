// SPDX-License-Identifier: MPL-2.0
#include "editor.hpp"
#include "property_panel.hpp"
#include "support.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <algorithm>
#include <iostream>
using namespace motion;

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        Editor editor(solidScene());
        editor.select({2});
        PropertyPanel panel(&editor, nullptr, PropertyPanel::Section::Effects);
        panel.refresh();
        panel.show();
        app.processEvents();

        auto *types = panel.findChild<QComboBox *>("effectTypeSelector");
        auto *add = panel.findChild<QPushButton *>("addEffectButton");
        auto *tree = panel.findChild<QTreeWidget *>("effectControlsTree");
        check(types && add && tree, "effect panel exposes its type selector, add button, and rows");
        check(types->count() == 4 && types->count() == int(effectSpecs().size()),
              "type selector lists exactly the four supported effects");
        for (int i = 0; i < types->count(); ++i) {
            const auto &spec = effectSpecs().at(size_t(i));
            check(types->itemText(i) == spec.name && types->itemData(i).toString() == spec.type,
                  "effect labels map through stable type IDs");
            check(types->itemData(i, Qt::ToolTipRole).toString() == spec.category,
                  "effect category comes from its descriptor");
        }

        add->click();
        int exposureIndex = -1;
        for (int i = 0; i < types->count(); ++i)
            if (types->itemData(i).toString() == "motion.exposure")
                exposureIndex = i;
        check(exposureIndex >= 0, "effect browser includes Exposure by stable ID");
        types->setCurrentIndex(exposureIndex);
        add->click();
        for (const auto &spec : effectSpecs())
            if (spec.type != "motion.linear-color" && spec.type != "motion.exposure")
                panel.addEffect(spec.type);
        panel.refresh();
        app.processEvents();

        const auto effects = layer(editor.project(), 2).effects;
        check(effects.size() == effectSpecs().size(), "typed add creates each supported effect once");
        check(tree->topLevelItemCount() == int(effects.size()), "one compact group is rendered per effect");
        for (int i = 0; i < int(effects.size()); ++i) {
            const auto &effect = effects[size_t(i)];
            const auto &descriptors = effectParameterSpecs(effect.type);
            auto *group = tree->topLevelItem(i);
            check(group->text(1) == effect.name && group->childCount() == int(descriptors.size()),
                  "effect group renders exactly its typed parameter descriptors");
            for (int row = 0; row < int(descriptors.size()); ++row) {
                const auto &spec = descriptors[size_t(row)];
                check(group->child(row)->text(1) == spec.label, "effect row uses its descriptor label");
                const int components = std::max(1, int(spec.defaults.size()));
                for (int component = 0; component < components; ++component) {
                    const auto objectName = spec.id + "." + QString::number(component) + "." +
                                            QString::number(effect.id);
                    if (spec.type == ParameterType::Boolean || spec.type == ParameterType::Enum) {
                        check(panel.findChild<QComboBox *>(objectName) != nullptr,
                              "discrete effect parameter has a choice control");
                    } else {
                        auto *number = panel.findChild<QDoubleSpinBox *>(objectName);
                        check(number != nullptr, "numeric effect parameter has a value control");
                        check(number->suffix() == (spec.unit.isEmpty() ? QString{} : " " + spec.unit),
                              "effect control displays its descriptor unit");
                    }
                }
            }
        }

        Id exposure = 0;
        for (const auto &effect : effects)
            if (effect.type == "motion.exposure")
                exposure = effect.id;
        check(exposure != 0, "typed Exposure effect exists");
        const PropertyRef stops{"exposure.stops", 0, exposure};
        auto *stopsControl = panel.findChild<QDoubleSpinBox *>(
            stops.id + ".0." + QString::number(exposure));
        check(stopsControl != nullptr, "Exposure stops control is rendered");
        const double original = layer(editor.project(), 2).channel(stops).base;
        stopsControl->setValue(original + 1.25);
        QMetaObject::invokeMethod(stopsControl, "editingFinished");
        check(near(layer(editor.project(), 2).channel(stops).base, original + 1.25),
              "widget edit updates the typed effect parameter");
        editor.undoStack().undo();
        check(near(layer(editor.project(), 2).channel(stops).base, original) &&
                  layer(editor.project(), 2).effects.size() == effects.size(),
              "undo restores the value without removing the effect");

        panel.addColorEffect();
        check(layer(editor.project(), 2).effects.back().type == "motion.linear-color",
              "legacy addColorEffect forwards to typed Linear Color");
        editor.undoStack().undo();
        std::cout << "OK " << checkCount << " effect panel checks\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
