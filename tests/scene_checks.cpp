// SPDX-License-Identifier: MPL-2.0
#include "project_io.hpp"
#include "render.hpp"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <iostream>
using namespace motion;
int main(int argc, char **argv) {
    QStringList inputs;
    for (int i = 1; i < argc; ++i)
        inputs.append(QFileInfo(QString::fromLocal8Bit(argv[i])).absoluteFilePath());
    QApplication app(argc, argv);
    if (inputs.size() != 3) {
        std::cerr << "usage: motion_scene_checks project.json PNG-directory font.ttf\n";
        return 1;
    }
    try {
        registerFixtureFont(inputs[2]);
        const QString path = inputs[0];
        const auto project = loadProject(path);
        const auto &comp = project.compositions.front();
        const auto count = frameCount(comp.duration, comp.fps);
        QDir directory(inputs[1]);
        if (directory.entryList({"frame_*.png"}, QDir::Files).size() != count)
            throw std::runtime_error("Expected frame count does not match the reopened composition");
        Assets assets;
        assets.baseDirectory = QFileInfo(path).absolutePath();
        preflightSources(project, assets);
        for (std::int64_t i = 0; i < count; ++i) {
            const auto expectedPath = directory.filePath(QString("frame_%1.png").arg(i, 6, 10, QChar('0')));
            const auto expected = QImage(expectedPath).convertToFormat(QImage::Format_RGBA8888);
            const auto actual = pngImage(
                render(project, comp.id, frameTime(i, comp.fps), QSize(comp.width, comp.height), assets));
            if (expected.isNull() || actual != expected)
                throw std::runtime_error(
                    ("Reopened scene differs at frame " + QString::number(i)).toStdString());
        }
        std::cout << count << " reopened-scene frames match exported decoded pixels exactly\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
