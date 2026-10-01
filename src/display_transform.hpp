// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "render.hpp"

namespace motion {
enum class DisplayChannel { RGB, Red, Green, Blue, Alpha, Luminance };

struct DisplayOptions {
    DisplayChannel channel = DisplayChannel::RGB;
    bool grayscale = true;
    double exposureStops = 0;
};

QImage displayImage(const Frame &, const DisplayOptions &, std::stop_token = {});
} // namespace motion
