#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

#include "core/settings.hpp"

using mosaic::AudioSettings;
using mosaic::MicrophoneParameters;

// Regression test for a real use-after-free: AudioManager::start_monitoring()/
// start() bind a raw `const MicrophoneParameters&` into each AudioRecorder
// for that recorder's entire lifetime (see AudioRecorder::m_params), pointing
// directly into AudioSettings::microphones's vector storage. AudioSettingsW's
// constructor later reserve()s AudioSettings::kMaxMicrophones against that
// same vector — if AudioSettings::from_json() hadn't already reserved at
// least that much capacity when it first populated `microphones`, that
// reserve() (or any push_back() past whatever smaller capacity from_json()
// left behind) would reallocate and silently invalidate every already-bound
// AudioRecorder reference, crashing the next time a microphone control is
// edited. Mirrors CameraPersistence.FromJsonReservesCapacityForLiveReferenceStability
// exactly (tests/test_camera_persistence.cpp).
TEST(AudioPersistence, FromJsonReservesCapacityForLiveReferenceStability) {
    QJsonArray mics;
    for (int i = 0; i < 2; ++i) {
        MicrophoneParameters m;
        m.friendlyName = QString("Mic %1").arg(i);
        mics.append(m.to_json());
    }
    const QJsonObject audioObj{{"microphones", mics}};

    const auto loaded = AudioSettings::from_json(audioObj);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->microphones.size(), 2u);
    EXPECT_GE(loaded->microphones.capacity(), static_cast<size_t>(AudioSettings::kMaxMicrophones));
}
