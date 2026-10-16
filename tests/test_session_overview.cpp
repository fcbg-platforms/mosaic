// The Session Overview's lanes (analysis/session_overview.hpp), built from
// the golden session's real result files and from missing ones.

#include <gtest/gtest.h>

#include <cmath>

#include "analysis/conversation_result.hpp"
#include "analysis/eye_contact_result.hpp"
#include "analysis/face_dynamics_result.hpp"
#include "analysis/rppg_result.hpp"
#include "analysis/session_overview.hpp"

using namespace mosaic;

namespace {

const QString kGolden = QStringLiteral(MOSAIC_GOLDEN_DIR);

const OverviewLane* lane(const SessionOverview& ov, const QString& id) {
    for (const auto& l : ov.lanes) {
        if (l.id == id) {
            return &l;
        }
    }
    return nullptr;
}

} // namespace

TEST(SessionOverview, EveryLaneFromTheGoldenSession) {
    const auto conv = ConversationResult::load(kGolden + "/conversation/video_2.conversation.json");
    const auto eye  = EyeContactResult::load(kGolden + "/eye_contact/video_2.eye_contact.json");
    const auto face =
        FaceDynamicsResult::load(kGolden + "/face_dynamics/video_2.face_dynamics.json");
    const auto rppg = RppgResult::load(kGolden + "/rppg/video_2.pos.rppg.json");
    ASSERT_TRUE(conv.is_valid() && eye.is_valid() && face.is_valid() && rppg.is_valid());

    const SessionOverview ov = build_session_overview({&conv, &eye, &face, &rppg});
    ASSERT_EQ(ov.lanes.size(), 5);
    EXPECT_EQ(ov.lanes[0].id, "speaking");
    EXPECT_EQ(ov.lanes[4].id, "heart_rate");
    for (const auto& l : ov.lanes) {
        EXPECT_TRUE(l.note.isEmpty()) << l.id.toStdString();
    }
    // The span of the video, in ms from its first frame.
    EXPECT_NEAR(
        ov.durationMs,
        static_cast<double>(face.frames().last().timestampMs - face.frames().first().timestampMs),
        1000.0);

    // Who speaks: as many stretches as the result's subject/other spurts.
    int spurts = 0;
    for (const auto& s : conv.spurts()) {
        spurts += (s.speaker == "subject" || s.speaker == "other") ? 1 : 0;
    }
    EXPECT_EQ(lane(ov, "speaking")->spans.size(), spurts);

    // Every stretch and point lies within the video.
    for (const auto& l : ov.lanes) {
        for (const auto& s : l.spans) {
            EXPECT_GE(s.startMs, -1.0);
            EXPECT_LE(s.endMs, ov.durationMs + 1.0);
            EXPECT_LE(s.startMs, s.endMs);
        }
        for (const auto& p : l.points) {
            EXPECT_GE(p.x(), -1.0) << l.id.toStdString();
            EXPECT_LE(p.x(), ov.durationMs + 1.0) << l.id.toStdString();
        }
    }

    // Eye contact: looking and looking-away stretches never overlap.
    const auto& eyeSpans = lane(ov, "eye_contact")->spans;
    for (int i = 1; i < eyeSpans.size(); ++i) {
        EXPECT_GE(eyeSpans[i].startMs, eyeSpans[i - 1].endMs - 1e-6);
    }

    // Heart rate's range holds every rate drawn.
    const OverviewLane* hr = lane(ov, "heart_rate");
    for (const auto& p : hr->points) {
        if (std::isfinite(p.y())) {
            EXPECT_GE(p.y(), hr->minY);
            EXPECT_LE(p.y(), hr->maxY);
        }
    }
}

TEST(SessionOverview, LanesWithoutAnAnalysisSayWhichToRun) {
    const auto rppg          = RppgResult::load(kGolden + "/rppg/video_2.pos.rppg.json");
    const SessionOverview ov = build_session_overview({nullptr, nullptr, nullptr, &rppg});
    ASSERT_EQ(ov.lanes.size(), 5);
    EXPECT_EQ(lane(ov, "speaking")->note, "Conversation Timing not run");
    EXPECT_EQ(lane(ov, "eye_contact")->note, "Eye Contact not run");
    EXPECT_EQ(lane(ov, "smile")->note, "Face Dynamics not run");
    EXPECT_TRUE(lane(ov, "heart_rate")->note.isEmpty());
    EXPECT_GT(ov.durationMs, 0.0);

    const SessionOverview none = build_session_overview({});
    EXPECT_EQ(none.durationMs, 0.0);
    EXPECT_EQ(none.lanes.size(), 5);
}
