#include "test_lockstatewatcher.h"
#include <QtTest>

void LockStateWatcherTest::test_lockstatewatcher_debounce_logic()
{
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    // feed majority lock pattern
    std::deque<bool> pattern = {false,false,true,true,true};
    LockEvent event = LockEvent::None;
    for (bool state : pattern) {
        event = watcher.determineLockEvent(state);
    }
    QCOMPARE(event, LockEvent::Lock);

    // feed majority unlock pattern
    pattern = {true,true,false,false,false};
    for (bool state : pattern) {
        event = watcher.determineLockEvent(state);
    }
    QCOMPARE(event, LockEvent::Unlock);
}
