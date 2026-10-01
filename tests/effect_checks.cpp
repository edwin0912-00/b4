// SPDX-License-Identifier: MPL-2.0
#include "effects.hpp"
#include "support.hpp"
#include <QCoreApplication>
#include <bit>
#include <cstdint>
#include <limits>
#include <stop_token>
using namespace motion;

namespace {
Frame frame(int width, int height) { return {width, height, std::vector<Pixel>(size_t(width) * height)}; }
Pixel &pixel(Frame &f, int x, int y) { return f.pixels[size_t(y) * f.width + x]; }
bool sameBits(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
bool same(const Frame &a, const Frame &b) {
    if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size())
        return false;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        const auto &x = a.pixels[i], &y = b.pixels[i];
        if (!sameBits(x.r, y.r) || !sameBits(x.g, y.g) || !sameBits(x.b, y.b) || !sameBits(x.a, y.a))
            return false;
    }
    return true;
}
Effect effect(const QString &type) {
    Effect e;
    e.id = 17;
    e.type = type;
    e.name = type;
    e.parameters = defaultParameters(effectParameterSpecs(type));
    return e;
}
void setBase(Effect &e, const QString &id, double value, int component = 0) {
    e.parameters.at(id).components.at(size_t(component)).base = value;
}
Layer layerWith(const QString &type) {
    Layer l;
    l.effects.push_back(effect(type));
    return l;
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        auto source = frame(5, 3);
        pixel(source, 1, 1) = {1, 0, 0, 1};
        pixel(source, 2, 1) = {0, 1, 0, 1};
        pixel(source, 3, 1) = {0, 0, 1, 1};

        auto split = layerWith("motion.rgb-split");
        auto &splitEffect = split.effects.front();
        split.channel({"rgb-split.red-offset", 0, splitEffect.id}).base = 1;
        auto shifted = source;
        applyEffects(split, motion::time(0), shifted);
        check(pixel(shifted, 2, 1).r == 1 && pixel(shifted, 1, 1).r == 0,
              "RGB Split translates the red channel by an integer pixel");
        check(pixel(shifted, 2, 1).g == 1 && pixel(shifted, 3, 1).b == 1,
              "RGB Split preserves unshifted green and blue channels");

        split.channel({"rgb-split.red-offset", 0, splitEffect.id}).base = .5;
        shifted = source;
        applyEffects(split, motion::time(0), shifted);
        check(near(pixel(shifted, 1, 1).r, .5) && near(pixel(shifted, 2, 1).r, .5),
              "RGB Split bilinearly distributes a subpixel impulse");

        auto transparent = frame(3, 1);
        pixel(transparent, 0, 0) = {.25f, 0, 0, .25f};
        pixel(transparent, 1, 0) = {0, .5f, 0, .5f};
        split.channel({"rgb-split.red-offset", 0, splitEffect.id}).base = 1;
        split.channel({"rgb-split.amount", 0, splitEffect.id}).base = .5;
        applyEffects(split, motion::time(0), transparent);
        check(near(pixel(transparent, 1, 0).r, .125) && near(pixel(transparent, 1, 0).g, .5) &&
                  near(pixel(transparent, 1, 0).a, .5),
              "RGB Split mixes complete premultiplied pixels and unions channel alpha");
        check(near(pixel(transparent, 0, 0).r, .125) && near(pixel(transparent, 0, 0).a, .25) &&
                  near(pixel(transparent, 2, 0).a, .25),
              "RGB Split mixes original alpha with the maximum shifted-channel alpha");

        auto neutral = layerWith("motion.rgb-split");
        auto neutralFrame = source;
        applyEffects(neutral, motion::time(0), neutralFrame);
        check(same(neutralFrame, source), "zero RGB offsets are exact identity");
        neutral.effects.front().enabled = false;
        neutral.channel({"rgb-split.red-offset", 0, neutral.effects.front().id}).base = 100;
        neutralFrame = source;
        applyEffects(neutral, motion::time(0), neutralFrame);
        check(same(neutralFrame, source), "disabled effects are exact identity");

        auto neutralColor = layerWith("motion.linear-color");
        auto signedZeros = frame(1, 1);
        pixel(signedZeros, 0, 0) = {-0.0f, -0.0f, 0.0f, .5f};
        const auto signedZerosBefore = signedZeros;
        applyEffects(neutralColor, motion::time(0), signedZeros);
        check(same(signedZeros, signedZerosBefore), "neutral Linear Color preserves signed-zero bits");
        setBase(neutralColor.effects.front(), "color.gain", 2);
        setBase(neutralColor.effects.front(), "color.bias", .5);
        setBase(neutralColor.effects.front(), "color.amount", 0);
        signedZeros = signedZerosBefore;
        applyEffects(neutralColor, motion::time(0), signedZeros);
        check(same(signedZeros, signedZerosBefore), "zero-amount Linear Color is exact identity");

        auto exposed = layerWith("motion.exposure");
        const auto exposureId = exposed.effects.front().id;
        exposed.channel({"exposure.stops", 0, exposureId}).base = 1;
        auto onePixel = frame(1, 1);
        pixel(onePixel, 0, 0) = {.125f, .25f, .375f, .5f};
        applyEffects(exposed, motion::time(0), onePixel);
        check(pixel(onePixel, 0, 0).r == .25f && pixel(onePixel, 0, 0).g == .5f &&
                  pixel(onePixel, 0, 0).a == .5f,
              "exposure doubles premultiplied RGB and preserves alpha");
        exposed.channel({"exposure.amount", 0, exposureId}).base = .25;
        onePixel = frame(1, 1);
        pixel(onePixel, 0, 0) = {.2f, .4f, .6f, .5f};
        applyEffects(exposed, motion::time(0), onePixel);
        check(near(pixel(onePixel, 0, 0).r, .25) && near(pixel(onePixel, 0, 0).g, .5) &&
                  pixel(onePixel, 0, 0).a == .5f,
              "exposure Amount .25 interpolates stops 1 to the identity factor");
        exposed.channel({"exposure.amount", 0, exposureId}).base = 1;
        exposed.channel({"exposure.stops", 0, exposureId}).keys = {{motion::time(0), 0}, {motion::time(2), 2}};
        onePixel = frame(1, 1);
        pixel(onePixel, 0, 0) = {.125f, 0, 0, .5f};
        applyEffects(exposed, motion::time(1), onePixel);
        check(pixel(onePixel, 0, 0).r == .25f, "animated exposure uses the requested local time");

        auto overflow = layerWith("motion.exposure");
        overflow.channel({"exposure.stops", 0, overflow.effects.front().id}).base = 20;
        auto huge = frame(1, 1);
        pixel(huge, 0, 0) = {std::numeric_limits<float>::max(), 0, 0, 1};
        const auto hugeBefore = huge;
        check(rejects([&] { applyEffects(overflow, motion::time(0), huge); }),
              "exposure refuses a finite-float overflow");
        check(same(huge, hugeBefore), "exposure overflow leaves its pixel unchanged");

        auto blur = layerWith("motion.gaussian-blur");
        blur.channel({"gaussian-blur.sigma", 0, blur.effects.front().id}).base = 1;
        auto impulse = frame(9, 9);
        pixel(impulse, 4, 4) = {1, .5f, .25f, 1};
        applyEffects(blur, motion::time(0), impulse);
        double redMass = 0, alphaMass = 0;
        for (int y = 0; y < impulse.height; ++y) {
            for (int x = 0; x < impulse.width; ++x) {
                redMass += pixel(impulse, x, y).r;
                alphaMass += pixel(impulse, x, y).a;
                check(near(pixel(impulse, x, y).r, pixel(impulse, 8 - x, y).r, 1e-7) &&
                          near(pixel(impulse, x, y).r, pixel(impulse, x, 8 - y).r, 1e-7),
                      "Gaussian impulse is symmetric");
            }
        }
        check(near(redMass, 1, 1e-6) && near(alphaMass, 1, 1e-6),
              "normalized Gaussian preserves interior impulse mass");
        auto halfBlur = layerWith("motion.gaussian-blur");
        setBase(halfBlur.effects.front(), "gaussian-blur.sigma", 1);
        setBase(halfBlur.effects.front(), "gaussian-blur.amount", .5);
        auto halfImpulse = frame(9, 9);
        pixel(halfImpulse, 4, 4) = {1, 0, 0, 1};
        applyEffects(halfBlur, motion::time(0), halfImpulse);
        const double kernelSum = 1 + 2 * (std::exp(-.5) + std::exp(-2) + std::exp(-4.5));
        const double expectedCenter = .5 + .5 / (kernelSum * kernelSum);
        check(near(pixel(halfImpulse, 4, 4).r, expectedCenter, 1e-7),
              "Gaussian Amount .5 mixes the impulse with the analytic kernel center");
        auto zeroBlur = frame(1, 1);
        pixel(zeroBlur, 0, 0) = {.25f, .5f, .75f, .5f};
        const auto zeroBefore = zeroBlur;
        blur.channel({"gaussian-blur.sigma", 0, blur.effects.front().id}).base = 0;
        applyEffects(blur, motion::time(0), zeroBlur);
        check(same(zeroBlur, zeroBefore), "zero sigma blur is exact identity");
        blur.channel({"gaussian-blur.sigma", 0, blur.effects.front().id}).base = 1;
        std::stop_source stopped;
        stopped.request_stop();
        bool canceled = false;
        try {
            applyEffects(blur, motion::time(0), zeroBlur, stopped.get_token());
        } catch (const RenderCancelled &) {
            canceled = true;
        }
        check(canceled && same(zeroBlur, zeroBefore), "Gaussian blur observes cancellation before mutation");

        auto ordered = layerWith("motion.linear-color");
        auto color = ordered.effects.front();
        setBase(color, "color.bias", .25);
        auto exp = effect("motion.exposure");
        exp.id = 18;
        setBase(exp, "exposure.stops", 1);
        ordered.effects = {color, exp};
        onePixel = frame(1, 1);
        pixel(onePixel, 0, 0) = {.2f, 0, 0, .5f};
        applyEffects(ordered, motion::time(0), onePixel);
        check(near(pixel(onePixel, 0, 0).r, .65, 1e-7), "effect stack runs in stored order");

        std::cout << "effect checks passed: " << checkCount << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "effect checks failed: " << e.what() << '\n';
        return 1;
    }
}
