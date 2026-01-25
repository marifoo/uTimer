#include "test_settings.h"
#include <QtTest>

void SettingsTest::test_settings_defaults_and_bounds()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = QDir(tempDir.path()).filePath("user-settings.ini");

    // Write nothing: should load defaults
    Settings defaults(settingsPath);
    QCOMPARE(defaults.getHistoryDays(), 99);
    QCOMPARE(defaults.isAutopauseEnabled(), true);
    QCOMPARE(defaults.logToFile(), false);
    QCOMPARE(defaults.getBackpauseMsec(), convMinToMsec(15));
    QCOMPARE(defaults.getCheckpointIntervalMsec(), convMinToMsec(5));

    // Negative history clamps to 0
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/history_days_to_keep", -5);
    writer.sync();
    Settings clamped(settingsPath);
    QCOMPARE(clamped.getHistoryDays(), 0);

    // Bounds for checkpoint interval [0,60]
    writer.setValue("uTimer/checkpoint_interval_minutes", 120);
    writer.sync();
    Settings capped(settingsPath);
    QCOMPARE(capped.getCheckpointIntervalMsec(), convMinToMsec(60));
}
