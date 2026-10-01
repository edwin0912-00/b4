// SPDX-License-Identifier: MPL-2.0
#include "project_io.hpp"
#include "render_worker.hpp"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sys/resource.h>
using namespace motion;
static QJsonObject stats(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    auto n = values.size();
    double median = n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
    return {{"samples", int(n)}, {"median_ms", median}, {"p95_ms", values[size_t(std::ceil(n * .95)) - 1]}};
}
int main(int argc, char **argv) {
    const auto inputPath =
        argc > 1 ? QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath() : QString{};
    QGuiApplication app(argc, argv);
    if (argc != 2) {
        std::cerr << "usage: motion_benchmark project.json\n";
        return 1;
    }
    try {
        registerFixtureFont(QString(MOTION_SOURCE_DIR) + "/fixtures/font/NotoSans-Regular.ttf");
        const auto project = loadProject(inputPath);
        const auto base = QFileInfo(inputPath).absolutePath();
        RenderWorker worker;
        int index = 0;
        std::vector<double> cold, warm, full, coldRoundtrip, warmRoundtrip, fullRoundtrip;
        QElapsedTimer timer;
        std::function<void()> next;
        next = [&] {
            const auto &c = project.compositions.front();
            int frame = index < 60 ? index % 30 : (index - 60) * 30;
            bool high = index >= 60;
            PreviewRequest request{std::uint64_t(index + 1),
                                   1,
                                   project,
                                   c.id,
                                   frameTime(frame, c.fps),
                                   QSize(c.width / (high ? 1 : 2), c.height / (high ? 1 : 2)),
                                   base};
            timer.start();
            worker.preview(request);
        };
        QObject::connect(&worker, &RenderWorker::previewReady, &app, [&](PreviewResult result) {
            if (!result.error.isEmpty()) {
                std::cerr << result.error.toStdString() << '\n';
                app.exit(1);
                return;
            }
            auto &render = index < 30 ? cold : (index < 60 ? warm : full);
            auto &delivery = index < 30 ? coldRoundtrip : (index < 60 ? warmRoundtrip : fullRoundtrip);
            render.push_back(result.milliseconds);
            delivery.push_back(timer.nsecsElapsed() / 1e6);
            if (++index < 70) {
                next();
                return;
            }
            rusage usage{};
            getrusage(RUSAGE_SELF, &usage);
            QJsonObject output{{"cold_half_render", stats(cold)},
                               {"warm_half_render", stats(warm)},
                               {"full_render", stats(full)},
                               {"cold_half_request_to_ui_thread", stats(coldRoundtrip)},
                               {"warm_half_request_to_ui_thread", stats(warmRoundtrip)},
                               {"full_request_to_ui_thread", stats(fullRoundtrip)},
                               {"peak_rss_bytes", double(usage.ru_maxrss)},
                               {"scope", "30 half-resolution frames cold then cached; 10 full-resolution "
                                         "frames; OS display scanout and QWidget painting excluded"}};
            std::cout << QJsonDocument(output).toJson().toStdString();
            app.quit();
        });
        QTimer::singleShot(0, &app, next);
        QTimer::singleShot(120000, &app, [&] {
            std::cerr << "Benchmark timeout\n";
            app.exit(1);
        });
        return app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
