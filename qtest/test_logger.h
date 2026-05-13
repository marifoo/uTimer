#ifndef TEST_LOGGER_H
#define TEST_LOGGER_H

#include <QObject>
#include <QTemporaryDir>
#include "testcommon.h"

class Settings;

class LoggerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // Test A: disabled-logging path is silent
    void test_a_disabled_logging_is_silent();
    // Test B: enabled-logging path writes to the log file
    void test_b_enabled_logging_writes();
    // Test C: toggle at runtime is honoured (pre-flip messages absent, post-flip present)
    void test_c_toggle_at_runtime_is_honoured();

private:
    QTemporaryDir settingsDir_;
    Settings* settingsOff_ = nullptr;
    Settings* settingsOn_  = nullptr;
};

#endif // TEST_LOGGER_H
