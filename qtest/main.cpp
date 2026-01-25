#include <QApplication>
#include <QtTest>

// Forward declarations of all test classes
class SettingsTest;
class HelpersTest;
class LockStateWatcherTest;
class CleanDurationsTest;
class TimeTrackerTest;
class DatabaseTest;
class IntegrationTest;
class HistoryDialogTest;

// Include test class headers
#include "test_settings.h"
#include "test_helpers.h"
#include "test_lockstatewatcher.h"
#include "test_cleanduration.h"
#include "test_timetracker.h"
#include "test_database.h"
#include "test_integration.h"
#include "test_historydialog.h"

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
    runTest(new SettingsTest, "SettingsTest");
    runTest(new HelpersTest, "HelpersTest");
    runTest(new LockStateWatcherTest, "LockStateWatcherTest");
    runTest(new CleanDurationsTest, "CleanDurationsTest");
    runTest(new TimeTrackerTest, "TimeTrackerTest");
    runTest(new DatabaseTest, "DatabaseTest");
    runTest(new IntegrationTest, "IntegrationTest");
    runTest(new HistoryDialogTest, "HistoryDialogTest");

    qDebug() << "\n================ All Tests Complete ================";
    return status;
}
