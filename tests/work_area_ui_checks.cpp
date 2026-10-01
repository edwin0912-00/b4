// SPDX-License-Identifier: MPL-2.0
#include "editor.hpp"
#include "project_io.hpp"
#include "support.hpp"
#include "timeline.hpp"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <iostream>
using namespace motion;

namespace {
void mouse(QWidget *widget, QEvent::Type type, QPointF position, Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, widget->mapToGlobal(position.toPoint()),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}
void key(QWidget *widget, int code) {
    QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}
void doubleClick(QWidget *widget, QPointF position) {
    QMouseEvent event(QEvent::MouseButtonDblClick, position, widget->mapToGlobal(position.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}
Project sceneWithRange(std::int64_t first, std::int64_t end) {
    auto project = solidScene();
    auto &comp = project.compositions.front();
    comp.fps = time(24);
    comp.duration = time(12);
    setWorkAreaFrames(comp, first, end);
    return project;
}
FrameRange range(const Editor &editor) {
    return compositionFrameRange(editor.project().compositions.front(), true);
}
void checkOnlyWorkAreaChanged(const Project &before, const Project &after) {
    auto withoutRange = before;
    auto afterWithoutRange = after;
    withoutRange.compositions.front().workArea.reset();
    afterWithoutRange.compositions.front().workArea.reset();
    check(encodeProject(withoutRange) == encodeProject(afterWithoutRange),
          "Timeline Work Area edits leave layers and other project data unchanged");
}
void drag(Timeline &timeline, QPointF from, QPointF to, bool release = true) {
    mouse(&timeline, QEvent::MouseButtonPress, from, Qt::LeftButton);
    mouse(&timeline, QEvent::MouseMove, to, Qt::LeftButton);
    if (release)
        mouse(&timeline, QEvent::MouseButtonRelease, to, Qt::NoButton);
}
} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        auto initialProject = solidScene();
        initialProject.compositions.front().fps = time(24);
        Editor editor(initialProject);
        Timeline timeline(&editor);
        timeline.resize(1200, 250);
        timeline.setComposition(1);

        check(Timeline::rowPitch() == 17, "Work Area preserves dense 17 px layer rows");
        check(Timeline::headerHeight() == 76, "Timeline header includes a separate Work Area track");
        check(timeline.workAreaRect().top() > 44 && timeline.workAreaRect().bottom() < Timeline::headerHeight(),
              "Work Area band sits below the ruler and above layer rows");

        int navigationStarted = 0, timeChanged = 0;
        QObject::connect(&timeline, &Timeline::navigationStarted, &timeline,
                         [&] { ++navigationStarted; });
        QObject::connect(&timeline, &Timeline::timeChanged, &timeline,
                         [&](Time) { ++timeChanged; });
        const auto initialView = timeline.visibleRange();
        const auto original = editor.project();

        timeline.setTime(frameTime(30, time(24)));
        timeline.setWorkAreaBoundary(false);
        check(range(editor).firstFrame == 30, "B boundary starts at the CTI containing frame");
        check(range(editor).frameCount == 258, "B preserves the old exclusive end");
        check(editor.undoStack().index() == 1, "B creates one undo command");
        timeline.setTime(frameTime(34, time(24)));
        timeline.setWorkAreaBoundary(true);
        check(range(editor).firstFrame == 30 && range(editor).frameCount == 5,
              "N includes the CTI frame through the following exclusive boundary");
        check(editor.undoStack().index() == 2 && navigationStarted == 2,
              "B and N each notify navigation and add one edit");
        check(timeChanged == 0 && timeline.visibleRange() == initialView,
              "Work Area shortcuts leave the CTI and Time Navigator untouched");
        checkOnlyWorkAreaChanged(original, editor.project());

        auto shortScene = solidScene();
        shortScene.compositions.front().fps = time(24);
        shortScene.compositions.front().duration = time(13, 96); // 3.25 frames
        editor.replace(shortScene);
        timeline.setTime(frameTime(3, time(24)));
        timeline.setWorkAreaBoundary(false);
        timeline.setWorkAreaBoundary(true);
        check(range(editor).firstFrame == 3 && range(editor).frameCount == 1,
              "B/N retain the final frame in a partial-frame composition");
        check(resolvedWorkArea(editor.project().compositions.front()).end == time(13, 96),
              "N resolves the exclusive terminal boundary to the exact composition end");
        editor.apply("Place range before partial end", [](Project &p) {
            setWorkAreaFrames(composition(p, 1), 2, 3);
        });
        const auto partialEnd = timeline.workAreaEndHandleCenter();
        const auto compEndX = timeline.timeX(time(13, 96));
        drag(timeline, partialEnd, QPointF(compEndX, partialEnd.y()));
        check(range(editor).firstFrame == 2 && range(editor).frameCount == 2 &&
                  resolvedWorkArea(editor.project().compositions.front()).end == time(13, 96),
              "end-handle snapping preserves the exact partial composition endpoint");

        editor.replace(sceneWithRange(20, 40));
        timeline.setComposition(1);
        auto beforeGesture = editor.project();
        const int startUndo = editor.undoStack().index();
        drag(timeline, timeline.workAreaStartHandleCenter(),
             QPointF(timeline.timeX(frameTime(50, time(24))), timeline.workAreaStartHandleCenter().y()));
        check(range(editor).firstFrame == 39 && range(editor).frameCount == 1,
              "crossing the end with the start handle clamps to one frame before it");
        check(editor.undoStack().index() == startUndo + 1,
              "a start-handle drag is one undo command");
        checkOnlyWorkAreaChanged(beforeGesture, editor.project());
        editor.undoStack().undo();
        check(range(editor).firstFrame == 20 && range(editor).frameCount == 20,
              "undo restores the complete original range after a handle drag");
        editor.undoStack().redo();
        check(range(editor).firstFrame == 39 && range(editor).frameCount == 1,
              "redo restores the clamped handle result");

        editor.replace(sceneWithRange(20, 40));
        timeline.setComposition(1);
        const auto endHandle = timeline.workAreaEndHandleCenter();
        const auto startHandle = timeline.workAreaStartHandleCenter();
        drag(timeline, endHandle, QPointF(timeline.timeX(frameTime(10, time(24))), endHandle.y()));
        check(range(editor).firstFrame == 20 && range(editor).frameCount == 1,
              "crossing the start with the end handle clamps to one frame after it");
        check(startHandle.x() < endHandle.x(), "handle geometry follows the selected interval");

        editor.replace(sceneWithRange(20, 40));
        timeline.setComposition(1);
        const auto viewBeforeMove = timeline.visibleRange();
        const auto moveBefore = editor.project();
        const int moveUndo = editor.undoStack().index();
        const auto band = timeline.workAreaRect();
        const auto moveFrom = band.center();
        const auto moveTo = moveFrom + QPointF(timeline.timeX(frameTime(35, time(24))) -
                                                   timeline.timeX(frameTime(30, time(24))),
                                               0);
        mouse(&timeline, QEvent::MouseButtonPress, moveFrom, Qt::LeftButton);
        mouse(&timeline, QEvent::MouseMove, moveTo, Qt::LeftButton);
        check(range(editor).firstFrame == 25 && range(editor).frameCount == 20,
              "center drag moves the interval by an integer frame delta without changing its count");
        key(&timeline, Qt::Key_Escape);
        check(range(editor).firstFrame == 20 && range(editor).frameCount == 20 && !editor.gesturing(),
              "Escape cancels a Work Area gesture and restores its snapshot");
        mouse(&timeline, QEvent::MouseButtonRelease, moveTo, Qt::NoButton);
        check(editor.undoStack().index() == moveUndo,
              "cancelled Work Area drag leaves the undo stack unchanged");
        check(timeChanged == 0 && timeline.visibleRange() == viewBeforeMove,
              "center dragging does not change the CTI or navigator interval");
        checkOnlyWorkAreaChanged(moveBefore, editor.project());

        const auto dragStart = timeline.workAreaRect().center();
        const auto dragEnd = dragStart + QPointF(timeline.timeX(frameTime(35, time(24))) -
                                                     timeline.timeX(frameTime(30, time(24))),
                                                 0);
        const int beforeCommit = editor.undoStack().index();
        drag(timeline, dragStart, dragEnd);
        check(range(editor).firstFrame == 25 && range(editor).frameCount == 20 &&
                  editor.undoStack().index() == beforeCommit + 1,
              "committed center drag moves the interval and adds exactly one undo command");

        editor.replace(sceneWithRange(20, 40));
        timeline.setComposition(1);
        timeline.setTime(frameTime(240, time(24)));
        const auto beforeZoomRange = range(editor);
        const auto navigator = timeline.navigatorRect();
        doubleClick(&timeline, navigator.center());
        check(timeline.visibleRange() != std::pair{motion::time(0), time(12)},
              "navigator zoom changes the visible interval independently");
        check(timeline.workAreaRect().isEmpty() && range(editor).firstFrame == beforeZoomRange.firstFrame &&
                  range(editor).frameCount == beforeZoomRange.frameCount,
              "an offscreen Work Area is clipped after navigator zoom without changing its range");
        const auto plotLeft = timeline.timeX(timeline.visibleRange().first);
        check(timeline.workAreaStartHandleCenter().x() < plotLeft &&
                  timeline.workAreaEndHandleCenter().x() < plotLeft,
              "offscreen Work Area handles remain outside the visible plot and cannot be hit");

        editor.replace(sceneWithRange(20, 40));
        timeline.setComposition(1);
        const auto resetUndo = editor.undoStack().index();
        doubleClick(&timeline, timeline.workAreaRect().center());
        check(!editor.project().compositions.front().workArea.has_value() &&
                  range(editor).firstFrame == 0 && range(editor).frameCount == 288,
              "double-clicking the Work Area track resets it to full composition");
        check(editor.undoStack().index() == resetUndo + 1,
              "full Work Area reset is a single undoable edit");
        check(navigationStarted >= 6 && timeChanged == 0,
              "Work Area interactions stop playback without scrubbing the CTI");

        editor.replace(sceneWithRange(20, 40));
        timeline.setComposition(1);
        timeline.setTime(frameTime(60, time(24)));
        const auto pixels = timeline.grab().toImage();
        const double scale = pixels.devicePixelRatio();
        const double trackY = timeline.workAreaRect().center().y();
        const auto inactive = pixels.pixelColor(int(timeline.timeX(frameTime(10, time(24))) * scale),
                                               int(trackY * scale));
        const auto selected = pixels.pixelColor(int(timeline.timeX(frameTime(25, time(24))) * scale),
                                               int(trackY * scale));
        check(inactive.lightness() < selected.lightness() && inactive.blue() < 100,
              "inactive Work Area stays dark rather than inheriting the playhead brush");

        std::cout << "PASS work_area_ui_checks " << checkCount << " checks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL work_area_ui_checks: " << e.what() << '\n';
        return 1;
    }
}
