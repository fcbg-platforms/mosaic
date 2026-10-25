// Entry point for mosaic_qml_tests: the QML pages, loaded headless.
//
// A QML page needs a QGuiApplication (mosaic_tests runs on a plain
// QCoreApplication). No window is made and nothing is drawn: the pages are
// created, their bindings run, and any QML warning is recorded for the
// tests to fail on. The default platform plugin is the one deployed next
// to the program (offscreen is not).
#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QStringList>

QStringList g_qmlMessages; // what the pages warned about, read by the tests

namespace {

void record(QtMsgType type, const QMessageLogContext&, const QString& message) {
    if (type != QtDebugMsg && type != QtInfoMsg) {
        g_qmlMessages << message;
    }
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    qInstallMessageHandler(record);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
