#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include "trigger/trigger_recorder.hpp"

using mosaic::TriggerEvent;
using mosaic::TriggerRecorder;

TEST(TriggerRecorder, HeaderIncludesElapsedNs) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/trigger.csv";

    TriggerRecorder recorder;
    ASSERT_TRUE(recorder.start(path));
    recorder.stop();

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QTextStream ts(&f);
    const QString header = ts.readLine();
    EXPECT_EQ(header, "elapsed_ms,elapsed_ns,wall_clock,source,label,value,code");
}

// This is the regression test for the clock-origin bug: elapsed_ns must be
// the RAW TriggerEvent::timestampNs, not recomputed relative to
// TriggerRecorder::start()'s own snapshot — that's what elapsed_ms already
// is, on a different (recording-relative) zero-point. A wrong reimplementation
// that derives elapsed_ns from elapsed_ms would still pass a naive "column
// exists" check but fail this exact-value assertion.
TEST(TriggerRecorder, RecordsRawElapsedNs) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/trigger.csv";

    TriggerRecorder recorder;
    ASSERT_TRUE(recorder.start(path));

    TriggerEvent ev;
    ev.timestampNs = 1'523'004'112'000LL;
    ev.source      = "parallel_port";
    ev.label       = "D3_RISE";
    ev.value       = 1.0;
    recorder.record_event(ev);
    recorder.stop();

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QTextStream ts(&f);
    ts.readLine(); // header
    const QStringList row = ts.readLine().split(',');
    ASSERT_GE(row.size(), 2);
    EXPECT_EQ(row[1].toLongLong(), ev.timestampNs);
}

// The `code` column is what an external system (an EEG amplifier's own
// trigger channel) matches on — a free-text label is useless there. It must
// reach the CSV as a plain integer, not via `value`'s 6-decimal float format.
TEST(TriggerRecorder, WritesTriggerCodeAsAPlainInteger) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/trigger.csv";

    TriggerRecorder recorder;
    ASSERT_TRUE(recorder.start(path));

    TriggerEvent ev;
    ev.timestampNs = 1000;
    ev.source      = "keyboard";
    ev.label       = "Trial onset";
    ev.code        = 42;
    recorder.record_event(ev);
    recorder.stop();

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QTextStream ts(&f);
    const QStringList header = ts.readLine().split(',');
    const QStringList row    = ts.readLine().split(',');
    const int idxCode        = header.indexOf("code");
    ASSERT_GE(idxCode, 0);
    ASSERT_GT(row.size(), idxCode);
    EXPECT_EQ(row[idxCode], "42");
}

TEST(TriggerRecorder, QuotesLabelAndEscapesEmbeddedQuotes) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/trigger.csv";

    TriggerRecorder recorder;
    ASSERT_TRUE(recorder.start(path));

    TriggerEvent ev;
    ev.timestampNs = 1000;
    ev.source      = "keyboard";
    ev.label       = R"(Say "hi")";
    ev.value       = 0.0;
    recorder.record_event(ev);
    recorder.stop();

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QTextStream ts(&f);
    ts.readLine(); // header
    const QString row = ts.readLine();
    EXPECT_TRUE(row.contains(R"("Say ""hi""")"));
}
