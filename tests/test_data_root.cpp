#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "core/data_root.hpp"

using namespace mosaic;

namespace {

void touch(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("{}");
}

QStringList names(const QList<SessionMove>& moves, const QString& base) {
    QStringList out;
    for (const auto& m : moves) {
        out << QDir(base).relativeFilePath(m.session) + " -> " +
                   QDir(base).relativeFilePath(m.destParent);
    }
    return out;
}

} // namespace

TEST(DataRoot, FindsTheCheckoutAboveTheProgram) {
    QTemporaryDir dir;
    touch(dir.filePath("mosaic/analysis/pyproject.toml"));
    const QString exe = dir.filePath("mosaic/build/Release/bin/Release");
    QDir().mkpath(exe);
    EXPECT_EQ(find_checkout_root(exe), QDir::cleanPath(dir.filePath("mosaic")));
    EXPECT_EQ(find_checkout_root(dir.filePath("mosaic")), QDir::cleanPath(dir.filePath("mosaic")));
}

TEST(DataRoot, NoCheckoutGivesNothing) {
    QTemporaryDir dir;
    QDir().mkpath(dir.filePath("app/bin"));
    EXPECT_TRUE(find_checkout_root(dir.filePath("app/bin")).isEmpty());
}

class StrandedSessions : public ::testing::Test {
   protected:
    void SetUp() override {
        oldDir = tmp.filePath("build/bin/recordings");
        newDir = tmp.filePath("recordings");
        touch(oldDir + "/virginie/s1/session_meta.json");
        touch(oldDir + "/virginie/s2/session_meta.json");
        touch(oldDir + "/virginie/s3.partial/session_meta.json"); // a left-over copy
        touch(oldDir + "/virginie/notes/readme.txt");             // not a session
        touch(oldDir + "/csru/c1/session_meta.json");
        touch(oldDir + "/loose/session_meta.json"); // the old shared layout
    }
    QTemporaryDir tmp;
    QString oldDir, newDir;
};

TEST_F(StrandedSessions, AProfileMovesOnlyItsOwn) {
    const auto moves = stranded_sessions(oldDir, newDir, "virginie", false);
    EXPECT_EQ(names(moves, tmp.path()),
              (QStringList{"build/bin/recordings/virginie/s1 -> recordings/virginie",
                           "build/bin/recordings/virginie/s2 -> recordings/virginie"}));
}

TEST_F(StrandedSessions, AnAdminMovesEveryProfilesAndLooseOnes) {
    const QStringList all = names(stranded_sessions(oldDir, newDir, "boss", true), tmp.path());
    EXPECT_EQ(all.size(), 4);
    EXPECT_TRUE(all.contains("build/bin/recordings/csru/c1 -> recordings/csru"));
    EXPECT_TRUE(all.contains("build/bin/recordings/loose -> recordings"));
}

TEST_F(StrandedSessions, NothingWhenTheOldPlaceIsTheDataFolder) {
    EXPECT_TRUE(stranded_sessions(oldDir, oldDir, "virginie", true).isEmpty());
    EXPECT_TRUE(stranded_sessions(oldDir + "/../recordings/.", oldDir, "virginie", true).isEmpty());
    EXPECT_TRUE(stranded_sessions(tmp.filePath("nope"), newDir, "virginie", true).isEmpty());
}
