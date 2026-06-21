#include "test_lockstatewatcher.h"
#include <QtTest>

void LockStateWatcherTest::test_lockstatewatcher_debounce_logic()
{
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    // feed majority lock pattern
    std::deque<LockQueryResult> pattern = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked };
    LockEvent event = LockEvent::None;
    for (LockQueryResult r : pattern) {
        event = watcher.determineLockEvent_dbg(r);
    }
    QCOMPARE(event, LockEvent::Lock);

    // feed majority unlock pattern
    pattern = {
        LockQueryResult::Locked,   LockQueryResult::Locked,
        LockQueryResult::Unlocked, LockQueryResult::Unlocked, LockQueryResult::Unlocked };
    for (LockQueryResult r : pattern) {
        event = watcher.determineLockEvent_dbg(r);
    }
    QCOMPARE(event, LockEvent::Unlock);
}

// 9.5: Unknown (query failure) must not modify the debounce buffer.
// Scenario: buffer is in a partially-locked state (3 of 5 = Locked).
// Feeding Unknown should not trigger a false Unlock transition.
void LockStateWatcherTest::test_lockstatewatcher_unknown_does_not_unlock()
{
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    // Prime the buffer toward the Lock pattern:
    // [Unlocked, Unlocked, Locked, Locked, Locked] → triggers Lock
    std::deque<LockQueryResult> primePattern = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked };
    for (LockQueryResult r : primePattern) {
        watcher.determineLockEvent_dbg(r);
    }
    // Buffer is now the lock pattern — next Unknown must not change it.

    // Feeding Unknown should leave the buffer unchanged (no Unlock event).
    LockEvent event = watcher.determineLockEvent_dbg(LockQueryResult::Unknown);
    QCOMPARE(event, LockEvent::None);

    // Feeding another Unknown also leaves buffer unchanged.
    event = watcher.determineLockEvent_dbg(LockQueryResult::Unknown);
    QCOMPARE(event, LockEvent::None);

    // A real Unlock sequence must still work after the Unknown samples.
    LockEvent finalEvent = LockEvent::None;
    std::deque<LockQueryResult> unlockPattern = {
        LockQueryResult::Locked,   LockQueryResult::Locked,
        LockQueryResult::Unlocked, LockQueryResult::Unlocked, LockQueryResult::Unlocked };
    for (LockQueryResult r : unlockPattern) {
        finalEvent = watcher.determineLockEvent_dbg(r);
    }
    QCOMPARE(finalEvent, LockEvent::Unlock);
}
