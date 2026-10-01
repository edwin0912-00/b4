// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "render.hpp"
#include <functional>
#include <memory>
namespace motion {
constexpr int audioSampleRate = 48000;
QString mediaFingerprint(const QString &, std::stop_token = {});
Asset inspectMedia(const QString &, Id, std::stop_token = {});
struct VideoFrame {
    QImage image;
    Time at;
};
class MovieSource {
  public:
    MovieSource(const QString &, const QString &expectedHash, std::stop_token = {});
    ~MovieSource();
    const Asset &info() const;
    VideoFrame frame(Time, std::stop_token = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class PcmSource {
  public:
    PcmSource(const QString &, const QString &expectedHash, std::stop_token = {});
    ~PcmSource();
    std::int64_t frames() const;
    const std::vector<double> &peaks() const;
    void read(std::int64_t first, int count, float *stereo) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::shared_ptr<PcmSource> pcmSource(const QString &, const QString &, std::stop_token = {});
class AudioMix {
  public:
    AudioMix(Project, Id comp, QString baseDirectory, std::stop_token = {}, bool loopWorkArea = false);
    bool hasAudio() const { return !clips_.empty(); }
    void read(Time first, int count, float *stereo) const;
    double duration() const;

  private:
    struct Gain {
        const Layer *layer;
        Time origin;
    };
    struct Clip {
        std::shared_ptr<PcmSource> source;
        Time origin, in, out;
        std::vector<Gain> gains;
    };
    Project project_;
    Id comp_;
    std::vector<Clip> clips_;
    Time loopStart_{}, loopEnd_{};
};
class AudioPlayback {
  public:
    AudioPlayback(std::shared_ptr<AudioMix>, Time start);
    ~AudioPlayback();
    std::int64_t playedSamples() const;
    void update(std::shared_ptr<AudioMix>);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class MovieWriter {
  public:
    MovieWriter(const QString &newFile, QSize, Time fps, Time duration, bool audio);
    ~MovieWriter();
    void video(const QImage &, Time at, std::stop_token = {});
    void audio(const float *stereo, int frames, std::int64_t first, std::stop_token = {});
    void finishVideo();
    void finishAudio();
    void finish(Time duration, std::stop_token = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace motion
