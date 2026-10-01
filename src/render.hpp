// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "evaluate.hpp"
#include <QImage>
#include <QSize>
#include <map>
#include <memory>
#include <stop_token>
namespace motion {
struct Pixel {
    float r = 0, g = 0, b = 0, a = 0;
};
struct Frame {
    int width = 0, height = 0;
    std::vector<Pixel> pixels;
};
class MovieSource;
struct Assets {
    std::map<QString, std::shared_ptr<MovieSource>> movies;
    QString baseDirectory;
    std::map<QString, std::shared_ptr<const Frame>> sources;
    std::map<QString, std::shared_ptr<const Frame>> pinnedSources;
    size_t pinnedBytes = 0;
    size_t sourceBytes = 0, sourceBudget = 256u * 1024u * 1024u;
    QStringList warnings;
    bool pinned = false;
};
struct RenderCancelled {};
enum class MotionBlurOverride { CurrentSettings, Off, OnForCheckedLayers };
struct RenderOptions {
    MotionBlurOverride motionBlur = MotionBlurOverride::CurrentSettings;
};
Pixel sourceOver(Pixel, Pixel);
float srgbToLinear(float);
float linearToSrgb(float);
Frame render(const Project &, Id, Time, QSize, Assets &, std::stop_token cancel = {},
             const RenderOptions &options = {});
QImage pngImage(const Frame &);
QImage opaqueImage(const Frame &);
void registerFixtureFont(const QString &path);
Asset inspectPng(const QString &path, Id id);
void preflightSources(const Project &, Assets &);
} // namespace motion
