// SPDX-License-Identifier: MPL-2.0
#include "main_window.hpp"
#include <QApplication>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <iostream>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    // Keep the QSettings identity so existing MotionProof layouts and preferences remain available.
    app.setOrganizationName("IndependentMotionProof");
    app.setApplicationName("MotionProof");
    app.setApplicationDisplayName("Before Effects");
    motion::applyApplicationTheme(app);
    QCommandLineParser args;
    args.addHelpOption();
    args.addOption({"author-proof", "Author original scene in a fresh directory", "directory"});
    args.addOption({"open", "Open native project", "path"});
    args.addOption({"export", "Export opened/authored scene to a new directory", "directory"});
    args.addOption({"export-movie", "Export H.264/AAC movie to a new directory", "directory"});
    args.addOption({"screenshot", "Save actual window after rendering", "path"});
    args.addOption({"quit-after-export", "Exit after export"});
    args.addOption({"quit-after-screenshot", "Exit after saving the requested screenshot"});
    args.addOption({"save-copy", "Migrate --open project to a new native project and exit", "path"});
    args.addOption(
        {"select-layer", "Select a layer of the opened composition and reveal its properties", "id"});
    args.addOption({"frame", "Inspect a frame of the opened composition", "index"});
    args.addOption({"graph", "Show the value or speed graph for the selected layer", "value|speed"});
    args.process(app);
    try {
        if (args.isSet("save-copy")) {
            const auto output = args.value("save-copy");
            if (!args.isSet("open") || QFileInfo::exists(output))
                throw std::runtime_error("--save-copy needs --open and a new output path");
            auto project = motion::loadProject(args.value("open"));
            const QDir source(motion::projectDirectory(args.value("open")));
            const QDir destination(motion::projectDirectory(output));
            for (auto &asset : project.assets)
                asset.path = destination.relativeFilePath(source.absoluteFilePath(asset.path));
            const auto bytes = motion::encodeProject(project);
            QFile copy(output);
            if (!copy.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                throw std::runtime_error("Cannot create a new project copy at the requested path");
            if (copy.write(bytes) != bytes.size() || !copy.flush()) {
                copy.remove();
                throw std::runtime_error("Project copy could not be written completely");
            }
            copy.close();
            return 0;
        }
        if (args.isSet("export") && args.isSet("export-movie"))
            throw std::runtime_error("Choose one export format");
        if (args.isSet("quit-after-screenshot") && !args.isSet("screenshot"))
            throw std::runtime_error("--quit-after-screenshot requires --screenshot");
        QString font =
            QDir(QCoreApplication::applicationDirPath()).filePath("../Resources/font/NotoSans-Regular.ttf");
        motion::registerFixtureFont(font);
        motion::MainWindow window;
        window.show();
        bool captured = false, initialized = false;
        QObject::connect(&window, &motion::MainWindow::frameDisplayed, &window, [&] {
            if (initialized && !captured && args.isSet("screenshot")) {
                captured = true;
                QTimer::singleShot(150, &window, [&] {
                    const bool saved = window.grab().save(args.value("screenshot"));
                    if (!saved)
                        std::cerr << "Screenshot could not be saved" << std::endl;
                    if (args.isSet("quit-after-screenshot"))
                        app.exit(saved ? 0 : 1);
                });
            }
        });
        QObject::connect(&window, &motion::MainWindow::outputFinished, &window, [&](motion::ExportResult r) {
            std::cout << r.state.toStdString() << " frames=" << r.completed << " " << r.error.toStdString()
                      << std::endl;
            if (args.isSet("quit-after-export"))
                QTimer::singleShot(250, &app, [&, r] { app.exit(r.state == "complete" ? 0 : 1); });
        });
        QTimer::singleShot(0, &window, [&] {
            try {
                if (args.isSet("author-proof"))
                    window.authorProof(args.value("author-proof"));
                if (args.isSet("open") && !window.openProject(args.value("open")))
                    throw std::runtime_error(window.lastError().toStdString());
                const auto &composition = window.editor().project().compositions.front();
                if (args.isSet("select-layer")) {
                    bool ok;
                    auto id = args.value("select-layer").toULongLong(&ok);
                    if (!ok || std::none_of(composition.layers.begin(), composition.layers.end(),
                                            [id](const motion::Layer &layer) { return layer.id == id; }))
                        throw std::runtime_error(
                            "--select-layer requires a layer id from the opened composition");
                    window.editor().select({id});
                    window.timeline()->setFilter("");
                }
                if (args.isSet("frame")) {
                    bool ok;
                    auto frame = args.value("frame").toLongLong(&ok);
                    if (!ok || frame < 0 ||
                        frame >= motion::frameCount(composition.duration, composition.fps))
                        throw std::runtime_error("--frame must be within the opened composition");
                    window.setTime(motion::frameTime(frame, composition.fps));
                }
                if (args.isSet("graph")) {
                    const auto mode = args.value("graph");
                    if (mode != "value" && mode != "speed")
                        throw std::runtime_error("--graph must be value or speed");
                    window.findChild<QToolButton *>("graphToggle")->setChecked(true);
                    window.findChild<QComboBox *>("graphType")->setCurrentIndex(mode == "speed" ? 1 : 0);
                }
                initialized = true;
                if (args.isSet("export"))
                    QTimer::singleShot(700, &window, [&] { window.exportTo(args.value("export")); });
                if (args.isSet("export-movie"))
                    QTimer::singleShot(700, &window,
                                       [&] { window.exportTo(args.value("export-movie"), false, true); });
            } catch (const std::exception &e) {
                std::cerr << e.what() << std::endl;
                app.exit(1);
            }
        });
        return app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
