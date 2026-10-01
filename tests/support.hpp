// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "project.hpp"
#include <cmath>
#include <iostream>
inline int checkCount = 0;
inline void check(bool ok, const char *message) {
    ++checkCount;
    if (!ok)
        throw std::runtime_error(message);
}
inline bool near(double a, double b, double eps = 1e-6) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= eps;
}
template <class F> bool rejects(F f) {
    try {
        f();
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}
inline motion::Project minimalProject() { return {}; }
inline motion::Project parentScene() {
    using namespace motion;
    Project p;
    Layer parent;
    parent.id = 2;
    parent.kind = LayerKind::Null;
    parent.channel(Property(2)).base = 100;
    parent.channel(Property(3)).base = 50;
    parent.channel(Property(6)).base = 90;
    Layer child;
    child.id = 3;
    child.parent = 2;
    child.channel(Property(2)).base = 10;
    p.compositions[0].layers = {child, parent};
    p.nextId = 4;
    return p;
}
inline motion::Project solidScene() {
    using namespace motion;
    Project p;
    auto &c = p.compositions[0];
    c.width = c.height = 2;
    Layer red;
    red.id = 2;
    red.width = red.height = 2;
    red.setColor(QColor("red"));
    red.channel(Property(7)).base = .5;
    Layer blue = red;
    blue.id = 3;
    blue.setColor(QColor("blue"));
    blue.channel(Property(7)).base = 1;
    c.layers = {red, blue};
    p.nextId = 4;
    return p;
}
