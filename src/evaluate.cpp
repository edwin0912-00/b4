// SPDX-License-Identifier: MPL-2.0
#include "evaluate.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
namespace motion {
namespace {
double bezier(double a, double b, double c, double d, double u) {
    const double v = 1 - u;
    return v * v * v * a + 3 * v * v * u * b + 3 * v * u * u * c + u * u * u * d;
}
} // namespace
double valueAt(const Channel &c, Time t) {
    if (c.keys.empty())
        return c.base;
    auto it =
        std::lower_bound(c.keys.begin(), c.keys.end(), t, [](const Key &k, Time v) { return k.at < v; });
    if (it == c.keys.begin())
        return it->value;
    if (it == c.keys.end())
        return c.keys.back().value;
    if (it->at == t)
        return it->value;
    const auto &a = *(it - 1);
    const auto &b = *it;
    if (a.outgoing == Interpolation::Hold)
        return a.value;
    const double duration = seconds(b.at - a.at), elapsed = seconds(t - a.at);
    if (a.outgoing == Interpolation::Linear)
        return a.value + (b.value - a.value) * elapsed / duration;
    double lo = 0, hi = 1;
    for (int i = 0; i < 60; ++i) {
        double mid = (lo + hi) / 2;
        const double x = bezier(0, a.outHandle.dtSeconds, duration + b.inHandle.dtSeconds, duration, mid);
        if (x == elapsed)
            return bezier(a.value, a.value + a.outHandle.dv, b.value + b.inHandle.dv, b.value, mid);
        if (x < elapsed)
            lo = mid;
        else
            hi = mid;
    }
    return bezier(a.value, a.value + a.outHandle.dv, b.value + b.inHandle.dv, b.value, (lo + hi) / 2);
}
std::optional<double> velocityAt(const Channel &c, Time t) {
    if (c.keys.size() < 2 || t < c.keys.front().at || c.keys.back().at < t)
        return 0.;
    auto it =
        std::lower_bound(c.keys.begin(), c.keys.end(), t, [](const Key &k, Time at) { return k.at < at; });
    if (it != c.keys.begin() && it->at == t && (it - 1)->outgoing == Interpolation::Hold &&
        (it - 1)->value != it->value)
        return {};
    auto right = it->at == t && it + 1 != c.keys.end() ? it + 1 : it;
    const auto &a = *(right - 1), &b = *right;
    if (a.outgoing == Interpolation::Hold)
        return 0.;
    const double d = seconds(b.at - a.at);
    if (a.outgoing == Interpolation::Linear)
        return (b.value - a.value) / d;
    double low = 0, high = 1, u = 0;
    const double elapsed = seconds(t - a.at);
    for (int i = 0; i < 60; ++i) {
        u = (low + high) / 2;
        const double x = bezier(0, a.outHandle.dtSeconds, d + b.inHandle.dtSeconds, d, u);
        if (x == elapsed)
            break;
        if (x < elapsed)
            low = u;
        else
            high = u;
    }
    auto derivative = [u](double a, double b, double c, double d, int order) {
        if (order == 1)
            return 3 * ((1 - u) * (1 - u) * (b - a) + 2 * (1 - u) * u * (c - b) + u * u * (d - c));
        if (order == 2)
            return 6 * ((1 - u) * (c - 2 * b + a) + u * (d - 2 * c + b));
        return 6 * (d - 3 * c + 3 * b - a);
    };
    for (int order = 1; order <= 3; ++order) {
        const double dx = derivative(0, a.outHandle.dtSeconds, d + b.inHandle.dtSeconds, d, order);
        const double dy =
            derivative(a.value, a.value + a.outHandle.dv, b.value + b.inHandle.dv, b.value, order);
        if (std::abs(dx) > d * 1e-12) {
            double speed = dy / dx;
            return std::isfinite(speed) ? std::optional<double>(speed) : std::nullopt;
        }
        if (std::abs(dy) > std::max(1., std::abs(b.value - a.value)) * 1e-12)
            return {};
    }
    return {};
}
double valueAt(const Layer &l, PropertyRef ref, Time t) {
    const auto &spec = parameterSpec(ref.id);
    return std::clamp(valueAt(l.channel(ref), t), spec.minimum, spec.maximum);
}
Mat3 identity() { return {{1, 0, 0, 0, 1, 0, 0, 0, 1}}; }
Mat3 multiply(const Mat3 &a, const Mat3 &b) {
    Mat3 m{};
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            for (int k = 0; k < 3; ++k)
                m.values[y * 3 + x] += a.values[y * 3 + k] * b.values[k * 3 + x];
    for (double v : m.values)
        if (!std::isfinite(v))
            throw std::runtime_error("Transform exceeds numeric range");
    return m;
}
Vec2 mapPoint(const Mat3 &m, Vec2 p) {
    return {m.values[0] * p.x + m.values[1] * p.y + m.values[2],
            m.values[3] * p.x + m.values[4] * p.y + m.values[5]};
}
Mat3 inverse(const Mat3 &m) {
    const auto &v = m.values;
    const double det = v[0] * v[4] - v[1] * v[3], norm = std::hypot(v[0], v[3]) * std::hypot(v[1], v[4]);
    if (!std::isfinite(det) || !std::isfinite(norm) || !norm || std::abs(det) <= 1e-12 * norm)
        throw std::runtime_error("Transform is singular or too close to shear collapse");
    return {{v[4] / det, -v[1] / det, (v[1] * v[5] - v[4] * v[2]) / det, -v[3] / det, v[0] / det,
             (v[3] * v[2] - v[0] * v[5]) / det, 0, 0, 1}};
}
Time sourceTime(const Layer &l, Time t) { return t - l.start; }
bool isVisible(const Layer &l, Time t) { return l.visible && !(t < l.in) && t < l.out; }
Mat3 localTransform(const Layer &l, Time t) {
    std::array<double, 8> v;
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = valueAt(l.channel(Property(i)), sourceTime(l, t));
    double angle = std::fmod(v[6], 360) * std::numbers::pi / 180, cs = std::cos(angle), sn = std::sin(angle);
    double a = cs * v[4], b = -sn * v[5], c = sn * v[4], d = cs * v[5];
    return {{a, b, v[2] - a * v[0] - b * v[1], c, d, v[3] - c * v[0] - d * v[1], 0, 0, 1}};
}
Mat3 worldTransform(const Project &p, Id comp, Id id, Time t) {
    const auto &c = composition(p, comp);
    Mat3 m = identity();
    std::set<Id> seen;
    std::optional<Id> current = id;
    while (current) {
        if (!seen.insert(*current).second)
            throw std::runtime_error("Parent cycle");
        auto it =
            std::find_if(c.layers.begin(), c.layers.end(), [&](const Layer &l) { return l.id == *current; });
        if (it == c.layers.end())
            throw std::runtime_error("Parent does not exist in composition");
        m = multiply(localTransform(*it, t), m);
        current = it->parent;
    }
    return m;
}
} // namespace motion
