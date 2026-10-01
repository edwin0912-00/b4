// SPDX-License-Identifier: MPL-2.0
#include "display_transform.hpp"
#include <QColorSpace>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace motion {
namespace {
void checkCancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw RenderCancelled{};
}

void validate(const Frame &frame, const DisplayOptions &options) {
    switch (options.channel) {
    case DisplayChannel::RGB:
    case DisplayChannel::Red:
    case DisplayChannel::Green:
    case DisplayChannel::Blue:
    case DisplayChannel::Alpha:
    case DisplayChannel::Luminance:
        break;
    default:
        throw std::runtime_error("Invalid display channel");
    }
    if (!std::isfinite(options.exposureStops) || options.exposureStops < -20 || options.exposureStops > 20)
        throw std::runtime_error("Display exposure must be finite and in [-20,20]");
    if (frame.width <= 0 || frame.height <= 0 || frame.width > std::numeric_limits<int>::max() / 4)
        throw std::runtime_error("Invalid display frame dimensions");
    const auto width = static_cast<std::size_t>(frame.width);
    const auto height = static_cast<std::size_t>(frame.height);
    if (width > std::numeric_limits<std::size_t>::max() / height || frame.pixels.size() != width * height)
        throw std::runtime_error("Invalid display frame pixels");
}

void validatePixel(const Pixel &pixel) {
    if (!std::isfinite(pixel.r) || !std::isfinite(pixel.g) || !std::isfinite(pixel.b) || !std::isfinite(pixel.a))
        throw std::runtime_error("Display frame contains non-finite pixels");
}

uchar linearByte(double value) {
    const double encoded = value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    return static_cast<uchar>(std::lround(std::clamp(encoded, 0.0, 1.0) * 255.0));
}

uchar coverageByte(double value) {
    return static_cast<uchar>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

uchar legacyByte(float value) {
    return static_cast<uchar>(std::lround(std::clamp(value, 0.f, 1.f) * 255));
}
} // namespace

QImage displayImage(const Frame &frame, const DisplayOptions &options, std::stop_token stop) {
    checkCancelled(stop);
    validate(frame, options);
    QImage image(frame.width, frame.height, QImage::Format_RGBA8888);
    if (image.isNull())
        throw std::runtime_error("Cannot allocate display image");

    const bool identity = options.channel == DisplayChannel::RGB && options.exposureStops == 0;
    const double exposure = std::exp2(options.exposureStops);
    for (int y = 0; y < frame.height; ++y) {
        checkCancelled(stop);
        auto *out = image.scanLine(y);
        for (int x = 0; x < frame.width; ++x) {
            const auto &pixel = frame.pixels[static_cast<std::size_t>(y) * frame.width + x];
            validatePixel(pixel);
            auto *rgba = out + x * 4;
            if (identity) {
                rgba[0] = pixel.a > 0 ? legacyByte(linearToSrgb(pixel.r / pixel.a)) : 0;
                rgba[1] = pixel.a > 0 ? legacyByte(linearToSrgb(pixel.g / pixel.a)) : 0;
                rgba[2] = pixel.a > 0 ? legacyByte(linearToSrgb(pixel.b / pixel.a)) : 0;
                rgba[3] = legacyByte(pixel.a);
                continue;
            }

            if (options.channel == DisplayChannel::Alpha) {
                const uchar coverage = coverageByte(pixel.a);
                rgba[0] = rgba[1] = rgba[2] = coverage;
                rgba[3] = 255;
                continue;
            }

            double red = 0, green = 0, blue = 0;
            if (pixel.a > 0) {
                const double alpha = pixel.a;
                red = static_cast<double>(pixel.r) / alpha;
                green = static_cast<double>(pixel.g) / alpha;
                blue = static_cast<double>(pixel.b) / alpha;
            }

            if (options.channel == DisplayChannel::RGB) {
                rgba[0] = linearByte(red * exposure);
                rgba[1] = linearByte(green * exposure);
                rgba[2] = linearByte(blue * exposure);
                rgba[3] = legacyByte(pixel.a);
                continue;
            }

            double value = 0;
            switch (options.channel) {
            case DisplayChannel::Red:
                value = red;
                break;
            case DisplayChannel::Green:
                value = green;
                break;
            case DisplayChannel::Blue:
                value = blue;
                break;
            case DisplayChannel::Luminance:
                value = .2126 * red + .7152 * green + .0722 * blue;
                break;
            case DisplayChannel::RGB:
            case DisplayChannel::Alpha:
                break;
            }
            const uchar intensity = linearByte(value * exposure);
            rgba[0] = options.grayscale || options.channel == DisplayChannel::Luminance || options.channel == DisplayChannel::Red
                          ? intensity
                          : 0;
            rgba[1] = options.grayscale || options.channel == DisplayChannel::Luminance || options.channel == DisplayChannel::Green
                          ? intensity
                          : 0;
            rgba[2] = options.grayscale || options.channel == DisplayChannel::Luminance || options.channel == DisplayChannel::Blue
                          ? intensity
                          : 0;
            rgba[3] = 255;
        }
    }
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}
} // namespace motion
