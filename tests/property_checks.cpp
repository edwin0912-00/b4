// SPDX-License-Identifier: MPL-2.0
#include "editor.hpp"
#include "evaluate.hpp"
#include "support.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <limits>
using namespace motion;
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        auto p = parentScene();
        auto &l = layer(p, 3);
        l.channel(Property::PositionX).keys = {{time(1, 7), 12, Interpolation::Cubic, {0, 0}, {.1, 3}},
                                               {time(7, 3), 24, Interpolation::Linear, {-.2, -2}, {0, 0}}};
        l.channel(Property::PositionY).keys = {{time(2, 9), 43, Interpolation::Hold}, {time(11, 4), 65}};
        const auto encoded = encodeProject(p);
        const auto reopened = decodeProject(encoded);
        check(encoded == encodeProject(reopened), "typed curves serialize exactly");
        check(layer(reopened, 3).channel(Property::PositionX).keys[0].at == time(1, 7) &&
                  layer(reopened, 3).channel(Property::PositionY).keys[0].at == time(2, 9),
              "vector grouping retains independent rational component times");
        check(layer(reopened, 3).channel(Property::PositionX).keys[1].inHandle.dv == -2,
              "vector grouping preserves handles");
        check(toDisplay(Property::ScaleX, 1) == 100 && fromDisplay(Property::Opacity, 50) == .5,
              "percent conversion preserves ratio storage");
        check(scaleTime(time(3, 8), 7, 3) == time(7, 8) && scaleTime(time(-1, 6), 3) == time(-1, 2),
              "time scaling preserves exact rational values and signs");
        check(rejects([] { scaleTime(time(1), 1, 0); }), "time scaling rejects a zero denominator");
        check(rejects([] { scaleTime(time(std::numeric_limits<std::int64_t>::max()), 2); }),
              "time scaling rejects results outside the rational range");
        check(scaleTime(time(std::numeric_limits<std::int64_t>::max()), 1) ==
                      time(std::numeric_limits<std::int64_t>::max()) &&
                  scaleTime(time(std::numeric_limits<std::int64_t>::min()), 1) ==
                      time(std::numeric_limits<std::int64_t>::min()) &&
                  rejects([] { scaleTime(time(1, std::numeric_limits<std::int64_t>::max()), 1, 2); }),
              "time scaling accepts int64 boundaries and rejects denominator overflow");
        check(scaleTime(time(1, 30), -1) == time(-1, 30), "time scaling preserves negative shutter offsets");
        const auto &effectCatalog = effectSpecs();
        check(effectCatalog.size() == 4 && effectCatalog[0].type == "motion.linear-color" &&
                  effectCatalog[1].type == "motion.rgb-split" && effectCatalog[2].type == "motion.exposure" &&
                  effectCatalog[3].type == "motion.gaussian-blur",
              "effect catalog exposes only the four implemented types");
        check(effectSpec("motion.rgb-split").name == "RGB Split" &&
                  effectParameterSpecs().size() == 3 &&
                  effectParameterSpecs().front().id == effectParameterSpecs("motion.linear-color").front().id,
              "legacy effect-spec API remains the Linear Color alias");
        const auto &splitSpecs = effectParameterSpecs("motion.rgb-split");
        check(splitSpecs.size() == 4 && splitSpecs[0].id == "rgb-split.red-offset" &&
                  splitSpecs[0].defaults == std::vector<double>({0, 0}) && splitSpecs[0].minimum == -4096 &&
                  splitSpecs[0].maximum == 4096 && splitSpecs[1].id == "rgb-split.green-offset" &&
                  splitSpecs[2].id == "rgb-split.blue-offset" && splitSpecs[3].id == "rgb-split.amount" &&
                  splitSpecs[3].defaults == std::vector<double>({1}) && splitSpecs[3].minimum == 0 &&
                  splitSpecs[3].maximum == 1,
              "RGB Split parameter IDs and original defaults stay stable");
        const auto &exposureSpecs = effectParameterSpecs("motion.exposure");
        const auto &blurSpecs = effectParameterSpecs("motion.gaussian-blur");
        check(exposureSpecs.size() == 2 && exposureSpecs[0].id == "exposure.stops" &&
                  exposureSpecs[0].defaults == std::vector<double>({0}) && exposureSpecs[0].minimum == -20 &&
                  exposureSpecs[0].maximum == 20 && exposureSpecs[1].id == "exposure.amount" &&
                  exposureSpecs[1].defaults == std::vector<double>({1}) && exposureSpecs[1].minimum == 0 &&
                  exposureSpecs[1].maximum == 1 && blurSpecs.size() == 2 &&
                  blurSpecs[0].id == "gaussian-blur.sigma" &&
                  blurSpecs[0].defaults == std::vector<double>({0}) && blurSpecs[0].minimum == 0 &&
                  blurSpecs[0].maximum == 128 && blurSpecs[1].id == "gaussian-blur.amount" &&
                  blurSpecs[1].defaults == std::vector<double>({1}) && blurSpecs[1].minimum == 0 &&
                  blurSpecs[1].maximum == 1,
              "Exposure and Gaussian Blur parameter IDs and ranges stay stable");
        check(rejects([] { effectSpec("unknown"); }) && rejects([] { effectParameterSpecs("unknown"); }),
              "unknown effect type has no fallback descriptor");
        check(!Composition{}.motionBlurEnabled && Composition{}.shutterAngle == 180 &&
                  Composition{}.shutterPhase == -90 && Composition{}.motionBlurSamples == 16 &&
                  !Layer{}.motionBlur,
              "motion blur uses the original disabled defaults");
        for (int fault = 0; fault < 8; ++fault) {
            auto invalid = p;
            auto &bad = layer(invalid, 3);
            if (fault == 0)
                bad.parameter("transform.position").type = ParameterType::Scalar;
            if (fault == 1)
                bad.parameter("transform.scale").components.pop_back();
            if (fault == 2)
                bad.base("mask.inverted") = .5;
            if (fault == 3)
                bad.base("text.alignment") = 3;
            if (fault == 4)
                bad.base("text.size") = std::numeric_limits<double>::infinity();
            if (fault == 5)
                bad.channel({"mask.enabled"}).keys = {{motion::time(0), 0, Interpolation::Linear},
                                                      {time(1), 1}};
            if (fault == 6)
                bad.parameters.erase("text.source");
            if (fault == 7)
                bad.parameters.emplace("unknown", Parameter{});
            check(rejects([&] { encodeProject(invalid); }), "invalid typed parameter rejected");
        }
        auto duplicateDocument = QJsonDocument::fromJson(encoded).object();
        auto comps = duplicateDocument["compositions"].toArray();
        auto comp = comps[0].toObject();
        auto layers = comp["layers"].toArray();
        auto duplicatedLayer = layers[0].toObject();
        auto params = duplicatedLayer["parameters"].toArray();
        params.append(params[0]);
        duplicatedLayer["parameters"] = params;
        layers[0] = duplicatedLayer;
        comp["layers"] = layers;
        comps[0] = comp;
        duplicateDocument["compositions"] = comps;
        check(rejects([&] { decodeProject(QJsonDocument(duplicateDocument).toJson()); }),
              "duplicate stable parameter IDs rejected");
        auto tinyTypography = p;
        layer(tinyTypography, 3).base("text.size") = 1e-10;
        layer(tinyTypography, 3).base("text.leading") = 1e-10;
        check(encodeProject(decodeProject(encodeProject(tinyTypography))) == encodeProject(tinyTypography),
              "positive submillipixel legacy typography remains serializable");
        Editor editor(p);
        auto zeroScale = parentScene();
        layer(zeroScale, 3).base("transform.scale", 0) = 0;
        layer(zeroScale, 3).base("transform.scale", 1) = 0;
        Editor zeroEditor(zeroScale);
        zeroEditor.apply("Scale from zero",
                         [&](Project &p) { zeroEditor.setNumericValue(p, 3, Property::ScaleX, time(1), 1); });
        check(layer(zeroEditor.project(), 3).base("transform.scale", 1) == 1,
              "linked Scale can grow from zero");
        check(rejects([&] {
                  zeroEditor.apply("Invalid opacity", [&](Project &p) {
                      zeroEditor.setNumericValue(p, 3, Property::Opacity, time(1), 1.2);
                  });
              }),
              "new opacity edits use the AE percentage range");
        auto curves = parentScene();
        auto &curve = layer(curves, 3).channel(Property::PositionX);
        curve.keys = {{motion::time(0), 0}, {time(2), 100}, {time(4), 200}, {time(6), 300}};
        Editor curveEditor(curves);
        check(near(velocityAt(curve, time(1)).value_or(-1), 50),
              "linear scalar velocity is value change per second");
        curveEditor.apply("Single key ease",
                          [](Project &p) { easeKey(p, {3, Property::PositionX, time(2)}); });
        auto eased = layer(curveEditor.project(), 3).channel(Property::PositionX);
        check(
            near(keyVelocity(curveEditor.project(), {3, Property::PositionX, time(2)}, true)->speed, 0) &&
                near(keyVelocity(curveEditor.project(), {3, Property::PositionX, time(2)}, false)->speed, 0),
            "single-key easing changes both adjacent tangents");
        check(near(eased.keys[0].outHandle.dv, 100. / 3) && eased.keys[2].outgoing == Interpolation::Linear,
              "single-key easing preserves the other endpoint tangent and unrelated segment");
        check(near(valueAt(eased, time(1)), 62.5) && near(valueAt(eased, time(3)), 137.5) &&
                  near(valueAt(eased, time(5)), 250),
              "single-key curve has independently calculated values");
        curveEditor.undoStack().undo();
        check(encodeProject(curveEditor.project()) == encodeProject(curves), "ease is one lossless undo");
        auto two = curves;
        layer(two, 3).channel(Property::PositionX).keys.resize(2);
        Editor twoEditor(two);
        twoEditor.apply("Ease both endpoints", [](Project &p) {
            easeKey(p, {3, Property::PositionX, motion::time(0)});
            easeKey(p, {3, Property::PositionX, time(2)});
        });
        auto both = layer(twoEditor.project(), 3).channel(Property::PositionX);
        check(near(valueAt(both, time(1)), 50) && near(velocityAt(both, time(1)).value_or(-1), 75),
              "symmetric eased midpoint value50 and speed75");
        check(near(velocityAt(both, motion::time(0)).value_or(-1), 0) &&
                  near(velocityAt(both, time(2)).value_or(-1), 0),
              "eased endpoints have zero speed");
        twoEditor.apply("Incoming velocity", [](Project &p) {
            setKeyVelocity(p, {3, Property::PositionX, time(2)}, true, {20, 50});
        });
        auto h = layer(twoEditor.project(), 3).channel(Property::PositionX).keys[1].inHandle;
        check(near(h.dtSeconds, -1) && near(h.dv, -20),
              "incoming speed and influence convert to signed handle offsets");
        twoEditor.apply("Crossing temporal handles", [](Project &p) {
            setKeyVelocity(p, {3, Property::PositionX, motion::time(0)}, false, {0, 100});
            setKeyVelocity(p, {3, Property::PositionX, time(2)}, true, {0, 100});
        });
        auto crossed = layer(twoEditor.project(), 3).channel(Property::PositionX);
        check(crossed.keys[0].outHandle.dtSeconds == 2 && crossed.keys[1].inHandle.dtSeconds == -2,
              "100 percent opposing influences remain intact");
        check(!velocityAt(crossed, time(1)),
              "vertical speed singularity is undefined instead of fabricated zero");
        check(near(valueAt(crossed, time(1)), 50, 1e-9), "crossed handles preserve the symmetric midpoint");
        const auto crossedBytes = encodeProject(twoEditor.project());
        check(encodeProject(decodeProject(crossedBytes)) == crossedBytes,
              "crossed temporal handles round-trip exactly in schema5");
        check(rejects([&] {
                  twoEditor.apply("Bad influence", [](Project &p) {
                      setKeyVelocity(p, {3, Property::PositionX, time(2)}, true, {0, 101});
                  });
              }) &&
                  encodeProject(twoEditor.project()) == crossedBytes,
              "out-of-range influence rejected without mutation");
        auto reverseCurve = curve;
        reverseCurve.keys = {{motion::time(0), 100}, {time(2), 0}};
        check(near(velocityAt(reverseCurve, time(1)).value_or(0), -50) &&
                  velocityAt(reverseCurve, time(-1)).value_or(1) == 0 &&
                  velocityAt(reverseCurve, time(3)).value_or(1) == 0,
              "signed velocity and constant extrapolation are explicit");
        auto fractionalCurveProject = twoEditor.project();
        auto &fractionalCurve = layer(fractionalCurveProject, 3).channel(Property::PositionX);
        fractionalCurve.keys[0].at = time(1, 7);
        fractionalCurve.keys[1].at = time(15, 7);
        setKeyVelocity(fractionalCurveProject, {3, Property::PositionX, time(15, 7)}, true, {12, 25});
        auto fractionalReopened = decodeProject(encodeProject(fractionalCurveProject));
        check(layer(fractionalReopened, 3).channel(Property::PositionX).keys[0].at == time(1, 7) &&
                  layer(fractionalReopened, 3).channel(Property::PositionX).keys[1].at == time(15, 7),
              "velocity editing and reopening retain exact fractional key times");
        auto differenceCurve = both;
        differenceCurve.keys[0].outHandle = {.4, 8};
        differenceCurve.keys[1].inHandle = {-.8, -24};
        bool derivativeMatches = true;
        for (int i = 1; i < 20; ++i) {
            double t = i / 10.;
            double numerical = (valueAt(differenceCurve, fromSeconds(t + 1e-5)) -
                                valueAt(differenceCurve, fromSeconds(t - 1e-5))) /
                               2e-5;
            derivativeMatches &=
                near(velocityAt(differenceCurve, fromSeconds(t)).value_or(-1), numerical, 1e-4);
        }
        check(derivativeMatches, "analytic cubic speed agrees with independent finite differences");
        auto shortInterval = two;
        auto &shortKeys = layer(shortInterval, 3).channel(Property::PositionX).keys;
        shortKeys[0].at = time(1, 7);
        shortKeys[1].at = time(2, 7);
        setKeyVelocity(shortInterval, {3, Property::PositionX, time(1, 7)}, false, {0, 100});
        setKeyVelocity(shortInterval, {3, Property::PositionX, time(2, 7)}, true, {0, 100});
        check(!encodeProject(shortInterval).isEmpty(),
              "100 percent influences remain bounded on fractional intervals");
        check(rejects([&] {
                  setKeyVelocity(shortInterval, {3, Property::PositionX, time(1, 7)}, false, {20, 0});
              }),
              "a finite requested speed requires nonzero influence");
        auto holdCurve = curve;
        holdCurve.keys[0].outgoing = Interpolation::Hold;
        check(velocityAt(holdCurve, time(1)).value_or(-1) == 0 && !velocityAt(holdCurve, time(2)),
              "Hold speed is zero between keys and undefined at its jump");
        auto lockedCurves = curves;
        layer(lockedCurves, 3).locked = true;
        check(
            rejects([&] { setKeyVelocity(lockedCurves, {3, Property::PositionX, time(2)}, true, {0, 50}); }),
            "locked velocity editing refused");
        check(rejects([&] {
                  auto p = curves;
                  setAnimated(p, 3, {"mask.enabled"}, time(1), true);
                  easeKey(p, {3, {"mask.enabled"}, time(1)});
              }),
              "discrete key easing refused");
        auto valuesProject = parentScene();
        layer(valuesProject, 2).base("transform.scale", 0) = 1.25;
        layer(valuesProject, 2).base("transform.scale", 1) = .75;
        Editor valuesEditor(valuesProject);
        const std::vector<PropertyRef> scaleRefs{Property::ScaleX, Property::ScaleY};
        const auto copiedValues = copyPropertyValues(valuesEditor.project(), 2, scaleRefs, motion::time(0));
        check(copiedValues == "[125,75]", "shared value clipboard uses displayed percentages");
        valuesEditor.apply("Paste vector", [&](Project &p) {
            pastePropertyValues(p, 3, scaleRefs, motion::time(0), copiedValues);
        });
        check(layer(valuesEditor.project(), 3).base("transform.scale", 0) == 1.25 &&
                  layer(valuesEditor.project(), 3).base("transform.scale", 1) == .75,
              "vector paste preserves exact components even when Scale linkage is enabled");
        valuesEditor.undoStack().undo();
        check(encodeProject(valuesEditor.project()) == encodeProject(valuesProject),
              "shared value paste undo restores the entire vector");
        for (auto bytes :
             {QByteArray("[1]"), QByteArray(R"([1,"bad"])"), QByteArray("[1,1e99]"), QByteArray(1025, '0')}) {
            check(rejects([&] {
                      valuesEditor.apply("Invalid value paste", [&](Project &p) {
                          pastePropertyValues(p, 3, scaleRefs, motion::time(0), bytes);
                      });
                  }) &&
                      encodeProject(valuesEditor.project()) == encodeProject(valuesProject),
                  "invalid clipboard is rejected atomically");
        }
        check(rejects([&] {
                  valuesEditor.apply("Invalid opacity paste", [&](Project &p) {
                      pastePropertyValues(p, 3, {Property::Opacity}, motion::time(0), "[101]");
                  });
              }),
              "all value-paste surfaces enforce opacity limits");
        const auto before = encodeProject(editor.project());
        editor.apply("Animation", [](Project &p) { setAnimated(p, 3, {"mask.enabled"}, time(1, 5), true); });
        check(layer(editor.project(), 3).channel({"mask.enabled"}).keys[0].outgoing == Interpolation::Hold,
              "boolean animation uses hold");
        editor.undoStack().undo();
        check(encodeProject(editor.project()) == before, "animation undo is lossless");
        const double held = valueAt(l.channel(Property::PositionX), time(1));
        editor.apply("Disable", [](Project &p) { setAnimated(p, 3, Property::PositionX, time(1), false); });
        check(layer(editor.project(), 3).channel(Property::PositionX).keys.empty() &&
                  near(layer(editor.project(), 3).channel(Property::PositionX).base, held),
              "stopwatch disable preserves current value");
        editor.apply("Reset", [](Project &p) { resetProperty(p, 3, Property::PositionX); });
        check(layer(editor.project(), 3).channel(Property::PositionX).base == 0,
              "reset restores descriptor default");
        Id first = 0, second = 0;
        editor.apply("Effects", [&](Project &p) {
            first = addEffect(p, 3);
            second = addEffect(p, 3);
            layer(p, 3).channel({"color.gain", 0, first}).base = 2;
        });
        editor.apply("Reorder", [&](Project &p) { moveEffect(p, 3, second, -1); });
        check(layer(editor.project(), 3).effects.front().id == second, "effect order changes");
        auto fx = decodeProject(encodeProject(editor.project()));
        check(layer(fx, 3).effects[1].id == first && layer(fx, 3).channel({"color.gain", 0, first}).base == 2,
              "effect identity and values reopen");
        Project typedEffects = parentScene();
        const auto splitId = addEffect(typedEffects, 3, "motion.rgb-split");
        const auto exposureId = addEffect(typedEffects, 3, "motion.exposure");
        const auto blurId = addEffect(typedEffects, 3, "motion.gaussian-blur");
        check(layer(typedEffects, 3).effects[0].name == "RGB Split" &&
                  layer(typedEffects, 3).effects[0].parameters.size() == splitSpecs.size() &&
                  layer(typedEffects, 3).channel({"rgb-split.red-offset", 1, splitId}).base == 0 &&
                  layer(typedEffects, 3).channel({"exposure.stops", 0, exposureId}).base == 0 &&
                  layer(typedEffects, 3).channel({"gaussian-blur.sigma", 0, blurId}).base == 0,
              "typed effect creation supplies matching names and parameter defaults");
        check(toDisplay({"rgb-split.amount", 0, splitId}, .25) == 25 &&
                  fromDisplay({"rgb-split.amount", 0, splitId}, 25) == .25,
              "type-specific parameters retain PropertyRef display conversion");
        int splitRows = 0;
        for (const auto &row : propertyRows(layer(typedEffects, 3)))
            if (row.ref.effect == splitId)
                ++splitRows;
        check(splitRows == 7, "property rows expose only the selected effect type's parameters");
        auto typedReopened = decodeProject(encodeProject(typedEffects));
        check(layer(typedReopened, 3).effects[0].type == "motion.rgb-split" &&
                  layer(typedReopened, 3).effects[1].type == "motion.exposure" &&
                  layer(typedReopened, 3).effects[2].type == "motion.gaussian-blur" &&
                  layer(typedReopened, 3).channel({"rgb-split.amount", 0, splitId}).base == 1,
              "schema5 preserves typed effects and their values");
        auto badAmount = typedEffects;
        layer(badAmount, 3).channel({"exposure.amount", 0, exposureId}).base = 1.01;
        check(rejects([&] { encodeProject(badAmount); }), "typed effect range is validated");
        const Id copy = duplicateLayer(fx, 1, 3);
        validateProject(fx);
        check(layer(fx, copy).effects[0].id != second, "layer duplication assigns new effect IDs");
        auto badEffect = fx;
        layer(badEffect, copy).effects[0].type = "unknown";
        check(rejects([&] { encodeProject(badEffect); }), "unknown effect never silently bypassed");
        auto settings = parentScene();
        auto &settingsComp = settings.compositions.front();
        settingsComp.motionBlurEnabled = true;
        settingsComp.shutterAngle = 360;
        settingsComp.shutterPhase = 90;
        settingsComp.motionBlurSamples = 32;
        layer(settings, 3).motionBlur = true;
        const auto settingsBytes = encodeProject(settings);
        const auto settingsReopened = decodeProject(settingsBytes);
        check(settingsReopened.schemaVersion == 5 &&
                  composition(settingsReopened, settingsComp.id).motionBlurEnabled &&
                  composition(settingsReopened, settingsComp.id).shutterAngle == 360 &&
                  composition(settingsReopened, settingsComp.id).shutterPhase == 90 &&
                  composition(settingsReopened, settingsComp.id).motionBlurSamples == 32 &&
                  layer(settingsReopened, 3).motionBlur,
              "schema5 persists composition and layer motion-blur settings");
        for (const auto invalid : {721, -1}) {
            auto badSettings = settings;
            badSettings.compositions.front().shutterAngle = invalid;
            check(rejects([&] { encodeProject(badSettings); }), "motion-blur shutter angle is range-checked");
        }
        auto badSamples = settings;
        badSamples.compositions.front().motionBlurSamples = 0;
        check(rejects([&] { encodeProject(badSamples); }), "motion-blur sample count is range-checked");
        for (const auto invalid : {-361, 361}) {
            auto badSettings = settings;
            badSettings.compositions.front().shutterPhase = invalid;
            check(rejects([&] { encodeProject(badSettings); }), "motion-blur shutter phase is range-checked");
        }
        auto badSchema5 = QJsonDocument::fromJson(settingsBytes).object();
        auto badComps = badSchema5["compositions"].toArray();
        auto badComp = badComps[0].toObject();
        badComp.remove("shutterPhase");
        badComps[0] = badComp;
        badSchema5["compositions"] = badComps;
        check(rejects([&] { decodeProject(QJsonDocument(badSchema5).toJson()); }),
              "schema5 requires its shutter fields");
        auto missingWorkArea = QJsonDocument::fromJson(settingsBytes).object();
        auto workAreaComps = missingWorkArea["compositions"].toArray();
        auto noWorkArea = workAreaComps[0].toObject();
        noWorkArea.remove("workArea");
        workAreaComps[0] = noWorkArea;
        missingWorkArea["compositions"] = workAreaComps;
        check(rejects([&] { decodeProject(QJsonDocument(missingWorkArea).toJson()); }),
              "schema5 requires an explicit null or custom Work Area field");
        auto fractionalSamples = QJsonDocument::fromJson(settingsBytes).object();
        auto sampleComps = fractionalSamples["compositions"].toArray();
        auto sampleComp = sampleComps[0].toObject();
        sampleComp["motionBlurSamples"] = 1.5;
        sampleComps[0] = sampleComp;
        fractionalSamples["compositions"] = sampleComps;
        check(rejects([&] { decodeProject(QJsonDocument(fractionalSamples).toJson()); }),
              "schema5 rejects fractional motion-blur sample counts");
        QFile file(QString(MOTION_SOURCE_DIR) + "/fixtures/original-scene.json");
        check(file.open(QIODevice::ReadOnly), "legacy fixture available");
        const auto legacyBytes = file.readAll();
        const auto legacy = QJsonDocument::fromJson(legacyBytes).object();
        check(legacy["schemaVersion"].toInt() == 1, "migration oracle remains schema1");
        const auto migrated = decodeProject(legacyBytes);
        check(migrated.schemaVersion == 5, "legacy project migrates to schema5");
        check(!composition(migrated, 1).motionBlurEnabled && composition(migrated, 1).shutterAngle == 180 &&
                  composition(migrated, 1).shutterPhase == -90 &&
                  composition(migrated, 1).motionBlurSamples == 16 && !layer(migrated, 2).motionBlur,
              "schema1 migration applies disabled motion-blur defaults");
        for (const auto comp : legacy["compositions"].toArray()) {
            for (const auto value : comp.toObject()["layers"].toArray()) {
                const auto oldLayer = value.toObject();
                const auto &current = layer(migrated, oldLayer["id"].toString().toULongLong());
                const auto oldChannels = oldLayer["channels"].toArray();
                for (int i = 0; i < 8; ++i) {
                    const auto ch = oldChannels[i].toObject();
                    const auto &now = current.channel(Property(i));
                    check(now.base == ch["base"].toDouble() &&
                              now.keys.size() == size_t(ch["keys"].toArray().size()),
                          "migration preserves every base and key count");
                    int k = 0;
                    for (const auto oldKey : ch["keys"].toArray()) {
                        const auto key = oldKey.toObject();
                        const auto t = key["time"].toArray();
                        check(now.keys[k].at ==
                                      time(t[0].toString().toLongLong(), t[1].toString().toLongLong()) &&
                                  now.keys[k].value == key["value"].toDouble(),
                              "migration preserves every key time/value");
                        const auto in = key["in"].toArray(), out = key["out"].toArray();
                        check(now.keys[k].inHandle.dtSeconds == in[0].toDouble() &&
                                  now.keys[k].inHandle.dv == in[1].toDouble() &&
                                  now.keys[k].outHandle.dtSeconds == out[0].toDouble() &&
                                  now.keys[k].outHandle.dv == out[1].toDouble(),
                              "migration preserves every handle");
                        ++k;
                    }
                }
            }
        }
        check(encodeProject(decodeProject(encodeProject(migrated))) == encodeProject(migrated),
              "migrated schema1 fixture stable on reopen");
        for (const auto &legacyPath : {QString("fixtures/legacy/curve-schema2.json"),
                                       QString("fixtures/legacy/media-schema3.json")}) {
            QFile legacyFile(QString(MOTION_SOURCE_DIR) + "/" + legacyPath);
            check(legacyFile.open(QIODevice::ReadOnly), "historical schema2/3 fixture available");
            const auto oldBytes = legacyFile.readAll();
            const auto oldDocument = QJsonDocument::fromJson(oldBytes).object();
            const auto oldSchema = oldDocument["schemaVersion"].toInt();
            check(oldSchema == (legacyPath.contains("schema2") ? 2 : 3), "historical fixture keeps its source schema");
            const auto upgraded = decodeProject(oldBytes);
            check(upgraded.schemaVersion == 5, "schema2/3 fixture migrates to schema5");
            check(!upgraded.compositions.front().motionBlurEnabled &&
                      upgraded.compositions.front().shutterAngle == 180 &&
                      upgraded.compositions.front().shutterPhase == -90 &&
                      upgraded.compositions.front().motionBlurSamples == 16,
                  "schema2/3 migration applies disabled motion-blur defaults");
            const QStringList interpolationNames{"hold", "linear", "cubic"};
            for (const auto oldCompValue : oldDocument["compositions"].toArray()) {
                const auto oldComp = oldCompValue.toObject();
                for (const auto oldLayerValue : oldComp["layers"].toArray()) {
                    const auto oldLayer = oldLayerValue.toObject();
                    const auto &newLayer = layer(upgraded, oldLayer["id"].toString().toULongLong());
                    check(!newLayer.motionBlur, "schema2/3 migration disables each layer's motion blur");
                    check(newLayer.audioEnabled == (oldSchema >= 3 ? oldLayer["audioEnabled"].toBool() : true),
                          "schema2/3 migration preserves or defaults audio switches");
                    for (const auto oldParameterValue : oldLayer["parameters"].toArray()) {
                        const auto oldParameter = oldParameterValue.toObject();
                        const auto &newParameter = newLayer.parameter(oldParameter["id"].toString());
                        const auto oldComponents = oldParameter["components"].toArray();
                        check(newParameter.components.size() == size_t(oldComponents.size()),
                              "schema2/3 migration preserves parameter dimensions");
                        for (int component = 0; component < oldComponents.size(); ++component) {
                            const auto oldChannel = oldComponents[component].toObject();
                            const auto &newChannel = newParameter.components[size_t(component)];
                            const auto oldKeys = oldChannel["keys"].toArray();
                            check(newChannel.base == oldChannel["base"].toDouble() &&
                                      newChannel.keys.size() == size_t(oldKeys.size()),
                                  "schema2/3 migration preserves curve bases and key counts");
                            for (int keyIndex = 0; keyIndex < oldKeys.size(); ++keyIndex) {
                                const auto oldKey = oldKeys[keyIndex].toObject();
                                const auto oldTime = oldKey["time"].toArray();
                                const auto newKey = newChannel.keys[size_t(keyIndex)];
                                const auto in = oldKey["in"].toArray(), out = oldKey["out"].toArray();
                                check(newKey.at == time(oldTime[0].toString().toLongLong(),
                                                        oldTime[1].toString().toLongLong()) &&
                                          newKey.value == oldKey["value"].toDouble() &&
                                          int(newKey.outgoing) ==
                                              interpolationNames.indexOf(oldKey["interpolation"].toString()) &&
                                          newKey.inHandle.dtSeconds == in[0].toDouble() &&
                                          newKey.inHandle.dv == in[1].toDouble() &&
                                          newKey.outHandle.dtSeconds == out[0].toDouble() &&
                                          newKey.outHandle.dv == out[1].toDouble(),
                                      "schema2/3 migration preserves exact key data");
                            }
                        }
                    }
                }
            }
            check(encodeProject(decodeProject(encodeProject(upgraded))) == encodeProject(upgraded),
                  "migrated schema2/3 fixture stable on reopen");
            for (const auto oldAssetValue : oldDocument["assets"].toArray()) {
                const auto oldAsset = oldAssetValue.toObject();
                const auto &newAsset = asset(upgraded, oldAsset["id"].toString().toULongLong());
                check(newAsset.path == oldAsset["path"].toString() &&
                          newAsset.sha256 == oldAsset["sha256"].toString(),
                      "schema2/3 migration preserves media paths and fingerprints");
                if (oldSchema == 3)
                    check(newAsset.kind == AssetKind::Video && newAsset.hasAudio &&
                              newAsset.audioRate == oldAsset["audioRate"].toInt() &&
                              newAsset.variableRate == oldAsset["variableRate"].toBool(),
                          "schema3 migration preserves video and audio metadata");
            }
        }
        QFile schema3File(QString(MOTION_SOURCE_DIR) + "/fixtures/legacy/media-schema3.json");
        check(schema3File.open(QIODevice::ReadOnly), "historical schema3 fixture available for negative check");
        auto schema3WithNewEffect = QJsonDocument::fromJson(schema3File.readAll()).object();
        auto schema3Comps = schema3WithNewEffect["compositions"].toArray();
        auto schema3Comp = schema3Comps[0].toObject();
        auto schema3Layers = schema3Comp["layers"].toArray();
        auto schema3Layer = schema3Layers[0].toObject();
        auto schema3Effects = schema3Layer["effects"].toArray();
        schema3Effects.append(QJsonObject{{"id", "999999"},
                                          {"type", "motion.exposure"},
                                          {"name", "Exposure"},
                                          {"enabled", true},
                                          {"parameters", QJsonArray{}}});
        schema3Layer["effects"] = schema3Effects;
        schema3Layers[0] = schema3Layer;
        schema3Comp["layers"] = schema3Layers;
        schema3Comps[0] = schema3Comp;
        schema3WithNewEffect["compositions"] = schema3Comps;
        check(rejects([&] { decodeProject(QJsonDocument(schema3WithNewEffect).toJson()); }),
              "legacy schemas reject effect types introduced in schema4");
        check(schema3File.seek(0), "historical schema3 fixture can be reread");
        auto legacyLinear = QJsonDocument::fromJson(schema3File.readAll()).object();
        auto encodedEffectProject = parentScene();
        const auto legacyEffectId = addEffect(encodedEffectProject, 3);
        layer(encodedEffectProject, 3).channel({"color.gain", 0, legacyEffectId}).base = 2;
        auto currentEffectDoc = QJsonDocument::fromJson(encodeProject(encodedEffectProject)).object();
        auto currentEffectComps = currentEffectDoc["compositions"].toArray();
        auto currentEffectComp = currentEffectComps[0].toObject();
        auto currentEffectLayers = currentEffectComp["layers"].toArray();
        auto currentEffectLayer = currentEffectLayers[0].toObject();
        auto linearEffect = currentEffectLayer["effects"].toArray()[0].toObject();
        linearEffect["id"] = "999999";
        auto legacyComps = legacyLinear["compositions"].toArray();
        auto legacyComp = legacyComps[0].toObject();
        auto legacyLayers = legacyComp["layers"].toArray();
        auto legacyLayer = legacyLayers[0].toObject();
        auto legacyEffects = legacyLayer["effects"].toArray();
        legacyEffects.append(linearEffect);
        legacyLayer["effects"] = legacyEffects;
        legacyLayers[0] = legacyLayer;
        legacyComp["layers"] = legacyLayers;
        legacyComps[0] = legacyComp;
        legacyLinear["compositions"] = legacyComps;
        legacyLinear["nextId"] = "1000000";
        const auto reopenedLegacyEffect = decodeProject(QJsonDocument(legacyLinear).toJson());
        const auto importedLayerId = legacyLayer["id"].toString().toULongLong();
        check(layer(reopenedLegacyEffect, importedLayerId).effects.size() == 1 &&
                  layer(reopenedLegacyEffect, importedLayerId).effects[0].type == "motion.linear-color" &&
                  layer(reopenedLegacyEffect, importedLayerId)
                          .channel({"color.gain", 0, 999999})
                          .base == 2,
              "legacy schema3 retains its existing Linear Color effect and values");
        std::cout << checkCount << " property and migration checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
