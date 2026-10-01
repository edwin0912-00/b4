// SPDX-License-Identifier: MPL-2.0
#include "main_window.hpp"
#include "media.hpp"
#include "support.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTabBar>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeWidget>
#include <iostream>
using namespace motion;
static void events(int ms = 20) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
}
template <class F> static bool until(F f, int ms = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!f() && timer.elapsed() < ms)
        events(5);
    return f();
}
static void click(QWidget *w, QPointF point) {
    for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, point, w->mapToGlobal(point.toPoint()), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(w, &event);
    }
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    applyApplicationTheme(app);
    app.setOrganizationName("MotionMediaTests");
    app.setApplicationName("MotionMediaTests");
    try {
        QTemporaryDir settings;
        check(settings.isValid(), "temporary UI settings directory exists");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        registerFixtureFont(QString(MOTION_SOURCE_DIR) + "/fixtures/font/NotoSans-Regular.ttf");
        MainWindow window;
        window.show();
        window.activateWindow();
        events();
        window.importMediaFile(QString(MOTION_SOURCE_DIR) + "/fixtures/media/source.mp4");
        for (auto *button : window.findChildren<QPushButton *>())
            if (button->text() == "Cancel import")
                button->click();
        events(150);
        check(window.editor().project().assets.empty(), "cancelled import cannot commit a late result");
        window.importMediaFile(QString(MOTION_SOURCE_DIR) + "/fixtures/media/source.mp4");
        window.editor().replace(Project{});
        events(150);
        check(window.editor().project().assets.empty(), "replaced document cannot receive an old import");
        Id imported = 0;
        QObject::connect(&window, &MainWindow::mediaImported, &window, [&](Id id) { imported = id; });
        window.importMediaFile(QString(MOTION_SOURCE_DIR) + "/fixtures/media/source.mp4");
        check(until([&] { return imported != 0; }),
              ("async import: " + window.lastError()).toUtf8().constData());
        check(window.editor().project().compositions.front().layers.empty(),
              "File Import adds footage to Project without inventing a layer");
        auto *tree = window.findChild<QTreeWidget *>("projectTree");
        QTreeWidgetItem *media = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->data(0, Qt::UserRole + 1) == "asset")
                media = tree->topLevelItem(i);
        check(media && media->text(1) == "Video", "Project labels actual video media");
        bool reached = false;
        QTimer::singleShot(0, &window, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (auto *a : menu->actions())
                if (a->text() == "New composition from footage") {
                    reached = true;
                    menu->setActiveAction(a);
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(menu, &enter);
                    return;
                }
            menu->close();
        });
        QMetaObject::invokeMethod(tree, "customContextMenuRequested",
                                  Q_ARG(QPoint, tree->visualItemRect(media).center()));
        check(reached && window.editor().project().compositions.front().layers.size() == 1,
              "Project context menu creates a working composition from footage");
        auto compId = window.currentComposition();
        const auto &c = composition(window.editor().project(), compId);
        Id video = c.layers.front().id;
        check(c.width == 640 && c.height == 360 && c.fps == time(30000, 1001) &&
                  c.duration == time(1001, 250),
              "composition adopts source geometry and exact timing");
        check(until([&] { return !window.displayedImage().isNull(); }),
              "video appears in Composition viewer");
        const auto documentBeforeSourceView = encodeProject(window.editor().project());
        const auto compTime = window.currentTime();
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->data(0, Qt::UserRole + 1) == "asset")
                media = tree->topLevelItem(i);
        QMetaObject::invokeMethod(tree, "itemActivated", Q_ARG(QTreeWidgetItem *, media), Q_ARG(int, 0));
        events(100);
        auto before = window.displayedImage();
        auto *slider = window.findChild<QSlider *>("sourceTimeSlider");
        slider->setValue(60);
        QMetaObject::invokeMethod(slider, "sliderMoved", Q_ARG(int, 60));
        check(until([&] { return window.displayedImage() != before; }),
              "Footage slider seeks an actual changing video frame");
        check(window.currentTime() == compTime &&
                  encodeProject(window.editor().project()) == documentBeforeSourceView,
              "Footage seeking preserves composition time and data");
        window.findChild<QTabBar *>("sourceViewTabs")->setCurrentIndex(1);
        events();
        auto *timeline = window.timeline();
        QMimeData data;
        data.setData("application/x-motionproof-item",
                     QJsonDocument(QJsonObject{{"id", QString::number(imported)},
                                               {"kind", "asset"},
                                               {"token", tree->property("dragToken").toString()}})
                         .toJson());
        QPoint position(int(timeline->timeX(time(1))), 50);
        QDragEnterEvent enter(position, Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(timeline, &enter);
        QDropEvent drop(position, Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(timeline, &drop);
        check(composition(window.editor().project(), compId).layers.size() == 2,
              "Project media drag adds a real layer to Timeline");
        Id pip = window.editor().selection().front();
        check(near(seconds(layer(window.editor().project(), pip).start), 1, 1. / 29),
              "drop time sets the media source offset");
        timeline->setFilter("P");
        events();
        click(timeline, {28, timeline->propertyY(pip, Property::PositionX) - 17});
        check(!layer(window.editor().project(), pip).audioEnabled &&
                  layer(window.editor().project(), pip).visible,
              "speaker switch mutes independently of the eye");
        window.editor().select({video});
        auto *properties = window.findChild<QDockWidget *>("propertiesDock");
        properties->show();
        properties->raise();
        events();
        auto *level = window.findChild<QDoubleSpinBox *>("audio.levels.0");
        check(level != nullptr, "Audio Levels use the real parameter controls");
        level->setValue(-6);
        QMetaObject::invokeMethod(level, "editingFinished");
        check(layer(window.editor().project(), video).base("audio.levels", 0) == -6,
              "stereo level UI updates the audio mix model");
        QPushButton *play = nullptr;
        for (auto *button : window.findChildren<QPushButton *>())
            if (button->text() == "Play")
                play = button;
        check(play != nullptr, "transport exists");
        window.setTime(motion::time(0));
        play->click();
        check(until([&] { return window.isPlaying(); }), "audio preview preparation starts real playback");
        events(180);
        check(seconds(window.currentTime()) > 0, "audio device clock advances the timeline");
        window.editor().apply("Change live level",
                              [&](Project &p) { layer(p, video).base("audio.levels", 0) = -3; });
        events(30);
        check(window.isPlaying(), "gain edits update the live audio snapshot without stopping playback");
        play->click();
        check(!window.isPlaying(), "pause stops audio playback");
        auto *previewSpan = window.findChild<QComboBox *>("previewTimeSpan");
        auto *renderSpan = window.findChild<QComboBox *>("renderTimeSpan");
        check(previewSpan && renderSpan && !previewSpan->currentData().toBool() &&
                  !renderSpan->currentData().toBool(),
              "preview and output default independently to full composition");
        const auto rate = composition(window.editor().project(), compId).fps;
        const auto navigatorBeforeWorkArea = timeline->visibleRange();
        window.findChild<QDockWidget *>("projectDock")->raise();
        window.activateWindow();
        events();
        tree->setFocus();
        events();
        check(QApplication::focusWidget() == tree, "Project tree owns keyboard focus before B/N");
        window.setTime(frameTime(30, rate));
        QKeyEvent setStart(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, "b");
        QApplication::sendEvent(tree, &setStart);
        window.setTime(frameTime(34, rate));
        QKeyEvent setEnd(QEvent::KeyPress, Qt::Key_N, Qt::NoModifier, "n");
        QApplication::sendEvent(tree, &setEnd);
        const auto shortcutRange = compositionFrameRange(composition(window.editor().project(), compId), true);
        check(shortcutRange.firstFrame == 30 && shortcutRange.frameCount == 5,
              "MainWindow B/N shortcuts include the CTI frame");
        check(window.currentTime() == frameTime(34, rate), "MainWindow B/N preserve CTI");
        check(timeline->visibleRange() == navigatorBeforeWorkArea, "MainWindow B/N preserve navigator");
        auto *search = window.findChild<QLineEdit *>("timelineSearch");
        check(search != nullptr, "Timeline search editor exists");
        const auto oldSearch = search->text();
        const auto beforeTyping = encodeProject(window.editor().project());
        search->clear();
        search->setFocus();
        QKeyEvent typedB(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, "b");
        QApplication::sendEvent(search, &typedB);
        check(search->text() == "b" && encodeProject(window.editor().project()) == beforeTyping,
              "typing B inside a text editor does not edit Work Area");
        search->setText(oldSearch);
        timeline->setFocus();
        previewSpan->setCurrentIndex(1);
        for (const auto [cti, expected] : {std::pair{32, 32}, {20, 30}, {35, 30}}) {
            window.setTime(frameTime(cti, rate));
            play->click();
            check(until([&] { return window.isPlaying(); }), "Work Area audio preview starts");
            check(window.currentTime() == frameTime(expected, rate),
                  "Work Area starts exactly at inside CTI or range start when outside");
            events(210);
            const auto at = window.currentTime();
            check(!(at < frameTime(30, rate)) && at < frameTime(35, rate),
                  "Work Area preview wraps within its nonzero exclusive interval");
            play->click();
        }
        window.setTime(frameTime(20, rate));
        play->click();
        window.editor().apply("Change range during preview preparation", [&](Project &p) {
            setWorkAreaFrames(composition(p, compId), 0, 5);
        });
        check(until([&] { return window.isPlaying(); }), "changed preview range starts fresh preparation");
        check(window.currentTime() == motion::time(0), "stale preparation cannot start the prior Work Area");
        window.editor().apply("Change active Work Area", [&](Project &p) {
            setWorkAreaFrames(composition(p, compId), 10, 15);
        });
        check(until([&] { return window.isPlaying(); }), "active range edit recreates playback");
        check(!(window.currentTime() < frameTime(10, rate)) && window.currentTime() < frameTime(15, rate),
              "recreated playback uses the new audio/video interval");
        play->click();
        previewSpan->setCurrentIndex(0);
        window.editor().apply("Reset preview work area", [&](Project &p) {
            resetWorkArea(composition(p, compId));
        });
        window.editor().apply("Compose picture in picture", [&](Project &p) {
            auto &l = layer(p, pip);
            l.base("transform.scale", 0) = l.base("transform.scale", 1) = .4;
            l.base("transform.position", 0) = 500;
            l.base("transform.position", 1) = 260;
        });
        for (auto *a : window.findChildren<QAction *>())
            if (a->text() == "Add Text") {
                a->trigger();
                break;
            }
        auto selected = window.editor().selection();
        check(!selected.empty() && layer(window.editor().project(), selected.front()).kind == LayerKind::Text,
              "existing Text tool composes over video");
        Id textLayer = selected.front();
        window.editor().apply("Caption over video", [&](Project &p) {
            auto &l = layer(p, textLayer);
            l.string("text.source") = "VIDEO + AUDIO";
            l.base("text.size") = 38;
            l.base("transform.position", 0) = 24;
            l.base("transform.position", 1) = 24;
            l.channel(Property::Opacity).keys = {{motion::time(0), 0}, {time(1, 2), 1}};
        });
        QTemporaryDir temporary;
        QString example = qEnvironmentVariableIsSet("MOTION_QA_MEDIA_PROJECT")
                              ? qEnvironmentVariable("MOTION_QA_MEDIA_PROJECT")
                              : temporary.filePath("Media-project.json");
        check(window.saveAs(example), "editable media composition saves with source references");
        check(window.openProject(example), "media project reopens without flattening");
        window.editor().select({video});
        timeline->setFilter("L");
        window.setTime(time(3, 2));
        events(500);
        if (qEnvironmentVariableIsSet("MOTION_QA_MEDIA_PROJECT")) {
            window.resize(1506, 989);
            events();
            for (auto *a : window.findChildren<QAction *>())
                if (a->text() == "Reset workspace") {
                    a->trigger();
                    break;
                }
            events(100);
            check(window.grab().save(example + ".png"), "real media workspace capture");
        }
        bool finished = false;
        ExportResult output;
        QObject::connect(&window, &MainWindow::outputFinished, &window, [&](ExportResult r) {
            output = r;
            finished = true;
        });
        window.editor().apply("Custom range with full-composition output", [&](Project &p) {
            setWorkAreaFrames(composition(p, compId), 15, 20);
        });
        check(!renderSpan->currentData().toBool(), "full-composition output remains the default");
        window.exportTo(temporary.filePath("movie-ui"), false, true);
        check(until([&] { return finished; }, 120000) && output.state == "complete" && output.completed == 120,
              ("movie UI export: " + output.error).toUtf8().constData());
        if (qEnvironmentVariableIsSet("MOTION_QA_MEDIA_PROJECT"))
            check(QFile::copy(QDir(output.directory).filePath("Movie.mp4"), example + ".mp4"),
                  "actual edited movie delivered");
        window.editor().apply("Set export work area", [&](Project &p) {
            setWorkAreaFrames(composition(p, compId), 15, 20);
        });
        renderSpan->setCurrentIndex(1);
        window.setTime(frameTime(40, rate));
        const Project queuedProject = window.editor().project();
        finished = false;
        const auto rangeOutput = temporary.filePath("work-area-ui");
        window.exportTo(rangeOutput);
        window.editor().apply("Change range after submission", [&](Project &p) {
            setWorkAreaFrames(composition(p, compId), 30, 35);
        });
        check(until([&] { return finished; }, 120000) && output.state == "complete" && output.completed == 5,
              "queued Work Area exports the submitted five frames");
        const auto rangeFiles = QDir(rangeOutput).entryList({"frame_*.png"}, QDir::Files, QDir::Name);
        check(rangeFiles.size() == 5 && rangeFiles.front() == "frame_000015.png" &&
                  rangeFiles.back() == "frame_000019.png",
              "later Work Area edits cannot change the queued start/end");
        Assets expectedAssets;
        expectedAssets.baseDirectory = projectDirectory(example);
        const auto &queuedComp = composition(queuedProject, compId);
        for (int i = 0; i < rangeFiles.size(); ++i) {
            const auto expectedFrame = pngImage(render(queuedProject, compId, frameTime(15 + i, rate),
                                                       QSize(queuedComp.width, queuedComp.height), expectedAssets));
            check(QImage(QDir(rangeOutput).filePath(rangeFiles[i])).convertToFormat(QImage::Format_RGBA8888) ==
                      expectedFrame.convertToFormat(QImage::Format_RGBA8888),
                  "each queued range output matches its requested source frame");
        }
        finished = false;
        const auto stillOutput = temporary.filePath("still-outside-work-area");
        window.exportTo(stillOutput, true);
        check(until([&] { return finished; }, 120000) && output.state == "complete" && output.completed == 1,
              "still export completes outside Work Area");
        check(QDir(stillOutput).entryList({"frame_*.png"}, QDir::Files) == QStringList{"frame_000040.png"},
              "still export uses CTI rather than Work Area start");
        std::cout << checkCount << " media UI checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
