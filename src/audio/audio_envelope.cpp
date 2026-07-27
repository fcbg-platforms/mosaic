#include "audio/audio_envelope.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mosaic {

AudioEnvelope compute_envelope(const char* data, qint64 bytes) {
    const int numSamples = static_cast<int>(bytes / 2);
    if (numSamples == 0) { return {}; }

    const auto* s = reinterpret_cast<const int16_t*>(data);
    int16_t lo = s[0];
    int16_t hi = s[0];
    for (int i = 1; i < numSamples; ++i) {
        lo = std::min(lo, s[i]);
        hi = std::max(hi, s[i]);
    }
    return { lo / 32768.0f, hi / 32768.0f };
}

// RMS of 16-bit signed PCM samples, normalised to [0, 1].
float compute_rms(const char* data, qint64 bytes) {
    const int numSamples = static_cast<int>(bytes / 2);
    if (numSamples == 0) { return 0.0f; }

    const auto* s = reinterpret_cast<const int16_t*>(data);
    double sum = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        const double v = s[i] / 32768.0;
        sum += v * v;
    }
    return static_cast<float>(std::sqrt(sum / numSamples));
}

QByteArray convert_float32_to_int16(const char* data, qint64 bytes) {
    const int numSamples = static_cast<int>(bytes / 4);
    QByteArray out;
    out.resize(numSamples * 2);
    if (numSamples == 0) { return out; }

    const auto* src = reinterpret_cast<const float*>(data);
    auto*       dst = reinterpret_cast<int16_t*>(out.data());
    for (int i = 0; i < numSamples; ++i) {
        const float clamped = std::clamp(src[i], -1.0f, 1.0f);
        dst[i] = static_cast<int16_t>(std::lround(clamped * 32767.0f));
    }
    return out;
}

QByteArray convert_int32_to_int16(const char* data, qint64 bytes) {
    const int numSamples = static_cast<int>(bytes / 4);
    QByteArray out;
    out.resize(numSamples * 2);
    if (numSamples == 0) { return out; }

    const auto* src = reinterpret_cast<const int32_t*>(data);
    auto*       dst = reinterpret_cast<int16_t*>(out.data());
    for (int i = 0; i < numSamples; ++i) {
        dst[i] = static_cast<int16_t>(src[i] >> 16);
    }
    return out;
}

} // namespace mosaic
