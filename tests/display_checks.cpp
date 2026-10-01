// SPDX-License-Identifier: MPL-2.0
#include "display_transform.hpp"
#include "support.hpp"
#include <QGuiApplication>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

using namespace motion;

static const uchar *pixel(const QImage &image, int x = 0) { return image.constScanLine(0) + x * 4; }

static int encoded(double linear) {
    const double srgb = linear <= .0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - .055;
    return static_cast<int>(std::lround(std::clamp(srgb, 0.0, 1.0) * 255));
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        Frame frame{2, 1, {{.125f, .25f, .375f, .5f}, {0, .1f, .2f, 0}}};
        const auto legacy = pngImage(frame);
        const auto identity = displayImage(frame, {});
        check(identity == legacy, "default display pixels exactly match pngImage");

        const auto brighter = displayImage(frame, {DisplayChannel::RGB, true, 1});
        check(pixel(brighter)[0] == encoded(.5) && pixel(brighter)[3] == pixel(legacy)[3],
              "positive exposure applies to float RGB and preserves byte alpha");
        check(pixel(brighter)[0] != std::min(255, int(pixel(legacy)[0]) * 2),
              "exposure is applied before sRGB byte quantization");
        const auto darker = displayImage(frame, {DisplayChannel::RGB, true, -1});
        check(pixel(darker)[0] == encoded(.125) && pixel(darker)[3] == pixel(legacy)[3],
              "negative exposure applies before quantization and keeps alpha");

        const auto redGray = displayImage(frame, {DisplayChannel::Red, true, 0});
        check(pixel(redGray)[0] == encoded(.25) && pixel(redGray)[1] == pixel(redGray)[0] &&
                  pixel(redGray)[2] == pixel(redGray)[0] && pixel(redGray)[3] == 255,
              "red grayscale view repeats the unpremultiplied red channel");
        const auto redColor = displayImage(frame, {DisplayChannel::Red, false, 0});
        const auto greenColor = displayImage(frame, {DisplayChannel::Green, false, 0});
        const auto blueColor = displayImage(frame, {DisplayChannel::Blue, false, 0});
        check(pixel(redColor)[0] == encoded(.25) && pixel(redColor)[1] == 0 && pixel(redColor)[2] == 0 &&
                  pixel(redColor)[3] == 255,
              "red color view isolates red");
        check(pixel(greenColor)[0] == 0 && pixel(greenColor)[1] == encoded(.5) && pixel(greenColor)[2] == 0 &&
                  pixel(greenColor)[3] == 255,
              "green color view isolates green");
        check(pixel(blueColor)[0] == 0 && pixel(blueColor)[1] == 0 && pixel(blueColor)[2] == encoded(.75) &&
                  pixel(blueColor)[3] == 255,
              "blue color view isolates blue");

        const double luminance = .2126 * .25 + .7152 * .5 + .0722 * .75;
        const auto luma = displayImage(frame, {DisplayChannel::Luminance, false, 0});
        check(pixel(luma)[0] == encoded(luminance) && pixel(luma)[1] == pixel(luma)[0] &&
                  pixel(luma)[2] == pixel(luma)[0] && pixel(luma)[3] == 255,
              "linear luminance uses explicit coefficients and remains grayscale");

        const auto alpha = displayImage(frame, {DisplayChannel::Alpha, false, 20});
        const auto alphaUnexposed = displayImage(frame, {DisplayChannel::Alpha, true, -20});
        check(pixel(alpha)[0] == 128 && pixel(alpha)[1] == 128 && pixel(alpha)[2] == 128 && pixel(alpha)[3] == 255 &&
                  alpha == alphaUnexposed,
              "alpha view maps coverage directly and ignores exposure");
        check(pixel(alpha, 1)[0] == 0 && pixel(alpha, 1)[3] == 255,
              "transparent alpha diagnostic is opaque black");

        Frame zeroAlpha{1, 1, {{.5f, 1.f, 1.5f, 0.f}}};
        const auto guarded = displayImage(zeroAlpha, {DisplayChannel::Blue, true, 0});
        check(pixel(guarded)[0] == 0 && pixel(guarded)[3] == 255,
              "single-channel display guards unpremultiplication at zero alpha");

        Frame extremes{2, 1, {{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max(), 1.f},
                              {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                               -std::numeric_limits<float>::max(), 1.f}}};
        const auto clamped = displayImage(extremes, {DisplayChannel::RGB, true, 20});
        check(pixel(clamped)[0] == 255 && pixel(clamped)[1] == 255 && pixel(clamped)[2] == 255 &&
                  pixel(clamped, 1)[0] == 0 && pixel(clamped, 1)[1] == 0 && pixel(clamped, 1)[2] == 0,
              "large finite positive and negative values clamp safely");

        check(rejects([&] { displayImage(frame, {DisplayChannel::RGB, true, 21}); }),
              "exposure above the supported range is rejected");
        check(rejects([&] {
                  displayImage(frame, {DisplayChannel::RGB, true, std::numeric_limits<double>::quiet_NaN()});
              }),
              "non-finite exposure is rejected");
        check(rejects([&] { displayImage(frame, {static_cast<DisplayChannel>(99), true, 0}); }),
              "unknown display channel is rejected");

        std::stop_source stopped;
        stopped.request_stop();
        bool cancelled = false;
        try {
            displayImage(frame, {}, stopped.get_token());
        } catch (const RenderCancelled &) {
            cancelled = true;
        }
        check(cancelled, "pre-cancelled display conversion stops with RenderCancelled");

        std::cout << checkCount << " display checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
