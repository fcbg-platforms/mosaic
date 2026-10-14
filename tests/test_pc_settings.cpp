#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "core/pc_settings.hpp"

using namespace mosaic;

TEST(PcSettings, TokenRoundTripsAndKeepsOtherKeys) {
    QTemporaryDir dir;
    const QString path = pc_settings_path(dir.path());
    EXPECT_TRUE(load_pc_hf_token(path).isEmpty()); // no file yet

    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(R"({"other": 7})");
    }
    ASSERT_TRUE(save_pc_hf_token(path, "  hf_abc\n"));
    EXPECT_EQ(load_pc_hf_token(path), "hf_abc");

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    EXPECT_EQ(o["other"].toInt(), 7);
#ifdef _WIN32
    EXPECT_NE(o["hf_token"].toString(), "hf_abc"); // encrypted at rest
#endif
    f.close();

    ASSERT_TRUE(save_pc_hf_token(path, ""));
    EXPECT_TRUE(load_pc_hf_token(path).isEmpty());
}

TEST(PcSettings, FirstProfileTokenGoesToThePc) {
    const HfTokens t = adopt_profile_hf_token("", "hf_a");
    EXPECT_EQ(t.pc, "hf_a");
    EXPECT_TRUE(t.profile.isEmpty());
    EXPECT_TRUE(t.pcChanged);
}

TEST(PcSettings, SameTokenDropsTheProfileCopy) {
    const HfTokens t = adopt_profile_hf_token("hf_a", " hf_a ");
    EXPECT_EQ(t.pc, "hf_a");
    EXPECT_TRUE(t.profile.isEmpty());
    EXPECT_FALSE(t.pcChanged);
}

TEST(PcSettings, DifferentTokenStaysWithTheProfile) {
    const HfTokens t = adopt_profile_hf_token("hf_a", "hf_b");
    EXPECT_EQ(t.pc, "hf_a");
    EXPECT_EQ(t.profile, "hf_b");
    EXPECT_FALSE(t.pcChanged);
}

TEST(PcSettings, NoProfileTokenChangesNothing) {
    const HfTokens t = adopt_profile_hf_token("hf_a", "");
    EXPECT_EQ(t.pc, "hf_a");
    EXPECT_TRUE(t.profile.isEmpty());
    EXPECT_FALSE(t.pcChanged);
}
