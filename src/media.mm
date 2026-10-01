// SPDX-License-Identifier: MPL-2.0
#include "media.hpp"
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <ImageIO/ImageIO.h>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <numeric>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
namespace motion {
namespace {
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw RenderCancelled{};
}
void fail(NSString *message) {
    throw std::runtime_error(message ? message.UTF8String : "Native media operation failed");
}
void wait(dispatch_semaphore_t done, std::stop_token stop = {}) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC))) {
        cancelled(stop);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Native media operation timed out");
    }
    cancelled(stop);
}
Time fromCM(CMTime t) {
    if (!CMTIME_IS_NUMERIC(t) || t.timescale <= 0 || t.epoch != 0)
        throw std::runtime_error("Media has invalid timing");
    return time(t.value, t.timescale);
}
CMTime toCM(Time t) {
    if (t.denominator <= INT32_MAX)
        return CMTimeMake(t.numerator, int32_t(t.denominator));
    return CMTimeMakeWithSeconds(seconds(t), 1000000000);
}
AVURLAsset *openAsset(const QString &path) {
    QFileInfo f(path);
    if (!f.isFile())
        throw std::runtime_error("Media source is missing or not a regular file");
    return
        [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:f.canonicalFilePath().toNSString()]
                            options:@{
                                AVURLAssetPreferPreciseDurationAndTimingKey : @YES,
                                AVURLAssetReferenceRestrictionsKey : @(AVAssetReferenceRestrictionForbidAll)
                            }];
}
NSArray<AVAssetTrack *> *tracks(AVAsset *asset, AVMediaType type, std::stop_token stop) {
    struct Result {
        NSArray<AVAssetTrack *> *__strong value = nil;
        NSError *__strong error = nil;
    };
    auto state = std::make_shared<Result>();
    auto done = dispatch_semaphore_create(0);
    [asset loadTracksWithMediaType:type
                 completionHandler:^(NSArray<AVAssetTrack *> *v, NSError *e) {
                   state->value = v;
                   state->error = e;
                   dispatch_semaphore_signal(done);
                 }];
    std::stop_callback cancel(stop, [asset] { [asset cancelLoading]; });
    try {
        wait(done, stop);
    } catch (...) {
        [asset cancelLoading];
        throw;
    }
    if (state->error)
        fail(state->error.localizedDescription);
    if (state->value.count > 1)
        throw std::runtime_error(
            "Multiple media tracks need explicit stream selection and are not supported yet");
    return state->value;
}
struct Stamp {
    struct stat value{};
    explicit Stamp(const QString &path) {
        if (::stat(path.toUtf8().constData(), &value))
            throw std::runtime_error("Media source is missing");
    }
    bool operator==(const Stamp &b) const {
        return value.st_dev == b.value.st_dev && value.st_ino == b.value.st_ino &&
               value.st_size == b.value.st_size && value.st_mtimespec.tv_sec == b.value.st_mtimespec.tv_sec &&
               value.st_mtimespec.tv_nsec == b.value.st_mtimespec.tv_nsec &&
               value.st_ctimespec.tv_sec == b.value.st_ctimespec.tv_sec &&
               value.st_ctimespec.tv_nsec == b.value.st_ctimespec.tv_nsec;
    }
};
QImage imageFromCG(CGImageRef image) {
    const auto width = CGImageGetWidth(image), height = CGImageGetHeight(image);
    if (!width || !height || width > 8192 || height > 8192 || width * height > 16777216)
        throw std::runtime_error("Video dimensions exceed the raster limit");
    QImage result(int(width), int(height), QImage::Format_RGBA8888_Premultiplied);
    if (result.isNull())
        throw std::runtime_error("Could not allocate video frame");
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context =
        CGBitmapContextCreate(result.bits(), width, height, 8, result.bytesPerLine(), space,
                              uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapByteOrder32Big));
    CGColorSpaceRelease(space);
    if (!context)
        throw std::runtime_error("Could not create video color conversion");
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    result = result.convertToFormat(QImage::Format_RGBA8888);
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}
void writeAt(int fd, const char *data, size_t bytes, off_t offset) {
    while (bytes) {
        auto n = ::pwrite(fd, data, bytes, offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw std::runtime_error("Audio cache write failed");
        data += n;
        bytes -= n;
        offset += n;
    }
}
} // namespace
QString mediaFingerprint(const QString &path, std::stop_token stop) {
    QFile f(path);
    if (!QFileInfo(path).isFile() || !f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot open media source");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!f.atEnd()) {
        cancelled(stop);
        auto bytes = f.read(1024 * 1024);
        if (bytes.isEmpty() && f.error() != QFileDevice::NoError)
            throw std::runtime_error("Media read failed");
        hash.addData(bytes);
    }
    return QString::fromLatin1(hash.result().toHex());
}
struct MovieSource::Impl {
    QString path;
    Stamp stamp;
    Asset info;
    AVURLAsset *__strong asset;
    AVAssetImageGenerator *__strong generator = nil;
    std::vector<CMTime> times;
    Time videoEnd{};
    std::mutex mutex;
    int last = -1;
    QImage lastImage;
    Impl(QString p, QString expected, std::stop_token stop)
        : path(QFileInfo(p).canonicalFilePath()), stamp(path) {
        @autoreleasepool {
            info.path = path;
            info.sha256 = mediaFingerprint(path, stop);
            if (!expected.isEmpty() && info.sha256 != expected)
                throw std::runtime_error("Media source changed; reimport it explicitly");
            asset = openAsset(path);
            auto video = tracks(asset, AVMediaTypeVideo, stop).firstObject;
            auto audio = tracks(asset, AVMediaTypeAudio, stop).firstObject;
            if (asset.hasProtectedContent)
                throw std::runtime_error("Protected media is unsupported");
            info.duration = fromCM(asset.duration);
            if (seconds(info.duration) <= 0 || seconds(info.duration) > 86400)
                throw std::runtime_error("Media duration is unsupported");
            info.width = info.height = 1;
            info.hasAudio = audio != nil;
            if (audio) {
                auto d = (__bridge CMAudioFormatDescriptionRef)audio.formatDescriptions.firstObject;
                auto *asbd = d ? CMAudioFormatDescriptionGetStreamBasicDescription(d) : nullptr;
                if (!asbd || !asbd->mChannelsPerFrame || asbd->mChannelsPerFrame > 32)
                    throw std::runtime_error("Audio channel layout is unsupported");
                info.audioChannels = asbd->mChannelsPerFrame;
                info.audioRate = int(std::llround(asbd->mSampleRate));
            }
            if (!video && !audio)
                throw std::runtime_error("No supported video or audio stream");
            info.kind = video ? AssetKind::Video : AssetKind::Audio;
            if (video) {
                for (id object in video.formatDescriptions) {
                    CMFormatDescriptionRef desc = (__bridge CMFormatDescriptionRef)object;
                    CFPropertyListRef transfer =
                        CMFormatDescriptionGetExtension(desc, kCMFormatDescriptionExtension_TransferFunction);
                    if (transfer &&
                        (CFEqual(transfer, kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ) ||
                         CFEqual(transfer, kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG)))
                        throw std::runtime_error(
                            "HDR video needs the future HDR color pipeline; SDR media is supported");
                }
                videoEnd = fromCM(CMTimeRangeGetEnd(video.timeRange));
                NSError *error = nil;
                AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
                if (!reader)
                    fail(error.localizedDescription);
                AVAssetReaderTrackOutput *output = [[AVAssetReaderTrackOutput alloc] initWithTrack:video
                                                                                    outputSettings:nil];
                if (![reader canAddOutput:output])
                    throw std::runtime_error("Cannot read video timestamps");
                [reader addOutput:output];
                if (![reader startReading])
                    fail(reader.error.localizedDescription);
                // ponytail: keep reader calls on this thread; use async output if one-read cancellation
                // latency becomes measurable.
                while (true) {
                    cancelled(stop);
                    CMSampleBufferRef sample = [output copyNextSampleBuffer];
                    if (!sample)
                        break;
                    auto release = std::unique_ptr<const void, decltype(&CFRelease)>(sample, &CFRelease);
                    auto count = CMSampleBufferGetNumSamples(sample);
                    for (CMItemCount i = 0; i < count; ++i) {
                        CMSampleTimingInfo timing{};
                        if (CMSampleBufferGetSampleTimingInfo(sample, i, &timing) != noErr) {
                            throw std::runtime_error("Missing video timestamp");
                        }
                        const auto pts = fromCM(timing.presentationTimeStamp);
                        if (pts < time(0))
                            throw std::runtime_error("Negative video timestamps are unsupported");
                        times.push_back(timing.presentationTimeStamp);
                    }
                    cancelled(stop);
                    if (times.size() > 1000000)
                        throw std::runtime_error("Video exceeds one million indexed frames");
                }
                cancelled(stop);
                if (reader.status != AVAssetReaderStatusCompleted)
                    fail(reader.error.localizedDescription);
                if (times.empty())
                    throw std::runtime_error("Video has no decodable frames");
                std::sort(times.begin(), times.end(),
                          [](CMTime a, CMTime b) { return CMTimeCompare(a, b) < 0; });
                times.erase(std::unique(times.begin(), times.end(),
                                        [](CMTime a, CMTime b) { return CMTimeCompare(a, b) == 0; }),
                            times.end());
                if (times.size() > 1) {
                    auto delta = fromCM(CMTimeSubtract(times[1], times[0]));
                    info.fps = time(delta.denominator, delta.numerator);
                    for (size_t i = 2; i < times.size(); ++i)
                        if (!(fromCM(CMTimeSubtract(times[i], times[i - 1])) == delta))
                            info.variableRate = true;
                } else
                    info.fps = time(30);
                if (info.variableRate)
                    info.fps = time(0);
                if (seconds(info.fps) > 1000)
                    throw std::runtime_error("Source frame rate exceeds1000fps");
                generator = [[AVAssetImageGenerator alloc] initWithAsset:asset];
                generator.appliesPreferredTrackTransform = YES;
                generator.requestedTimeToleranceBefore = kCMTimeZero;
                generator.requestedTimeToleranceAfter = kCMTimeZero;
                lastImage = decode(0, stop);
                last = 0;
                info.width = lastImage.width();
                info.height = lastImage.height();
            }
            if (!(Stamp(path) == stamp))
                throw std::runtime_error("Media source changed during inspection");
        }
    }
    QImage decode(int index, std::stop_token stop) {
        struct Result {
            CGImageRef image = nullptr;
            CMTime at{};
            QString error;
            ~Result() {
                if (image)
                    CGImageRelease(image);
            }
        };
        auto result = std::make_shared<Result>();
        auto done = dispatch_semaphore_create(0);
        const CMTime requested = times.at(index);
        [generator generateCGImageAsynchronouslyForTime:requested
                                      completionHandler:^(CGImageRef image, CMTime actual, NSError *error) {
                                        result->image = image ? CGImageRetain(image) : nullptr;
                                        result->at = actual;
                                        if (error)
                                            result->error = QString::fromNSString(error.localizedDescription);
                                        dispatch_semaphore_signal(done);
                                      }];
        std::stop_callback cancel(stop, [g = generator] { [g cancelAllCGImageGeneration]; });
        try {
            wait(done, stop);
        } catch (...) {
            [generator cancelAllCGImageGeneration];
            throw;
        }
        if (!result->image)
            throw std::runtime_error(result->error.isEmpty() ? "Video frame decode failed"
                                                             : result->error.toStdString());
        if (CMTimeCompare(result->at, requested) != 0)
            throw std::runtime_error("Decoder returned a different video timestamp");
        return imageFromCG(result->image);
    }
};
MovieSource::MovieSource(const QString &p, const QString &h, std::stop_token stop)
    : impl_(std::make_unique<Impl>(p, h, stop)) {}
MovieSource::~MovieSource() = default;
const Asset &MovieSource::info() const { return impl_->info; }
VideoFrame MovieSource::frame(Time at, std::stop_token stop) {
    @autoreleasepool {
        cancelled(stop);
        std::lock_guard lock(impl_->mutex);
        if (!(Stamp(impl_->path) == impl_->stamp))
            throw std::runtime_error("Media source changed; reimport it explicitly");
        if (impl_->times.empty() || at < fromCM(impl_->times.front()) || !(at < impl_->videoEnd))
            return {};
        auto it = std::upper_bound(impl_->times.begin(), impl_->times.end(), at,
                                   [](Time t, CMTime p) { return t < fromCM(p); });
        int index = int(it - impl_->times.begin()) - 1;
        if (index != impl_->last) {
            impl_->lastImage = impl_->decode(index, stop);
            impl_->last = index;
        }
        return {impl_->lastImage, fromCM(impl_->times[index])};
    }
}
struct PcmSource::Impl {
    int fd = -1;
    QString file;
    std::int64_t count = 0;
    std::vector<double> waveform = std::vector<double>(128, 0);
    ~Impl() {
        if (fd >= 0)
            ::close(fd);
        if (!file.isEmpty())
            ::unlink(file.toUtf8().constData());
    }
};
PcmSource::PcmSource(const QString &path, const QString &expected, std::stop_token stop)
    : impl_(std::make_unique<Impl>()) {
    @autoreleasepool {
        Stamp before(path);
        if (mediaFingerprint(path, stop) != expected)
            throw std::runtime_error("Audio source changed");
        AVURLAsset *asset = openAsset(path);
        AVAssetTrack *track = tracks(asset, AVMediaTypeAudio, stop).firstObject;
        if (!track)
            throw std::runtime_error("Source has no audio track");
        const auto end = fromCM(CMTimeRangeGetEnd(track.timeRange));
        if (seconds(end) <= 0 || seconds(end) > 86400)
            throw std::runtime_error("Audio duration is unsupported");
        const auto limit = std::int64_t(std::ceil(seconds(end) * audioSampleRate));
        if (limit <= 0 || limit > std::int64_t(8ull * 1024 * 1024 * 1024 / 8))
            throw std::runtime_error("Decoded audio exceeds the8GiB source-cache limit");
        struct statvfs disk{};
        auto dir = QDir::tempPath();
        if (statvfs(dir.toUtf8().constData(), &disk))
            throw std::runtime_error("Cannot inspect audio-cache storage");
        auto free = std::uint64_t(disk.f_bavail) * disk.f_frsize;
        auto bytes = std::uint64_t(limit) * 8;
        if (free < bytes * 3 || (bytes > 1024ull * 1024 * 1024 && free < 10ull * 1024 * 1024 * 1024))
            throw std::runtime_error("Insufficient free space for decoded audio cache");
        auto pattern = (dir + "/motion-audio-XXXXXX").toUtf8();
        impl_->fd = mkstemp(pattern.data());
        if (impl_->fd < 0)
            throw std::runtime_error("Cannot create private audio cache");
        impl_->file = QString::fromUtf8(pattern);
        fcntl(impl_->fd, F_SETFD, FD_CLOEXEC);
        NSError *error = nil;
        AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
        if (!reader)
            fail(error.localizedDescription);
        auto settings = @{
            AVFormatIDKey : @(kAudioFormatLinearPCM),
            AVSampleRateKey : @(audioSampleRate),
            AVNumberOfChannelsKey : @2,
            AVLinearPCMBitDepthKey : @32,
            AVLinearPCMIsFloatKey : @YES,
            AVLinearPCMIsNonInterleaved : @NO
        };
        AVAssetReaderTrackOutput *output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track
                                                                            outputSettings:settings];
        [reader addOutput:output];
        if (![reader startReading])
            fail(reader.error.localizedDescription);
        // ponytail: keep reader calls on this thread; use async output if one-buffer cancellation
        // latency becomes measurable.
        while (true) {
            cancelled(stop);
            CMSampleBufferRef sample = [output copyNextSampleBuffer];
            if (!sample)
                break;
            auto release = std::unique_ptr<const void, decltype(&CFRelease)>(sample, &CFRelease);
            auto frames = CMSampleBufferGetNumSamples(sample);
            auto block = CMSampleBufferGetDataBuffer(sample);
            if (frames < 0 || frames > 1048576 || !block ||
                CMBlockBufferGetDataLength(block) != size_t(frames) * 8) {
                throw std::runtime_error("Unexpected decoded audio layout");
            }
            auto timestamp = fromCM(CMSampleBufferGetPresentationTimeStamp(sample));
            if (std::abs(seconds(timestamp)) > 864000) {
                throw std::runtime_error("Audio timestamp exceeds limits");
            }
            auto first = std::llround(seconds(timestamp) * audioSampleRate);
            std::vector<float> data(size_t(frames) * 2);
            auto status = CMBlockBufferCopyDataBytes(block, 0, data.size() * 4, data.data());
            if (status)
                throw std::runtime_error("Cannot read decoded audio samples");
            auto skip = std::max<std::int64_t>(0, -first);
            first = std::max<std::int64_t>(0, first);
            auto n = std::min<std::int64_t>(frames - skip, limit - first);
            if (n > 0) {
                for (std::int64_t i = 0; i < n; ++i) {
                    float left = data[(i + skip) * 2], right = data[(i + skip) * 2 + 1];
                    if (!std::isfinite(left) || !std::isfinite(right))
                        throw std::runtime_error("Non-finite audio sample");
                    auto bin = std::min<std::int64_t>(127, (first + i) * 128 / limit);
                    impl_->waveform[bin] =
                        std::max(impl_->waveform[bin],
                                 double(std::min(1.f, std::max(std::abs(left), std::abs(right)))));
                }
                writeAt(impl_->fd, reinterpret_cast<const char *>(data.data() + skip * 2), size_t(n) * 8,
                        off_t(first) * 8);
                impl_->count = std::max(impl_->count, first + n);
            }
            cancelled(stop);
        }
        cancelled(stop);
        if (reader.status != AVAssetReaderStatusCompleted)
            fail(reader.error.localizedDescription);
        if (impl_->count <= 0)
            throw std::runtime_error("No decoded audio samples");
        if (ftruncate(impl_->fd, impl_->count * 8))
            throw std::runtime_error("Audio cache finalization failed");
        if (!(Stamp(path) == before))
            throw std::runtime_error("Audio source changed during decoding");
    }
}
PcmSource::~PcmSource() = default;
std::int64_t PcmSource::frames() const { return impl_->count; }
const std::vector<double> &PcmSource::peaks() const { return impl_->waveform; }
void PcmSource::read(std::int64_t first, int count, float *out) const {
    if (count < 0 || count > 65537)
        throw std::runtime_error("Audio read exceeds block limit");
    std::fill(out, out + size_t(count) * 2, 0.f);
    if (first >= impl_->count || first <= -std::int64_t(count))
        return;
    auto begin = std::max<std::int64_t>(0, first), end = std::min<std::int64_t>(impl_->count, first + count);
    if (end <= begin)
        return;
    char *dest = reinterpret_cast<char *>(out + (begin - first) * 2);
    size_t bytes = size_t(end - begin) * 8;
    off_t offset = begin * 8;
    while (bytes) {
        auto n = pread(impl_->fd, dest, bytes, offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw std::runtime_error("Audio cache read failed");
        dest += n;
        bytes -= n;
        offset += n;
    }
}
std::shared_ptr<PcmSource> pcmSource(const QString &path, const QString &hash, std::stop_token stop) {
    static std::mutex mutex;
    struct Entry {
        Stamp stamp;
        std::shared_ptr<PcmSource> source;
    };
    static std::map<QString, Entry> cache;
    const auto key = QFileInfo(path).canonicalFilePath() + ":" + hash;
    const Stamp stamp(path);
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.find(key); it != cache.end() && it->second.stamp == stamp)
            return it->second.source;
    }
    auto result = std::make_shared<PcmSource>(path, hash, stop);
    std::lock_guard lock(mutex);
    // ponytail: evict unreferenced decoded sources above a1GiB soft budget; active clips retain their cache.
    std::uint64_t bytes = 0;
    for (const auto &[key, p] : cache)
        bytes += p.source->frames() * 8;
    for (auto it = cache.begin(); it != cache.end() && bytes + result->frames() * 8 > 1024ull * 1024 * 1024;)
        if (it->second.source.use_count() == 1) {
            bytes -= it->second.source->frames() * 8;
            it = cache.erase(it);
        } else
            ++it;
    cache.insert_or_assign(key, Entry{stamp, result});
    return result;
}
Asset inspectMedia(const QString &path, Id id, std::stop_token stop) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot open media");
    auto magic = file.read(8);
    file.close();
    if (magic == QByteArray::fromHex("89504e470d0a1a0a"))
        return inspectPng(path, id);
    MovieSource source(path, {}, stop);
    auto result = source.info();
    result.id = id;
    result.path = QFileInfo(path).canonicalFilePath();
    if (result.hasAudio) {
        auto pcm = pcmSource(result.path, result.sha256, stop);
        result.waveform.assign(128, 0);
        for (size_t i = 0; i < pcm->peaks().size(); ++i) {
            auto bin = std::clamp(int((i + .5) * pcm->frames() / audioSampleRate / seconds(result.duration)),
                                  0, 127);
            result.waveform[bin] = std::max(result.waveform[bin], pcm->peaks()[i]);
        }
    }
    return result;
}
struct MovieWriter::Impl {
    AVAssetWriter *__strong writer = nil;
    AVAssetWriterInput *__strong video = nil, *__strong audio = nil;
    AVAssetWriterInputPixelBufferAdaptor *__strong adaptor = nil;
    CMAudioFormatDescriptionRef format = nullptr;
    QSize size;
    bool finished = false, videoFinished = false, audioFinished = false;
    ~Impl() {
        if (writer && !finished)
            [writer cancelWriting];
        if (format)
            CFRelease(format);
    }
    void ready(AVAssetWriterInput *input, std::stop_token stop) {
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!input.readyForMoreMediaData) {
            cancelled(stop);
            if (writer.status == AVAssetWriterStatusFailed)
                fail(writer.error.localizedDescription);
            if (std::chrono::steady_clock::now() > until)
                throw std::runtime_error("Movie encoder timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};
MovieWriter::MovieWriter(const QString &file, QSize size, Time fps, Time duration, bool withAudio)
    : impl_(std::make_unique<Impl>()) {
    @autoreleasepool {
        if (QFileInfo::exists(file))
            throw std::runtime_error("Movie output must be a new file");
        if (size.width() % 2 || size.height() % 2)
            throw std::runtime_error("H.264 output requires even dimensions");
        if (fps.numerator <= 0 || fps.denominator <= 0 || duration.numerator <= 0 || duration.denominator <= 0)
            throw std::runtime_error("Movie frame rate and duration must be positive exact times");
        fps = time(fps.numerator, fps.denominator);
        duration = time(duration.numerator, duration.denominator);
        const auto exactTimeScale = [](std::int64_t a, std::int64_t b) {
            const auto divisor = std::gcd(a, b);
            const auto quotient = a / divisor;
            if (b > INT32_MAX || quotient > INT32_MAX / b)
                throw std::runtime_error("Movie timing needs an unsupported exact media timebase");
            return int32_t(quotient * b);
        };
        const auto videoTimeScale = exactTimeScale(fps.numerator, duration.denominator);
        // The movie edit list must also represent every 48 kHz audio sample boundary.
        const auto movieTimeScale = withAudio ? exactTimeScale(videoTimeScale, audioSampleRate)
                                              : videoTimeScale;
        impl_->size = size;
        NSError *error = nil;
        impl_->writer = [[AVAssetWriter alloc] initWithURL:[NSURL fileURLWithPath:file.toNSString()]
                                                  fileType:AVFileTypeMPEG4
                                                     error:&error];
        if (!impl_->writer)
            fail(error.localizedDescription);
        const auto bitrate =
            std::clamp(double(size.width()) * size.height() * seconds(fps) * .35, 4000000., 80000000.);
        NSDictionary *settings = @{
            AVVideoCodecKey : AVVideoCodecTypeH264,
            AVVideoWidthKey : @(size.width()),
            AVVideoHeightKey : @(size.height()),
            AVVideoColorPropertiesKey : @{
                AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey : AVVideoTransferFunction_IEC_sRGB,
                AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2
            },
            AVVideoCompressionPropertiesKey : @{
                AVVideoAverageBitRateKey : @(bitrate),
                AVVideoExpectedSourceFrameRateKey : @(seconds(fps)),
                AVVideoAllowFrameReorderingKey : @NO,
                AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel
            }
        };
        impl_->video = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                                      outputSettings:settings];
        impl_->video.expectsMediaDataInRealTime = NO;
        // Keep video frame PTS/terminal edges exact and give the movie timeline audio-sample precision.
        impl_->writer.movieTimeScale = movieTimeScale;
        impl_->video.mediaTimeScale = videoTimeScale;
        impl_->adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
               initWithAssetWriterInput:impl_->video
            sourcePixelBufferAttributes:@{
                (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
                (id)kCVPixelBufferWidthKey : @(size.width()),
                (id)kCVPixelBufferHeightKey : @(size.height()),
                (id)kCVPixelBufferIOSurfacePropertiesKey : @{}
            }];
        if (![impl_->writer canAddInput:impl_->video])
            throw std::runtime_error("H.264 encoder is unavailable");
        [impl_->writer addInput:impl_->video];
        if (withAudio) {
            AudioStreamBasicDescription description{};
            description.mSampleRate = audioSampleRate;
            description.mFormatID = kAudioFormatLinearPCM;
            description.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
            description.mBytesPerPacket = description.mBytesPerFrame = 8;
            description.mFramesPerPacket = 1;
            description.mChannelsPerFrame = 2;
            description.mBitsPerChannel = 32;
            if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &description, 0, nullptr, 0, nullptr,
                                               nullptr, &impl_->format))
                throw std::runtime_error("Cannot create audio format");
            impl_->audio = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeAudio
                                                          outputSettings:@{
                                                              AVFormatIDKey : @(kAudioFormatMPEG4AAC),
                                                              AVSampleRateKey : @(audioSampleRate),
                                                              AVNumberOfChannelsKey : @2,
                                                              AVEncoderBitRateKey : @192000
                                                          }
                                                        sourceFormatHint:impl_->format];
            impl_->audio.expectsMediaDataInRealTime = NO;
            if (![impl_->writer canAddInput:impl_->audio])
                throw std::runtime_error("AAC encoder is unavailable");
            [impl_->writer addInput:impl_->audio];
        }
        if (![impl_->writer startWriting])
            fail(impl_->writer.error.localizedDescription);
        [impl_->writer startSessionAtSourceTime:kCMTimeZero];
    }
}
MovieWriter::~MovieWriter() = default;
void MovieWriter::video(const QImage &image, Time at, std::stop_token stop) {
    @autoreleasepool {
        if (image.size() != impl_->size || image.isNull())
            throw std::runtime_error("Encoder frame dimensions differ from composition");
        impl_->ready(impl_->video, stop);
        auto pixels = image.convertToFormat(QImage::Format_ARGB32);
        CVPixelBufferRef buffer = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, impl_->adaptor.pixelBufferPool, &buffer))
            throw std::runtime_error("Cannot allocate encoder frame");
        CVPixelBufferLockBaseAddress(buffer, 0);
        auto *out = static_cast<char *>(CVPixelBufferGetBaseAddress(buffer));
        auto stride = CVPixelBufferGetBytesPerRow(buffer);
        for (int y = 0; y < pixels.height(); ++y)
            std::memcpy(out + y * stride, pixels.constScanLine(y), pixels.width() * 4);
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        BOOL ok = [impl_->adaptor appendPixelBuffer:buffer withPresentationTime:toCM(at)];
        CVPixelBufferRelease(buffer);
        if (!ok)
            fail(impl_->writer.error.localizedDescription);
    }
}
void MovieWriter::audio(const float *data, int count, std::int64_t first, std::stop_token stop) {
    @autoreleasepool {
        if (!impl_->audio)
            return;
        if (!data || count < 1 || count > 65536 || first < 0)
            throw std::runtime_error("Invalid audio encoder block");
        impl_->ready(impl_->audio, stop);
        CMBlockBufferRef block = nullptr;
        CMSampleBufferRef sample = nullptr;
        auto status =
            CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, size_t(count) * 8,
                                               kCFAllocatorDefault, nullptr, 0, size_t(count) * 8, 0, &block);
        if (!status)
            status = CMBlockBufferReplaceDataBytes(data, block, 0, size_t(count) * 8);
        if (!status)
            status = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
                kCFAllocatorDefault, block, impl_->format, count, CMTimeMake(first, audioSampleRate), nullptr,
                &sample);
        if (block)
            CFRelease(block);
        if (status)
            throw std::runtime_error("Cannot create audio encoder samples");
        BOOL ok = [impl_->audio appendSampleBuffer:sample];
        CFRelease(sample);
        if (!ok)
            fail(impl_->writer.error.localizedDescription);
    }
}
void MovieWriter::finishVideo() {
    if (!impl_->videoFinished) {
        [impl_->video markAsFinished];
        impl_->videoFinished = true;
    }
}
void MovieWriter::finishAudio() {
    if (impl_->audio && !impl_->audioFinished) {
        [impl_->audio markAsFinished];
        impl_->audioFinished = true;
    }
}
void MovieWriter::finish(Time duration, std::stop_token stop) {
    @autoreleasepool {
        cancelled(stop);
        finishVideo();
        finishAudio();
        [impl_->writer endSessionAtSourceTime:toCM(duration)];
        auto done = dispatch_semaphore_create(0);
        [impl_->writer finishWritingWithCompletionHandler:^{
          dispatch_semaphore_signal(done);
        }];
        std::stop_callback cancel(stop, [writer = impl_->writer] { [writer cancelWriting]; });
        wait(done, stop);
        if (impl_->writer.status != AVAssetWriterStatusCompleted)
            fail(impl_->writer.error.localizedDescription);
        impl_->finished = true;
    }
}
struct AudioPlayback::Impl {
    AudioQueueRef queue = nullptr;
    std::shared_ptr<AudioMix> mix;
    Time start;
    std::int64_t cursor = 0;
    std::atomic<bool> failed = false;
    static void callback(void *opaque, AudioQueueRef queue, AudioQueueBufferRef buffer) {
        auto *self = static_cast<Impl *>(opaque);
        try {
            auto snapshot = std::atomic_load(&self->mix);
            snapshot->read(self->start + frameTime(self->cursor, time(audioSampleRate)), 512,
                           static_cast<float *>(buffer->mAudioData));
            self->cursor += 512;
            buffer->mAudioDataByteSize = 512 * 8;
            AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
        } catch (...) {
            self->failed = true;
            std::memset(buffer->mAudioData, 0, 512 * 8);
            buffer->mAudioDataByteSize = 512 * 8;
            AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
        }
    }
    ~Impl() {
        if (queue) {
            AudioQueueStop(queue, true);
            AudioQueueDispose(queue, true);
        }
    }
};
AudioPlayback::AudioPlayback(std::shared_ptr<AudioMix> mix, Time start) : impl_(std::make_unique<Impl>()) {
    std::atomic_store(&impl_->mix, std::move(mix));
    impl_->start = start;
    AudioStreamBasicDescription format{};
    format.mSampleRate = audioSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = format.mBytesPerFrame = 8;
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    if (AudioQueueNewOutput(&format, &Impl::callback, impl_.get(), nullptr, nullptr, 0, &impl_->queue))
        throw std::runtime_error("Audio output is unavailable");
    for (int i = 0; i < 3; ++i) {
        AudioQueueBufferRef buffer = nullptr;
        if (AudioQueueAllocateBuffer(impl_->queue, 512 * 8, &buffer))
            throw std::runtime_error("Cannot allocate audio output buffer");
        Impl::callback(impl_.get(), impl_->queue, buffer);
    }
    if (AudioQueueStart(impl_->queue, nullptr))
        throw std::runtime_error("Cannot start audio output");
}
AudioPlayback::~AudioPlayback() = default;
std::int64_t AudioPlayback::playedSamples() const {
    if (impl_->failed)
        throw std::runtime_error("Audio preview failed");
    AudioTimeStamp at{};
    if (AudioQueueGetCurrentTime(impl_->queue, nullptr, &at, nullptr))
        return 0;
    return std::max<std::int64_t>(0, std::llround(at.mSampleTime));
}
void AudioPlayback::update(std::shared_ptr<AudioMix> mix) { std::atomic_store(&impl_->mix, std::move(mix)); }
} // namespace motion
