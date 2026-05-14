#include "test_shutdowncoordinator.h"
#include "fakedatabasemanager.h"
#include "shutdowncoordinator.h"
#include <QtTest>

using TestCommon::createSettingsFile;

// ============================================================================
// Test G — happy path: stop, flush, marker
// ============================================================================

void ShutdownCoordinatorTest::test_G_happy_path_stop_flush_marker()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb, settings);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.getOngoingDuration().has_value());

    fakeDb.callLog.clear();

    coordinator.run(false);

    QVERIFY(!tracker.getOngoingDuration().has_value());
    QVERIFY(fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));

    int flushIdx  = fakeDb.callLog.lastIndexOf("flushToDisc");
    int markerIdx = fakeDb.callLog.lastIndexOf("setLastCleanShutdownMarker");
    QVERIFY2(flushIdx < markerIdx, "flush must precede marker write");
}

// ============================================================================
// Test H — idempotent: second run() is a no-op
// ============================================================================

void ShutdownCoordinatorTest::test_H_idempotent_second_run_is_noop()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb, settings);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    coordinator.run(false);

    fakeDb.callLog.clear();

    coordinator.run(false);

    QVERIFY(!fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));
}

// ============================================================================
// Test I — force-direct path skips retry loop (flushToDisc still happens)
// ============================================================================

void ShutdownCoordinatorTest::test_I_force_direct_skips_retry_loop()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeDatabaseManager fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb, settings);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.getOngoingDuration().has_value());

    fakeDb.callLog.clear();

    // The force-direct path should stop the timer with a single direct call.
    // We verify the net result (stopped) and that the essential post-stop
    // operations (flush, marker) still happen.
    coordinator.run(true);

    QVERIFY(!tracker.getOngoingDuration().has_value());
    QVERIFY(fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));
}
