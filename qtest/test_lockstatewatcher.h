#ifndef TEST_LOCKSTATEWATCHER_H
#define TEST_LOCKSTATEWATCHER_H

#include <QObject>
#include "testcommon.h"

class LockStateWatcherTest : public QObject
{
    Q_OBJECT

private slots:
    void test_lockstatewatcher_debounce_logic();
    // 9.5: Unknown query result must not change the debounce buffer
    void test_lockstatewatcher_unknown_does_not_unlock();
};

#endif // TEST_LOCKSTATEWATCHER_H
