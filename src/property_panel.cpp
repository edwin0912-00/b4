// SPDX-License-Identifier: MPL-2.0
#include "property_panel.hpp"
#include "evaluate.hpp"
#include "numeric_control.hpp"
#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace motion {
namespace {
QString controlName(PropertyRef ref) {
    for (int i = 0; i < 8; ++i)
        if (ref == PropertyRef(Property(i)))
            return "property" + QString::number(i);
    return ref.id + "." + QString::number(ref.component) +
           (ref.effect ? "." + QString::number(ref.effect) : "");
}
QIcon clockIcon() {
    QPixmap pix(14, 14);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor("#b8b8b8"), 1));
    p.drawEllipse(QRectF(3, 4, 8, 8));
    p.drawLine(7, 1, 7, 4);
    p.drawLine(5, 1, 9, 1);
    p.drawLine(7, 6, 7, 9);
    p.drawLine(7, 9, 9, 9);
    return QIcon(pix);
}
QPushButton *smallButton(const QString &label, const QString &tip, QWidget *parent = nullptr) {
    auto *b = new QPushButton(label, parent);
    b->setFixedSize(16, 17);
    b->setFlat(true);
    b->setToolTip(tip);
    b->setAccessibleName(tip);
    b->setStyleSheet("QPushButton{padding:0;border:0;background:transparent;} "
                     "QPushButton:hover{background:#424242;} QPushButton:checked{background:#37516c;}");
    return b;
}
} // namespace
PropertyPanel::PropertyPanel(Editor *editor, QWidget *parent, Section section)
    : QWidget(parent), editor_(editor), section_(section) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    search_ = new QLineEdit;
    search_->setClearButtonEnabled(true);
    search_->setFixedHeight(22);
    search_->setPlaceholderText(section == Section::Effects ? "Search effect controls" : "Search properties");
    search_->setObjectName(section == Section::Effects ? "effectControlSearch" : "parameterSearch");
    layout->addWidget(search_);
    tree_ = new QTreeWidget;
    tree_->setObjectName(section == Section::Effects ? "effectControlsTree" : "parameterTree");
    tree_->setColumnCount(3);
    tree_->setHeaderHidden(true);
    tree_->setIndentation(10);
    tree_->setRootIsDecorated(true);
    tree_->setTreePosition(1);
    tree_->setUniformRowHeights(false);
    tree_->setFrameShape(QFrame::NoFrame);
    tree_->setMinimumHeight(140);
    tree_->setStyleSheet("QTreeWidget::item{min-height:17px;} "
                         "QDoubleSpinBox{background:transparent;color:#43a5f5;padding:0;border:0;} "
                         "QDoubleSpinBox:focus{background:#171717;border-bottom:1px solid #69a9ef;}");
    tree_->header()->setStretchLastSection(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    tree_->setColumnWidth(0, 20);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    tree_->setColumnWidth(1, 106);
    tree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(tree_, 1);
    connect(search_, &QLineEdit::textChanged, this, &PropertyPanel::filterRows);
    if (section == Section::Effects) {
        auto *types = new QComboBox;
        types->setObjectName("effectTypeSelector");
        types->setFixedHeight(22);
        types->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        types->setMinimumContentsLength(10);
        int linearColor = -1;
        for (const auto &spec : effectSpecs()) {
            types->addItem(spec.name, spec.type);
            types->setItemData(types->count() - 1, spec.category, Qt::ToolTipRole);
            if (spec.type == "motion.linear-color")
                linearColor = types->count() - 1;
        }
        if (linearColor >= 0)
            types->setCurrentIndex(linearColor);
        auto *add = new QPushButton("+ Add effect");
        add->setObjectName("addEffectButton");
        add->setFixedHeight(22);
        auto *actions = new QHBoxLayout;
        actions->setContentsMargins(0, 0, 0, 0);
        actions->setSpacing(2);
        actions->addWidget(types, 1);
        actions->addWidget(add);
        layout->addLayout(actions);
        connect(add, &QPushButton::clicked, this,
                [this, types] { addEffect(types->currentData().toString()); });
    }
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (refreshing_)
            return;
        for (const auto &c : controls_)
            if (c.item == item && !c.text && !c.multiline) {
                emit propertySelected(c.ref);
                break;
            }
    });
}
void PropertyPanel::edit(const QString &label, std::function<void(Project &, Id)> action) {
    if (refreshing_ || !isEnabled() || editor_->selection().empty())
        return;
    try {
        const auto selected = editor_->selection();
        editor_->apply(label, [&](Project &p) {
            for (Id id : selected) {
                if (layer(p, id).locked)
                    throw std::runtime_error("Unlock layer first");
                action(p, id);
            }
        });
    } catch (const std::exception &e) {
        emit error(e.what());
        refresh();
    }
}
void PropertyPanel::addEffect(const QString &type) {
    edit("Add " + effectSpec(type).name,
         [type](Project &p, Id id) { motion::addEffect(p, id, type); });
}
void PropertyPanel::addColorEffect() {
    addEffect("motion.linear-color");
}
void PropertyPanel::setValue(PropertyRef ref, double value) {
    edit("Edit " + propertyLabel(ref), [=, this](Project &p, Id id) {
        editor_->setNumericValue(p, id, ref, sourceTime(layer(p, id), now_), fromDisplay(ref, value));
    });
}
void PropertyPanel::filterRows() {
    const auto query = search_->text().trimmed();
    for (int g = 0; g < tree_->topLevelItemCount(); ++g) {
        auto *group = tree_->topLevelItem(g);
        bool any = false;
        for (int i = 0; i < group->childCount(); ++i) {
            auto *item = group->child(i);
            bool show = query.isEmpty() || item->text(1).contains(query, Qt::CaseInsensitive) ||
                        group->text(1).contains(query, Qt::CaseInsensitive);
            item->setHidden(!show);
            any |= show;
        }
        group->setHidden(!any);
        if (!query.isEmpty() && any)
            group->setExpanded(true);
    }
}
void PropertyPanel::rebuild(const Layer &l) {
    std::map<QString, bool> expanded;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto *g = tree_->topLevelItem(i);
        expanded[g->data(1, Qt::UserRole).toString()] = g->isExpanded();
    }
    controls_.clear();
    tree_->clear();
    std::map<QString, QTreeWidgetItem *> groups;
    auto group = [&](QString key, QString title) {
        if (!groups.count(key)) {
            auto *g = new QTreeWidgetItem(tree_, {"", title});
            g->setData(1, Qt::UserRole, key);
            g->setFirstColumnSpanned(false);
            g->setText(0, {});
            g->setExpanded(expanded.count(key)
                               ? expanded[key]
                               : key == "Transform" || key == "Text" ||
                                     (key == "Mask" && l.base("mask.enabled")) || key.startsWith("effect:"));
            groups[key] = g;
        }
        return groups[key];
    };
    auto build = [&](const ParameterSpec &spec, Id effect, QString groupKey, QString title) {
        auto *item = new QTreeWidgetItem(group(groupKey, title), {"", spec.label});
        item->setSizeHint(1, QSize(90, 17));
        auto *values = new QWidget;
        auto *row = new QHBoxLayout(values);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(2);
        tree_->setItemWidget(item, 2, values);
        std::vector<PropertyRef> refs;
        for (int i = 0; i < int(spec.defaults.size()); ++i)
            refs.push_back({spec.id, i, effect});
        if (refs.empty())
            refs.push_back({spec.id, 0, effect});
        QPushButton *clock = nullptr, *link = nullptr;
        if (spec.animated) {
            clock = smallButton("", "Toggle animation for " + spec.label);
            clock->setIcon(clockIcon());
            clock->setIconSize(QSize(14, 14));
            clock->setCheckable(true);
            clock->setObjectName("clock." + controlName(refs.front()));
            tree_->setItemWidget(item, 0, clock);
            connect(clock, &QPushButton::clicked, this, [=, this](bool on) {
                edit("Toggle animation", [=, this](Project &p, Id id) {
                    for (const auto &ref : refs)
                        setAnimated(p, id, ref, sourceTime(layer(p, id), now_), on);
                });
            });
        }
        if (spec.type == ParameterType::Text) {
            Control c{refs.front(), item};
            if (spec.id == "text.source") {
                c.multiline = new QPlainTextEdit;
                c.multiline->setObjectName("layerText");
                c.multiline->setFixedHeight(54);
                item->setSizeHint(1, QSize(90, 56));
                row->addWidget(c.multiline, 1);
                auto *apply = smallButton("✓", "Apply source text");
                apply->setObjectName("applyTextButton");
                row->addWidget(apply);
                connect(apply, &QPushButton::clicked, this, [this, box = c.multiline] {
                    const auto text = box->toPlainText();
                    edit("Edit source text",
                         [=](Project &p, Id id) { layer(p, id).string("text.source") = text; });
                });
            } else {
                c.fontChoice = new QComboBox;
                c.fontChoice->setEditable(true);
                c.fontChoice->setFixedHeight(17);
                c.fontChoice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
                c.fontChoice->setMinimumContentsLength(4);
                c.fontChoice->setObjectName(spec.id + ".choices");
                if (spec.id == "text.family")
                    c.fontChoice->addItems(QFontDatabase::families());
                c.text = c.fontChoice->lineEdit();
                c.text->setObjectName(spec.id);
                row->addWidget(c.fontChoice, 1);
                connect(c.fontChoice, qOverload<int>(&QComboBox::activated), this,
                        [this, box = c.fontChoice, id = spec.id] {
                            auto value = box->currentText();
                            edit("Choose font",
                                 [=](Project &p, Id layerId) { layer(p, layerId).string(id) = value; });
                        });
                connect(c.text, &QLineEdit::textEdited, this,
                        [box = c.text] { box->setProperty("userEdited", true); });
                connect(c.text, &QLineEdit::editingFinished, this, [this, box = c.text, id = spec.id] {
                    if (!box->property("userEdited").toBool())
                        return;
                    box->setProperty("userEdited", false);
                    auto value = box->text();
                    edit("Edit text", [=](Project &p, Id layerId) { layer(p, layerId).string(id) = value; });
                });
            }
            controls_.push_back(c);
        } else {
            for (const auto &ref : refs) {
                Control c{ref, item};
                c.clock = clock;
                if (discreteProperty(ref)) {
                    c.choice = new QComboBox;
                    c.choice->setFixedHeight(17);
                    c.choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
                    c.choice->setMinimumContentsLength(4);
                    c.choice->addItems(spec.type == ParameterType::Boolean ? QStringList{"Off", "On"}
                                                                           : spec.choices);
                    c.choice->setObjectName(controlName(ref));
                    row->addWidget(c.choice, 1);
                    connect(c.choice, qOverload<int>(&QComboBox::activated), this,
                            [this, ref](int value) { setValue(ref, value); });
                } else {
                    auto *box = new ScrubNumber(values);
                    c.number = box;
                    box->setObjectName(controlName(ref));
                    box->setDecimals(8);
                    box->angle = ref.id == "transform.rotation";
                    box->setRange(toDisplay(ref, spec.minimum), toDisplay(ref, spec.maximum));
                    if (!spec.unit.isEmpty() && !box->angle)
                        box->setSuffix(" " + spec.unit);
                    box->setToolTip(propertyLabel(ref) + " · drag to scrub, Shift faster, Alt finer");
                    row->addWidget(box, 1);
                    connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, box] {
                        if (!refreshing_)
                            box->setProperty("userEdited", true);
                    });
                    connect(box, &QDoubleSpinBox::editingFinished, this, [this, box, ref] {
                        if (refreshing_ || !box->property("userEdited").toBool())
                            return;
                        box->setProperty("userEdited", false);
                        setValue(ref, box->value());
                    });
                    box->focused = [this, ref] { emit propertySelected(ref); };
                    box->begin = [this] { editor_->beginGesture(); };
                    box->scrub = [this, ref](double value) {
                        try {
                            editor_->previewGesture([&](Project &p) {
                                for (Id id : editor_->selection())
                                    editor_->setNumericValue(p, id, ref, sourceTime(layer(p, id), now_),
                                                             fromDisplay(ref, value));
                            });
                        } catch (const std::exception &e) {
                            emit error(e.what());
                        }
                    };
                    box->finish = [this] { editor_->commitGesture("Scrub parameter"); };
                    box->cancel = [this] { editor_->cancelGesture(); };
                }
                controls_.push_back(c);
            }
            if (spec.id == "transform.scale") {
                link = smallButton("↔", "Constrain Scale proportions");
                link->setObjectName("scaleLink");
                link->setCheckable(true);
                link->setChecked(editor_->scaleLinked());
                row->addWidget(link);
                connect(link, &QPushButton::clicked, this, [this](bool on) { editor_->setScaleLinked(on); });
                for (auto &c : controls_)
                    if (c.item == item)
                        c.link = link;
            }
            auto *key = smallButton("◆", "Add or update keyframe");
            key->setObjectName("key." + controlName(refs.front()));
            row->addWidget(key);
            connect(key, &QPushButton::clicked, this, [=, this] {
                edit("Add keyframe", [=, this](Project &p, Id id) {
                    auto &layerRef = layer(p, id);
                    auto t = sourceTime(layerRef, now_);
                    for (const auto &ref : refs) {
                        auto &ch = layerRef.channel(ref);
                        putKey(ch, {t, valueAt(ch, t),
                                    discreteProperty(ref) ? Interpolation::Hold : Interpolation::Linear});
                    }
                });
            });
        }
        auto *reset = smallButton("↺", "Reset " + spec.label);
        reset->setObjectName("reset." + controlName(refs.front()));
        row->addWidget(reset);
        connect(reset, &QPushButton::clicked, this, [=, this] {
            edit("Reset " + spec.label, [=](Project &p, Id id) {
                if (spec.type == ParameterType::Text)
                    layer(p, id).string(spec.id) = spec.defaultText;
                else
                    for (const auto &ref : refs)
                        resetProperty(p, id, ref);
            });
        });
        values->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(values, &QWidget::customContextMenuRequested, this, [=, this](QPoint point) {
            QMenu menu;
            auto *copy = menu.addAction("Copy value");
            auto *paste = menu.addAction("Paste value");
            copy->setEnabled(spec.type != ParameterType::Text);
            paste->setEnabled(spec.type != ParameterType::Text);
            menu.addSeparator();
            auto *previous = menu.addAction("Previous keyframe");
            auto *next = menu.addAction("Next keyframe");
            auto *clear = menu.addAction("Remove key at current time");
            menu.addSeparator();
            auto *restore = menu.addAction("Reset " + spec.label);
            previous->setEnabled(spec.animated);
            next->setEnabled(spec.animated);
            clear->setEnabled(spec.animated);
            auto *action = menu.exec(values->mapToGlobal(point));
            if (!action)
                return;
            if (action == copy && !editor_->selection().empty()) {
                QApplication::clipboard()->setText(
                    copyPropertyValues(editor_->project(), editor_->selection().front(), refs, now_));
                return;
            }
            if (action == paste) {
                const auto bytes = QApplication::clipboard()->text().toUtf8();
                edit("Paste " + spec.label,
                     [=, this](Project &p, Id id) { pastePropertyValues(p, id, refs, now_, bytes); });
                return;
            }
            if (action == restore) {
                reset->click();
                return;
            }
            if (action == clear) {
                edit("Remove key", [=, this](Project &p, Id id) {
                    for (const auto &ref : refs)
                        eraseKey(layer(p, id).channel(ref), sourceTime(layer(p, id), now_));
                });
                return;
            }
            if (editor_->selection().empty())
                return;
            const auto &current = layer(editor_->project(), editor_->selection().front());
            std::optional<Time> target;
            for (const auto &ref : refs)
                for (const auto &key : current.channel(ref).keys) {
                    auto t = key.at + current.start;
                    if (action == previous && t < now_ && (!target || *target < t))
                        target = t;
                    if (action == next && now_ < t && (!target || t < *target))
                        target = t;
                }
            if (target)
                emit timeChanged(*target);
        });
    };
    if (section_ == Section::Properties) {
        for (const auto &spec : layerParameterSpecs()) {
            if (spec.group == "Audio" && !layerHasAudio(editor_->project(), l)) continue;
            if (l.kind == LayerKind::Audio && spec.group != "Audio") continue;
            if (spec.group == "Text" && l.kind != LayerKind::Text)
                continue;
            if (spec.group == "Source" && l.kind != LayerKind::Solid && l.kind != LayerKind::Text)
                continue;
            if (spec.group == "Mask" && l.kind == LayerKind::Null)
                continue;
            build(spec, 0, spec.group, spec.group == "Transform" ? "Layer Transform" : spec.group);
        }
        if (l.kind == LayerKind::Solid || l.kind == LayerKind::Text) {
            auto *color = new QPushButton("Color…");
            color->setObjectName("layerColorButton");
            color->setFixedHeight(17);
            tree_->setItemWidget(groups.at("Source"), 2, color);
            groups.at("Source")->setFirstColumnSpanned(false);
            connect(color, &QPushButton::clicked, this, [this] {
                if (editor_->selection().empty())
                    return;
                const auto &l = layer(editor_->project(), editor_->selection().front());
                auto t = sourceTime(l, now_);
                auto v = [&](int i) { return valueAt(l, {"source.color", i}, t); };
                auto picked = QColorDialog::getColor(QColor::fromRgbF(v(0), v(1), v(2), v(3)), this,
                                                     "Layer color", QColorDialog::ShowAlphaChannel);
                if (!picked.isValid())
                    return;
                edit("Layer color", [=, this](Project &p, Id id) {
                    const std::array<double, 4> color = {picked.redF(), picked.greenF(), picked.blueF(),
                                                         picked.alphaF()};
                    for (int i = 0; i < 4; ++i)
                        motion::setProperty(p, id, {"source.color", i}, sourceTime(layer(p, id), now_),
                                            color[i]);
                });
            });
        }
    } else
        for (const auto &effect : l.effects) {
            const QString key = "effect:" + QString::number(effect.id);
            for (const auto &spec : effectParameterSpecs(effect.type))
                build(spec, effect.id, key, effect.name);
            auto *item = groups.at(key);
            item->setFirstColumnSpanned(false);
            item->setText(0, {});
            item->setText(1, effect.name);
            auto *buttons = new QWidget;
            auto *row = new QHBoxLayout(buttons);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(2);
            for (const QString &action :
                 {QString("bypass"), QString("up"), QString("down"), QString("remove")}) {
                const QString label = action == "bypass" ? "fx"
                                      : action == "up"   ? "↑"
                                      : action == "down" ? "↓"
                                                         : "×";
                auto *b = smallButton(label, action + " effect");
                b->setObjectName("effect." + QString::number(effect.id) + "." + action);
                row->addWidget(b);
                if (action == "bypass") {
                    b->setCheckable(true);
                    b->setChecked(!effect.enabled);
                }
                connect(b, &QPushButton::clicked, this, [this, id = effect.id, action](bool on) {
                    edit("Edit effect stack", [=](Project &p, Id layerId) {
                        auto &effects = layer(p, layerId).effects;
                        if (action == "up" || action == "down") {
                            moveEffect(p, layerId, id, action == "up" ? -1 : 1);
                            return;
                        }
                        auto it = std::find_if(effects.begin(), effects.end(),
                                               [&](const Effect &e) { return e.id == id; });
                        if (it == effects.end())
                            throw std::runtime_error("Effect no longer exists");
                        if (action == "remove")
                            effects.erase(it);
                        else
                            it->enabled = !on;
                    });
                });
            }
            row->addStretch();
            tree_->setItemWidget(item, 2, buttons);
        }
    filterRows();
}
void PropertyPanel::refresh() {
    QScopedValueRollback guard(refreshing_, true);
    if (editor_->selection().empty()) {
        setEnabled(false);
        return;
    }
    const auto &l = layer(editor_->project(), editor_->selection().front());
    setEnabled(!l.locked);
    QString signature = QString::number(int(l.kind));
    if (section_ == Section::Effects)
        for (const auto &e : l.effects)
            signature += "/" + QString::number(e.id) + ":" + e.type + ":" + e.name;
    if (signature != signature_) {
        signature_ = signature;
        rebuild(l);
    }
    for (const auto &c : controls_) {
        bool mixed = false;
        if (c.multiline) {
            if (!c.multiline->hasFocus())
                c.multiline->setPlainText(l.string(c.ref.id));
        } else if (c.text) {
            auto text = l.string(c.ref.id);
            for (Id id : editor_->selection())
                mixed |= layer(editor_->project(), id).string(c.ref.id) != text;
            if (!c.text->hasFocus()) {
                if (c.ref.id == "text.style") {
                    QSignalBlocker block(c.fontChoice);
                    c.fontChoice->clear();
                    c.fontChoice->addItems(QFontDatabase::styles(l.string("text.family")));
                }
                c.fontChoice->setCurrentText(mixed ? "" : text);
            }
            c.text->setPlaceholderText(mixed ? "Mixed" : "");
            c.text->setProperty("userEdited", false);
        } else {
            const auto &channel = l.channel(c.ref);
            double value = valueAt(channel, sourceTime(l, now_));
            if (!c.ref.effect)
                for (Id id : editor_->selection()) {
                    const auto &other = layer(editor_->project(), id);
                    mixed |= valueAt(other.channel(c.ref), sourceTime(other, now_)) != value;
                }
            if (c.number) {
                QSignalBlocker block(c.number);
                c.number->setSpecialValueText(mixed ? "Mixed" : "");
                c.number->setValue(mixed ? c.number->minimum() : toDisplay(c.ref, value));
                c.number->setProperty("userEdited", false);
            }
            if (c.choice) {
                QSignalBlocker block(c.choice);
                c.choice->setCurrentIndex(mixed ? -1 : int(value));
            }
            if (c.clock) {
                const auto &components = l.parameter(c.ref.id, c.ref.effect).components;
                c.clock->setChecked(std::any_of(components.begin(), components.end(),
                                                [](const Channel &c) { return !c.keys.empty(); }));
            }
            if (c.link)
                c.link->setChecked(editor_->scaleLinked());
        }
        const bool enabled = !l.locked && (!c.ref.effect || editor_->selection().size() == 1);
        for (int column : {0, 2})
            if (auto *widget = tree_->itemWidget(c.item, column))
                widget->setEnabled(enabled);
        c.item->setToolTip(1, parameterSpec(c.ref.id).label);
    }
    if (section_ == Section::Effects)
        for (const auto &e : l.effects)
            if (auto *b = findChild<QPushButton *>("effect." + QString::number(e.id) + ".bypass"))
                b->setChecked(!e.enabled);
}
} // namespace motion
