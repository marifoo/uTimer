#include <QApplication>
#include <QtTest>

// Forward declarations of all test classes
class LoggerTest;
class SettingsTest;
class HelpersTest;
class LockStateWatcherTest;
class CleanDurationsTest;
class TimerTest;
class DatabaseTest;
class IntegrationTest;
class HistoryDialogTest;
class ShutdownCoordinatorTest;
class HealthMonitorTest;
class TimelineTest;
class RenamesTest;

// Include test class headers
#include "test_logger.h"
#include "test_settings.h"
#include "test_helpers.h"
#include "test_lockstatewatcher.h"
#include "test_cleanduration.h"
#include "test_timer.h"
#include "test_database.h"
#include "test_integration.h"
#include "test_historydialog.h"
#include "test_shutdowncoordinator.h"
#include "test_healthmonitor.h"
#include "test_timeline.h"
#include "test_renames.h"

// Test runner main function
// Executes all test suites sequentially using QTest::qExec()
// Returns non-zero exit code if any test suite fails
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    int status = 0;

    // Lambda to run a test suite and track failures
    auto runTest = [&](auto* test, const char* name) {
        qDebug() << "\n================" << name << "================";
        int result = QTest::qExec(test, argc, argv);
        delete test;
        if (result != 0) {
            status = result;
        }
        return result;
    };

    // Run test suites
    runTest(new LoggerTest, "LoggerTest");
    runTest(new SettingsTest, "SettingsTest");
    runTest(new HelpersTest, "HelpersTest");
    runTest(new LockStateWatcherTest, "LockStateWatcherTest");
    runTest(new CleanDurationsTest, "CleanDurationsTest");
    runTest(new TimerTest, "TimerTest");
    runTest(new DatabaseTest, "DatabaseTest");
    runTest(new IntegrationTest, "IntegrationTest");
    runTest(new HistoryDialogTest, "HistoryDialogTest");
    runTest(new ShutdownCoordinatorTest, "ShutdownCoordinatorTest");
    runTest(new HealthMonitorTest, "HealthMonitorTest");
    runTest(new TimelineTest, "TimelineTest");
    runTest(new RenamesTest, "RenamesTest");

    qDebug() << "\n================ All Tests Complete ================";
    return status;
}
