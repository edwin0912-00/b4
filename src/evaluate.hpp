// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "project.hpp"
namespace motion {
double valueAt(const Channel &, Time);
// Interior keys use the outgoing limit; the final key uses the incoming limit.
// Hold jumps and vertical tangents have no finite velocity.
std::optional<double> velocityAt(const Channel &, Time);
double valueAt(const Layer &, PropertyRef, Time);
struct Mat3 {
    std::array<double, 9> values;
};
Mat3 identity();
Mat3 multiply(const Mat3 &, const Mat3 &);
Mat3 inverse(const Mat3 &);
Vec2 mapPoint(const Mat3 &, Vec2);
Mat3 localTransform(const Layer &, Time compTime);
Mat3 worldTransform(const Project &, Id comp, Id layer, Time compTime);
bool isVisible(const Layer &, Time);
Time sourceTime(const Layer &, Time);
} // namespace motion
