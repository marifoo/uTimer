#include "test_logger.h"
#include <QtTest>
#include <QSettings>
#include "logger.h"
#include "settings.h"

void LoggerTest::initTestCase()
{
    QVERIFY(settingsDir_.isValid());

    // settingsOff_: defaults — logToFile is false by default (verified in test_settings)
    settingsOff_ = new Settings(settingsDir_.filePath("off.ini"));
    QCOMPARE(settingsOff_->logToFile(), false);

    // settingsOn_: write debug_log_to_file = true before constructing
    {
        QSettings ini(settingsDir_.filePath("on.ini"), QSettings::IniFormat);
        ini.setValue("uTimer/debug_log_to_file", true);
        ini.sync();
    }
    settingsOn_ = new Settings(settingsDir_.filePath("on.ini"));
    QCOMPARE(settingsOn_->logToFile(), true);

    // Start each run with logging off so earlier test suites that call Logger::Log
    // don't accidentally create the file before Test A.
    Logger::registerSettings(*settingsOff_);
}

void LoggerTest::cleanupTestCase()
{
    // Leave logging disabled so later test suites (run after LoggerTest) don't
    // write to the shared log file unexpectedly.
    Logger::registerSettings(*settingsOff_);
    delete settingsOff_;
    delete settingsOn_;
    settingsOff_ = nullptr;
    settingsOn_  = nullptr;
}

// Test A: with logToFile = false, 100 Log() calls must not create or grow the log file.
void LoggerTest::test_a_disabled_logging_is_silent()
{
    Logger::registerSettings(*settingsOff_);

    const QString logPath = Logger::logFilePath();
    const qint64 sizeBefore = QFile::exists(logPath) ? QFileInfo(logPath).size() : -1;

    for (int i = 0; i < 100; i++) {
        Logger::Log(QString("test_a_should_not_appear_%1").arg(i));
    }

    if (sizeBefore == -1) {
        QVERIFY2(!QFile::exists(logPath), "Log file must not be created when logging is disabled");
    } else {
        QCOMPARE(QFileInfo(logPath).size(), sizeBefore);
    }
}

// Test B: with logToFile = true, a Log() call must appear in the log file.
void LoggerTest::test_b_enabled_logging_writes()
{
    Logger::registerSettings(*settingsOn_);

    const QString marker = QString("test_b_marker_%1").arg(QDateTime::currentMSecsSinceEpoch());
    Logger::Log(marker);

    const QString logPath = Logger::logFilePath();
    QVERIFY2(QFile::exists(logPath), "Log file must be created when logging is enabled");

    QFile f(logPath);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    QVERIFY2(content.contains(marker),
             qPrintable(QString("Log file must contain marker '%1'").arg(marker)));
}

// Test C: toggling the setting mid-run — messages before the flip must not appear,
// messages after must appear.
void LoggerTest::test_c_toggle_at_runtime_is_honoured()
{
    // Logging off: this message must NOT appear
    Logger::registerSettings(*settingsOff_);
    const QString beforeMarker = QString("BEFORE_FLIP_%1").arg(QDateTime::currentMSecsSinceEpoch());
    Logger::Log(beforeMarker);

    // Logging on: this message MUST appear
    Logger::registerSettings(*settingsOn_);
    const QString afterMarker = QString("AFTER_FLIP_%1").arg(QDateTime::currentMSecsSinceEpoch());
    Logger::Log(afterMarker);

    const QString logPath = Logger::logFilePath();
    QFile f(logPath);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    QVERIFY2(!content.contains(beforeMarker),
             qPrintable(QString("Pre-flip message '%1' must not appear in log").arg(beforeMarker)));
    QVERIFY2(content.contains(afterMarker),
             qPrintable(QString("Post-flip message '%1' must appear in log").arg(afterMarker)));
}
