#include "test_healthmonitor.h"
#include "healthmonitor.h"
#include <QtTest>
#include <QSignalSpy>

using TestCommon::createSettingsFile;

// Settings helpers: thresholds are fixed in settings.cpp:
//   activity threshold : 9h 45min  = 585 min = 35 100 000 ms
//   nopause threshold  : 6h        = 360 min = 21 600 000 ms
//   pause required     : 30 min    =  1 800 000 ms

static constexpr qint64 kActivityThresholdMs = 9LL * 60 * 60 * 1000 + 45LL * 60 * 1000;
static constexpr qint64 kNoPauseActivityMs   = 6LL * 60 * 60 * 1000;
static constexpr qint64 kMinPauseMs          = 30LL * 60 * 1000;

// ============================================================================
// Test J — activity warning fires exactly once per session
// ============================================================================

void HealthMonitorTest::test_J_activity_warning_fires_once()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QSettings seed(QDir(tempDir.path()).filePath("user-settings.ini"), QSettings::IniFormat);
    seed.setValue("uTimer/show_warning_after_9h45min_activity", true);
    seed.setValue("uTimer/show_warning_when_not_30min_pause_after_6h_activity", false);
    seed.sync();
    Settings settings(QDir(tempDir.path()).filePath("user-settings.ini"));

    HealthMonitor monitor(settings);
    monitor.reset();
    QSignalSpy spy(&monitor, &HealthMonitor::warningTriggered);

    // Below threshold — no warning
    for (int i = 0; i < 5; ++i)
        monitor.check(kActivityThresholdMs - 1, 0);
    QCOMPARE(spy.count(), 0);

    // Cross threshold — fires once
    monitor.check(kActivityThresholdMs + 1, 0);
    QCOMPARE(spy.count(), 1);

    // Further calls — still only one
    for (int i = 0; i < 10; ++i)
        monitor.check(kActivityThresholdMs + i * 1000, 0);
    QCOMPARE(spy.count(), 1);
}

// ============================================================================
// Test K — no-pause warning condition
// ============================================================================

void HealthMonitorTest::test_K_nopause_warning_condition()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QSettings seed(QDir(tempDir.path()).filePath("user-settings.ini"), QSettings::IniFormat);
    seed.setValue("uTimer/show_warning_after_9h45min_activity", false);
    seed.setValue("uTimer/show_warning_when_not_30min_pause_after_6h_activity", true);
    seed.sync();
    Settings settings(QDir(tempDir.path()).filePath("user-settings.ini"));

    // Activity high, pause LOW → warning fires
    {
        HealthMonitor monitor(settings);
        monitor.reset();
        QSignalSpy spy(&monitor, &HealthMonitor::warningTriggered);
        monitor.check(kNoPauseActivityMs + 1, kMinPauseMs - 1);
        QCOMPARE(spy.count(), 1);
    }

    // Activity high, pause OK → no warning
    {
        HealthMonitor monitor(settings);
        monitor.reset();
        QSignalSpy spy(&monitor, &HealthMonitor::warningTriggered);
        monitor.check(kNoPauseActivityMs + 1, kMinPauseMs + 1);
        QCOMPARE(spy.count(), 0);
    }

    // Activity below threshold → no warning even with low pause
    {
        HealthMonitor monitor(settings);
        monitor.reset();
        QSignalSpy spy(&monitor, &HealthMonitor::warningTriggered);
        monitor.check(kNoPauseActivityMs - 1, 0);
        QCOMPARE(spy.count(), 0);
    }
}

// ============================================================================
// Test L — reset re-arms both warnings
// ============================================================================

void HealthMonitorTest::test_L_reset_rearms_warnings()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QSettings seed(QDir(tempDir.path()).filePath("user-settings.ini"), QSettings::IniFormat);
    seed.setValue("uTimer/show_warning_after_9h45min_activity", true);
    seed.setValue("uTimer/show_warning_when_not_30min_pause_after_6h_activity", true);
    seed.sync();
    Settings settings(QDir(tempDir.path()).filePath("user-settings.ini"));

    HealthMonitor monitor(settings);
    monitor.reset();
    QSignalSpy spy(&monitor, &HealthMonitor::warningTriggered);

    // Fire both warnings once
    monitor.check(kActivityThresholdMs + 1, 0);
    monitor.check(kNoPauseActivityMs + 1, kMinPauseMs - 1);
    int firstRoundCount = spy.count();
    QVERIFY(firstRoundCount >= 1);

    // reset() re-arms
    monitor.reset();
    spy.clear();

    // Both should fire again after reset
    monitor.check(kActivityThresholdMs + 1, 0);
    monitor.check(kNoPauseActivityMs + 1, kMinPauseMs - 1);
    QVERIFY(spy.count() >= 1);
}
