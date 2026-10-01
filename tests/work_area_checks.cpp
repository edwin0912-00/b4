// SPDX-License-Identifier: MPL-2.0
#include "editor.hpp"
#include "project_io.hpp"
#include "support.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <limits>
using namespace motion;

namespace {
QJsonArray encodedTime(Time value) {
    return {QString::number(value.numerator), QString::number(value.denominator)};
}
QByteArray jsonBytes(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject projectObject(const Project &project) { return QJsonDocument::fromJson(encodeProject(project)).object(); }
QJsonObject setSerializedWorkArea(QJsonObject document, int compositionIndex, QJsonValue value) {
    auto compositions = document["compositions"].toArray();
    auto item = compositions[compositionIndex].toObject();
    item["workArea"] = value;
    compositions[compositionIndex] = item;
    document["compositions"] = compositions;
    return document;
}
QByteArray fixture(const QString &path) {
    QFile file(QString(MOTION_SOURCE_DIR) + "/" + path);
    check(file.open(QIODevice::ReadOnly), "legacy fixture available");
    return file.readAll();
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        Composition c;
        c.fps = time(30);
        c.duration = time(12);
        setWorkAreaStart(c, time(1));
        setWorkAreaEnd(c, time(17, 15));
        check(c.workArea == WorkArea{time(1), time(7, 6)}, "B/N select the intended containing and following frames");
        const auto fiveFrames = compositionFrameRange(c, true);
        check(fiveFrames.firstFrame == 30 && fiveFrames.frameCount == 5,
              "Work Area projects to its exact half-open output frame range");
        const auto fullFrames = compositionFrameRange(c, false);
        check(fullFrames.firstFrame == 0 && fullFrames.frameCount == 360,
              "full composition range retains the 360-frame oracle");

        Composition crossed;
        crossed.fps = time(30);
        crossed.duration = time(12);
        setWorkAreaEnd(crossed, time(1, 3));
        setWorkAreaStart(crossed, time(2, 5));
        const auto afterCross = compositionFrameRange(crossed, true);
        check(afterCross.firstFrame == 12 && afterCross.frameCount == 1,
              "B after N moves the exclusive end to retain the B frame");
        Composition edge;
        edge.fps = time(30);
        edge.duration = time(13, 120);
        setWorkAreaEnd(edge, time(1, 15));
        setWorkAreaStart(edge, time(100));
        check(compositionFrameRange(edge, true).firstFrame == 3 &&
                  compositionFrameRange(edge, true).frameCount == 1,
              "B at or beyond composition end selects the last legal frame");
        setWorkAreaEnd(edge, time(-1));
        check(compositionFrameRange(edge, true).firstFrame == 0 &&
                  compositionFrameRange(edge, true).frameCount == 1,
              "N before composition start clamps to the first legal frame");

        Composition moved;
        moved.fps = time(30);
        moved.duration = time(12);
        setWorkAreaFrames(moved, 10, 15);
        moveWorkArea(moved, std::numeric_limits<std::int64_t>::max());
        check(compositionFrameRange(moved, true).firstFrame == 355 &&
                  compositionFrameRange(moved, true).frameCount == 5,
              "large positive move clamps while preserving frame count");
        moveWorkArea(moved, std::numeric_limits<std::int64_t>::min());
        check(compositionFrameRange(moved, true).firstFrame == 0 &&
                  compositionFrameRange(moved, true).frameCount == 5,
              "large negative move clamps while preserving frame count");
        setWorkAreaFrames(moved, -100, 1000);
        check(!moved.workArea && compositionFrameRange(moved, true).frameCount == 360,
              "clamped full range uses the canonical null representation");
        moveWorkArea(moved, 50);
        check(!moved.workArea && compositionFrameRange(moved, true).firstFrame == 0,
              "moving a full-composition interval leaves it full");
        setWorkAreaFrames(moved, 5, 8);
        resetWorkArea(moved);
        check(!moved.workArea && compositionFrameRange(moved, true).frameCount == 360,
              "reset restores the full-composition interval");

        check(frameIndexFloor(time(15, 100), time(10)) == 1 &&
                  frameIndexNearest(time(15, 100), time(10)) == 2,
              "floor and nearest remain distinct at an exact half-frame");
        check(frameIndexFloor(time(-1, 20), time(10)) == -1 &&
                  frameIndexNearest(time(-1, 20), time(10)) == 0,
              "signed floor and ties-later nearest are exact for negative times");
        check(frameCount(time(1001, 10000), time(30000, 1001)) == 3,
              "fractional composition rate uses exact ceiling frame count");

        Composition shortComp;
        shortComp.fps = time(30);
        shortComp.duration = time(1, 100);
        check(compositionFrameRange(shortComp, true).firstFrame == 0 &&
                  compositionFrameRange(shortComp, true).frameCount == 1,
              "a duration shorter than one frame still exports one selectable frame");
        setWorkAreaFrames(shortComp, 0, 1);
        check(!shortComp.workArea, "the sub-frame full range remains canonical null");

        Composition fractional;
        fractional.fps = time(30);
        fractional.duration = time(13, 120);
        check(frameCount(fractional.duration, fractional.fps) == 4 &&
                  compositionBoundaryIndex(fractional, time(101, 1000)) == 3 &&
                  compositionBoundaryIndex(fractional, time(5, 48)) == 4 &&
                  compositionBoundaryIndex(fractional, fractional.duration) == 4,
              "fractional terminal boundary wins after the exact tail midpoint, ties later");
        setWorkAreaFrames(fractional, 3, 4);
        check(fractional.workArea == WorkArea{time(1, 10), time(13, 120)} &&
                  compositionFrameRange(fractional, true).firstFrame == 3 &&
                  compositionFrameRange(fractional, true).frameCount == 1,
              "terminal partial frame remains selectable without extending beyond duration");
        setCompositionTiming(fractional, time(24), time(13, 120));
        check(fractional.workArea && fractional.workArea->end == time(13, 120),
              "FPS-only change preserves an end that is the exact composition duration");

        Composition timing;
        timing.fps = time(30);
        timing.duration = time(13, 120);
        setWorkAreaFrames(timing, 2, 4);
        setCompositionTiming(timing, time(30), time(1));
        check(timing.workArea && timing.workArea->end == time(1, 10),
              "a former fractional composition edge snaps when it becomes interior");
        Composition collapse;
        collapse.fps = time(30);
        collapse.duration = time(12);
        setWorkAreaFrames(collapse, 1, 2);
        setCompositionTiming(collapse, time(1), time(12));
        check(compositionFrameRange(collapse, true).firstFrame == 0 &&
                  compositionFrameRange(collapse, true).frameCount == 1,
              "retiming collapsed boundaries retains a nonempty interval");
        Composition fullTiming;
        fullTiming.workArea = WorkArea{time(0, 7), fullTiming.duration};
        setCompositionTiming(fullTiming, time(24), time(20));
        check(!fullTiming.workArea && fullTiming.duration == time(20),
              "an explicit in-memory full pair follows duration changes and canonicalizes to null");
        Composition nullFullTiming;
        setCompositionTiming(nullFullTiming, time(24), time(20));
        check(!nullFullTiming.workArea && compositionFrameRange(nullFullTiming, true).frameCount == 480,
              "the canonical full range follows duration changes");
        Composition retimedTie;
        retimedTie.fps = time(20);
        retimedTie.duration = time(1);
        setWorkAreaFrames(retimedTie, 3, 5);
        setCompositionTiming(retimedTie, time(10), time(1));
        check(compositionFrameRange(retimedTie, true).firstFrame == 2 &&
                  compositionFrameRange(retimedTie, true).frameCount == 1,
              "retimed start and end ties choose later regular boundaries and remain nonempty");
        Composition shortened;
        shortened.fps = time(30);
        shortened.duration = time(12);
        setWorkAreaFrames(shortened, 300, 305);
        setCompositionTiming(shortened, time(30), time(13, 120));
        check(compositionFrameRange(shortened, true).firstFrame == 3 &&
                  compositionFrameRange(shortened, true).frameCount == 1 &&
                  shortened.workArea->end == time(13, 120),
              "shortening clamps a custom range to the fractional terminal frame");

        auto project = parentScene();
        auto &savedComp = project.compositions.front();
        savedComp.fps = time(30);
        savedComp.duration = time(13, 120);
        setWorkAreaFrames(savedComp, 2, 4);
        const auto savedRange = *savedComp.workArea;
        const auto encoded = encodeProject(project);
        const auto reopened = decodeProject(encoded);
        check(reopened.schemaVersion == 5 && composition(reopened, savedComp.id).workArea == savedRange &&
                  encodeProject(reopened) == encoded,
              "schema5 persists and reopens exact custom rational boundaries");

        auto explicitFull = parentScene();
        explicitFull.compositions.front().workArea = WorkArea{time(0, 9), explicitFull.compositions.front().duration};
        const auto fullDocument = projectObject(explicitFull);
        check(fullDocument["compositions"].toArray()[0].toObject()["workArea"].isNull() &&
                  !composition(decodeProject(jsonBytes(fullDocument)), 1).workArea,
              "programmatic explicit full intervals serialize only as null");
        auto fullPairDocument = setSerializedWorkArea(
            fullDocument, 0,
            QJsonObject{{"start", QJsonArray{QStringLiteral("0"), QStringLiteral("5")}},
                        {"end", QJsonArray{QStringLiteral("24"), QStringLiteral("2")}}});
        check(!composition(decodeProject(jsonBytes(fullPairDocument)), 1).workArea,
              "an equivalent unreduced full pair decodes to null before validation");

        for (const auto &path : {QStringLiteral("fixtures/original-scene.json"),
                                 QStringLiteral("fixtures/legacy/curve-schema2.json"),
                                 QStringLiteral("fixtures/legacy/media-schema3.json")}) {
            auto migrated = decodeProject(fixture(path));
            check(migrated.schemaVersion == 5 &&
                      std::all_of(migrated.compositions.begin(), migrated.compositions.end(),
                                  [](const Composition &item) { return !item.workArea; }),
                  "schemas1–3 migrate to a full-composition Work Area");
        }
        auto legacy4 = projectObject(parentScene());
        legacy4["schemaVersion"] = 4;
        auto legacy4Comps = legacy4["compositions"].toArray();
        for (int i = 0; i < legacy4Comps.size(); ++i) {
            auto item = legacy4Comps[i].toObject();
            item.remove("workArea");
            legacy4Comps[i] = item;
        }
        legacy4["compositions"] = legacy4Comps;
        auto migrated4 = decodeProject(jsonBytes(legacy4));
        check(migrated4.schemaVersion == 5 && !migrated4.compositions.front().workArea,
              "schema4 migrates to a full-composition Work Area");

        auto invalid = setSerializedWorkArea(projectObject(parentScene()), 0,
                                             QJsonObject{{"start", encodedTime(time(1, 100))},
                                                         {"end", encodedTime(time(1, 10))}});
        check(rejects([&] { decodeProject(jsonBytes(invalid)); }), "unaligned custom Work Area start is rejected");
        invalid = setSerializedWorkArea(projectObject(parentScene()), 0,
                                        QJsonObject{{"start", encodedTime(motion::time(0))},
                                                    {"end", encodedTime(time(1, 100))}});
        check(rejects([&] { decodeProject(jsonBytes(invalid)); }), "unaligned interior Work Area end is rejected");
        invalid = setSerializedWorkArea(projectObject(parentScene()), 0,
                                        QJsonObject{{"start", encodedTime(time(1))},
                                                    {"end", encodedTime(time(13))}});
        check(rejects([&] { decodeProject(jsonBytes(invalid)); }), "Work Area beyond duration is rejected");
        invalid = setSerializedWorkArea(projectObject(parentScene()), 0,
                                        QJsonObject{{"start", encodedTime(motion::time(0))},
                                                    {"end", encodedTime(time(1))},
                                                    {"unexpected", true}});
        check(rejects([&] { decodeProject(jsonBytes(invalid)); }), "unknown Work Area fields are rejected");
        for (const auto &range : {WorkArea{time(-1, 30), time(1)},
                                  WorkArea{time(2), time(1)},
                                  WorkArea{time(1), time(1)}}) {
            invalid = setSerializedWorkArea(projectObject(parentScene()), 0,
                                            QJsonObject{{"start", encodedTime(range.start)},
                                                        {"end", encodedTime(range.end)}});
            check(rejects([&] { decodeProject(jsonBytes(invalid)); }),
                  "negative, reversed and empty serialized Work Areas are rejected");
        }

        auto nested = solidScene();
        setWorkAreaFrames(nested.compositions.front(), 30, 35);
        const auto outerRange = nested.compositions.front().workArea;
        precompose(nested, 1, {2});
        check(!nested.compositions.back().workArea && nested.compositions.front().workArea == outerRange,
              "precompose starts full while retaining the outer custom Work Area");

        auto editProject = parentScene();
        Editor editor(editProject);
        const auto before = encodeProject(editor.project());
        editor.beginGesture();
        editor.previewGesture([](Project &document) {
            setWorkAreaFrames(document.compositions.front(), 30, 35);
        });
        editor.cancelGesture();
        check(encodeProject(editor.project()) == before,
              "cancelled Work Area gesture restores the document snapshot");
        editor.apply("Set Work Area", [](Project &document) {
            setWorkAreaFrames(document.compositions.front(), 30, 35);
        });
        const auto edited = encodeProject(editor.project());
        check(editor.project().compositions.front().workArea == WorkArea{time(1), time(7, 6)} &&
                  editor.undoStack().count() == 1,
              "committed Work Area edit creates one undo command");
        editor.undoStack().undo();
        check(encodeProject(editor.project()) == before, "Work Area edit undoes exactly");
        editor.undoStack().redo();
        check(encodeProject(editor.project()) == edited, "Work Area edit redoes exactly");

        std::cout << "WA01 model and schema checks passed: " << checkCount << " assertions\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "WA01 model check failed: " << e.what() << "\n";
        return 1;
    }
}
