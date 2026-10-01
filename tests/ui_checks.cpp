// SPDX-License-Identifier: MPL-2.0
#include "main_window.hpp"
#include "numeric_control.hpp"
#include "project_io.hpp"
#include "support.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>
#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <vector>
using namespace motion;
static void events(int milliseconds = 20) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
}
static void key(QWidget *w, int code, QString text = {}, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
    QApplication::sendEvent(w, &event);
}
static void mouse(QWidget *w, QEvent::Type type, QPointF pos, Qt::MouseButtons buttons,
                  Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, pos, w->mapToGlobal(pos.toPoint()),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, modifiers);
    QApplication::sendEvent(w, &event);
}
static void wheel(QWidget *w, QPointF pos, int delta, Qt::KeyboardModifiers modifiers) {
    QWheelEvent event(pos, w->mapToGlobal(pos.toPoint()), {}, QPoint(0, delta), Qt::NoButton, modifiers,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &event);
}
static QImage captureAfter(MainWindow &window, const std::function<void()> &action, const char *message) {
    int frames = 0;
    auto connection = QObject::connect(&window, &MainWindow::frameDisplayed, &window, [&] { ++frames; });
    action();
    QElapsedTimer timeout;
    timeout.start();
    while (frames == 0 && timeout.elapsed() < 10000)
        events(1);
    QObject::disconnect(connection);
    check(frames > 0 && !window.displayedImage().isNull(), message);
    return window.displayedImage();
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    applyApplicationTheme(app);
    app.setOrganizationName("MotionProofTests");
    app.setApplicationName("MotionProofTests");
    try {
        registerFixtureFont(QString(MOTION_SOURCE_DIR) + "/fixtures/font/NotoSans-Regular.ttf");
        MainWindow window;
        window.show();
        window.editor().replace(parentScene());
        window.editor().select({3});
        events();
        check(window.findChildren<QDockWidget *>().size() == 6, "reference panel groups exist");
        auto *properties = window.findChild<QDockWidget *>("propertiesDock");
        check(window.tabifiedDockWidgets(window.findChild<QDockWidget *>("projectDock"))
                  .contains(window.findChild<QDockWidget *>("effectControlsDock")),
              "Project and Effect Controls share tabs");
        properties->show();
        properties->raise();
        window.resizeDocks({properties}, {335}, Qt::Horizontal);
        events();
        check(properties->findChild<QScrollArea *>()->horizontalScrollBar()->maximum() == 0,
              "inspector controls fit without horizontal scrolling");
        auto *name = window.findChild<QLineEdit *>("layerName");
        check(name != nullptr, "named layer input");
        auto *tree = window.findChild<QTreeWidget *>("projectTree");
        tree->setFocus();
        tree->setCurrentItem(tree->topLevelItem(0));
        const auto selectionBeforeProject = window.editor().selection();
        key(tree, Qt::Key_Down);
        check(window.editor().selection() == selectionBeforeProject,
              "project navigation does not change timeline layer selection");
        window.editor().select({3});
        check(tree->topLevelItem(0)->childCount() == 0, "Project composition does not contain layer rows");
        window.timeline()->setFilter("S");
        name->setFocus();
        name->clear();
        events();
        key(name, Qt::Key_P, "p");
        check(name->text() == "p", "shortcut preserves text typing");
        check(window.timeline()->filter() == "S", "typing does not filter timeline");
        window.timeline()->setFocus();
        events();
        key(window.timeline(), Qt::Key_P, "p");
        check(window.timeline()->filter() == "P", "focused property shortcut works");
        check(window.timeline()->isVisible(),
              "property shortcut exposes the actual timeline, not Render Queue");
        auto *spin = window.findChild<QDoubleSpinBox *>("property2");
        check(spin != nullptr, "numeric property control");
        spin->setValue(33);
        QMetaObject::invokeMethod(spin, "editingFinished");
        check(near(layer(window.editor().project(), 3).channel(Property(2)).base, 33),
              "numeric edit reaches document");
        window.editor().undoStack().undo();
        check(near(layer(window.editor().project(), 3).channel(Property(2)).base, 10),
              "numeric UI edit undo");
        auto fractional = parentScene();
        fractional.compositions[0].fps = time(30000, 1001);
        auto &fractionalLayer = layer(fractional, 3);
        fractionalLayer.channel(Property(2)).keys = {{motion::time(0), 10}, {motion::time(1), 20}};
        fractionalLayer.in = fractionalLayer.start = frameTime(1, fractional.compositions[0].fps);
        fractionalLayer.out = frameTime(359, fractional.compositions[0].fps);
        window.editor().replace(fractional);
        window.editor().select({3});
        window.setTime(time(1, 3));
        const auto untouched = encodeProject(window.editor().project());
        QMetaObject::invokeMethod(spin, "editingFinished");
        check(encodeProject(window.editor().project()) == untouched,
              "leaving an unchanged animated field does not create or round a key");
        auto *inTime = window.findChild<QDoubleSpinBox *>("layerInTime");
        check(inTime != nullptr, "layer timing input exists");
        QMetaObject::invokeMethod(inTime, "editingFinished");
        check(encodeProject(window.editor().project()) == untouched,
              "leaving a timing field preserves exact fractional times");
        inTime->setValue(.5);
        QMetaObject::invokeMethod(inTime, "editingFinished");
        check(layer(window.editor().project(), 3).in == time(1, 2) &&
                  layer(window.editor().project(), 3).out == fractionalLayer.out &&
                  layer(window.editor().project(), 3).start == fractionalLayer.start,
              "editing in point does not round unrelated out and source times");
        auto navigatorProject = solidScene();
        navigatorProject.compositions[0].duration = time(12);
        navigatorProject.compositions[0].fps = time(24);
        Composition otherComposition;
        otherComposition.id = 7;
        otherComposition.name = "Other";
        otherComposition.duration = time(6);
        otherComposition.fps = time(25);
        navigatorProject.compositions.push_back(otherComposition);
        navigatorProject.nextId = 8;
        window.editor().replace(navigatorProject);
        window.timeline()->setComposition(1);
        window.setTime(time(6));
        events();
        auto *timeline = window.timeline();
        timeline->setFocus();
        const auto navRect = timeline->navigatorRect();
        const auto fullRange = std::pair{motion::time(0), motion::time(12)};
        check(timeline->visibleRange() == fullRange, "navigator starts at the full composition range");
        check(Timeline::headerHeight() > Timeline::rowPitch(), "Timeline exposes its actual header geometry");
        const auto navDocument = encodeProject(window.editor().project());
        const auto navRevision = window.editor().revision();
        const auto navUndoIndex = window.editor().undoStack().index();
        const auto navTime = window.currentTime();
        const auto navDirty = window.editor().dirty();
        int navigationSignals = 0;
        auto navConnection = QObject::connect(timeline, &Timeline::navigationStarted, timeline,
                                              [&] { ++navigationSignals; });
        auto navX = [&](Time t) {
            return navRect.left() + navRect.width() * seconds(t) / seconds(navigatorProject.compositions[0].duration);
        };
        auto dragNav = [&](QPointF from, QPointF to) {
            mouse(timeline, QEvent::MouseButtonPress, from, Qt::LeftButton);
            mouse(timeline, QEvent::MouseMove, to, Qt::LeftButton);
            mouse(timeline, QEvent::MouseButtonRelease, to, Qt::NoButton);
        };
        auto dragNavHandle = [&](bool startHandle, Time target) {
            const auto range = timeline->visibleRange();
            const Time current = startHandle ? range.first : range.second;
            const auto from = startHandle ? timeline->navigatorStartHandleCenter()
                                          : timeline->navigatorEndHandleCenter();
            const double dx = navRect.width() * seconds(target - current) /
                              seconds(navigatorProject.compositions[0].duration);
            dragNav(from, from + QPointF(dx, 0));
        };
        auto checkViewOnly = [&] {
            check(encodeProject(window.editor().project()) == navDocument &&
                      window.editor().revision() == navRevision &&
                      window.editor().undoStack().index() == navUndoIndex &&
                      window.editor().dirty() == navDirty && window.currentTime() == navTime,
                  "navigator changes only the visible interval");
        };
        dragNavHandle(true, time(1));
        check(timeline->visibleRange() == std::pair{time(1), time(12)},
              "Start handle snaps to a composition frame and keeps End fixed");
        checkViewOnly();
        dragNavHandle(false, time(7));
        check(timeline->visibleRange() == std::pair{time(1), time(7)},
              "End handle snaps to a composition frame and keeps Start fixed");
        checkViewOnly();
        const auto selectionCenter =
            QPointF((timeline->navigatorStartHandleCenter().x() + timeline->navigatorEndHandleCenter().x()) / 2,
                   timeline->navigatorRect().center().y());
        const QPointF panByTwoSeconds(2 * navRect.width() / 12, 0);
        dragNav(selectionCenter, selectionCenter + panByTwoSeconds);
        check(timeline->visibleRange() == std::pair{time(3), time(9)},
              "navigator center drag pans while preserving its span");
        auto panCenter = QPointF((timeline->navigatorStartHandleCenter().x() +
                                  timeline->navigatorEndHandleCenter().x()) / 2,
                                 timeline->navigatorRect().center().y());
        dragNav(panCenter, panCenter - QPointF(navRect.width() * 2, 0));
        check(timeline->visibleRange() == std::pair{motion::time(0), time(6)},
              "navigator pan clamps at composition start");
        panCenter = QPointF((timeline->navigatorStartHandleCenter().x() +
                             timeline->navigatorEndHandleCenter().x()) / 2,
                            timeline->navigatorRect().center().y());
        dragNav(panCenter, panCenter + QPointF(navRect.width() * 2, 0));
        check(timeline->visibleRange() == std::pair{time(6), time(12)}, "navigator pan clamps at composition end");
        dragNav(timeline->navigatorStartHandleCenter(),
                QPointF(navRect.right() + 20, timeline->navigatorRect().center().y()));
        auto oneFrameRange = timeline->visibleRange();
        check(oneFrameRange.second == time(12) &&
                  oneFrameRange.second - oneFrameRange.first == frameTime(1, time(24)),
              "Start handle stops at the explicit one-frame minimum");
        dragNavHandle(true, time(10));
        dragNavHandle(false, frameTime(241, time(24)));
        check(timeline->visibleRange() == std::pair{time(10), frameTime(241, time(24))},
              "overlapping handles still allow the End handle to set a one-frame range");
        const auto oneFrameMiddle = timeline->visibleRange();
        check(timeline->navigatorSelectionRect().width() >= 24,
              "one-frame viewport keeps a separate handle and center-pan hit area");
        auto minCenter = QPointF((timeline->navigatorStartHandleCenter().x() +
                                  timeline->navigatorEndHandleCenter().x()) / 2,
                                 timeline->navigatorRect().center().y());
        dragNav(minCenter, minCenter - QPointF(navRect.width() * 2, 0));
        check(timeline->visibleRange() == std::pair{motion::time(0), frameTime(1, time(24))},
              "minimum visual range center pans to the composition start");
        minCenter = QPointF((timeline->navigatorStartHandleCenter().x() +
                             timeline->navigatorEndHandleCenter().x()) / 2,
                            timeline->navigatorRect().center().y());
        dragNav(minCenter, minCenter + QPointF(navRect.width() * 2, 0));
        check(timeline->visibleRange() == std::pair{frameTime(287, time(24)), time(12)},
              "minimum visual range center pans to the composition end");
        minCenter = QPointF((timeline->navigatorStartHandleCenter().x() +
                             timeline->navigatorEndHandleCenter().x()) / 2,
                            timeline->navigatorRect().center().y());
        const double panToMiddle = navRect.width() * seconds(oneFrameMiddle.first - frameTime(287, time(24))) /
                                   seconds(navigatorProject.compositions[0].duration);
        dragNav(minCenter, minCenter + QPointF(panToMiddle, 0));
        check(timeline->visibleRange() == oneFrameMiddle,
              "minimum visual range center pan preserves its exact frame span");
        dragNavHandle(true, frameTime(239, time(24)));
        check(timeline->visibleRange() == std::pair{frameTime(239, time(24)), frameTime(241, time(24))},
              "overlapping Start handle expands the minimum range to the left");
        dragNavHandle(false, frameTime(242, time(24)));
        check(timeline->visibleRange() == std::pair{frameTime(239, time(24)), frameTime(242, time(24))},
              "overlapping End handle expands the minimum range to the right");
        checkViewOnly();
        check(navigationSignals >= 9, "navigator actions emit the playback-stop signal");
        int openedLayers = 0;
        auto openedConnection = QObject::connect(timeline, &Timeline::layerOpened, timeline,
                                                 [&](Id) { ++openedLayers; });
        const auto navCenter = timeline->navigatorRect().center();
        mouse(timeline, QEvent::MouseButtonDblClick, navCenter, Qt::LeftButton);
        check(timeline->visibleRange() == fullRange && openedLayers == 0,
              "navigator double-click returns to full duration without opening a layer");
        mouse(timeline, QEvent::MouseButtonDblClick, navCenter, Qt::LeftButton, Qt::ShiftModifier);
        check(timeline->visibleRange() == std::pair{frameTime(239, time(24)), frameTime(242, time(24))},
              "Shift-double-click restores the prior navigator interval");
        key(timeline, Qt::Key_Colon, ":", Qt::ShiftModifier);
        check(timeline->visibleRange() == fullRange, "Shift+semicolon toggles to full duration");
        key(timeline, Qt::Key_Semicolon, ":", Qt::ShiftModifier);
        check(timeline->visibleRange() == std::pair{frameTime(239, time(24)), frameTime(242, time(24))},
              "Shift+semicolon restores the prior interval without replacing it");
        const auto beforeZoom = timeline->visibleRange();
        key(timeline, Qt::Key_Equal, "=");
        const auto zoomed = timeline->visibleRange();
        check(seconds(zoomed.second - zoomed.first) < seconds(beforeZoom.second - beforeZoom.first) &&
                  window.currentTime() == navTime,
              "equals zooms around the interval center without moving CTI");
        key(timeline, Qt::Key_Minus, "-");
        check(seconds(timeline->visibleRange().second - timeline->visibleRange().first) >
                  seconds(zoomed.second - zoomed.first),
              "minus zooms back out without changing CTI");
        key(timeline, Qt::Key_Equal, "=");
        key(timeline, Qt::Key_Equal, "=");
        check(timeline->visibleRange().second - timeline->visibleRange().first == frameTime(1, time(24)),
              "zoom-in crosses the two-frame minimum instead of rounding to a plateau");
        key(timeline, Qt::Key_Minus, "-");
        check(timeline->visibleRange().second - timeline->visibleRange().first == frameTime(2, time(24)),
              "zoom-out expands a one-frame viewport");
        window.setTime(time(6));
        key(timeline, Qt::Key_D, "d");
        auto centered = timeline->visibleRange();
        check(near((seconds(centered.first) + seconds(centered.second)) / 2, 6, 1. / 24),
              "D centers the viewport on the CTI");
        mouse(timeline, QEvent::MouseButtonDblClick, timeline->navigatorRect().center(), Qt::LeftButton);
        check(timeline->visibleRange() == fullRange,
              "double-click from a custom interval returns to full composition duration");
        mouse(timeline, QEvent::MouseButtonDblClick, timeline->navigatorRect().center(), Qt::LeftButton);
        auto detailRange = timeline->visibleRange();
        check(detailRange != fullRange && frameTime(1, time(24)) < detailRange.second - detailRange.first &&
                  timeline->rulerStepFrames() == 1,
              "double-click shows readable individual-frame ticks across multiple frames");
        mouse(timeline, QEvent::MouseButtonDblClick, timeline->navigatorRect().center(), Qt::LeftButton);
        check(timeline->visibleRange() == fullRange, "double-click returns from frame detail to full duration");
        const auto beforeWheel = timeline->visibleRange();
        wheel(timeline, timeline->navigatorRect().center(), 120, Qt::ControlModifier);
        const auto wheelZoom = timeline->visibleRange();
        check(seconds(wheelZoom.second - wheelZoom.first) < seconds(beforeWheel.second - beforeWheel.first),
              "Control-wheel zoom updates the navigator range");
        wheel(timeline, timeline->navigatorRect().center(), 120, Qt::ShiftModifier);
        const auto wheelPan = timeline->visibleRange();
        check(wheelPan.first == motion::time(0) && wheelPan.second - wheelPan.first ==
                                                  wheelZoom.second - wheelZoom.first,
              "Shift-wheel pans by the navigator span and clamps at the composition start");
        const auto wheelSelection = timeline->navigatorSelectionRect();
        check(wheelSelection.left() < timeline->navigatorStartHandleCenter().x() &&
                  timeline->navigatorStartHandleCenter().x() < timeline->navigatorEndHandleCenter().x() &&
                  timeline->navigatorEndHandleCenter().x() < wheelSelection.right(),
              "wheel changes and navigator handles share one viewport geometry");
        checkViewOnly();
        dragNavHandle(true, time(1));
        const auto escapeRange = timeline->visibleRange();
        const auto escapeStart = timeline->navigatorStartHandleCenter();
        mouse(timeline, QEvent::MouseButtonPress, escapeStart, Qt::LeftButton);
        mouse(timeline, QEvent::MouseMove,
              QPointF(navX(time(3)), timeline->navigatorRect().center().y()), Qt::LeftButton);
        key(timeline, Qt::Key_Escape);
        mouse(timeline, QEvent::MouseButtonRelease,
              QPointF(navX(time(3)), timeline->navigatorRect().center().y()), Qt::NoButton);
        check(timeline->visibleRange() == escapeRange, "Escape restores the viewport from an active handle drag");
        const auto dragStart = timeline->navigatorStartHandleCenter();
        mouse(timeline, QEvent::MouseButtonPress, dragStart, Qt::LeftButton);
        mouse(timeline, QEvent::MouseMove,
              QPointF(navX(time(2)), timeline->navigatorRect().center().y()), Qt::LeftButton);
        timeline->setComposition(7);
        mouse(timeline, QEvent::MouseButtonRelease,
              QPointF(navX(time(2)), timeline->navigatorRect().center().y()), Qt::NoButton);
        check(timeline->visibleRange() == std::pair{motion::time(0), time(6)},
              "composition switching cancels stale view drags and resets the range");
        timeline->setComposition(1);
        dragNavHandle(true, time(1));
        mouse(timeline, QEvent::MouseButtonPress, timeline->navigatorStartHandleCenter(), Qt::LeftButton);
        mouse(timeline, QEvent::MouseMove,
              QPointF(navX(time(2)), timeline->navigatorRect().center().y()), Qt::LeftButton);
        window.editor().replace(navigatorProject);
        mouse(timeline, QEvent::MouseButtonRelease,
              QPointF(navX(time(2)), timeline->navigatorRect().center().y()), Qt::NoButton);
        check(timeline->visibleRange() == fullRange,
              "project replacement invalidates an active navigator drag even for the same composition ID");
        window.editor().apply("Change composition frame rate", [](Project &p) {
            composition(p, 1).fps = time(25);
        });
        check(timeline->visibleRange() == fullRange, "frame-rate changes reset a stale minimum viewport");
        dragNavHandle(true, time(1));
        window.editor().apply("Change composition duration", [](Project &p) {
            composition(p, 1).duration = time(8);
        });
        check(timeline->visibleRange() == std::pair{motion::time(0), time(8)},
              "duration changes reset an out-of-bounds navigator range");
        window.editor().replace(navigatorProject);
        timeline->setComposition(1);
        window.editor().select({2});
        const auto selectionBeforeHeader = window.editor().selection();
        mouse(timeline, QEvent::MouseButtonDblClick,
              QPointF(timeline->navigatorRect().left() - 1, Timeline::headerHeight() - .5), Qt::LeftButton);
        check(openedLayers == 0 && window.editor().selection() == selectionBeforeHeader,
              "empty header double-click cannot truncate into layer zero");
        window.editor().apply("Long composition handle check", [](Project &p) {
            composition(p, 1).duration = time(3600);
        });
        const auto longRange = timeline->visibleRange();
        const auto longNav = timeline->navigatorRect();
        const auto offCenterStart = timeline->navigatorStartHandleCenter() + QPointF(5, 0);
        mouse(timeline, QEvent::MouseButtonPress, offCenterStart, Qt::LeftButton);
        mouse(timeline, QEvent::MouseMove, offCenterStart, Qt::LeftButton);
        check(timeline->visibleRange() == longRange, "off-center handle grab does not jump the viewport");
        const auto shortMoveFrames = std::llround(3600.0 / longNav.width() * seconds(time(24)));
        const auto shortMove = offCenterStart + QPointF(1, 0);
        mouse(timeline, QEvent::MouseMove, shortMove, Qt::LeftButton);
        mouse(timeline, QEvent::MouseButtonRelease, shortMove, Qt::NoButton);
        check(timeline->visibleRange() ==
                  std::pair{frameTime(shortMoveFrames, time(24)), time(3600)},
              "small handle motion quantizes only its delta on a long composition");
        window.editor().replace(navigatorProject);
        timeline->setComposition(1);
        timeline->setComposition(999);
        check(!timeline->grab().isNull(), "Timeline safely paints after its composition is removed");
        timeline->setComposition(1);
        QObject::disconnect(openedConnection);
        QObject::disconnect(navConnection);
        auto *timelineSearch = window.findChild<QLineEdit *>("timelineSearch");
        timelineSearch->setFocus();
        timelineSearch->clear();
        key(timelineSearch, Qt::Key_Equal, "=");
        key(timelineSearch, Qt::Key_Minus, "-");
        key(timelineSearch, Qt::Key_D, "d");
        check(timelineSearch->text() == "=-d", "Timeline shortcuts preserve focused text editing");
        timelineSearch->clear();
        window.editor().replace(navigatorProject);
        timeline->setComposition(1);
        window.setTime(time(4));
        auto fractionalNav = navigatorProject;
        fractionalNav.compositions[0].fps = time(30000, 1001);
        window.editor().replace(fractionalNav);
        timeline->setComposition(1);
        window.setTime(time(4));
        auto fractionalFull = timeline->visibleRange();
        auto fractionalEnd = timeline->navigatorEndHandleCenter();
        mouse(timeline, QEvent::MouseButtonPress, fractionalEnd, Qt::LeftButton);
        mouse(timeline, QEvent::MouseMove, fractionalEnd, Qt::LeftButton);
        mouse(timeline, QEvent::MouseButtonRelease, fractionalEnd, Qt::NoButton);
        check(timeline->visibleRange() == fractionalFull,
              "zero-motion End grab preserves a fractional composition boundary");
        const auto fractionalEndDelta =
            std::llround(seconds(time(7) - time(12)) * seconds(fractionalNav.compositions[0].fps));
        dragNavHandle(false, time(7));
        const auto movedEndFrame =
            std::llround(seconds(time(12)) * seconds(fractionalNav.compositions[0].fps)) +
            fractionalEndDelta;
        check(timeline->visibleRange().second == frameTime(movedEndFrame, fractionalNav.compositions[0].fps),
              "moving End snaps to an exact frame at 30000/1001 fps");
        dragNavHandle(false, time(12));
        check(timeline->visibleRange().second == time(12), "End restores the fractional composition boundary");
        dragNavHandle(false, motion::time(0));
        check(timeline->visibleRange() ==
                  std::pair{motion::time(0), frameTime(1, fractionalNav.compositions[0].fps)},
              "End respects the one-frame minimum at the composition start");
        dragNavHandle(false, time(12));
        dragNavHandle(true, time(12));
        const Time fractionalMinimumStart =
            time(12) - frameTime(1, fractionalNav.compositions[0].fps);
        check(timeline->visibleRange() ==
                  std::pair{fractionalMinimumStart, time(12)},
              "Start clamps to fractional composition End minus one frame");
        dragNavHandle(false, motion::time(0));
        check(timeline->visibleRange() == std::pair{fractionalMinimumStart, time(12)},
              "End cannot cross a fractional-frame Start near composition End");
        window.editor().replace(fractionalNav);
        timeline->setComposition(1);
        window.setTime(time(4));
        const auto fractionalBytes = encodeProject(window.editor().project());
        const auto fractionalUndo = window.editor().undoStack().index();
        const Time fractionalTarget = frameTime(121, time(30000, 1001));
        dragNavHandle(true, time(2));
        dragNavHandle(false, time(8));
        const QPointF rulerPoint(timeline->timeX(fractionalTarget), 40);
        mouse(timeline, QEvent::MouseButtonPress, rulerPoint, Qt::LeftButton);
        mouse(timeline, QEvent::MouseButtonRelease, rulerPoint, Qt::NoButton);
        check(window.currentTime() == fractionalTarget,
              "ruler click uses the selected viewport and seeks an exact 30000/1001 frame");
        check(encodeProject(window.editor().project()) == fractionalBytes &&
                  window.editor().undoStack().index() == fractionalUndo,
              "CTI seeking leaves project bytes and undo history unchanged");
        key(timeline, Qt::Key_Semicolon, ":", Qt::ShiftModifier);
        const QPointF endRuler(timeline->timeX(time(12)), 40);
        mouse(timeline, QEvent::MouseButtonPress, endRuler, Qt::LeftButton);
        mouse(timeline, QEvent::MouseButtonRelease, endRuler, Qt::NoButton);
        check(window.currentTime() == frameTime(frameCount(time(12), time(30000, 1001)) - 1,
                                               time(30000, 1001)),
              "ruler end seek clamps to the final exact frame");
        window.editor().replace(parentScene());
        window.editor().select({3});
        window.setTime(motion::time(0));
        QTemporaryDir tmp;
        auto good = tmp.filePath("scene.json");
        check(window.saveAs(good), "UI save as");
        auto before = encodeProject(window.editor().project());
        atomicWrite(tmp.filePath("broken.json"), "{bad");
        check(!window.openProject(tmp.filePath("broken.json")), "invalid open returns failure");
        check(encodeProject(window.editor().project()) == before,
              "failed UI open preserves current document");
        check(window.openProject(good), "UI reopen good project");
        window.editor().replace(solidScene());
        window.editor().select({2});
        window.setTime(motion::time(0));
        events();
        auto *viewer = window.findChild<QWidget *>("compositionViewer");
        check(viewer != nullptr, "viewer exists");
        before = encodeProject(window.editor().project());
        QPointF center(viewer->width() / 2., viewer->height() / 2.);
        mouse(viewer, QEvent::MouseButtonPress, center, Qt::LeftButton);
        mouse(viewer, QEvent::MouseMove, center + QPointF(18, 0), Qt::LeftButton);
        check(encodeProject(window.editor().project()) != before, "viewer drag previews document");
        key(viewer, Qt::Key_Escape);
        check(encodeProject(window.editor().project()) == before, "viewer Escape restores document");
        window.editor().apply("Keys", [](Project &p) {
            layer(p, 2).channel(Property(2)).keys = {{motion::time(0), 0}, {motion::time(1), 1}};
        });
        window.timeline()->setFilter("P");
        window.timeline()->setFocus();
        events();
        before = encodeProject(window.editor().project());
        const QPointF keyPoint(window.timeline()->timeX(motion::time(0)),
                               window.timeline()->propertyY(2, Property::PositionX));
        mouse(window.timeline(), QEvent::MouseButtonPress, keyPoint, Qt::LeftButton);
        mouse(window.timeline(), QEvent::MouseMove, keyPoint + QPointF(40, 0), Qt::LeftButton);
        check(encodeProject(window.editor().project()) != before, "timeline key drag previews");
        key(window.timeline(), Qt::Key_Escape);
        check(encodeProject(window.editor().project()) == before, "timeline Escape restores keys");
        bool shown = false;
        QObject::connect(&window, &MainWindow::frameDisplayed, &window, [&] { shown = true; });
        window.setTime(motion::time(1));
        QElapsedTimer wait;
        wait.start();
        while (!shown && wait.elapsed() < 10000)
            events(10);
        check(shown && !window.displayedImage().isNull(), "worker displays a real frame");
        window.editor().replace(solidScene());
        window.setTime(motion::time(0));
        QPushButton *play = nullptr;
        for (auto *button : window.findChildren<QPushButton *>())
            if (button->text() == "Play")
                play = button;
        check(play != nullptr, "playback control exists");
        bool firstEditedFrame = false;
        QImage editedImage;
        QObject::connect(&window, &MainWindow::frameDisplayed, &window, [&] {
            if (!firstEditedFrame) {
                firstEditedFrame = true;
                editedImage = window.displayedImage();
            }
        });
        play->click();
        window.editor().apply("Edit during playback", [](Project &p) {
            layer(p, 2).setColor(QColor(0, 255, 0));
            layer(p, 2).channel(Property(7)).base = 1;
        });
        wait.restart();
        while (!firstEditedFrame && wait.elapsed() < 10000)
            events(1);
        check(firstEditedFrame && editedImage.pixelColor(0, 0) == QColor(0, 255, 0),
              "editing during playback refuses the old document frame");
        play->click();
        auto backlogProject = solidScene();
        auto &backlogComposition = backlogProject.compositions[0];
        backlogComposition.width = 1280;
        backlogComposition.height = 720;
        const auto layerTemplates = backlogComposition.layers;
        backlogComposition.layers.clear();
        for (int i = 0; i < 16; ++i) {
            auto item = layerTemplates[i % layerTemplates.size()];
            item.id = 2 + i;
            item.name = "Preview layer " + QString::number(i + 1);
            item.width = backlogComposition.width;
            item.height = backlogComposition.height;
            item.parent.reset();
            item.setColor(QColor::fromHsv(i * 21, 210, 220));
            backlogComposition.layers.push_back(item);
        }
        backlogProject.nextId = 18;
        for (const auto &item : backlogComposition.layers)
            addEffect(backlogProject, item.id);
        window.editor().replace(backlogProject);
        window.timeline()->setComposition(1);
        const auto backlogBytes = encodeProject(window.editor().project());
        window.setTime(motion::time(0));
        QLabel *displayedTime = nullptr, *renderStatus = nullptr;
        for (auto *label : window.findChildren<QLabel *>()) {
            if (label->toolTip().contains("exact time"))
                displayedTime = label;
            if (label->text() == "CPU · Rendering…")
                renderStatus = label;
        }
        check(displayedTime && renderStatus, "preview timing labels are observable in the real window");
        auto displayedAt = [&](Time at) {
            const auto suffix = QString("exact time %1/%2 s").arg(QString::number(at.numerator),
                                                                   QString::number(at.denominator));
            return displayedTime->toolTip().contains(suffix);
        };
        auto waitForDisplayedAt = [&](Time at, int timeoutMs) {
            QElapsedTimer timeout;
            timeout.start();
            while (!displayedAt(at) && timeout.elapsed() < timeoutMs)
                events(1);
            return displayedAt(at);
        };
        check(waitForDisplayedAt(motion::time(0), 10000), "nontrivial scene displays its initial exact frame");
        auto *backlogTimeline = window.timeline();
        auto backlogNav = backlogTimeline->navigatorRect();
        auto targetNavX = [&](Time t) {
            return backlogNav.left() + backlogNav.width() * seconds(t) / seconds(backlogComposition.duration);
        };
        auto dragBacklogHandle = [&](QPointF from, QPointF to) {
            mouse(backlogTimeline, QEvent::MouseButtonPress, from, Qt::LeftButton);
            mouse(backlogTimeline, QEvent::MouseMove, to, Qt::LeftButton);
            mouse(backlogTimeline, QEvent::MouseButtonRelease, to, Qt::NoButton);
        };
        dragBacklogHandle(backlogTimeline->navigatorStartHandleCenter(),
                          QPointF(targetNavX(time(1)), backlogNav.center().y()));
        dragBacklogHandle(backlogTimeline->navigatorEndHandleCenter(),
                          QPointF(targetNavX(time(11)), backlogNav.center().y()));
        window.setTime(motion::time(0));
        check(waitForDisplayedAt(motion::time(0), 10000), "navigator setup preserves the CTI preview frame");
        play->click();
        QElapsedTimer backlogTimeout;
        backlogTimeout.start();
        while (!window.isPlaying() && backlogTimeout.elapsed() < 10000)
            events(1);
        check(window.isPlaying(), "nontrivial scene starts playback");
        bool stoppedWithPendingFrame = false;
        Time stoppedAt{};
        while (backlogTimeout.elapsed() < 10000 && !stoppedWithPendingFrame) {
            events(1);
            if (!window.isPlaying() || window.currentTime() == motion::time(0) ||
                displayedAt(window.currentTime()) || renderStatus->text() != "CPU · Rendering…")
                continue;
            stoppedAt = window.currentTime();
            const QPointF center((backlogTimeline->navigatorStartHandleCenter().x() +
                                  backlogTimeline->navigatorEndHandleCenter().x()) / 2,
                                 backlogTimeline->navigatorRect().center().y());
            mouse(backlogTimeline, QEvent::MouseButtonPress, center, Qt::LeftButton);
            mouse(backlogTimeline, QEvent::MouseMove, center + QPointF(1, 0), Qt::LeftButton);
            mouse(backlogTimeline, QEvent::MouseButtonRelease, center + QPointF(1, 0), Qt::NoButton);
            stoppedWithPendingFrame = !window.isPlaying();
        }
        check(stoppedWithPendingFrame && window.currentTime() == stoppedAt &&
                  encodeProject(window.editor().project()) == backlogBytes,
              "navigator stops a pending preview without changing CTI or the project");
        check(waitForDisplayedAt(stoppedAt, 10000) && !window.isPlaying(),
              "stopped playback catches the displayed preview up to its latest CTI");
        // Identical document revision, composition id, time and raster dimensions: only the view changes.
        auto waitForPixel = [&](QColor expected) {
            QElapsedTimer timeout;
            timeout.start();
            while (timeout.elapsed() < 10000) {
                events(5);
                auto image = window.displayedImage();
                if (!image.isNull() && image.pixelColor(0, 0) == expected)
                    return true;
            }
            return false;
        };
        auto *views = window.findChild<QTabBar *>("sourceViewTabs");
        window.editor().replace(solidScene());
        window.editor().select({2});
        window.setTime(motion::time(0));
        check(waitForPixel(QColor(188, 0, 188)), "composition view composites the layers");
        const auto viewDocument = encodeProject(window.editor().project());
        views->setCurrentIndex(0);
        check(waitForPixel(QColor(255, 0, 0)), "Layer view isolates source, ignoring composition opacity");
        window.editor().select({3});
        check(waitForPixel(QColor(0, 0, 255)), "Layer view refreshes when selected source changes");
        views->setCurrentIndex(1);
        check(waitForPixel(QColor(188, 0, 188)), "switching back restores composition cache context");
        check(encodeProject(window.editor().project()) == viewDocument,
              "source views never edit the project");
        views->setCurrentIndex(2);
        check(window.displayedImage().isNull(), "empty footage clears the old displayed frame");
        views->setCurrentIndex(1);
        check(window.openProject(QString(MOTION_SOURCE_DIR) + "/fixtures/original-scene.json"),
              "open media fixture");
        QTreeWidgetItem *media = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->data(0, Qt::UserRole + 1).toString() == "asset")
                media = tree->topLevelItem(i);
        check(media != nullptr, "Project exposes footage separately from layers");
        const auto mediaDocument = encodeProject(window.editor().project());
        bool footageDisplayed = false;
        auto footageConnection =
            QObject::connect(&window, &MainWindow::frameDisplayed, &window, [&] { footageDisplayed = true; });
        QMetaObject::invokeMethod(tree, "itemActivated", Q_ARG(QTreeWidgetItem *, media), Q_ARG(int, 0));
        wait.restart();
        while (!footageDisplayed && wait.elapsed() < 10000)
            events(5);
        QObject::disconnect(footageConnection);
        check(views->currentIndex() == 2 && footageDisplayed &&
                  window.displayedImage().size() == QSize(280, 280),
              "Project media activation displays its real source raster");
        check(encodeProject(window.editor().project()) == mediaDocument, "opening footage is read-only");
        auto *mediaSlider = window.findChild<QSlider *>("sourceTimeSlider");
        int stillPreviewFrames = 0;
        auto stillPreviewConnection = QObject::connect(&window, &MainWindow::frameDisplayed, &window,
                                                       [&] { ++stillPreviewFrames; });
        check(QMetaObject::invokeMethod(mediaSlider, "sliderMoved", Qt::DirectConnection,
                                        Q_ARG(int, 30)),
              "still-footage time callback is reachable for a stale-position check");
        wait.restart();
        while (!stillPreviewFrames && wait.elapsed() < 10000)
            events(5);
        const auto stillFrameCount = stillPreviewFrames;
        events(120);
        check(stillFrameCount == 1 && stillPreviewFrames == stillFrameCount &&
                  window.displayedImage().size() == QSize(280, 280),
              "still-image footage renders at zero without looping on a stale media position");
        QObject::disconnect(stillPreviewConnection);
        views->setCurrentIndex(1);
        window.editor().replace(parentScene());
        window.editor().select({3});
        window.setTime(motion::time(0));
        events();
        window.timeline()->setFilter("P");
        const auto inlineBefore = encodeProject(window.editor().project());
        QPointF valuePoint(285, window.timeline()->propertyY(3, Property::PositionX));
        mouse(window.timeline(), QEvent::MouseButtonPress, valuePoint, Qt::LeftButton);
        mouse(window.timeline(), QEvent::MouseMove, valuePoint + QPointF(20, 0), Qt::LeftButton);
        mouse(window.timeline(), QEvent::MouseButtonRelease, valuePoint + QPointF(20, 0), Qt::NoButton);
        check(near(layer(window.editor().project(), 3).base("transform.position"), 30),
              "timeline value scrub edits X only");
        window.editor().undoStack().undo();
        check(encodeProject(window.editor().project()) == inlineBefore, "timeline scrub is reversible");
        mouse(window.timeline(), QEvent::MouseButtonDblClick, valuePoint, Qt::LeftButton);
        auto *inlineBox = window.timeline()->findChild<QDoubleSpinBox *>("timelineInlineValue");
        check(inlineBox != nullptr, "double-click creates inline numeric input");
        const auto beforeInlineShortcut = timeline->visibleRange();
        auto *inlineText = inlineBox->findChild<QLineEdit *>();
        inlineText->setFocus();
        key(inlineText, Qt::Key_D, "d");
        check(timeline->visibleRange() == beforeInlineShortcut,
              "inline numeric focus does not trigger a bubbled Timeline shortcut");
        inlineBox->setValue(42);
        key(inlineBox->findChild<QLineEdit *>(), Qt::Key_Escape);
        events();
        check(encodeProject(window.editor().project()) == inlineBefore, "inline Escape discards typed edit");
        mouse(window.timeline(), QEvent::MouseButtonDblClick, valuePoint, Qt::LeftButton);
        inlineBox = window.timeline()->findChild<QDoubleSpinBox *>("timelineInlineValue");
        inlineBox->setValue(42);
        QMetaObject::invokeMethod(inlineBox, "editingFinished");
        events();
        check(near(layer(window.editor().project(), 3).base("transform.position"), 42),
              "inline value commits typed edit");
        window.editor().undoStack().undo();
        auto *timecode = window.findChild<QDoubleSpinBox *>("currentFrame");
        auto *timeText = timecode->findChild<QLineEdit *>();
        timeText->setText("0:00:01:15");
        timecode->interpretText();
        QMetaObject::invokeMethod(timecode, "editingFinished");
        check(window.currentTime() == time(3, 2), "timecode input seeks the exact frame");
        window.setTime(motion::time(0));
        auto *scale = window.findChild<QDoubleSpinBox *>("property4");
        auto *opacity = window.findChild<QDoubleSpinBox *>("property7");
        check(scale && opacity && scale->value() == 100 && opacity->value() == 100,
              "scale and opacity display percent");
        opacity->setValue(50);
        QMetaObject::invokeMethod(opacity, "editingFinished");
        check(layer(window.editor().project(), 3).channel(Property::Opacity).base == .5,
              "UI opacity percent stores a ratio");
        check(window.findChild<QDoubleSpinBox *>("property2")->parentWidget() ==
                  window.findChild<QDoubleSpinBox *>("property3")->parentWidget(),
              "X and Y editors share one property row");
        check(Timeline::rowPitch() == 17, "timeline row pitch matches measured AE reference");
        const auto linkBefore = encodeProject(window.editor().project());
        scale->setValue(150);
        QMetaObject::invokeMethod(scale, "editingFinished");
        check(near(layer(window.editor().project(), 3).base("transform.scale", 0), 1.5) &&
                  near(layer(window.editor().project(), 3).base("transform.scale", 1), 1.5),
              "linked Scale updates both components");
        window.editor().undoStack().undo();
        check(encodeProject(window.editor().project()) == linkBefore, "linked Scale is one reversible edit");
        auto propertyMenu = [&](QWidget *values, const QString &label) {
            bool invoked = false;
            QTimer::singleShot(0, &window, [&] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu)
                    return;
                for (auto *action : menu->actions())
                    if (action->text() == label) {
                        invoked = true;
                        menu->setActiveAction(action);
                        key(menu, Qt::Key_Return);
                        return;
                    }
                menu->close();
            });
            QMetaObject::invokeMethod(values, "customContextMenuRequested", Q_ARG(QPoint, QPoint(2, 2)));
            check(invoked, "property context action is reachable");
        };
        propertyMenu(scale->parentWidget(), "Copy value");
        check(QApplication::clipboard()->text() == "[100,100]",
              "Properties copy uses shared percentage format");
        QApplication::clipboard()->setText("[125,75]");
        propertyMenu(scale->parentWidget(), "Paste value");
        check(layer(window.editor().project(), 3).base("transform.scale", 0) == 1.25 &&
                  layer(window.editor().project(), 3).base("transform.scale", 1) == .75,
              "Properties context paste applies both explicit components");
        window.editor().undoStack().undo();
        check(angleValue("1x+45°").value_or(0) == 405 && angleValue("-1x-30°").value_or(0) == -390 &&
                  !angleValue("bad"),
              "rotation turns-and-degrees input parses without eval");
        auto *rotation = window.findChild<QDoubleSpinBox *>("property6");
        auto *rotationText = rotation->findChild<QLineEdit *>();
        check(rotationText->text().count(QChar(0x00b0)) == 1, "rotation shows one degree suffix");
        rotationText->setText("1x+45°");
        rotation->interpretText();
        QMetaObject::invokeMethod(rotation, "editingFinished");
        check(near(layer(window.editor().project(), 3).base("transform.rotation"), 405),
              "rotation input stores complete turns and degrees");
        window.editor().undoStack().undo();
        auto *clock = window.findChild<QPushButton *>("clock.property7");
        check(clock != nullptr, "property stopwatch exists");
        clock->click();
        check(layer(window.editor().project(), 3).channel(Property::Opacity).keys.size() == 1,
              "stopwatch creates key");
        window.setTime(time(1));
        opacity->setValue(75);
        QMetaObject::invokeMethod(opacity, "editingFinished");
        check(layer(window.editor().project(), 3).channel(Property::Opacity).keys.size() == 2,
              "editing animated property adds current key");
        window.timeline()->setFilter("T");
        window.setTime(time(1, 2));
        window.timeline()->navigateKey(true);
        check(window.currentTime() == time(1), "next key navigation follows grouped visible properties");
        window.timeline()->navigateKey(false);
        check(window.currentTime() == motion::time(0),
              "previous key navigation seeks the previous exact key");
        auto *reset = window.findChild<QPushButton *>("reset.property7");
        reset->click();
        check(layer(window.editor().project(), 3).channel(Property::Opacity).keys.empty() &&
                  layer(window.editor().project(), 3).channel(Property::Opacity).base == 1,
              "reset clears keys and restores default");
        window.editor().undoStack().undo();
        check(layer(window.editor().project(), 3).channel(Property::Opacity).keys.size() == 2,
              "reset undo restores keys");
        auto *parameters = window.findChild<QTreeWidget *>("parameterTree");
        check(parameters && parameters->topLevelItemCount() >= 2, "grouped parameter tree exists");
        check(parameters->horizontalScrollBar()->maximum() == 0,
              "parameter tree fits narrow inspector without horizontal scrolling");
        const auto selectionSnapshot = encodeProject(window.editor().project());
        parameters->setCurrentItem(parameters->topLevelItem(0)->child(0));
        check(encodeProject(window.editor().project()) == selectionSnapshot,
              "property selection does not edit data");
        spin = window.findChild<QDoubleSpinBox *>("property2");
        auto *input = spin->findChild<QLineEdit *>();
        const int undoCount = window.editor().undoStack().count();
        const int undoIndex = window.editor().undoStack().index();
        const double oldPosition = layer(window.editor().project(), 3).channel(Property::PositionX).base;
        mouse(input, QEvent::MouseButtonPress, {10, 10}, Qt::LeftButton);
        mouse(input, QEvent::MouseMove, {42, 10}, Qt::LeftButton);
        mouse(input, QEvent::MouseButtonRelease, {42, 10}, Qt::NoButton);
        check(near(layer(window.editor().project(), 3).channel(Property::PositionX).base, oldPosition + 32),
              "hot-text scrub changes the selected parameter");
        check(window.editor().undoStack().index() == undoIndex + 1 &&
                  window.editor().undoStack().count() <= undoCount + 1,
              "scrub is one undo command");
        window.editor().undoStack().undo();
        check(near(layer(window.editor().project(), 3).channel(Property::PositionX).base, oldPosition),
              "scrub undo restores position");
        mouse(input, QEvent::MouseButtonPress, {10, 10}, Qt::LeftButton);
        mouse(input, QEvent::MouseMove, {50, 10}, Qt::LeftButton);
        key(input, Qt::Key_Escape);
        check(near(layer(window.editor().project(), 3).channel(Property::PositionX).base, oldPosition),
              "scrub Escape restores position");
        auto *effectButton = window.findChild<QPushButton *>("addEffectButton");
        effectButton->click();
        check(layer(window.editor().project(), 3).effects.size() == 1, "effect UI creates actual effect");
        const auto effectId = layer(window.editor().project(), 3).effects.front().id;
        auto *gain = window.findChild<QDoubleSpinBox *>("color.gain.0." + QString::number(effectId));
        check(gain != nullptr, "effect controls use shared parameter tree");
        gain->setValue(2);
        QMetaObject::invokeMethod(gain, "editingFinished");
        check(layer(window.editor().project(), 3).channel({"color.gain", 0, effectId}).base == 2,
              "effect parameter UI reaches renderer model");
        window.findChild<QPushButton *>("effect." + QString::number(effectId) + ".bypass")->click();
        check(!layer(window.editor().project(), 3).effects.front().enabled,
              "effect bypass control updates model");
        window.editor().undoStack().undo();
        check(layer(window.editor().project(), 3).effects.front().enabled, "effect bypass undo");
        window.findChild<QPushButton *>("addEffectButton")->click();
        const auto secondEffectId = layer(window.editor().project(), 3).effects.back().id;
        window.findChild<QPushButton *>("effect." + QString::number(secondEffectId) + ".up")->click();
        check(layer(window.editor().project(), 3).effects.front().id == secondEffectId,
              "effect reorder button changes stack");
        window.findChild<QPushButton *>("effect." + QString::number(secondEffectId) + ".remove")->click();
        check(layer(window.editor().project(), 3).effects.size() == 1, "effect remove control");
        events();
        gain = window.findChild<QDoubleSpinBox *>("color.gain.0." + QString::number(effectId));
        window.editor().apply("Lock", [](Project &p) { layer(p, 3).locked = true; });
        check(!gain->isEnabled(), "locked layer disables effect edits");
        window.editor().undoStack().undo();
        window.editor().apply("Animate effect", [=](Project &p) {
            layer(p, 3).channel({"color.gain", 0, effectId}).keys = {{motion::time(0), 1}, {time(1), 2}};
        });
        window.timeline()->setFilter("E");
        const QPointF effectKey(window.timeline()->timeX(motion::time(0)),
                                window.timeline()->propertyY(3, {"color.gain", 0, effectId}));
        mouse(window.timeline(), QEvent::MouseButtonPress, effectKey, Qt::LeftButton);
        mouse(window.timeline(), QEvent::MouseButtonRelease, effectKey, Qt::NoButton);
        window.findChild<QPushButton *>("effect." + QString::number(effectId) + ".remove")->click();
        window.timeline()->copyKeys();
        window.timeline()->deleteKeys();
        check(layer(window.editor().project(), 3).effects.empty(),
              "removed effect prunes stale key selection safely");
        {
            const auto effectScene = solidScene();
            window.editor().replace(effectScene);
            window.editor().select({3});
            window.setTime(motion::time(0));
            auto *effectTypes = window.findChild<QComboBox *>("effectTypeSelector");
            auto *addEffect = window.findChild<QPushButton *>("addEffectButton");
            check(effectTypes && addEffect, "effect controls expose the catalog selector and Add effect button");
            std::map<QString, Id> uiEffectIds;
            for (const auto &spec : effectSpecs()) {
                const int index = effectTypes->findData(spec.type);
                check(index >= 0, "each supported effect is selectable by its stable type ID");
                const auto oldCount = layer(window.editor().project(), 3).effects.size();
                effectTypes->setCurrentIndex(index);
                addEffect->click();
                events(1);
                const auto added = layer(window.editor().project(), 3).effects.back();
                check(layer(window.editor().project(), 3).effects.size() == oldCount + 1 &&
                          added.type == spec.type && added.name == spec.name,
                      "real catalog selection and Add effect button create the selected effect");
                uiEffectIds[spec.type] = added.id;

                window.editor().undoStack().undo();
                check(layer(window.editor().project(), 3).effects.size() == oldCount,
                      "effect Add button is undoable");
                window.editor().undoStack().redo();
                check(layer(window.editor().project(), 3).effects.back().id == added.id,
                      "effect Add button redo restores the same effect instance");

                const auto &parameter = effectParameterSpecs(spec.type).front();
                const PropertyRef ref{parameter.id, 0, added.id};
                const QString controlId = ref.id + ".0." + QString::number(added.id);
                auto *value = window.findChild<QDoubleSpinBox *>(controlId);
                check(value != nullptr, "each effect has a real numeric control in the main window");
                const double originalValue = layer(window.editor().project(), 3).channel(ref).base;
                const double editedValue = originalValue + .25;
                value->setValue(toDisplay(ref, editedValue));
                QMetaObject::invokeMethod(value, "editingFinished");
                check(near(layer(window.editor().project(), 3).channel(ref).base, editedValue),
                      "real effect control edits its typed project parameter");
                window.editor().undoStack().undo();
                check(near(layer(window.editor().project(), 3).channel(ref).base, originalValue),
                      "effect parameter edit undo restores its prior value");
                window.editor().undoStack().redo();
                check(near(layer(window.editor().project(), 3).channel(ref).base, editedValue),
                      "effect parameter edit redo restores its value");

                window.setTime(motion::time(1, 2));
                auto *keyButton = window.findChild<QPushButton *>("key." + controlId);
                check(keyButton != nullptr, "animated effect parameter exposes its keyframe button");
                keyButton->click();
                const auto &keys = layer(window.editor().project(), 3).channel(ref).keys;
                check(keys.size() == 1 && keys.front().at == motion::time(1, 2),
                      "effect keyframe button records the current exact time");
                window.editor().undoStack().undo();
                check(layer(window.editor().project(), 3).channel(ref).keys.empty(),
                      "effect keyframe creation undo removes the key");
                window.editor().undoStack().redo();
                check(layer(window.editor().project(), 3).channel(ref).keys.size() == 1,
                      "effect keyframe creation redo restores the key");
            }

            const auto orderBefore = [&] {
                std::vector<Id> ids;
                for (const auto &effect : layer(window.editor().project(), 3).effects)
                    ids.push_back(effect.id);
                return ids;
            }();
            auto *moveUp = window.findChild<QPushButton *>(
                "effect." + QString::number(uiEffectIds.at("motion.gaussian-blur")) + ".up");
            check(moveUp != nullptr, "effect stack exposes its reorder control");
            moveUp->click();
            auto orderAfter = [&] {
                std::vector<Id> ids;
                for (const auto &effect : layer(window.editor().project(), 3).effects)
                    ids.push_back(effect.id);
                return ids;
            }();
            check(orderAfter.size() == 4 && orderAfter[2] == uiEffectIds.at("motion.gaussian-blur") &&
                      orderAfter != orderBefore,
                  "real effect reorder control changes stored stack order");
            window.editor().undoStack().undo();
            check(orderBefore == [&] {
                      std::vector<Id> ids;
                      for (const auto &effect : layer(window.editor().project(), 3).effects)
                          ids.push_back(effect.id);
                      return ids;
                  }(),
                  "effect reorder undo restores the previous stack");
            window.editor().undoStack().redo();
            check(orderAfter == [&] {
                      std::vector<Id> ids;
                      for (const auto &effect : layer(window.editor().project(), 3).effects)
                          ids.push_back(effect.id);
                      return ids;
                  }(),
                  "effect reorder redo restores the reordered stack");

            const Id splitId = uiEffectIds.at("motion.rgb-split");
            auto *bypass = window.findChild<QPushButton *>("effect." + QString::number(splitId) + ".bypass");
            check(bypass != nullptr, "effect stack exposes its bypass control");
            bypass->click();
            check(!layer(window.editor().project(), 3).effects[1].enabled,
                  "real effect bypass control disables its effect");
            window.editor().undoStack().undo();
            check(layer(window.editor().project(), 3).effects[1].enabled, "effect bypass undo restores enabled state");
            window.editor().undoStack().redo();
            check(!layer(window.editor().project(), 3).effects[1].enabled, "effect bypass redo restores disabled state");

            const Id exposureId = uiEffectIds.at("motion.exposure");
            auto *remove = window.findChild<QPushButton *>("effect." + QString::number(exposureId) + ".remove");
            check(remove != nullptr, "effect stack exposes its delete control");
            remove->click();
            check(std::none_of(layer(window.editor().project(), 3).effects.begin(),
                               layer(window.editor().project(), 3).effects.end(),
                               [&](const Effect &effect) { return effect.id == exposureId; }),
                  "real effect delete control removes the selected effect");
            window.editor().undoStack().undo();
            check(std::any_of(layer(window.editor().project(), 3).effects.begin(),
                              layer(window.editor().project(), 3).effects.end(),
                              [&](const Effect &effect) { return effect.id == exposureId; }),
                  "effect delete undo restores its instance");
            window.editor().undoStack().redo();
            check(std::none_of(layer(window.editor().project(), 3).effects.begin(),
                               layer(window.editor().project(), 3).effects.end(),
                               [&](const Effect &effect) { return effect.id == exposureId; }),
                  "effect delete redo removes its instance");
            window.editor().undoStack().undo();

            const QString effectSave = tmp.filePath("v06-effect-ui.json");
            check(!QFileInfo::exists(effectSave) && window.saveAs(effectSave),
                  "real effect scene saves to a new private test project");
            const auto savedEffectBytes = encodeProject(window.editor().project());
            check(window.openProject(effectSave) && encodeProject(window.editor().project()) == savedEffectBytes,
                  "effect types, values, keys, order and bypass state survive save and reopen");
        }

        {
            auto viewScene = solidScene();
            layer(viewScene, 3).channel(Property::Opacity).base = .5;
            window.editor().replace(viewScene);
            window.editor().select({3});
            auto *channel = window.findChild<QComboBox *>("viewerChannel");
            auto *grayscale = window.findChild<QCheckBox *>("viewerGrayscale");
            auto *exposure = window.findChild<QDoubleSpinBox *>("viewerExposure");
            auto *previewScale = window.findChild<QComboBox *>("viewerPreviewScale");
            check(channel && grayscale && exposure && previewScale,
                  "real viewer exposes channel, grayscale, exposure and resolution controls");
            previewScale->setCurrentIndex(1);
            const QString viewSave = tmp.filePath("v06-view-ui.json");
            check(!QFileInfo::exists(viewSave) && window.saveAs(viewSave),
                  "viewer-only test starts from a saved private project");
            const auto viewDocument = encodeProject(window.editor().project());
            const auto viewRevision = window.editor().revision();
            const auto viewUndoIndex = window.editor().undoStack().index();
            const bool viewDirty = window.editor().dirty();
            auto unchangedViewDocument = [&] {
                check(encodeProject(window.editor().project()) == viewDocument &&
                          window.editor().revision() == viewRevision &&
                          window.editor().undoStack().index() == viewUndoIndex &&
                          window.editor().dirty() == viewDirty,
                      "viewer changes do not alter project bytes, dirty state or undo history");
            };
            auto selectViewerChannel = [&](DisplayChannel value) {
                const int index = channel->findData(int(value));
                check(index >= 0, "viewer channel choice is available");
                channel->setCurrentIndex(index);
            };
            const auto rgb = captureAfter(
                window,
                [&] {
                    selectViewerChannel(DisplayChannel::RGB);
                    grayscale->setChecked(true);
                    exposure->setValue(0);
                    window.setTime(motion::time(0));
                },
                "RGB view produces a real preview frame");
            const auto redGray = captureAfter(
                window,
                [&] {
                    selectViewerChannel(DisplayChannel::Red);
                    grayscale->setChecked(true);
                    window.setTime(motion::time(0));
                },
                "red-channel grayscale produces a real preview frame");
            check(redGray != rgb && grayscale->isEnabled() &&
                      redGray.pixelColor(0, 0).red() == redGray.pixelColor(0, 0).green() &&
                      redGray.pixelColor(0, 0).green() == redGray.pixelColor(0, 0).blue() &&
                      redGray.pixelColor(0, 0).alpha() == 255,
                  "red grayscale isolates the channel and presents it as opaque gray");
            const auto redColor = captureAfter(
                window,
                [&] {
                    grayscale->setChecked(false);
                    window.setTime(motion::time(0));
                },
                "red-channel color view produces a real preview frame");
            check(redColor != redGray && redColor.pixelColor(0, 0).red() > 0 &&
                      redColor.pixelColor(0, 0).green() == 0 && redColor.pixelColor(0, 0).blue() == 0,
                  "colored red view isolates red into its display channel");
            const auto greenColor = captureAfter(
                window,
                [&] {
                    selectViewerChannel(DisplayChannel::Green);
                    window.setTime(motion::time(0));
                },
                "green-channel view produces a real preview frame");
            check(greenColor.pixelColor(0, 0).red() == 0 && greenColor.pixelColor(0, 0).green() == 0 &&
                      greenColor.pixelColor(0, 0).blue() == 0,
                  "colored green view does not leak red or blue values");
            const auto blueColor = captureAfter(
                window,
                [&] {
                    selectViewerChannel(DisplayChannel::Blue);
                    window.setTime(motion::time(0));
                },
                "blue-channel view produces a real preview frame");
            check(blueColor.pixelColor(0, 0).red() == 0 && blueColor.pixelColor(0, 0).green() == 0 &&
                      blueColor.pixelColor(0, 0).blue() > 0,
                  "colored blue view isolates blue into its display channel");
            const auto alphaBase = captureAfter(
                window,
                [&] {
                    selectViewerChannel(DisplayChannel::Alpha);
                    exposure->setValue(0);
                    window.setTime(motion::time(0));
                },
                "alpha view produces a real preview frame");
            const QColor alphaPixel = alphaBase.pixelColor(0, 0);
            check(!grayscale->isEnabled() && near(alphaPixel.red() / 255., .75, 1. / 255.) &&
                      alphaPixel.red() == alphaPixel.green() && alphaPixel.green() == alphaPixel.blue() &&
                      alphaPixel.alpha() == 255,
                  "Alpha presents source coverage directly as an opaque diagnostic");
            const auto alphaExposed = captureAfter(
                window,
                [&] {
                    exposure->setValue(2);
                    window.setTime(motion::time(0));
                },
                "alpha view with exposure produces a real preview frame");
            check(alphaExposed == alphaBase, "display exposure does not change the Alpha diagnostic");
            const auto luminance = captureAfter(
                window,
                [&] {
                    selectViewerChannel(DisplayChannel::Luminance);
                    exposure->setValue(0);
                    window.setTime(motion::time(0));
                },
                "linear-luminance view produces a real preview frame");
            check(!grayscale->isEnabled() && luminance.pixelColor(0, 0).red() == luminance.pixelColor(0, 0).green() &&
                      luminance.pixelColor(0, 0).green() == luminance.pixelColor(0, 0).blue() &&
                      luminance.pixelColor(0, 0).alpha() == 255,
                  "luminance is an opaque grayscale diagnostic");
            const auto rgbExposed = captureAfter(
                window,
                [&] {
                    selectViewerChannel(DisplayChannel::RGB);
                    exposure->setValue(1);
                    window.setTime(motion::time(0));
                },
                "RGB exposure produces a real preview frame");
            check(rgbExposed != rgb && !grayscale->isEnabled(),
                  "viewer exposure changes RGB output while RGB disables the channel grayscale toggle");
            const auto rgbRestored = captureAfter(
                window,
                [&] {
                    exposure->setValue(0);
                    window.setTime(motion::time(0));
                },
                "default RGB view can be restored");
            check(rgbRestored == rgb, "returning to RGB at zero stops restores the original preview pixels");
            unchangedViewDocument();
        }

        {
            Project temporalScene;
            auto &comp = temporalScene.compositions.front();
            comp.name = "Motion blur UI contract";
            comp.width = 560;
            comp.height = 560;
            comp.fps = motion::time(30);
            comp.duration = motion::time(1);
            const QString originalImagePath =
                QFileInfo(QString(MOTION_SOURCE_DIR) + "/fixtures/original-image.png").absoluteFilePath();
            auto originalImage = inspectPng(originalImagePath, 10);
            originalImage.path = originalImagePath;
            temporalScene.assets.push_back(originalImage);
            Layer background;
            background.id = 2;
            background.name = "Black background";
            background.width = comp.width;
            background.height = comp.height;
            background.in = motion::time(0);
            background.out = comp.duration;
            background.setColor(QColor("black"));
            Layer mover;
            mover.id = 3;
            mover.name = "Animated original orb";
            mover.kind = LayerKind::Image;
            mover.source = originalImage.id;
            mover.width = originalImage.width;
            mover.height = originalImage.height;
            mover.in = motion::time(0);
            mover.out = comp.duration;
            mover.channel(Property::AnchorX).base = 0;
            mover.channel(Property::AnchorY).base = 0;
            mover.channel(Property::PositionX).base = 0;
            mover.channel(Property::PositionY).base = 0;
            comp.layers = {mover, background};
            temporalScene.nextId = 11;
            window.editor().replace(temporalScene);
            window.editor().select({3});
            window.setTime(motion::time(0));

            auto *position = window.findChild<QDoubleSpinBox *>("property2");
            check(position != nullptr, "animated layer exposes its real Position X control");
            position->setValue(0);
            QMetaObject::invokeMethod(position, "editingFinished");
            auto *positionKey = window.findChild<QPushButton *>("key.property2");
            check(positionKey != nullptr, "Position X exposes its keyframe button");
            positionKey->click();
            window.setTime(frameTime(29, motion::time(30)));
            position = window.findChild<QDoubleSpinBox *>("property2");
            position->setValue(80);
            QMetaObject::invokeMethod(position, "editingFinished");
            check(layer(window.editor().project(), 3).channel(Property::PositionX).keys.size() == 2,
                  "real property controls create the transform keys for the render scene");

            auto *motionBlurMaster = window.findChild<QToolButton *>("compositionMotionBlurToggle");
            check(motionBlurMaster && motionBlurMaster->isCheckable(),
                  "composition motion-blur master is a checkable toolbar control");
            motionBlurMaster->click();
            check(composition(window.editor().project(), 1).motionBlurEnabled,
                  "toolbar master enables composition motion blur");
            window.editor().undoStack().undo();
            check(!composition(window.editor().project(), 1).motionBlurEnabled,
                  "toolbar motion-blur master participates in Undo");
            window.editor().undoStack().redo();
            check(composition(window.editor().project(), 1).motionBlurEnabled,
                  "toolbar motion-blur master participates in Redo");

            QAction *settingsAction = nullptr;
            for (auto *action : window.findChildren<QAction *>())
                if (action->text().startsWith("Composition settings"))
                    settingsAction = action;
            check(settingsAction != nullptr, "composition shutter settings are reachable from the real menu");
            bool settingsUpdated = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                auto *enabled = dialog ? dialog->findChild<QCheckBox *>("compositionMotionBlurEnabled") : nullptr;
                auto *angle = dialog ? dialog->findChild<QSpinBox *>("motionBlurShutterAngle") : nullptr;
                auto *phase = dialog ? dialog->findChild<QSpinBox *>("motionBlurShutterPhase") : nullptr;
                auto *samples = dialog ? dialog->findChild<QSpinBox *>("motionBlurSamples") : nullptr;
                auto *buttons = dialog ? dialog->findChild<QDialogButtonBox *>() : nullptr;
                if (!dialog || !enabled || !angle || !phase || !samples || !buttons) {
                    if (dialog)
                        dialog->reject();
                    return;
                }
                enabled->setChecked(true);
                angle->setValue(720);
                phase->setValue(0);
                samples->setValue(16);
                settingsUpdated = true;
                buttons->button(QDialogButtonBox::Ok)->click();
            });
            settingsAction->trigger();
            check(settingsUpdated && composition(window.editor().project(), 1).motionBlurEnabled &&
                      composition(window.editor().project(), 1).shutterAngle == 720 &&
                      composition(window.editor().project(), 1).shutterPhase == 0 &&
                      composition(window.editor().project(), 1).motionBlurSamples == 16,
                  "modal composition controls persist shutter angle, phase, samples and master state");
            window.editor().undoStack().undo();
            check(composition(window.editor().project(), 1).motionBlurEnabled &&
                      composition(window.editor().project(), 1).shutterAngle == 180 &&
                      composition(window.editor().project(), 1).shutterPhase == -90 &&
                      composition(window.editor().project(), 1).motionBlurSamples == 16,
                  "composition shutter edit Undo restores the previous settings");
            window.editor().undoStack().redo();
            check(composition(window.editor().project(), 1).motionBlurEnabled &&
                      composition(window.editor().project(), 1).shutterAngle == 720 &&
                      composition(window.editor().project(), 1).shutterPhase == 0 &&
                      composition(window.editor().project(), 1).motionBlurSamples == 16,
                  "composition shutter edit Redo restores the selected settings");

            auto *timeline = window.timeline();
            const QRectF moverCell = timeline->motionBlurCellRect(3);
            check(!timeline->motionBlurCellRect(2).isEmpty() && !moverCell.isEmpty(),
                  "timeline exposes hit rectangles for each layer motion-blur switch");
            window.editor().select({2, 3});
            mouse(timeline, QEvent::MouseButtonPress, moverCell.center(), Qt::LeftButton);
            mouse(timeline, QEvent::MouseButtonRelease, moverCell.center(), Qt::NoButton);
            check(!layer(window.editor().project(), 2).motionBlur && layer(window.editor().project(), 3).motionBlur &&
                      window.editor().selection() == std::vector<Id>{2, 3},
                  "timeline switch changes only its layer and preserves a multi-selection");
            window.editor().undoStack().undo();
            check(!layer(window.editor().project(), 3).motionBlur,
                  "timeline layer motion-blur switch participates in Undo");
            window.editor().undoStack().redo();
            check(layer(window.editor().project(), 3).motionBlur,
                  "timeline layer motion-blur switch participates in Redo");
            window.editor().apply("Lock motion-blur layer", [](Project &p) { layer(p, 3).locked = true; });
            const int lockedUndoIndex = window.editor().undoStack().index();
            mouse(timeline, QEvent::MouseButtonPress, timeline->motionBlurCellRect(3).center(), Qt::LeftButton);
            mouse(timeline, QEvent::MouseButtonRelease, timeline->motionBlurCellRect(3).center(), Qt::NoButton);
            check(layer(window.editor().project(), 3).motionBlur &&
                      window.editor().undoStack().index() == lockedUndoIndex &&
                      window.lastError().contains("Unlock layer first"),
                  "locked layer refuses a motion-blur switch without a partial edit");
            window.editor().undoStack().undo();
            window.editor().select({3});

            auto *effectTypes = window.findChild<QComboBox *>("effectTypeSelector");
            auto *addEffect = window.findChild<QPushButton *>("addEffectButton");
            auto addAndSet = [&](const QString &type, const QString &parameter, double value) {
                const int index = effectTypes->findData(type);
                check(index >= 0, "render scene effect is available from the real catalog");
                effectTypes->setCurrentIndex(index);
                addEffect->click();
                const auto effectId = layer(window.editor().project(), 3).effects.back().id;
                const QString controlName = parameter + ".0." + QString::number(effectId);
                auto *control = window.findChild<QDoubleSpinBox *>(controlName);
                check(control != nullptr, "render scene effect has its visible numeric control");
                const PropertyRef ref{parameter, 0, effectId};
                control->setValue(toDisplay(ref, value));
                QMetaObject::invokeMethod(control, "editingFinished");
                check(near(layer(window.editor().project(), 3).channel(ref).base, value),
                      "render scene effect value is set through its real widget");
                return effectId;
            };
            const Id splitEffect = addAndSet("motion.rgb-split", "rgb-split.red-offset", 2);
            const Id exposureEffect = addAndSet("motion.exposure", "exposure.stops", .5);
            const Id blurEffect = addAndSet("motion.gaussian-blur", "gaussian-blur.sigma", 1);
            check(splitEffect && exposureEffect && blurEffect,
                  "real render scene contains RGB Split, Exposure and Gaussian Blur");

            const QString blurSave = tmp.filePath("v06-motion-blur-ui.json");
            check(!QFileInfo::exists(blurSave) && window.saveAs(blurSave),
                  "motion-blur scene saves to a new private test project");
            QFile savedBlurFile(blurSave);
            check(savedBlurFile.open(QIODevice::ReadOnly), "saved motion-blur project can be inspected");
            const auto persistedBlurBytes = savedBlurFile.readAll();
            const auto persistedBlurProject = decodeProject(persistedBlurBytes);
            const bool blurOpened = window.openProject(blurSave);
            auto normalizedReopen = window.editor().project();
            bool sameSource = persistedBlurProject.assets.size() == normalizedReopen.assets.size();
            for (size_t i = 0; i < std::min(persistedBlurProject.assets.size(), normalizedReopen.assets.size()); ++i) {
                const auto &before = persistedBlurProject.assets[i];
                const auto &after = normalizedReopen.assets[i];
                const auto beforeResolved =
                    QFileInfo(QDir(projectDirectory(blurSave)).absoluteFilePath(before.path)).canonicalFilePath();
                const auto afterResolved = QFileInfo(after.path).canonicalFilePath();
                sameSource &= before.id == after.id && !beforeResolved.isEmpty() &&
                              beforeResolved == afterResolved && before.sha256 == after.sha256;
                normalizedReopen.assets[i].path = before.path;
            }
            check(blurOpened && sameSource && encodeProject(normalizedReopen) == persistedBlurBytes,
                  "composition settings, layer switch, animated transforms and effects survive save and reopen");
            window.setTime(frameTime(15, motion::time(30)));
            captureAfter(window, [&] { window.setTime(frameTime(15, motion::time(30))); },
                         "motion-blur UI scene produces its requested preview frame");

            auto *renderOverride = window.findChild<QComboBox *>("renderMotionBlurOverride");
            auto *temporalChannel = window.findChild<QComboBox *>("viewerChannel");
            auto *temporalGrayscale = window.findChild<QCheckBox *>("viewerGrayscale");
            auto *temporalExposure = window.findChild<QDoubleSpinBox *>("viewerExposure");
            check(renderOverride && temporalChannel && temporalGrayscale && temporalExposure,
                  "render and view-only controls remain available for export checks");
            auto selectTemporalChannel = [&](DisplayChannel value) {
                const int index = temporalChannel->findData(int(value));
                check(index >= 0, "temporal viewer channel choice is available");
                temporalChannel->setCurrentIndex(index);
            };
            auto overrideIndex = [&](MotionBlurOverride value) {
                const int index = renderOverride->findData(int(value));
                check(index >= 0, "render-override mode is selectable by its stable value");
                return index;
            };
            const auto runStillExport = [&](const QString &directory, MotionBlurOverride mode,
                                            bool switchToOffAfterStart = false) {
                check(!QFileInfo::exists(directory), "each export uses a new output directory");
                renderOverride->setCurrentIndex(overrideIndex(mode));
                std::optional<ExportResult> result;
                const auto connection = QObject::connect(
                    &window, &MainWindow::outputFinished, &window,
                    [&](const ExportResult &finished) { result = finished; });
                window.exportTo(directory, true, false);
                if (switchToOffAfterStart) {
                    check(!renderOverride->isEnabled(),
                          "render override control is locked while the queued snapshot is running");
                    renderOverride->setCurrentIndex(overrideIndex(MotionBlurOverride::Off));
                }
                QElapsedTimer timeout;
                timeout.start();
                while (!result && timeout.elapsed() < 30000)
                    events(5);
                QObject::disconnect(connection);
                check(result && result->state == "complete" && result->completed == 1,
                      "real render queue completes its one-frame UI export");
                QImage image(QDir(directory).filePath("frame_000015.png"));
                check(!image.isNull() && image.size() == QSize(560, 560),
                      "real export publishes and decodes its requested frame");
                return image.convertToFormat(QImage::Format_RGBA8888);
            };
            const auto onImage = runStillExport(tmp.filePath("v06-render-on"),
                                                MotionBlurOverride::OnForCheckedLayers);
            const auto offImage = runStillExport(tmp.filePath("v06-render-off"), MotionBlurOverride::Off);
            check(onImage != offImage, "render override produces distinct blur-on and blur-off pixels");
            const auto currentSettingsImage = runStillExport(tmp.filePath("v06-render-current-settings"),
                                                             MotionBlurOverride::CurrentSettings);
            check(currentSettingsImage == onImage,
                  "Current Settings uses the enabled composition and checked layer switches");
            motionBlurMaster->click();
            const auto masterDisabledImage = runStillExport(tmp.filePath("v06-render-master-disabled"),
                                                            MotionBlurOverride::CurrentSettings);
            const auto forceCheckedImage = runStillExport(tmp.filePath("v06-render-master-bypassed"),
                                                          MotionBlurOverride::OnForCheckedLayers);
            check(masterDisabledImage == offImage && forceCheckedImage == onImage,
                  "On for Checked Layers bypasses composition masters while Current Settings honors them");
            window.editor().undoStack().undo();
            const auto moverBlurCell = window.timeline()->motionBlurCellRect(3);
            mouse(window.timeline(), QEvent::MouseButtonPress, moverBlurCell.center(), Qt::LeftButton);
            mouse(window.timeline(), QEvent::MouseButtonRelease, moverBlurCell.center(), Qt::NoButton);
            const auto uncheckedLayerImage = runStillExport(tmp.filePath("v06-render-layer-unchecked"),
                                                            MotionBlurOverride::OnForCheckedLayers);
            check(uncheckedLayerImage == offImage,
                  "On for Checked Layers still honors an unchecked layer's motion-blur switch");
            window.editor().undoStack().undo();
            const auto snapshotImage = runStillExport(tmp.filePath("v06-render-snapshot"),
                                                      MotionBlurOverride::OnForCheckedLayers, true);
            check(snapshotImage == onImage && renderOverride->currentData().toInt() ==
                                                   int(MotionBlurOverride::Off),
                  "render request keeps its start-time override after its widget value changes");
            const auto motionDocument = encodeProject(window.editor().project());
            const auto motionRevision = window.editor().revision();
            const auto motionUndoIndex = window.editor().undoStack().index();
            const bool motionDirty = window.editor().dirty();
            const auto offWithDiagnosticView = captureAfter(
                    window,
                    [&] {
                    selectTemporalChannel(DisplayChannel::Red);
                    temporalGrayscale->setChecked(false);
                    temporalExposure->setValue(2);
                    window.setTime(frameTime(15, motion::time(30)));
                },
                "diagnostic view is applied while leaving the render scene intact");
            check(!offWithDiagnosticView.isNull(), "diagnostic view has a real rendered preview");
            const auto diagnosticExport = runStillExport(tmp.filePath("v06-render-view-independent"),
                                                         MotionBlurOverride::Off);
            check(diagnosticExport == offImage,
                  "PNG export remains byte-pixel identical while the viewer uses a diagnostic channel and exposure");
            check(encodeProject(window.editor().project()) == motionDocument &&
                      window.editor().revision() == motionRevision &&
                      window.editor().undoStack().index() == motionUndoIndex &&
                      window.editor().dirty() == motionDirty,
                  "render override and viewer display controls do not dirty project data");

            const QString demoPath = qEnvironmentVariable("MOTION_QA_EFFECTS_DEMO").trimmed();
            if (!demoPath.isEmpty()) {
                const QStringList outputs{demoPath, demoPath + ".png", demoPath + ".frame.png",
                                          demoPath + ".metrics.json"};
                for (const auto &output : outputs)
                    check(!QFileInfo::exists(output),
                          "effects UI demo refuses to overwrite an existing project or image");
                check(QDir().mkpath(QFileInfo(demoPath).absolutePath()),
                      "effects UI demo output folder exists");
                window.findChild<QDockWidget *>("effectControlsDock")->show();
                window.findChild<QDockWidget *>("effectControlsDock")->raise();
                window.resize(1506, 1020);
                for (auto *action : window.findChildren<QAction *>())
                    if (action->text() == "Reset workspace") {
                        action->trigger();
                        break;
                    }
                captureAfter(
                    window,
                    [&] {
                        selectTemporalChannel(DisplayChannel::RGB);
                        temporalGrayscale->setChecked(true);
                        temporalExposure->setValue(0);
                        window.setTime(motion::time(1, 2));
                    },
                    "effects UI demo waits for the requested motion-blurred frame");
                check(window.saveAs(demoPath), "effects UI demo saves a new native editable project");
                const auto projectFrame = window.displayedImage();
                check(projectFrame.save(demoPath + ".frame.png") &&
                          window.grab().save(demoPath + ".png"),
                      "effects UI demo captures the real viewer frame and native controls");
                const QJsonObject metrics{
                    {"composition", composition(window.editor().project(), 1).name},
                    {"timeNumerator", QString::number(window.currentTime().numerator)},
                    {"timeDenominator", QString::number(window.currentTime().denominator)},
                    {"motionBlurEnabled", composition(window.editor().project(), 1).motionBlurEnabled},
                    {"shutterAngle", composition(window.editor().project(), 1).shutterAngle},
                    {"shutterPhase", composition(window.editor().project(), 1).shutterPhase},
                    {"motionBlurSamples", composition(window.editor().project(), 1).motionBlurSamples},
                    {"movingLayerEnabled", layer(window.editor().project(), 3).motionBlur},
                    {"effectCount", int(layer(window.editor().project(), 3).effects.size())}};
                atomicWrite(demoPath + ".metrics.json", QJsonDocument(metrics).toJson());
            }
        }
        window.editor().replace(solidScene());
        window.editor().select({3});
        window.setTime(motion::time(0));
        auto textProject = parentScene();
        layer(textProject, 3).kind = LayerKind::Text;
        window.editor().replace(textProject);
        window.editor().select({3});
        check(window.findChild<QComboBox *>("text.family.choices")->count() > 0 &&
                  window.findChild<QComboBox *>("text.style.choices")->count() > 0,
              "font family and style pickers remain available");
        check(window.findChild<QPushButton *>("layerColorButton") != nullptr,
              "native color picker remains available");
        auto *sourceText = window.findChild<QPlainTextEdit *>("layerText");
        sourceText->setPlainText("Line one\nРядок два");
        window.findChild<QPushButton *>("applyTextButton")->click();
        check(layer(window.editor().project(), 3).string("text.source") == "Line one\nРядок два",
              "source text retains multiline Unicode editing");
        auto transfer = solidScene();
        layer(transfer, 2).start = time(1);
        layer(transfer, 2).channel(Property::PositionX).keys = {
            {time(1, 7), 12, Interpolation::Cubic, {0, 0}, {.2, 3}},
            {time(8, 7), 36, Interpolation::Linear, {-.3, -4}, {0, 0}}};
        layer(transfer, 3).start = time(2);
        window.editor().replace(transfer);
        window.editor().select({2});
        window.timeline()->setFilter("P");
        events();
        auto shortRuler = transfer;
        shortRuler.compositions[0].duration = time(6);
        window.editor().replace(shortRuler);
        const auto step = window.timeline()->rulerStepFrames();
        check(window.timeline()->timeX(frameTime(step, transfer.compositions[0].fps)) -
                      window.timeline()->timeX(motion::time(0)) >=
                  69,
              "ruler label spacing follows the actual viewport width");
        window.editor().replace(transfer);
        window.editor().select({2});
        window.timeline()->setFilter("P");
        auto selectKey = [&](Time at, bool extend = false) {
            const QPointF point(window.timeline()->timeX(at),
                                window.timeline()->propertyY(2, Property::PositionX));
            QMouseEvent press(QEvent::MouseButtonPress, point,
                              window.timeline()->mapToGlobal(point.toPoint()), Qt::LeftButton, Qt::LeftButton,
                              extend ? Qt::ShiftModifier : Qt::NoModifier);
            QApplication::sendEvent(window.timeline(), &press);
            mouse(window.timeline(), QEvent::MouseButtonRelease, point, Qt::NoButton);
        };
        selectKey(time(8, 7));
        selectKey(time(15, 7), true);
        window.timeline()->copyKeys();
        window.editor().select({3});
        window.setTime(time(3));
        window.timeline()->pasteKeys();
        check(layer(window.editor().project(), 3).channel(Property::PositionX).keys.size() == 2,
              "click-release-copy transfers both selected keys to the destination layer");
        const auto pasted = layer(window.editor().project(), 3).channel(Property::PositionX);
        check(pasted.keys[0].at == time(1) && pasted.keys[1].at == time(2),
              "paste anchors earliest key to comp time despite different source starts");
        check(pasted.keys[0].outgoing == Interpolation::Cubic &&
                  near(pasted.keys[0].outHandle.dtSeconds, .2) && near(pasted.keys[0].outHandle.dv, 3) &&
                  near(pasted.keys[1].inHandle.dtSeconds, -.3),
              "paste retains interpolation and handles");
        window.editor().undoStack().undo();
        check(encodeProject(window.editor().project()) == encodeProject(transfer),
              "paste is one complete undo operation");
        window.editor().apply("Lock destination", [](Project &p) { layer(p, 3).locked = true; });
        window.editor().select({2, 3});
        auto lockedBefore = encodeProject(window.editor().project());
        auto lockedIndex = window.editor().undoStack().index();
        window.timeline()->pasteKeys();
        check(encodeProject(window.editor().project()) == lockedBefore &&
                  window.editor().undoStack().index() == lockedIndex,
              "locked destination refuses the entire multi-layer paste without partial edits");
        window.editor().undoStack().undo();
        window.editor().apply("Remove copied source", [](Project &p) {
            std::erase_if(p.compositions[0].layers, [](const Layer &l) { return l.id == 2; });
        });
        window.editor().select({3});
        window.setTime(time(4));
        window.timeline()->pasteKeys();
        check(layer(window.editor().project(), 3).channel(Property::PositionX).keys[0].at == time(2),
              "clipboard snapshot survives source deletion and retains original timing");
        window.timeline()->cutKeys();
        check(layer(window.editor().project(), 3).channel(Property::PositionX).keys.empty(),
              "cut removes the pasted selected keys");
        window.editor().undoStack().undo();
        check(layer(window.editor().project(), 3).channel(Property::PositionX).keys.size() == 2,
              "cut undo restores all keys");
        window.editor().replace(transfer);
        window.editor().select({2});
        window.timeline()->setFilter("P");
        selectKey(time(8, 7));
        selectKey(time(15, 7), true);
        const QPointF dragFrom(window.timeline()->timeX(time(8, 7)),
                               window.timeline()->propertyY(2, Property::PositionX));
        const auto dragTo =
            dragFrom +
            QPointF(window.timeline()->timeX(time(1)) - window.timeline()->timeX(motion::time(0)), 0);
        mouse(window.timeline(), QEvent::MouseButtonPress, dragFrom, Qt::LeftButton);
        mouse(window.timeline(), QEvent::MouseMove, dragTo, Qt::LeftButton);
        mouse(window.timeline(), QEvent::MouseButtonRelease, dragTo, Qt::NoButton);
        window.timeline()->copyKeys();
        window.editor().select({3});
        window.setTime(time(6));
        window.timeline()->pasteKeys();
        check(layer(window.editor().project(), 3).channel(Property::PositionX).keys.size() == 2 &&
                  layer(window.editor().project(), 3).channel(Property::PositionX).keys[0].at == time(4),
              "moved keys stay selected and copyable after mouse release");
        window.editor().replace(transfer);
        window.editor().select({2});
        window.timeline()->setFilter("P");
        selectKey(time(8, 7));
        window.timeline()->setFocus();
        key(window.timeline(), Qt::Key_Down);
        window.timeline()->cutKeys();
        check(encodeProject(window.editor().project()) == encodeProject(transfer),
              "keyboard layer navigation cannot cut keys left selected on another layer");
        auto multipleSources = transfer;
        layer(multipleSources, 3).channel(Property::PositionX).keys = {{time(1, 2), 90}};
        window.editor().replace(multipleSources);
        window.editor().select({2, 3});
        window.timeline()->setFilter("P");
        QPointF boxFrom(window.timeline()->timeX(motion::time(0)),
                        window.timeline()->propertyY(2, Property::PositionX) - 7);
        QPointF boxTo(window.timeline()->timeX(time(4)),
                      window.timeline()->propertyY(3, Property::PositionX) + 7);
        mouse(window.timeline(), QEvent::MouseButtonPress, boxFrom, Qt::LeftButton);
        mouse(window.timeline(), QEvent::MouseMove, boxTo, Qt::LeftButton);
        mouse(window.timeline(), QEvent::MouseButtonRelease, boxTo, Qt::NoButton);
        window.timeline()->copyKeys();
        window.editor().select({3});
        window.setTime(time(5));
        auto multiBefore = encodeProject(window.editor().project());
        window.timeline()->pasteKeys();
        check(encodeProject(window.editor().project()) == multiBefore &&
                  window.lastError().contains("from one layer"),
              "ambiguous multi-source to single-destination paste is refused");
        auto effectTransfer = solidScene();
        Id fromEffect = addEffect(effectTransfer, 2), toEffect = addEffect(effectTransfer, 3);
        layer(effectTransfer, 2).channel({"color.gain", 0, fromEffect}).keys = {{time(1, 3), 2}};
        window.editor().replace(effectTransfer);
        window.editor().select({2});
        window.timeline()->setFilter("E");
        const QPointF effectPoint(window.timeline()->timeX(time(1, 3)),
                                  window.timeline()->propertyY(2, {"color.gain", 0, fromEffect}));
        mouse(window.timeline(), QEvent::MouseButtonPress, effectPoint, Qt::LeftButton);
        mouse(window.timeline(), QEvent::MouseButtonRelease, effectPoint, Qt::NoButton);
        window.timeline()->copyKeys();
        window.editor().select({3});
        window.setTime(time(5));
        window.timeline()->pasteKeys();
        check(layer(window.editor().project(), 3).channel({"color.gain", 0, toEffect}).keys[0].at == time(5),
              "effect keys map to the destination instance instead of source IDs");
        window.editor().apply("Remove destination effect", [](Project &p) { layer(p, 3).effects.clear(); });
        auto absentEffectBefore = encodeProject(window.editor().project());
        window.timeline()->pasteKeys();
        check(encodeProject(window.editor().project()) == absentEffectBefore &&
                  window.lastError().contains("matching effect"),
              "missing destination effect is explicitly refused without creating a fake effect");
        window.activateWindow();
        events(100);
        auto graphScene = parentScene();
        layer(graphScene, 3).channel(Property::PositionX).keys = {
            {motion::time(0), 0}, {time(2), 100}, {time(4), 200}, {time(6), 300}};
        window.editor().replace(graphScene);
        window.editor().select({3});
        window.timeline()->setFilter("P");
        events();
        const QPointF middleKey(window.timeline()->timeX(time(2)),
                                window.timeline()->propertyY(3, Property::PositionX));
        mouse(window.timeline(), QEvent::MouseButtonPress, middleKey, Qt::LeftButton);
        mouse(window.timeline(), QEvent::MouseButtonRelease, middleKey, Qt::NoButton);
        auto *graph = window.findChild<Graph *>("graphEditor");
        auto *graphToggle = window.findChild<QToolButton *>("graphToggle");
        graphToggle->setChecked(true);
        window.activateWindow();
        events();
        graph->setFocus();
        events();
        check(QApplication::focusWidget() == graph, "Graph has keyboard focus before F9");
        check(graph->keySelection().has_value(), "Timeline selection reaches Graph before easing");
        check(window.findChild<QComboBox *>("graphProperty")->currentText().contains("Position X"),
              "showing Graph retains the selected parameter in its selector");
        key(graph, Qt::Key_F9);
        check(layer(window.editor().project(), 3).channel(Property::PositionX).keys[1].outgoing ==
                  Interpolation::Cubic,
              "F9 actually converts the selected graph interval to cubic");
        check(near(layer(window.editor().project(), 3).channel(Property::PositionX).keys[1].inHandle.dv, 0) &&
                  layer(window.editor().project(), 3).channel(Property::PositionX).keys[2].outgoing ==
                      Interpolation::Linear,
              "F9 in Graph edits its selected key and preserves unrelated segments");
        window.editor().undoStack().undo();
        check(encodeProject(window.editor().project()) == encodeProject(graphScene),
              "graph easing undo is lossless");
        check(!graph->keySelection(),
              "Undo clears time-based key selection before a later destructive command");
        const auto reselect = graph->keyPosition(time(2));
        mouse(graph, QEvent::MouseButtonPress, reselect, Qt::LeftButton);
        mouse(graph, QEvent::MouseButtonRelease, reselect, Qt::NoButton);
        auto velocityDialog = [&](bool change, bool accept) {
            bool reached = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<QDialog *>("keyframeVelocityDialog");
                if (!dialog)
                    return;
                reached = true;
                if (change) {
                    dialog->findChild<QDoubleSpinBox *>("incomingSpeed")->setValue(20);
                    dialog->findChild<QDoubleSpinBox *>("incomingInfluence")->setValue(50);
                }
                if (accept)
                    dialog->accept();
                else
                    dialog->reject();
            });
            window.findChild<QPushButton *>("keyframeVelocityButton")->click();
            check(reached, "Keyframe Velocity opens from the graph toolbar");
        };
        velocityDialog(false, true);
        check(encodeProject(window.editor().project()) == encodeProject(graphScene),
              "untouched velocity dialog leaves the document byte-for-value unchanged");
        velocityDialog(true, false);
        check(encodeProject(window.editor().project()) == encodeProject(graphScene),
              "velocity dialog Cancel does not apply fields");
        velocityDialog(true, true);
        const auto editedHandle =
            layer(window.editor().project(), 3).channel(Property::PositionX).keys[1].inHandle;
        check(near(editedHandle.dtSeconds, -1) && near(editedHandle.dv, -20),
              "velocity popup changes the actual selected incoming handle");
        auto beforeGraphMode = encodeProject(window.editor().project());
        graphToggle->setChecked(false);
        graphToggle->setChecked(true);
        events();
        check(graph->keySelection() && graph->keySelection()->at == time(2) &&
                  window.findChild<QComboBox *>("graphProperty")->currentText().contains("Position X"),
              "switching both directions preserves graph property and selected key on Cocoa");
        events();
        auto valueGraph = graph->grab().toImage();
        window.findChild<QComboBox *>("graphType")->setCurrentIndex(1);
        events();
        check(graph->speedGraph() && graph->grab().toImage() != valueGraph &&
                  encodeProject(window.editor().project()) == beforeGraphMode,
              "Speed Graph displays a distinct derivative plot without editing values");
        const QPointF speedKey = graph->keyPosition(time(2));
        mouse(graph, QEvent::MouseButtonPress, speedKey, Qt::LeftButton);
        mouse(graph, QEvent::MouseMove, speedKey + QPointF(40, 40), Qt::LeftButton);
        mouse(graph, QEvent::MouseButtonRelease, speedKey + QPointF(40, 40), Qt::NoButton);
        check(encodeProject(window.editor().project()) == beforeGraphMode,
              "Speed Graph selection does not accidentally drag value-graph coordinates");
        graph->setProperty(Property::Opacity);
        graph->easeSelected();
        check(encodeProject(window.editor().project()) == beforeGraphMode &&
                  window.lastError().contains("Select a graph keyframe"),
              "changing graph property clears stale key identity");
        graph->setProperty(Property::PositionX);
        graph->setSpeedGraph(false);
        events();
        const QPointF graphKey = graph->keyPosition(time(2));
        mouse(graph, QEvent::MouseButtonPress, graphKey, Qt::LeftButton);
        mouse(graph, QEvent::MouseMove, graphKey + QPointF(30, -15), Qt::LeftButton);
        key(graph, Qt::Key_Escape);
        check(encodeProject(window.editor().project()) == beforeGraphMode,
              "Graph key drag Escape restores curve and time");
        window.activateWindow();
        events();
        graph->setFocus();
        const auto historyBeforeGesture = window.editor().undoStack().index();
        mouse(graph, QEvent::MouseButtonPress, graph->keyPosition(time(2)), Qt::LeftButton);
        mouse(graph, QEvent::MouseMove, graph->keyPosition(time(2)) + QPointF(25, -10), Qt::LeftButton);
        const auto undoCombination = QKeySequence(QKeySequence::Undo)[0];
        QKeyEvent undoGesture(QEvent::KeyPress, undoCombination.key(), undoCombination.keyboardModifiers());
        QApplication::sendEvent(graph, &undoGesture);
        check(encodeProject(window.editor().project()) == beforeGraphMode &&
                  window.editor().undoStack().index() == historyBeforeGesture && !window.editor().gesturing(),
              "Undo during a graph drag cancels that gesture without undoing an unrelated command");
        graph->interpolateSelected(Interpolation::Hold);
        check(layer(window.editor().project(), 3).channel(Property::PositionX).keys[1].outgoing ==
                      Interpolation::Hold &&
                  layer(window.editor().project(), 3).channel(Property::PositionX).keys[2].outgoing ==
                      Interpolation::Linear,
              "graph interpolation acts only on the selected outgoing segment");
        window.editor().undoStack().undo();
        graphToggle->setChecked(false);
        graph->setComposition(99999);
        check(!graph->grab().isNull(), "Graph handles a removed composition without throwing from paint");
        graph->setComposition(1);
        if (qEnvironmentVariableIsSet("MOTION_QA_SCREENSHOT")) {
            window.openProject(QString(MOTION_SOURCE_DIR) + "/fixtures/original-scene.json");
            Id selected = 0;
            for (const auto &c : window.editor().project().compositions)
                for (const auto &l : c.layers)
                    if (l.kind == LayerKind::Image)
                        selected = l.id;
            window.editor().select({selected});
            window.editor().apply("Show effect controls", [=](Project &p) { addEffect(p, selected); });
            window.resize(1506, 1020);
            events(); // Cocoa may constrain the content height to the current screen.
            for (auto *action : window.findChildren<QAction *>())
                if (action->text() == "Reset workspace") {
                    action->trigger();
                    break;
            }
            window.timeline()->setFilter("");
            QLabel *screenshotFrameLabel = nullptr;
            for (auto *label : window.findChildren<QLabel *>())
                if (label->toolTip().contains("exact time"))
                    screenshotFrameLabel = label;
            check(screenshotFrameLabel != nullptr, "real screenshot uses the current preview frame");
            int screenshotFrameCount = 0;
            auto screenshotFrameConnection = QObject::connect(&window, &MainWindow::frameDisplayed, &window,
                                                               [&] { ++screenshotFrameCount; });
            window.setTime(time(3));
            const auto displayedThree = QString("exact time 3/1 s");
            QElapsedTimer screenshotWait;
            screenshotWait.start();
            while ((screenshotFrameCount == 0 ||
                    !screenshotFrameLabel->toolTip().contains(displayedThree)) &&
                   screenshotWait.elapsed() < 10000)
                events(5);
            QObject::disconnect(screenshotFrameConnection);
            check(screenshotFrameCount > 0 && screenshotFrameLabel->toolTip().contains(displayedThree),
                  "real screenshot waits for the requested CTI frame");
            QJsonObject metrics{{"contentWidth", window.width()},
                                {"contentHeight", window.height()},
                                {"devicePixelRatio", window.devicePixelRatioF()},
                                {"timelineRowPitch", Timeline::rowPitch()},
                                {"timelineHeaderHeight", Timeline::headerHeight()}};
            for (auto id : {"projectDock", "effectsBrowserDock", "auxiliaryDock", "timelineDock",
                            "compositionViewer"}) {
                auto *widget = window.findChild<QWidget *>(id);
                const auto position = widget->mapTo(&window, QPoint(0, 0));
                metrics[id] = QJsonObject{{"x", position.x()},
                                          {"y", position.y()},
                                          {"width", widget->width()},
                                          {"height", widget->height()}};
            }
            atomicWrite(qEnvironmentVariable("MOTION_QA_SCREENSHOT") + ".metrics.json",
                        QJsonDocument(metrics).toJson());
            check(window.grab().save(qEnvironmentVariable("MOTION_QA_SCREENSHOT")),
                  "requested full-range real UI screenshot saved");
            const auto originalDocument = encodeProject(window.editor().project());
            const auto originalTime = window.currentTime();
            auto *timeline = window.timeline();
            const auto nav = timeline->navigatorRect();
            const auto dragHandleTo = [&](bool startHandle, Time target) {
                const auto range = timeline->visibleRange();
                const Time current = startHandle ? range.first : range.second;
                const auto from = startHandle ? timeline->navigatorStartHandleCenter()
                                              : timeline->navigatorEndHandleCenter();
                const double duration = seconds(composition(window.editor().project(),
                                                            window.currentComposition()).duration);
                const QPointF to = from + QPointF(nav.width() * seconds(target - current) / duration, 0);
                mouse(timeline, QEvent::MouseButtonPress, from, Qt::LeftButton);
                mouse(timeline, QEvent::MouseMove, to, Qt::LeftButton);
                mouse(timeline, QEvent::MouseButtonRelease, to, Qt::NoButton);
            };
            dragHandleTo(true, time(1));
            dragHandleTo(false, time(7));
            check(timeline->visibleRange() == std::pair{time(1), time(7)} &&
                      encodeProject(window.editor().project()) == originalDocument &&
                      window.currentTime() == originalTime,
                  "screenshot zoom gesture changes view without changing CTI or project");
            check(window.grab().save(qEnvironmentVariable("MOTION_QA_SCREENSHOT") + ".zoomed.png"),
                  "requested zoomed navigator screenshot saved");
        }
        if (qEnvironmentVariableIsSet("MOTION_QA_TRANSFER_PROJECT")) {
            auto original = loadProject(QString(MOTION_SOURCE_DIR) + "/fixtures/original-scene.json");
            Project demo;
            demo.assets = original.assets;
            for (auto &asset : demo.assets)
                asset.path = QString(MOTION_SOURCE_DIR) + "/fixtures/" + asset.path;
            demo.nextId = 7;
            auto &comp = demo.compositions.front();
            comp.name = "Keyframe transfer";
            comp.duration = time(6);
            Layer source;
            source.id = 4;
            source.name = "Source · copied Position keys";
            source.kind = LayerKind::Image;
            source.source = demo.assets.front().id;
            source.width = source.height = 560;
            source.out = time(6);
            source.base("transform.anchor", 0) = source.base("transform.anchor", 1) = 280;
            source.base("transform.scale", 0) = source.base("transform.scale", 1) = .5;
            source.base("transform.position", 1) = 320;
            source.channel(Property::PositionX).keys = {
                {motion::time(0), 450, Interpolation::Cubic, {0, 0}, {2. / 3, 0}},
                {time(2), 1450, Interpolation::Linear, {-2. / 3, 0}, {0, 0}}};
            Layer destination = source;
            destination.id = 5;
            destination.name = "Destination · pasted at 2:15";
            destination.channel(Property::PositionX) = {450, {}};
            destination.base("transform.position", 1) = 760;
            destination.start = destination.in = time(1);
            Layer background;
            background.id = 6;
            background.name = "Background";
            background.setColor(QColor("#10242f"));
            background.out = time(6);
            comp.layers = {source, destination, background};
            window.editor().replace(demo);
            window.editor().select({4});
            window.timeline()->setFilter("P");
            window.resize(1506, 989);
            events();
            for (auto *action : window.findChildren<QAction *>())
                if (action->text() == "Reset workspace") {
                    action->trigger();
                    break;
                }
            for (auto t : {motion::time(0), time(2)}) {
                QPointF point(window.timeline()->timeX(t),
                              window.timeline()->propertyY(4, Property::PositionX));
                QMouseEvent press(QEvent::MouseButtonPress, point,
                                  window.timeline()->mapToGlobal(point.toPoint()), Qt::LeftButton,
                                  Qt::LeftButton, t == motion::time(0) ? Qt::NoModifier : Qt::ShiftModifier);
                QApplication::sendEvent(window.timeline(), &press);
                mouse(window.timeline(), QEvent::MouseButtonRelease, point, Qt::NoButton);
            }
            window.timeline()->copyKeys();
            window.editor().select({5});
            window.setTime(time(5, 2));
            window.timeline()->pasteKeys();
            check(layer(window.editor().project(), 5).channel(Property::PositionX).keys.size() == 2,
                  "editable demonstration is authored through real Timeline copy/paste");
            check(window.saveAs(qEnvironmentVariable("MOTION_QA_TRANSFER_PROJECT")),
                  "new editable transfer demonstration saved");
            window.setTime(time(3));
            events(700);
            check(window.grab().save(qEnvironmentVariable("MOTION_QA_TRANSFER_PROJECT") + ".png"),
                  "native transfer interaction screenshot saved");
        }
        if (qEnvironmentVariableIsSet("MOTION_QA_CURVE_PROJECT")) {
            check(window.openProject(QString(MOTION_SOURCE_DIR) + "/fixtures/original-scene.json"),
                  "open original assets for curve demonstration");
            window.editor().apply("Curve demonstration", [](Project &p) {
                p.compositions.front().name = "Curve controls";
                p.compositions.front().duration = time(8);
                layer(p, 4).channel(Property::PositionX).keys = {
                    {motion::time(0), 450}, {time(2), 1050}, {time(4), 600}, {time(6), 1400}};
            });
            window.editor().select({4});
            window.timeline()->setFilter("P");
            window.resize(1506, 989);
            events();
            for (auto *a : window.findChildren<QAction *>())
                if (a->text() == "Reset workspace") {
                    a->trigger();
                    break;
                }
            for (auto t : {motion::time(0), time(2), time(4), time(6)}) {
                QPointF point(window.timeline()->timeX(t),
                              window.timeline()->propertyY(4, Property::PositionX));
                QMouseEvent press(QEvent::MouseButtonPress, point,
                                  window.timeline()->mapToGlobal(point.toPoint()), Qt::LeftButton,
                                  Qt::LeftButton, t == motion::time(0) ? Qt::NoModifier : Qt::ShiftModifier);
                QApplication::sendEvent(window.timeline(), &press);
                mouse(window.timeline(), QEvent::MouseButtonRelease, point, Qt::NoButton);
            }
            window.timeline()->easeKeys();
            graphToggle->setChecked(true);
            window.findChild<QComboBox *>("graphType")->setCurrentIndex(0);
            events();
            auto point = graph->keyPosition(time(2));
            mouse(graph, QEvent::MouseButtonPress, point, Qt::LeftButton);
            mouse(graph, QEvent::MouseButtonRelease, point, Qt::NoButton);
            bool photographed = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<QDialog *>("keyframeVelocityDialog");
                if (!dialog)
                    return;
                dialog->findChild<QDoubleSpinBox *>("incomingSpeed")->setValue(75);
                dialog->findChild<QDoubleSpinBox *>("incomingInfluence")->setValue(50);
                dialog->findChild<QDoubleSpinBox *>("outgoingSpeed")->setValue(-100);
                dialog->findChild<QDoubleSpinBox *>("outgoingInfluence")->setValue(60);
                photographed =
                    dialog->grab().save(qEnvironmentVariable("MOTION_QA_CURVE_PROJECT") + ".velocity.png");
                dialog->accept();
            });
            window.findChild<QPushButton *>("keyframeVelocityButton")->click();
            check(photographed, "real velocity dialog captured with edited incoming and outgoing fields");
            check(window.saveAs(qEnvironmentVariable("MOTION_QA_CURVE_PROJECT")),
                  "editable curve demonstration saved");
            window.setTime(time(2));
            events(700);
            check(window.grab().save(qEnvironmentVariable("MOTION_QA_CURVE_PROJECT") + ".value.png"),
                  "native Value Graph captured");
            window.findChild<QComboBox *>("graphType")->setCurrentIndex(1);
            events(50);
            check(window.grab().save(qEnvironmentVariable("MOTION_QA_CURVE_PROJECT") + ".speed.png"),
                  "native Speed Graph captured");
        }
        auto shot = tmp.filePath("ui.png");
        check(window.grab().save(shot), "actual Qt window capture");
        std::cout << checkCount << " UI checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
