// SPDX-License-Identifier: MPL-2.0
#include "media.hpp"
#include <QDir>
#include <algorithm>
#include <cmath>
namespace motion {
AudioMix::AudioMix(Project p, Id comp, QString base, std::stop_token stop, bool loopWorkArea)
    : project_(std::move(p)), comp_(comp) {
    validateProject(project_);
    const auto &root = composition(project_, comp_);
    loopStart_ = time(0);
    loopEnd_ = root.duration;
    if (loopWorkArea) {
        const auto range = resolvedWorkArea(root);
        loopStart_ = range.start;
        loopEnd_ = range.end;
    }
    std::function<void(Id, Time, Time, Time, std::vector<Gain>, int)> visit;
    visit = [&](Id id, Time offset, Time in, Time out, std::vector<Gain> parents, int depth) {
        if (stop.stop_requested())
            throw RenderCancelled{};
        if (depth > 16)
            throw std::runtime_error("Audio precomposition depth exceeds limit");
        const auto &c = composition(project_, id);
        out = std::min(out, offset + c.duration);
        for (const auto &l : c.layers) {
            if (!l.audioEnabled)
                continue;
            const auto begin = std::max(in, offset + l.in), end = std::min(out, offset + l.out),
                       origin = offset + l.start;
            if (!(begin < end))
                continue;
            auto gains = parents;
            gains.push_back({&l, origin});
            if (l.kind == LayerKind::Precomp)
                visit(l.source, origin, begin, end, std::move(gains), depth + 1);
            else if ((l.kind == LayerKind::Video || l.kind == LayerKind::Audio) &&
                     asset(project_, l.source).hasAudio) {
                const auto &a = asset(project_, l.source);
                clips_.push_back({pcmSource(QDir(base).absoluteFilePath(a.path), a.sha256, stop), origin,
                                  begin, end, std::move(gains)});
            }
        }
    };
    visit(comp_, time(0), time(0), composition(project_, comp_).duration, {}, 0);
}
double AudioMix::duration() const { return seconds(composition(project_, comp_).duration); }
void AudioMix::read(Time first, int count, float *out) const {
    if (count < 0 || count > 65536)
        throw std::runtime_error("Audio block exceeds limit");
    std::fill(out, out + size_t(count) * 2, 0.f);
    int offset = 0;
    const Time loopSpan = loopEnd_ - loopStart_;
    const Time reciprocalLoopSpan = time(loopSpan.denominator, loopSpan.numerator);
    const auto wrapCursor = [&](Time at) {
        const Time relative = at - loopStart_;
        const auto cycles = frameIndexFloor(relative, reciprocalLoopSpan);
        return loopStart_ + relative - scaleTime(loopSpan, cycles);
    };
    Time cursorTime = wrapCursor(first);
    while (offset < count) {
        const int n = int(std::min<std::int64_t>(frameCount(loopEnd_ - cursorTime, time(audioSampleRate)),
                                               count - offset));
        const double cursor = seconds(cursorTime);
        for (const auto &clip : clips_) {
            if (cursor + double(n) / audioSampleRate <= seconds(clip.in) || cursor >= seconds(clip.out))
                continue;
            const double sourcePosition = seconds(cursorTime - clip.origin) * audioSampleRate;
            const auto sourceFirst = std::int64_t(std::floor(sourcePosition));
            const double fraction = sourcePosition - sourceFirst;
            std::vector<float> data(size_t(n + 1) * 2);
            clip.source->read(sourceFirst, n + 1, data.data());
            bool constant = true;
            double constantDb[2] = {0, 0};
            for (const auto &g : clip.gains)
                for (int channel = 0; channel < 2; ++channel) {
                    const auto &ch = g.layer->channel({"audio.levels", channel});
                    constant &= ch.keys.empty();
                    constantDb[channel] += ch.base;
                }
            const double constantGain[2] = {std::pow(10., constantDb[0] / 20),
                                            std::pow(10., constantDb[1] / 20)};
            for (int i = 0; i < n; ++i) {
                double t = cursor + double(i) / audioSampleRate;
                if (t < seconds(clip.in) || t >= seconds(clip.out))
                    continue;
                for (int channel = 0; channel < 2; ++channel) {
                    double gain = constantGain[channel];
                    if (!constant) {
                        double db = 0;
                        for (const auto &g : clip.gains)
                            db += valueAt(g.layer->channel({"audio.levels", channel}),
                                          fromSeconds(t) - g.origin);
                        gain = std::pow(10., db / 20);
                    }
                    double sample =
                        data[i * 2 + channel] * (1 - fraction) + data[(i + 1) * 2 + channel] * fraction;
                    double mixed = out[(offset + i) * 2 + channel] + sample * gain;
                    if (!std::isfinite(mixed))
                        throw std::runtime_error("Audio mix exceeded numeric range");
                    out[(offset + i) * 2 + channel] = float(mixed);
                }
            }
        }
        offset += n;
        cursorTime = wrapCursor(cursorTime + frameTime(n, time(audioSampleRate)));
    }
    for (int i = 0; i < count * 2; ++i)
        out[i] = std::clamp(out[i], -1.f, 1.f);
}
} // namespace motion
