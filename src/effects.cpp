// SPDX-License-Identifier: MPL-2.0
#include "effects.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace motion {
namespace {
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw RenderCancelled{};
}

void validateFrame(const Frame &frame) {
    if (frame.width < 0 || frame.height < 0 || frame.width > 8192 || frame.height > 8192)
        throw std::runtime_error("Invalid effect frame");
    const auto count = std::uint64_t(frame.width) * std::uint64_t(frame.height);
    if (count > 16777216 || count != frame.pixels.size())
        throw std::runtime_error("Invalid effect frame");
}

float finiteFloat(double value) {
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
        throw std::runtime_error("Effect stack exceeds floating-point range");
    return static_cast<float>(value);
}

double parameterValue(const Layer &layer, Id effect, const QString &id, int component, Time at) {
    const double value = valueAt(layer, {id, component, effect}, at);
    if (!std::isfinite(value))
        throw std::runtime_error("Effect parameter is not finite");
    return value;
}

Pixel mix(Pixel original, Pixel processed, double amount) {
    const double keep = 1 - amount;
    return {finiteFloat(double(original.r) * keep + double(processed.r) * amount),
            finiteFloat(double(original.g) * keep + double(processed.g) * amount),
            finiteFloat(double(original.b) * keep + double(processed.b) * amount),
            finiteFloat(double(original.a) * keep + double(processed.a) * amount)};
}

void linearColor(const Layer &layer, const Effect &effect, Time at, Frame &frame, std::stop_token stop) {
    const double gain = parameterValue(layer, effect.id, "color.gain", 0, at);
    const double bias = parameterValue(layer, effect.id, "color.bias", 0, at);
    const double amount = parameterValue(layer, effect.id, "color.amount", 0, at);
    if (amount == 0 || (gain == 1 && bias == 0))
        return;
    const float factor = float(1 + amount * (gain - 1));
    const float offset = float(amount * bias);
    for (int y = 0; y < frame.height; ++y) {
        cancelled(stop);
        for (int x = 0; x < frame.width; ++x) {
            auto &pixel = frame.pixels[size_t(y) * frame.width + x];
            pixel.r = pixel.r * factor + pixel.a * offset;
            pixel.g = pixel.g * factor + pixel.a * offset;
            pixel.b = pixel.b * factor + pixel.a * offset;
            if (!std::isfinite(pixel.r) || !std::isfinite(pixel.g) || !std::isfinite(pixel.b))
                throw std::runtime_error("Effect stack exceeds floating-point range");
        }
    }
}

void exposure(const Layer &layer, const Effect &effect, Time at, Frame &frame, std::stop_token stop) {
    const double stops = parameterValue(layer, effect.id, "exposure.stops", 0, at);
    const double amount = parameterValue(layer, effect.id, "exposure.amount", 0, at);
    if (stops == 0 || amount == 0)
        return;
    const double factor = std::exp2(stops);
    for (int y = 0; y < frame.height; ++y) {
        cancelled(stop);
        for (int x = 0; x < frame.width; ++x) {
            auto &pixel = frame.pixels[size_t(y) * frame.width + x];
            const double keep = 1 - amount;
            const double r = double(pixel.r) * keep + double(pixel.r) * factor * amount;
            const double g = double(pixel.g) * keep + double(pixel.g) * factor * amount;
            const double b = double(pixel.b) * keep + double(pixel.b) * factor * amount;
            const Pixel out{finiteFloat(r), finiteFloat(g), finiteFloat(b), pixel.a};
            pixel = out;
        }
    }
}

Pixel sample(const Frame &frame, double x, double y) {
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    const auto get = [&](int sx, int sy) -> Pixel {
        if (sx < 0 || sy < 0 || sx >= frame.width || sy >= frame.height)
            return {};
        return frame.pixels[size_t(sy) * frame.width + sx];
    };
    const Pixel p00 = get(x0, y0), p10 = get(x0 + 1, y0), p01 = get(x0, y0 + 1),
                p11 = get(x0 + 1, y0 + 1);
    const double w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy,
                 w11 = fx * fy;
    return {finiteFloat(double(p00.r) * w00 + double(p10.r) * w10 + double(p01.r) * w01 + double(p11.r) * w11),
            finiteFloat(double(p00.g) * w00 + double(p10.g) * w10 + double(p01.g) * w01 + double(p11.g) * w11),
            finiteFloat(double(p00.b) * w00 + double(p10.b) * w10 + double(p01.b) * w01 + double(p11.b) * w11),
            finiteFloat(double(p00.a) * w00 + double(p10.a) * w10 + double(p01.a) * w01 + double(p11.a) * w11)};
}

void rgbSplit(const Layer &layer, const Effect &effect, Time at, Frame &frame, std::stop_token stop) {
    const double redX = parameterValue(layer, effect.id, "rgb-split.red-offset", 0, at);
    const double redY = parameterValue(layer, effect.id, "rgb-split.red-offset", 1, at);
    const double greenX = parameterValue(layer, effect.id, "rgb-split.green-offset", 0, at);
    const double greenY = parameterValue(layer, effect.id, "rgb-split.green-offset", 1, at);
    const double blueX = parameterValue(layer, effect.id, "rgb-split.blue-offset", 0, at);
    const double blueY = parameterValue(layer, effect.id, "rgb-split.blue-offset", 1, at);
    const double amount = parameterValue(layer, effect.id, "rgb-split.amount", 0, at);
    if (amount == 0 || (redX == 0 && redY == 0 && greenX == 0 && greenY == 0 && blueX == 0 && blueY == 0))
        return;
    const Frame original = frame;
    for (int y = 0; y < frame.height; ++y) {
        cancelled(stop);
        for (int x = 0; x < frame.width; ++x) {
            const Pixel old = original.pixels[size_t(y) * frame.width + x];
            const Pixel red = sample(original, x - redX, y - redY);
            const Pixel green = sample(original, x - greenX, y - greenY);
            const Pixel blue = sample(original, x - blueX, y - blueY);
            const Pixel processed{red.r, green.g, blue.b, std::max({red.a, green.a, blue.a})};
            frame.pixels[size_t(y) * frame.width + x] = mix(old, processed, amount);
        }
    }
}

void gaussianBlur(const Layer &layer, const Effect &effect, Time at, Frame &frame, std::stop_token stop) {
    const double sigma = parameterValue(layer, effect.id, "gaussian-blur.sigma", 0, at);
    const double amount = parameterValue(layer, effect.id, "gaussian-blur.amount", 0, at);
    if (sigma == 0 || amount == 0)
        return;
    cancelled(stop);
    const int radius = int(std::ceil(3 * sigma));
    std::vector<double> kernel(size_t(radius * 2 + 1));
    double total = 0;
    for (int k = -radius; k <= radius; ++k) {
        const double ratio = double(k) / sigma;
        const double weight = std::exp(-.5 * ratio * ratio);
        kernel[size_t(k + radius)] = weight;
        total += weight;
    }
    for (auto &weight : kernel)
        weight /= total;

    std::vector<Pixel> horizontal(frame.pixels.size());
    for (int y = 0; y < frame.height; ++y) {
        cancelled(stop);
        for (int x = 0; x < frame.width; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int k = std::max(-radius, -x); k <= std::min(radius, frame.width - 1 - x); ++k) {
                const Pixel p = frame.pixels[size_t(y) * frame.width + x + k];
                const double weight = kernel[size_t(k + radius)];
                r += double(p.r) * weight;
                g += double(p.g) * weight;
                b += double(p.b) * weight;
                a += double(p.a) * weight;
            }
            horizontal[size_t(y) * frame.width + x] =
                {finiteFloat(r), finiteFloat(g), finiteFloat(b), finiteFloat(a)};
        }
    }
    for (int y = 0; y < frame.height; ++y) {
        cancelled(stop);
        for (int x = 0; x < frame.width; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int k = std::max(-radius, -y); k <= std::min(radius, frame.height - 1 - y); ++k) {
                const Pixel p = horizontal[size_t(y + k) * frame.width + x];
                const double weight = kernel[size_t(k + radius)];
                r += double(p.r) * weight;
                g += double(p.g) * weight;
                b += double(p.b) * weight;
                a += double(p.a) * weight;
            }
            const Pixel processed{finiteFloat(r), finiteFloat(g), finiteFloat(b), finiteFloat(a)};
            auto &pixel = frame.pixels[size_t(y) * frame.width + x];
            pixel = mix(pixel, processed, amount);
        }
    }
}
} // namespace

void applyEffects(const Layer &layer, Time localTime, Frame &frame, std::stop_token stop) {
    validateFrame(frame);
    cancelled(stop);
    for (const auto &effect : layer.effects) {
        cancelled(stop);
        if (!effect.enabled)
            continue;
        if (effect.type == "motion.linear-color")
            linearColor(layer, effect, localTime, frame, stop);
        else if (effect.type == "motion.rgb-split")
            rgbSplit(layer, effect, localTime, frame, stop);
        else if (effect.type == "motion.exposure")
            exposure(layer, effect, localTime, frame, stop);
        else if (effect.type == "motion.gaussian-blur")
            gaussianBlur(layer, effect, localTime, frame, stop);
        else
            throw std::runtime_error("Unsupported effect");
    }
}
} // namespace motion
