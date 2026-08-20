// Custom entry point, replacing GTest::gtest_main.
//
// QProcess's async signals (readyReadStandardOutput/Error, finished) are
// only delivered while a Qt event loop is actually running -- gtest's own
// default main() never constructs a QCoreApplication, which was fine while
// every test in this binary was pure synchronous logic. test_analysis_
// manager.cpp needs a real event loop to observe those signals (see
// wait_for_signal() there), so this file provides one for the whole
// binary. RUN_ALL_TESTS() itself still runs synchronously -- pumping
// happens only inside tests that explicitly ask for it.
#include <gtest/gtest.h>

#include <QCoreApplication>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
