// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "render.hpp"
#include <stop_token>

namespace motion {
void applyEffects(const Layer &, Time localTime, Frame &, std::stop_token = {});
} // namespace motion
